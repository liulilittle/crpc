// CRPC -- cluster health bridge tests.
//
// The bridge is the single writer between probe results and the service
// catalog. These cases pin the three behaviours the audit asked for:
//
//   * a debounce window suppresses a flapping probe sequence,
//   * a consecutive-failure threshold decides when the catalog goes critical,
//   * recovery is half-open: a critical node only leaves critical after the
//     configured number of consecutive passes.
//
// The concurrency case checks the single-writer contract directly: concurrent
// probes never touch the registry, and one flush applies every transition.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include <crpc/cluster/health_bridge.hpp>
#include <crpc/cluster/registry.hpp>
#include <crpc/resilience/load_balancer.hpp>

namespace cl = crpc::cluster;

using crpc::rpc_code;

namespace crpc {
namespace cluster {
inline std::ostream& operator<<(std::ostream& os, instance_state state) {
    return os << to_string(state);
}
inline std::ostream& operator<<(std::ostream& os, health_state state) {
    switch (state) {
    case health_state::passing:  return os << "passing";
    case health_state::warning:  return os << "warning";
    case health_state::critical: return os << "critical";
    }
    return os << "unknown";
}
} // namespace cluster
} // namespace crpc

namespace {

using namespace std::chrono_literals;

struct manual_clock {
    cl::monotonic_clock::time_point now{};
};

cl::monotonic_clock::time_point manual_now(void* context) noexcept {
    return static_cast<manual_clock*>(context)->now;
}

cl::monotonic_clock make_clock(manual_clock& clock) noexcept {
    return cl::monotonic_clock{&manual_now, &clock};
}

cl::service_instance make_instance(const std::string& service,
                                   const std::string& node, crpc::u16 port,
                                   crpc::u64 incarnation) {
    cl::service_instance instance;
    instance.service = service;
    instance.node_id = node;
    instance.address.host = "127.0.0.1";
    instance.address.port = port;
    instance.weight = 1;
    instance.incarnation = incarnation;
    instance.state = cl::instance_state::passing;
    return instance;
}

cl::write_context make_ctx(const std::string& node, crpc::u64 incarnation = 0) {
    cl::write_context context;
    context.node_id = node;
    context.incarnation = incarnation;
    return context;
}

cl::instance_state registry_state(cl::registry& reg, const std::string& service,
                                  const std::string& node) {
    cl::query_options options;
    options.service = service;
    options.node_id = node;
    const auto result = reg.query(options);
    if (!result.ok() || result.value().instances.empty()) {
        return cl::instance_state::deregistered;
    }
    return result.value().instances.front().state;
}

void register_node(cl::registry& reg, const std::string& service,
                   const std::string& node, crpc::u16 port) {
    CRPC_REQUIRE(reg.register_instance(make_instance(service, node, port, 1),
                                       make_ctx(node, 1))
                     .ok());
}

// A writer that is not the bridge and uses the legacy registration path. Kept
// for the negative case: under strict_incarnation the same (key, incarnation)
// with a different payload is rejected instead of clobbering health.
crpc::result<cl::write_result> legacy_external_register(
    cl::registry& reg, const std::string& service, const std::string& node,
    crpc::u16 port, cl::instance_state state, crpc::u64 incarnation = 1) {
    cl::service_instance instance = make_instance(service, node, port, incarnation);
    instance.state = state;
    return reg.register_instance(instance, make_ctx(node, incarnation));
}

// A writer that is not the bridge. set_state writes only the state field and
// carries an explicit fence above the published high-water, which is the legal
// external-writer contract under the strict default. The bridge must treat it
// as an external write (P5: register_instance no longer writes health at all).
void external_register(cl::registry& reg, const std::string& service,
                       const std::string& node, cl::instance_state state,
                       crpc::u64 incarnation = 1) {
    cl::write_context context = make_ctx(node, incarnation);
    const crpc::u64 published = reg.fence();
    context.fence = published == std::numeric_limits<crpc::u64>::max()
                        ? published
                        : published + 1;
    CRPC_REQUIRE(reg.set_state(service, node, state, context).ok());
}

} // namespace

// --- Debounce, failure threshold and half-open recovery ----------------------

CRPC_TEST(health_bridge_debounce_threshold_and_half_open) {
    cl::registry reg;
    register_node(reg, "api", "n1", 8001);

    manual_clock clock;
    cl::health_bridge bridge(reg, 100ms, 2, 2, 16, make_clock(clock));

    const auto t0 = clock.now;

    // A pass keeps the node passing.
    CRPC_CHECK(bridge.report("api", "n1", true, t0));
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush(t0));
    CRPC_CHECK_EQ(cl::health_state::passing, bridge.state("api", "n1"));
    CRPC_CHECK_EQ(cl::instance_state::passing, registry_state(reg, "api", "n1"));

    // First failure: below the threshold, so the candidate is warning, but the
    // debounce window has not elapsed yet.
    CRPC_CHECK(bridge.report("api", "n1", false, t0));
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush(t0));
    CRPC_CHECK_EQ(cl::instance_state::passing, registry_state(reg, "api", "n1"));

    // Second failure reaches the threshold; the debounce clock restarts.
    CRPC_CHECK(bridge.report("api", "n1", false, t0 + 50ms));
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush(t0 + 100ms));
    CRPC_CHECK_EQ(cl::instance_state::passing, registry_state(reg, "api", "n1"));
    CRPC_CHECK_EQ(crpc::u64{1}, bridge.flush(t0 + 150ms));
    CRPC_CHECK_EQ(cl::health_state::critical, bridge.state("api", "n1"));
    CRPC_CHECK_EQ(cl::instance_state::critical, registry_state(reg, "api", "n1"));

    // Half-open: the first pass is not enough to leave critical.
    CRPC_CHECK(bridge.report("api", "n1", true, t0 + 200ms));
    CRPC_CHECK(bridge.half_open("api", "n1"));
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush(t0 + 400ms));
    CRPC_CHECK_EQ(cl::instance_state::critical, registry_state(reg, "api", "n1"));

    // The second consecutive pass meets the recovery threshold. The state
    // change still honours the debounce window.
    CRPC_CHECK(bridge.report("api", "n1", true, t0 + 500ms));
    CRPC_CHECK_FALSE(bridge.half_open("api", "n1"));
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush(t0 + 550ms));
    CRPC_CHECK_EQ(cl::instance_state::critical, registry_state(reg, "api", "n1"));
    CRPC_CHECK_EQ(crpc::u64{1}, bridge.flush(t0 + 650ms));
    CRPC_CHECK_EQ(cl::instance_state::passing, registry_state(reg, "api", "n1"));
    CRPC_CHECK_EQ(cl::health_state::passing, bridge.state("api", "n1"));
}

CRPC_TEST(health_bridge_debounce_suppresses_flap) {
    cl::registry reg;
    register_node(reg, "api", "n1", 8001);

    manual_clock clock;
    cl::health_bridge bridge(reg, 100ms, 1, 1, 16, make_clock(clock));
    const auto t0 = clock.now;

    CRPC_CHECK(bridge.report("api", "n1", false, t0));
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush(t0 + 50ms));
    // The success arrives before the failure has been committed: the candidate
    // returns to the committed state and the pending failure is dropped.
    CRPC_CHECK(bridge.report("api", "n1", true, t0 + 60ms));
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush(t0 + 500ms));
    CRPC_CHECK_EQ(cl::health_state::passing, bridge.state("api", "n1"));
    CRPC_CHECK_EQ(cl::instance_state::passing, registry_state(reg, "api", "n1"));
}

// --- Single writer and bounded state -----------------------------------------

CRPC_TEST(health_bridge_concurrent_probes_have_one_writer) {
    cl::registry reg;
    constexpr int kNodes = 4;
    for (int i = 0; i < kNodes; ++i) {
        register_node(reg, "api", "n" + std::to_string(i),
                      static_cast<crpc::u16>(8100 + i));
    }

    manual_clock clock;
    cl::health_bridge bridge(reg, 0ms, 1, 1, 32, make_clock(clock));

    const crpc::u64 index_before = reg.index();
    std::vector<std::thread> threads;
    for (int i = 0; i < kNodes; ++i) {
        threads.emplace_back([&bridge, i] {
            const std::string node = "n" + std::to_string(i);
            for (int attempt = 0; attempt < 100; ++attempt) {
                (void)bridge.report("api", node, false);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    // No probe reached the registry: the bridge is the only writer and it only
    // writes from flush().
    CRPC_CHECK_EQ(index_before, reg.index());
    CRPC_CHECK_EQ(crpc::u64{static_cast<crpc::u64>(kNodes)}, bridge.flush());
    CRPC_CHECK_EQ(index_before + kNodes, reg.index());
    for (int i = 0; i < kNodes; ++i) {
        CRPC_CHECK_EQ(cl::instance_state::critical,
                      registry_state(reg, "api", "n" + std::to_string(i)));
    }
    // A second flush has nothing left to apply.
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush());
}

CRPC_TEST(health_bridge_bounds_and_prunes_failed_nodes) {
    cl::registry reg;
    register_node(reg, "api", "n1", 8001);
    register_node(reg, "api", "n2", 8002);

    manual_clock clock;
    cl::health_bridge bridge(reg, 0ms, 1, 1, 3, make_clock(clock));

    CRPC_CHECK(bridge.report("api", "n1", false));
    CRPC_CHECK(bridge.report("api", "n2", false));
    CRPC_CHECK_EQ(crpc::u64{2}, bridge.node_count());

    // A probe for an instance that is not in the catalog cannot be committed;
    // the bridge drops the record instead of leaking it.
    CRPC_CHECK(bridge.report("api", "ghost", false));
    CRPC_CHECK_EQ(crpc::u64{3}, bridge.node_count());
    // The node bound is a hard bound, not a hint: one more node is refused and
    // nothing is allocated for it.
    CRPC_CHECK_FALSE(bridge.report("api", "n3", false));
    CRPC_CHECK_EQ(crpc::u64{3}, bridge.node_count());

    CRPC_CHECK_EQ(crpc::u64{2}, bridge.flush());
    CRPC_CHECK_EQ(crpc::u64{2}, bridge.node_count());
}

CRPC_TEST(health_bridge_unregistered_node_is_not_written) {
    cl::registry reg;
    register_node(reg, "api", "n1", 8001);

    manual_clock clock;
    cl::health_bridge bridge(reg, 0ms, 1, 1, 8, make_clock(clock));
    const crpc::u64 index_before = reg.index();

    CRPC_CHECK(bridge.report("api", "ghost", false));
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush());
    CRPC_CHECK_EQ(index_before, reg.index());
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.node_count());
}

// --- Read-only load-balancer view -------------------------------------------

CRPC_TEST(health_bridge_lb_view_tracks_health) {
    cl::registry reg;
    register_node(reg, "api", "n1", 8001);
    register_node(reg, "api", "n2", 8002);
    register_node(reg, "api", "n3", 8003);

    manual_clock clock;
    cl::health_bridge bridge(reg, 0ms, 1, 1, 16, make_clock(clock));

    const auto view = bridge.lb_view("api");
    CRPC_REQUIRE(static_cast<bool>(view));
    CRPC_CHECK_EQ(std::size_t{3}, view->size());
    for (std::size_t i = 0; i < view->size(); ++i) {
        CRPC_CHECK(view->at(i).healthy);
    }

    crpc::u64 n1_id = 0;
    const crpc::u64 version_before = view->version();
    for (std::size_t i = 0; i < view->size(); ++i) {
        if (view->node_id_at(i) == "n1") {
            n1_id = view->at(i).id;
        }
    }
    CRPC_CHECK(n1_id != 0);

    CRPC_CHECK(bridge.report("api", "n1", false));
    CRPC_CHECK_EQ(crpc::u64{1}, bridge.flush());
    CRPC_CHECK(view->version() > version_before);

    bool n1_healthy = true;
    for (std::size_t i = 0; i < view->size(); ++i) {
        if (view->node_id_at(i) == "n1") {
            n1_healthy = view->at(i).healthy;
            CRPC_CHECK(!view->at(i).healthy);
        }
    }
    CRPC_CHECK_FALSE(n1_healthy);

    // The load balancer sees the health view and never selects the critical
    // endpoint.
    crpc::resilience::load_balancer lb(crpc::resilience::lb_strategy::round_robin);
    for (int i = 0; i < 32; ++i) {
        const auto picked = lb.select(*view);
        CRPC_REQUIRE(picked.ok());
        CRPC_CHECK(picked.value().id != n1_id);
        CRPC_CHECK(picked.value().healthy);
    }
}

// --- G23 H1: external state adoption ----------------------------------------
//
// The bridge is the single writer of health. An external write that makes the
// catalog *worse* (a lease-expiry critical) is a higher-priority liveness
// writer and must be adopted fail-safe. An external write that makes the
// catalog *better* (a register_instance clobbering health back to passing) must
// never be adopted as if the bridge had committed it: recovery only happens
// through the bridge's own fenced, debounced write.

CRPC_TEST(health_bridge_adopts_only_more_severe_external_state) {
    cl::registry reg;
    register_node(reg, "api", "n1", 8001);

    manual_clock clock;
    cl::health_bridge bridge(reg, 0ms, 1, 1, 16, make_clock(clock));

    CRPC_CHECK(bridge.report("api", "n1", true));
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush());
    CRPC_CHECK_EQ(cl::health_state::passing, bridge.state("api", "n1"));

    // Lease expiry writes critical through the registration path.
    external_register(reg, "api", "n1", cl::instance_state::critical);
    CRPC_CHECK_EQ(cl::instance_state::critical, registry_state(reg, "api", "n1"));

    // A health pass must not immediately undo the liveness verdict. The more
    // severe external state is adopted; there is no write back to passing.
    CRPC_CHECK(bridge.report("api", "n1", true));
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush());
    CRPC_CHECK_EQ(cl::health_state::critical, bridge.state("api", "n1"));
    CRPC_CHECK_EQ(cl::instance_state::critical, registry_state(reg, "api", "n1"));
}

CRPC_TEST(health_bridge_rejects_external_downgrade_of_health) {
    cl::registry reg;
    register_node(reg, "api", "n1", 8001);

    manual_clock clock;
    cl::health_bridge bridge(reg, 0ms, 1, 1, 16, make_clock(clock));

    CRPC_CHECK(bridge.report("api", "n1", false));
    CRPC_CHECK_EQ(crpc::u64{1}, bridge.flush());
    CRPC_CHECK_EQ(cl::health_state::critical, bridge.state("api", "n1"));

    // register_instance rewrites the instance and clobbers health back to
    // passing (P5). Under strict_incarnation, this is now a conflict and
    // cannot be attempted. The bridge's contract is now to repair only
    // through set_state (its own fenced write) and to treat all other
    // external writes as illegal.
    const auto conflicting =
        legacy_external_register(reg, "api", "n1", 8002, cl::instance_state::passing);
    CRPC_CHECK_FALSE(conflicting.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, conflicting.code());
    const std::string* reason = conflicting.error().metadata.find("registry.reason");
    CRPC_CHECK(reason != nullptr && *reason == "conflict");

    // The rejected write cannot clobber the catalog, so there is nothing to
    // repair: the bridge must not write and must not bump its version.
    const crpc::u64 version_before = bridge.version();
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush());
    CRPC_CHECK_EQ(cl::health_state::critical, bridge.state("api", "n1"));
    CRPC_CHECK_EQ(cl::instance_state::critical, registry_state(reg, "api", "n1"));
    CRPC_CHECK_EQ(version_before, bridge.version());
}

CRPC_TEST(health_bridge_requires_own_verified_write_for_recovery) {
    cl::registry reg;
    register_node(reg, "api", "n1", 8001);

    manual_clock clock;
    cl::health_bridge bridge(reg, 0ms, 1, 1, 16, make_clock(clock));

    CRPC_CHECK(bridge.report("api", "n1", false));
    CRPC_CHECK_EQ(crpc::u64{1}, bridge.flush());
    CRPC_CHECK_EQ(cl::health_state::critical, bridge.state("api", "n1"));

    // The probe recovered, but before the bridge's fenced write the catalog is
    // externally set to passing. The bridge may only claim recovery after its
    // own write is accepted, so this flush must perform exactly that write.
    CRPC_CHECK(bridge.report("api", "n1", true));
    external_register(reg, "api", "n1", cl::instance_state::passing);
    CRPC_CHECK_EQ(crpc::u64{1}, bridge.flush());
    CRPC_CHECK_EQ(cl::health_state::passing, bridge.state("api", "n1"));
    CRPC_CHECK_EQ(cl::instance_state::passing, registry_state(reg, "api", "n1"));
}

// --- G23 H2: fencing --------------------------------------------------------

CRPC_TEST(health_bridge_stale_epoch_cannot_overwrite_takeover) {
    cl::registry reg;
    register_node(reg, "api", "n1", 8001);

    manual_clock clock;

    // The old bridge owns the instance and has committed critical.
    cl::health_bridge old_bridge(reg, 0ms, 1, 1, 16, make_clock(clock));
    CRPC_CHECK(old_bridge.report("api", "n1", false));
    CRPC_CHECK_EQ(crpc::u64{1}, old_bridge.flush());
    CRPC_CHECK_EQ(cl::instance_state::critical, registry_state(reg, "api", "n1"));

    // A fresh bridge is constructed after the old epoch has published a fence;
    // it therefore owns a strictly newer fence epoch and takes over.
    cl::health_bridge new_bridge(reg, 0ms, 1, 1, 16, make_clock(clock));
    CRPC_CHECK(new_bridge.report("api", "n1", true));
    (void)new_bridge.flush();
    CRPC_CHECK(new_bridge.report("api", "n1", true));
    CRPC_CHECK_EQ(crpc::u64{1}, new_bridge.flush());
    CRPC_CHECK_EQ(cl::instance_state::passing, registry_state(reg, "api", "n1"));

    // The old bridge still believes critical. Its flush must be rejected by the
    // registry fence, must not adopt the newer passing state as its own, and
    // must stop retrying once it has yielded.
    CRPC_CHECK(old_bridge.report("api", "n1", false));
    CRPC_CHECK_EQ(crpc::u64{0}, old_bridge.flush());
    CRPC_CHECK_EQ(cl::instance_state::passing, registry_state(reg, "api", "n1"));
    CRPC_CHECK_EQ(cl::health_state::critical, old_bridge.state("api", "n1"));
    CRPC_CHECK_EQ(crpc::u64{0}, old_bridge.flush());
}

CRPC_TEST(health_bridge_reanchors_after_same_epoch_fence_bump) {
    cl::registry reg;
    register_node(reg, "api", "n1", 8001);

    manual_clock clock;
    cl::health_bridge bridge(reg, 0ms, 1, 1, 16, make_clock(clock));

    CRPC_CHECK(bridge.report("api", "n1", false));
    CRPC_CHECK_EQ(crpc::u64{1}, bridge.flush());
    CRPC_CHECK_EQ(cl::health_state::critical, bridge.state("api", "n1"));

    // Two ordinary re-registrations (not a newer health bridge) advance the
    // key's fence above the bridge's next token. The bridge must re-anchor and
    // keep writing instead of yielding its authority to a same-epoch writer.
    external_register(reg, "api", "n1", cl::instance_state::critical);
    external_register(reg, "api", "n1", cl::instance_state::critical);

    CRPC_CHECK(bridge.report("api", "n1", true));
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush()); // stale fence: re-anchor
    CRPC_CHECK_EQ(crpc::u64{1}, bridge.flush()); // retry succeeds
    CRPC_CHECK_EQ(cl::health_state::passing, bridge.state("api", "n1"));
    CRPC_CHECK_EQ(cl::instance_state::passing, registry_state(reg, "api", "n1"));
}

// --- G23 H3: default debounce thresholds ------------------------------------

CRPC_TEST(health_bridge_default_success_threshold_is_two) {
    cl::registry reg;
    register_node(reg, "api", "n1", 8001);

    // Defaults only: debounce 0, failure_threshold 1, success_threshold must
    // match health_check's default of 2.
    cl::health_bridge bridge(reg);

    const cl::monotonic_clock::time_point t0 =
        cl::monotonic_clock::time_point{} + std::chrono::seconds{1};

    CRPC_CHECK(bridge.report("api", "n1", false, t0));
    CRPC_CHECK_EQ(crpc::u64{1}, bridge.flush(t0));
    CRPC_CHECK_EQ(cl::health_state::critical, bridge.state("api", "n1"));

    // One pass is not a recovery under the default threshold.
    CRPC_CHECK(bridge.report("api", "n1", true, t0 + 1s));
    CRPC_CHECK(bridge.half_open("api", "n1"));
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush(t0 + 1s));
    CRPC_CHECK_EQ(cl::health_state::critical, bridge.state("api", "n1"));
    CRPC_CHECK_EQ(cl::instance_state::critical, registry_state(reg, "api", "n1"));

    // The second consecutive pass recovers.
    CRPC_CHECK(bridge.report("api", "n1", true, t0 + 2s));
    CRPC_CHECK_FALSE(bridge.half_open("api", "n1"));
    CRPC_CHECK_EQ(crpc::u64{1}, bridge.flush(t0 + 2s));
    CRPC_CHECK_EQ(cl::health_state::passing, bridge.state("api", "n1"));
    CRPC_CHECK_EQ(cl::instance_state::passing, registry_state(reg, "api", "n1"));
}

// --- G23 H4: view lifetime --------------------------------------------------

CRPC_TEST(health_bridge_view_survives_bridge_destruction) {
    cl::registry reg_a;
    register_node(reg_a, "api", "n1", 8001);
    register_node(reg_a, "api", "n2", 8002);

    cl::registry reg_b;
    register_node(reg_b, "other", "o1", 9001);
    register_node(reg_b, "other", "o2", 9002);
    register_node(reg_b, "other", "o3", 9003);

    manual_clock clock;

    // Placement-new at the same address makes the old dangling-pointer path
    // deterministic: a view that dereferences the destroyed bridge sees the
    // replacement bridge and the wrong catalog. A view that owns the bridge
    // state keeps reporting the last snapshot it observed.
    alignas(cl::health_bridge) unsigned char storage[sizeof(cl::health_bridge)];
    auto* first = new (storage)
        cl::health_bridge(reg_a, 0ms, 1, 1, 16, make_clock(clock));
    const std::shared_ptr<cl::health_instance_set> view = first->lb_view("api");
    CRPC_REQUIRE(static_cast<bool>(view));
    CRPC_CHECK_EQ(std::size_t{2}, view->size());
    CRPC_CHECK_EQ(std::string{"n1"}, view->node_id_at(0));
    CRPC_CHECK_EQ(std::string{"n2"}, view->node_id_at(1));

    first->~health_bridge();
    auto* second = new (storage)
        cl::health_bridge(reg_b, 0ms, 1, 1, 16, make_clock(clock));
    // Advance the replacement's state so a dangling view would be forced to
    // rebuild from the wrong catalog instead of returning its cached stamp.
    CRPC_CHECK(second->report("other", "o1", false));
    CRPC_CHECK_EQ(crpc::u64{1}, second->flush());

    CRPC_CHECK_EQ(std::size_t{2}, view->size());
    CRPC_CHECK_EQ(std::string{"n1"}, view->node_id_at(0));
    CRPC_CHECK_EQ(std::string{"n2"}, view->node_id_at(1));

    second->~health_bridge();
}

// --- G23 H5: tombstone handling ---------------------------------------------

CRPC_TEST(health_bridge_prunes_tombstones_without_writing) {
    cl::registry reg;
    register_node(reg, "api", "n1", 8001);

    manual_clock clock;
    cl::health_bridge bridge(reg, 0ms, 1, 1, 16, make_clock(clock));

    CRPC_CHECK(bridge.report("api", "n1", false));
    CRPC_CHECK_EQ(crpc::u64{1}, bridge.flush());
    CRPC_CHECK_EQ(std::size_t{1}, bridge.node_count());

    // The bridge's own flush fenced the key, so the deletion must carry a
    // fresh epoch like any other external write under the strict default.
    cl::write_context removal = make_ctx("n1", 1);
    const crpc::u64 published = reg.fence();
    removal.fence = published == std::numeric_limits<crpc::u64>::max()
                        ? published
                        : published + 1;
    CRPC_CHECK(reg.deregister("api", "n1", removal).ok());

    // The tombstone is visible only with include_deregistered=true; the bridge
    // must query it explicitly and prune the record instead of leaking it.
    cl::query_options options;
    options.service = "api";
    options.node_id = "n1";
    options.include_deregistered = true;
    const auto tombstone = reg.query(options);
    CRPC_REQUIRE(tombstone.ok());
    CRPC_REQUIRE(tombstone.value().instances.size() == 1);
    CRPC_CHECK_EQ(cl::instance_state::deregistered,
                  tombstone.value().instances.front().state);

    const crpc::u64 index_before = reg.index();
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush());
    CRPC_CHECK_EQ(std::size_t{0}, bridge.node_count());
    // Pruning is local; it must not touch the catalog.
    CRPC_CHECK_EQ(index_before, reg.index());
}

// --- G23 H6: version domain -------------------------------------------------

CRPC_TEST(health_bridge_version_is_single_epoch) {
    // version() is a change counter, not a sum of the catalog index and the
    // health version. Two bridges that have observed the same number of
    // changes report the same version even when their catalogs have different
    // sizes; a sum of the two domains would collide/alias.
    cl::registry small;
    register_node(small, "api", "n1", 8001);
    manual_clock clock;
    cl::health_bridge bridge_small(small, 0ms, 1, 1, 16, make_clock(clock));
    const auto view_small = bridge_small.lb_view("api");
    CRPC_REQUIRE(static_cast<bool>(view_small));
    CRPC_CHECK_EQ(std::size_t{1}, view_small->size());
    const crpc::u64 version_small = view_small->version();

    cl::registry big;
    register_node(big, "api", "n1", 8101);
    register_node(big, "api", "n2", 8102);
    register_node(big, "api", "n3", 8103);
    cl::health_bridge bridge_big(big, 0ms, 1, 1, 16, make_clock(clock));
    const auto view_big = bridge_big.lb_view("api");
    CRPC_REQUIRE(static_cast<bool>(view_big));
    CRPC_CHECK_EQ(std::size_t{3}, view_big->size());
    const crpc::u64 version_big = view_big->version();

    CRPC_CHECK_EQ(version_small, version_big);

    // One membership change advances the epoch by exactly one.
    register_node(small, "api", "n2", 8002);
    CRPC_CHECK_EQ(std::size_t{2}, view_small->size());
    CRPC_CHECK_EQ(version_small + 1, view_small->version());

    // A committed health transition advances it too.
    CRPC_CHECK(bridge_small.report("api", "n1", false));
    CRPC_CHECK_EQ(crpc::u64{1}, bridge_small.flush());
    CRPC_CHECK(view_small->version() > version_small + 1);
}

CRPC_TEST_MAIN()
