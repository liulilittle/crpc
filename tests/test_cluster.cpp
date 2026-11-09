// CRPC -- cluster module tests (registry, lease, election, health, watch).
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include <crpc/cluster/election.hpp>
#include <crpc/cluster/health.hpp>
#include <crpc/cluster/lease.hpp>
#include <crpc/cluster/registry.hpp>
#include <crpc/cluster/watch.hpp>

namespace cl = crpc::cluster;
using crpc::rpc_code;

namespace crpc {
inline std::ostream& operator<<(std::ostream& os, rpc_code code) {
    return os << to_string(code);
}
} // namespace crpc

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
inline std::ostream& operator<<(std::ostream& os, watch_event_kind kind) {
    switch (kind) {
    case watch_event_kind::snapshot: return os << "snapshot";
    case watch_event_kind::delta:    return os << "delta";
    case watch_event_kind::resync:   return os << "resync";
    case watch_event_kind::dropped:  return os << "dropped";
    }
    return os << "unknown";
}
} // namespace cluster
} // namespace crpc

namespace {

using namespace std::chrono_literals;

struct manual_clock {
    cl::monotonic_clock::time_point now{};

    void advance(std::chrono::milliseconds delta) { now += delta; }
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

cl::write_context make_ctx(const std::string& node, crpc::u64 fence = 0,
                           crpc::u64 incarnation = 0) {
    cl::write_context context;
    context.node_id = node;
    context.fence = fence;
    context.incarnation = incarnation;
    context.writer_epoch = 0;  // external writer: not epoch-tracked
    return context;
}

cl::write_context make_admin_ctx(const std::string& token,
                                 const std::string& node,
                                 crpc::u64 fence = 0) {
    cl::write_context context;
    context.node_id = node;
    context.admin_token = token;
    context.fence = fence;
    context.writer_epoch = 0;  // external admin writer: not epoch-tracked
    return context;
}

// The default config is strict (require_issued_fence), so a caller that writes
// must first obtain a token from the authority for that exact key.
cl::write_context make_issued_ctx(cl::registry& reg, const std::string& service,
                                  const std::string& node, crpc::u64 incarnation = 0) {
    cl::write_context context = make_ctx(node, 0, incarnation);
    const auto token = reg.next_fence(service, node);
    if (token.ok()) {
        context.fence = token.value();
    }
    return context;
}

struct watch_capture {
    std::vector<cl::watch_event_kind> kinds;
    std::vector<crpc::u64> indexes;
    std::size_t snapshot_instances = 0;
    std::size_t delta_instances = 0;
    std::size_t resyncs = 0;
    std::size_t drops = 0;
};

void capture_callback(void* context, const cl::watch_event& event) noexcept {
    auto* capture = static_cast<watch_capture*>(context);
    capture->kinds.push_back(event.kind);
    capture->indexes.push_back(event.index);
    switch (event.kind) {
    case cl::watch_event_kind::snapshot:
        if (event.snapshot != nullptr) {
            capture->snapshot_instances += event.snapshot->size();
        }
        break;
    case cl::watch_event_kind::delta:
        if (event.instance != nullptr) {
            ++capture->delta_instances;
        }
        break;
    case cl::watch_event_kind::resync:
        if (event.snapshot != nullptr) {
            capture->snapshot_instances += event.snapshot->size();
        }
        ++capture->resyncs;
        break;
    case cl::watch_event_kind::dropped:
        ++capture->drops;
        break;
    }
}

struct probe_flag {
    bool pass = false;
};

bool flag_probe(void* context) noexcept {
    return static_cast<probe_flag*>(context)->pass;
}

struct tcp_probe_state {
    bool pass = false;
    int calls = 0;
};

bool tcp_probe(void* context, const cl::endpoint&) noexcept {
    auto* state = static_cast<tcp_probe_state*>(context);
    ++state->calls;
    return state->pass;
}

class stub_quorum final : public cl::quorum {
public:
    crpc::u32 prevote_granted = 0;
    crpc::u32 vote_granted = 0;
    bool leader_alive = false;
    std::string leader;

    [[nodiscard]] cl::prevote_result prevote(const std::string&,
                                             crpc::u64) override {
        cl::prevote_result result;
        result.granted = prevote_granted;
        result.leader_alive = leader_alive;
        result.leader_id = leader;
        return result;
    }

    [[nodiscard]] cl::vote_result vote(const std::string&,
                                       crpc::u64) override {
        cl::vote_result result;
        result.granted = vote_granted;
        return result;
    }
};

} // namespace

// --- Registry ---------------------------------------------------------------

CRPC_TEST(registry_register_query_deregister) {
    cl::registry reg;
    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 1),
                                     make_issued_ctx(reg, "api", "n1", 1))
                     .ok());
    CRPC_CHECK(reg.register_instance(make_instance("api", "n2", 8002, 1),
                                     make_issued_ctx(reg, "api", "n2", 1))
                     .ok());

    auto tagged = make_instance("api", "n3", 8003, 1);
    tagged.meta.set("tag", "blue");
    CRPC_CHECK(reg.register_instance(tagged,
                                     make_issued_ctx(reg, "api", "n3", 1))
                     .ok());

    cl::query_options by_service;
    by_service.service = "api";
    auto all = reg.query(by_service);
    CRPC_CHECK(all.ok());
    CRPC_CHECK_EQ(std::size_t{3}, all.value().instances.size());
    CRPC_CHECK_EQ(std::string("n1"), all.value().instances[0].node_id);
    CRPC_CHECK_EQ(std::string("n2"), all.value().instances[1].node_id);
    CRPC_CHECK_EQ(std::string("n3"), all.value().instances[2].node_id);

    cl::query_options by_tag;
    by_tag.service = "api";
    by_tag.tag = "blue";
    auto tagged_result = reg.query(by_tag);
    CRPC_CHECK(tagged_result.ok());
    CRPC_CHECK_EQ(std::size_t{1}, tagged_result.value().instances.size());
    CRPC_CHECK_EQ(std::string("n3"), tagged_result.value().instances[0].node_id);

    cl::query_options by_node;
    by_node.node_id = "n2";
    auto node_result = reg.query(by_node);
    CRPC_CHECK(node_result.ok());
    CRPC_CHECK_EQ(std::size_t{1}, node_result.value().instances.size());
    CRPC_CHECK_EQ(std::string("api"), node_result.value().instances[0].service);

    CRPC_CHECK(reg.deregister("api", "n1",
                              make_issued_ctx(reg, "api", "n1", 1))
                     .ok());
    auto after = reg.query(by_service);
    CRPC_CHECK(after.ok());
    CRPC_CHECK_EQ(std::size_t{2}, after.value().instances.size());

    by_service.include_deregistered = true;
    auto tombstones = reg.query(by_service);
    CRPC_CHECK(tombstones.ok());
    CRPC_CHECK_EQ(std::size_t{3}, tombstones.value().instances.size());
    CRPC_CHECK_EQ(cl::instance_state::deregistered,
                  tombstones.value().instances[0].state);

    // Deregistration is idempotent. The first call spends its issuance, so the
    // replay needs a fresh authority token; the incarnation is the stored
    // tombstone incarnation (2), which the replay repeats exactly.
    CRPC_CHECK(reg.deregister("api", "n1",
                              make_issued_ctx(reg, "api", "n1", 2))
                     .ok());
}

CRPC_TEST(registry_passing_only_and_maintenance) {
    cl::registry reg;
    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 1),
                                     make_issued_ctx(reg, "api", "n1", 1))
                     .ok());
    CRPC_CHECK(reg.register_instance(make_instance("api", "n2", 8002, 1),
                                     make_issued_ctx(reg, "api", "n2", 1))
                     .ok());
    CRPC_CHECK(reg.set_state("api", "n1", cl::instance_state::maintenance,
                             make_issued_ctx(reg, "api", "n1", 1))
                     .ok());

    cl::query_options passing;
    passing.service = "api";
    passing.passing_only = true;
    auto result = reg.query(passing);
    CRPC_CHECK(result.ok());
    CRPC_CHECK_EQ(std::size_t{1}, result.value().instances.size());
    CRPC_CHECK_EQ(std::string("n2"), result.value().instances[0].node_id);
}

CRPC_TEST(registry_rejects_non_owner_write) {
    cl::registry reg;
    // The principal is wrong, so the authority check rejects before any fence
    // question. A token is still supplied so the reason is provably "authority"
    // and not "not_issued": the two are distinguishable only if both are legal
    // up to that point.
    const auto issued = reg.next_fence("api", "n1");
    CRPC_REQUIRE(issued.ok());
    cl::write_context forged = make_ctx("n2", issued.value(), 1);
    const auto rejected = reg.register_instance(
        make_instance("api", "n1", 8001, 1), forged);
    CRPC_CHECK_FALSE(rejected.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, rejected.code());
    CRPC_CHECK_EQ(std::string("authority"),
                  rejected.error().metadata.find("registry.reason") != nullptr
                      ? *rejected.error().metadata.find("registry.reason")
                      : std::string{});
}

CRPC_TEST(registry_admin_token) {
    // The admin path is exercised in its legacy shape on purpose. Under the
    // strict default (require_issued_fence) a token is bound to the principal it
    // was issued for, and next_fence() issues for the key owner's principal
    // only, so an admin writer holding a different node id has no way to obtain
    // a token the authority would accept. That is a gap in the admin API, not in
    // the authority check this case pins, so the case sets the switch off
    // explicitly and documents it.
    cl::registry_config config;
    config.admin_token = "secret";
    config.require_issued_fence = false;
    cl::registry reg(config);

    CRPC_CHECK(reg.register_instance(make_instance("api", "n9", 8009, 1),
                                     make_admin_ctx("secret", "ops")).ok());
    const auto denied = reg.register_instance(
        make_instance("api", "n10", 8010, 1), make_admin_ctx("wrong", "ops"));
    CRPC_CHECK_FALSE(denied.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, denied.code());
}

CRPC_TEST(registry_rejects_stale_incarnation) {
    cl::registry reg;
    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 5),
                                     make_issued_ctx(reg, "api", "n1", 5))
                     .ok());
    const auto stale = reg.register_instance(
        make_instance("api", "n1", 8001, 4), make_issued_ctx(reg, "api", "n1", 4));
    CRPC_CHECK_FALSE(stale.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, stale.code());
    // A changed endpoint is a new incarnation under the strict replay default;
    // the same incarnation with a different payload is a conflict.
    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8002, 6),
                                     make_issued_ctx(reg, "api", "n1", 6))
                     .ok());
}

CRPC_TEST(registry_tombstone_blocks_old_incarnation) {
    cl::registry reg;
    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 1),
                                     make_issued_ctx(reg, "api", "n1", 1))
                     .ok());
    CRPC_CHECK(reg.deregister("api", "n1",
                              make_issued_ctx(reg, "api", "n1", 1))
                     .ok());

    const auto old = reg.register_instance(
        make_instance("api", "n1", 8001, 1), make_issued_ctx(reg, "api", "n1", 1));
    CRPC_CHECK_FALSE(old.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, old.code());
    const auto equal = reg.register_instance(
        make_instance("api", "n1", 8001, 2), make_issued_ctx(reg, "api", "n1", 2));
    CRPC_CHECK_FALSE(equal.ok());
    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 3),
                                     make_issued_ctx(reg, "api", "n1", 3))
                     .ok());
}

CRPC_TEST(registry_index_and_fence_are_monotonic) {
    cl::registry reg;
    // The token is issued by the authority first: a caller-supplied 10 would be
    // an unissued token under the default config. The issuance returns
    // max(global, per-key floor) + 1, so the exact value is asserted through the
    // issued token rather than a hand-written constant.
    const auto issued = reg.next_fence("api", "n1");
    CRPC_REQUIRE(issued.ok());
    const auto first = reg.register_instance(make_instance("api", "n1", 8001, 1),
                                             make_ctx("n1", issued.value(), 1));
    CRPC_CHECK(first.ok());
    CRPC_CHECK_EQ(crpc::u64{1}, first.value().index);
    CRPC_CHECK_EQ(issued.value(), first.value().fence);

    const auto second = reg.register_instance(make_instance("api", "n2", 8002, 1),
                                              make_issued_ctx(reg, "api", "n2", 1));
    CRPC_CHECK(second.ok());
    CRPC_CHECK_EQ(crpc::u64{2}, second.value().index);
    CRPC_CHECK(second.value().index > first.value().index);
    CRPC_CHECK(second.value().fence > first.value().fence);

    // Below the key high-water is stale whatever the issuance check says.
    const auto stale = reg.set_state("api", "n1", cl::instance_state::critical,
                                     make_ctx("n1", issued.value() - 1, 1));
    CRPC_CHECK_FALSE(stale.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, stale.code());

    // The equal value is not a reusable ticket either: it is the high-water the
    // accepted write reported, and feeding it back is an equal-fence write.
    const auto reused = reg.set_state("api", "n1", cl::instance_state::critical,
                                      make_ctx("n1", first.value().fence, 1));
    CRPC_CHECK_FALSE(reused.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, reused.code());

    // A fresh issuance is accepted and advances the index.
    const auto next_issued = reg.next_fence("api", "n1");
    CRPC_REQUIRE(next_issued.ok());
    const auto accepted = reg.set_state("api", "n1", cl::instance_state::critical,
                                        make_ctx("n1", next_issued.value(), 1));
    CRPC_CHECK(accepted.ok());
    CRPC_CHECK_EQ(crpc::u64{3}, accepted.value().index);
    CRPC_CHECK_EQ(accepted.value().index, reg.index());
}

CRPC_TEST(registry_bounded_limits) {
    cl::registry_config config;
    config.max_services = 1;
    config.max_instances_per_service = 2;
    config.max_total_instances = 10;
    cl::registry reg(config);

    CRPC_CHECK(reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                     make_issued_ctx(reg, "svc", "n1", 1))
                     .ok());
    CRPC_CHECK(reg.register_instance(make_instance("svc", "n2", 8002, 1),
                                     make_issued_ctx(reg, "svc", "n2", 1))
                     .ok());
    const auto per_service = reg.register_instance(
        make_instance("svc", "n3", 8003, 1), make_issued_ctx(reg, "svc", "n3", 1));
    CRPC_CHECK_FALSE(per_service.ok());
    CRPC_CHECK_EQ(rpc_code::resource_exhausted, per_service.code());

    const auto service_limit = reg.register_instance(
        make_instance("other", "n4", 8004, 1), make_issued_ctx(reg, "other", "n4", 1));
    CRPC_CHECK_FALSE(service_limit.ok());
    CRPC_CHECK_EQ(rpc_code::resource_exhausted, service_limit.code());
}

CRPC_TEST(registry_bounded_metadata) {
    cl::registry_config config;
    config.max_metadata_bytes = 4;
    cl::registry reg(config);

    auto instance = make_instance("svc", "n1", 8001, 1);
    instance.meta.set("key", "value");
    const auto rejected = reg.register_instance(
        instance, make_issued_ctx(reg, "svc", "n1", 1));
    CRPC_CHECK_FALSE(rejected.ok());
    CRPC_CHECK_EQ(rpc_code::resource_exhausted, rejected.code());
}

CRPC_TEST(registry_tombstone_purge) {
    cl::registry_config config;
    config.tombstone_grace = 0;
    cl::registry reg(config);

    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 1),
                                     make_issued_ctx(reg, "api", "n1", 1))
                     .ok());
    CRPC_CHECK(reg.deregister("api", "n1",
                              make_issued_ctx(reg, "api", "n1", 1))
                     .ok());

    // Zero grace means "eligible immediately": the very next purge removes the
    // tombstone, and because the include_deregistered view changes it advances
    // the index exactly once for that removal.
    const crpc::u64 index_before = reg.index();
    CRPC_CHECK_EQ(std::size_t{1}, reg.purge_expired_tombstones());
    CRPC_CHECK_EQ(index_before + 1, reg.index());

    // The purged row cannot be resurrected by an old incarnation: the
    // tombstone incarnation high-water (2) survives the physical removal. The
    // zero-fence attempt is rejected by Z2 before the incarnation rule anyway,
    // which is the default config's own guarantee.
    const auto stale =
        reg.register_instance(make_instance("api", "n1", 8001, 1), make_ctx("n1"));
    CRPC_CHECK_FALSE(stale.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, stale.code());

    // The purged tombstone watermark survives, so the key is no longer fresh:
    // the resurrection must carry an authority-issued token above the surviving
    // high-water (F4).
    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 3),
                                     make_issued_ctx(reg, "api", "n1", 3))
                     .ok());
    CRPC_CHECK_EQ(std::size_t{0}, reg.purge_expired_tombstones());

    cl::query_options options;
    options.include_deregistered = true;
    auto result = reg.query(options);
    CRPC_CHECK(result.ok());
    CRPC_CHECK_EQ(std::size_t{1}, result.value().instances.size());
    CRPC_CHECK_EQ(std::string("n1"), result.value().instances[0].node_id);
}

// --- Lease ------------------------------------------------------------------

CRPC_TEST(lease_grant_renew_expire) {
    manual_clock clock;
    cl::registry reg;
    cl::lease_config config;
    config.default_ttl = 1000ms;
    config.critical_grace = 1000ms;
    config.max_ttl = 10000ms;
    cl::lease_manager leases(reg, make_clock(clock), config);

    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 1),
                                     make_issued_ctx(reg, "api", "n1", 1))
                     .ok());
    const auto granted = leases.grant("api", "n1", 1000ms);
    CRPC_CHECK(granted.ok());
    const crpc::u64 id = granted.value().id;
    const crpc::u64 first_fence = granted.value().fence;
    CRPC_CHECK(leases.is_active(id));

    const auto renewed = leases.renew(id);
    CRPC_CHECK(renewed.ok());
    CRPC_CHECK(renewed.value().fence > first_fence);

    clock.advance(1000ms);
    CRPC_CHECK_FALSE(leases.is_active(id));

    const auto sweep = leases.expire_now();
    CRPC_CHECK_EQ(crpc::u32{1}, sweep.expired);

    cl::query_options options;
    options.service = "api";
    options.node_id = "n1";
    auto critical = reg.query(options);
    CRPC_CHECK(critical.ok());
    CRPC_CHECK_EQ(std::size_t{1}, critical.value().instances.size());
    CRPC_CHECK_EQ(cl::instance_state::critical,
                  critical.value().instances[0].state);

    clock.advance(1000ms);
    const auto second = leases.expire_now();
    CRPC_CHECK_EQ(crpc::u32{1}, second.deregistered);
    CRPC_CHECK_EQ(std::size_t{0}, leases.size());

    auto live = reg.query(options);
    CRPC_CHECK(live.ok());
    CRPC_CHECK_EQ(std::size_t{0}, live.value().instances.size());
    options.include_deregistered = true;
    auto tombstone = reg.query(options);
    CRPC_CHECK(tombstone.ok());
    CRPC_CHECK_EQ(std::size_t{1}, tombstone.value().instances.size());
    CRPC_CHECK_EQ(cl::instance_state::deregistered,
                  tombstone.value().instances[0].state);
}

CRPC_TEST(lease_renew_after_critical_restores_passing) {
    manual_clock clock;
    cl::registry reg;
    cl::lease_config config;
    config.default_ttl = 1000ms;
    config.critical_grace = 5000ms;
    cl::lease_manager leases(reg, make_clock(clock), config);

    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 1),
                                     make_issued_ctx(reg, "api", "n1", 1))
                     .ok());
    const auto granted = leases.grant("api", "n1", 1000ms);
    CRPC_CHECK(granted.ok());

    clock.advance(1000ms);
    const auto sweep = leases.expire_now();
    CRPC_CHECK_EQ(crpc::u32{1}, sweep.expired);

    cl::query_options options;
    options.service = "api";
    options.node_id = "n1";
    auto critical = reg.query(options);
    CRPC_CHECK(critical.ok());
    CRPC_CHECK_EQ(cl::instance_state::critical,
                  critical.value().instances[0].state);

    const auto renewed = leases.renew(granted.value().id);
    CRPC_CHECK(renewed.ok());
    CRPC_CHECK(leases.is_active(granted.value().id));

    auto restored = reg.query(options);
    CRPC_CHECK(restored.ok());
    CRPC_CHECK_EQ(cl::instance_state::passing,
                  restored.value().instances[0].state);
}

CRPC_TEST(lease_fence_is_monotonic) {
    manual_clock clock;
    cl::registry reg;
    cl::lease_manager leases(reg, make_clock(clock));

    crpc::u64 previous = leases.next_fence();
    for (int i = 0; i < 4; ++i) {
        const crpc::u64 next = leases.next_fence();
        CRPC_CHECK(next > previous);
        previous = next;
    }
}

CRPC_TEST(lease_stale_fence_is_rejected) {
    manual_clock clock;
    cl::registry reg;
    cl::lease_config config;
    config.default_ttl = 1000ms;
    config.critical_grace = 1000ms;
    cl::lease_manager leases(reg, make_clock(clock), config);

    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 1),
                                     make_issued_ctx(reg, "api", "n1", 1))
                     .ok());
    const auto granted = leases.grant("api", "n1", 1000ms);
    CRPC_CHECK(granted.ok());
    const crpc::u64 lease_fence = granted.value().fence;

    // A newer authority writer supersedes the lease's barrier. The token is
    // issued by the authority (a caller-side lease_fence+100 would be refused as
    // not_issued), and it moves the key high-water above the lease's token.
    const auto newer = reg.set_state("api", "n1", cl::instance_state::passing,
                                     make_issued_ctx(reg, "api", "n1", 1));
    CRPC_CHECK(newer.ok());
    CRPC_CHECK(newer.value().fence > lease_fence);

    // A renewal after the lease has already expired is refused outright (P4), so
    // the entry stays critical; the sweep therefore cannot be "rejected" by a
    // stale write any more. The state the newer writer committed survives.
    clock.advance(2000ms);
    CRPC_CHECK_FALSE(leases.renew(granted.value().id).ok());
    const auto sweep = leases.expire_now();
    CRPC_CHECK_EQ(crpc::u32{1}, sweep.expired);
    CRPC_CHECK_EQ(crpc::u32{0}, sweep.rejected);
    CRPC_CHECK_EQ(std::size_t{1}, leases.size());

    cl::query_options options;
    options.service = "api";
    options.node_id = "n1";
    auto result = reg.query(options);
    CRPC_CHECK(result.ok());
    CRPC_CHECK_EQ(std::size_t{1}, result.value().instances.size());
    CRPC_CHECK_EQ(cl::instance_state::critical,
                  result.value().instances[0].state);
}

// --- Election ---------------------------------------------------------------

CRPC_TEST(election_campaign_with_quorum) {
    manual_clock clock;
    cl::registry reg;
    cl::lease_manager leases(reg, make_clock(clock));

    stub_quorum quorum;
    quorum.prevote_granted = 2;
    quorum.vote_granted = 2;

    cl::election_config config;
    config.quorum_size = 3;
    config.leader_ttl = 1000ms;
    config.election_timeout = 500ms;
    cl::election election(leases, quorum, make_clock(clock), config);

    const auto token = election.campaign("n1");
    CRPC_CHECK(token.ok());
    CRPC_CHECK_EQ(crpc::u64{1}, token.value().term);
    CRPC_CHECK(token.value().epoch > 0);
    CRPC_CHECK(election.is_leader());
    CRPC_CHECK_EQ(std::string("n1"), election.leader_id());

    clock.advance(1000ms);
    CRPC_CHECK_FALSE(election.is_leader());
    CRPC_CHECK_FALSE(election.renew().ok());

    const auto second = election.campaign("n1");
    CRPC_CHECK(second.ok());
    CRPC_CHECK_EQ(crpc::u64{2}, second.value().term);
    CRPC_CHECK(election.is_leader());

    CRPC_CHECK(election.resign().ok());
    CRPC_CHECK_FALSE(election.is_leader());
    CRPC_CHECK_EQ(std::size_t{0}, leases.size());
}

CRPC_TEST(election_prevote_rejected_with_live_leader) {
    manual_clock clock;
    cl::registry reg;
    cl::lease_manager leases(reg, make_clock(clock));

    stub_quorum quorum;
    quorum.prevote_granted = 2;
    quorum.vote_granted = 2;
    quorum.leader_alive = true;
    quorum.leader = "n1";

    cl::election_config config;
    config.quorum_size = 3;
    config.leader_ttl = 1000ms;
    config.election_timeout = 500ms;
    cl::election election(leases, quorum, make_clock(clock), config);

    const crpc::u64 before = election.term();
    const auto rejected = election.campaign("n2");
    CRPC_CHECK_FALSE(rejected.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, rejected.code());
    CRPC_CHECK_EQ(before, election.term());
    CRPC_CHECK_FALSE(election.is_leader());

    // A locally observed live leader also blocks a campaign.
    election.observe_leader("n1", 7);
    const crpc::u64 observed_term = election.term();
    const auto local = election.campaign("n3");
    CRPC_CHECK_FALSE(local.ok());
    CRPC_CHECK_EQ(observed_term, election.term());
}

CRPC_TEST(election_stale_epoch_write_is_rejected) {
    manual_clock clock;
    cl::registry reg;
    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 1),
                                     make_issued_ctx(reg, "api", "n1", 1))
                     .ok());

    cl::lease_manager leases(reg, make_clock(clock));
    stub_quorum quorum;

    cl::election_config config;
    config.quorum_size = 1;
    config.leader_ttl = 5000ms;
    config.election_timeout = 500ms;
    cl::election election(leases, quorum, make_clock(clock), config);

    const auto first = election.campaign("n1");
    CRPC_CHECK(first.ok());
    const crpc::u64 first_epoch = first.value().epoch;
    // The epoch is a per-candidate fence barrier for the election's own lease
    // key, not an issuance for "api"/"n1". A write to the instance therefore
    // needs a token the registry issued for that key, exactly as any other
    // writer does.
    const auto first_write = reg.set_state(
        "api", "n1", cl::instance_state::passing,
        make_issued_ctx(reg, "api", "n1", 1));
    CRPC_CHECK(first_write.ok());
    CRPC_CHECK(first_write.value().fence > 0);

    CRPC_CHECK(election.resign().ok());
    const auto second = election.campaign("n1");
    CRPC_CHECK(second.ok());
    const crpc::u64 second_epoch = second.value().epoch;
    CRPC_CHECK(second_epoch > first_epoch);
    const auto second_write = reg.set_state(
        "api", "n1", cl::instance_state::passing,
        make_issued_ctx(reg, "api", "n1", 1));
    CRPC_CHECK(second_write.ok());
    CRPC_CHECK(second_write.value().fence > first_write.value().fence);

    // An unissued value that merely resembles an old epoch is refused: knowing a
    // number is not the same as being allowed to use it (L-b).
    const auto stale = reg.set_state("api", "n1", cl::instance_state::critical,
                                     make_ctx("n1", first_epoch, 1));
    CRPC_CHECK_FALSE(stale.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, stale.code());
    const std::string* reason = stale.error().metadata.find("registry.reason");
    CRPC_CHECK(reason != nullptr &&
               (*reason == "not_issued" || *reason == "stale_fence"));
}

// --- Health -----------------------------------------------------------------

CRPC_TEST(health_worst_of_aggregation) {
    manual_clock clock;
    cl::health_monitor monitor(make_clock(clock));

    probe_flag good;
    good.pass = true;
    probe_flag bad;
    bad.pass = false;

    CRPC_CHECK(monitor.add_callback("good", &flag_probe, &good, 0ms) != nullptr);
    CRPC_CHECK(monitor.add_callback("bad", &flag_probe, &bad, 0ms) != nullptr);
    CRPC_CHECK_EQ(cl::health_state::critical, monitor.poll());
}

CRPC_TEST(health_recovery_needs_consecutive_passes) {
    manual_clock clock;
    cl::health_monitor monitor(make_clock(clock));

    probe_flag flag;
    auto* check = monitor.add_callback("cb", &flag_probe, &flag, 0ms);
    CRPC_CHECK(check != nullptr);

    CRPC_CHECK_EQ(cl::health_state::critical, monitor.poll());
    flag.pass = true;
    CRPC_CHECK_EQ(cl::health_state::critical, monitor.poll());
    CRPC_CHECK_EQ(cl::health_state::passing, monitor.poll());

    // Flapping is suppressed: a single pass after a failure is not enough.
    flag.pass = false;
    CRPC_CHECK_EQ(cl::health_state::critical, monitor.poll());
    flag.pass = true;
    CRPC_CHECK_EQ(cl::health_state::critical, monitor.poll());
    flag.pass = false;
    CRPC_CHECK_EQ(cl::health_state::critical, monitor.poll());
    flag.pass = true;
    CRPC_CHECK_EQ(cl::health_state::critical, monitor.poll());
    CRPC_CHECK_EQ(cl::health_state::passing, monitor.poll());
}

CRPC_TEST(health_ttl_warning_and_expiry) {
    manual_clock clock;
    cl::health_monitor monitor(make_clock(clock));

    auto* check = monitor.add_ttl("ttl", 1000ms);
    CRPC_CHECK(check != nullptr);
    CRPC_CHECK_EQ(cl::health_state::passing, monitor.poll());

    clock.advance(600ms);
    CRPC_CHECK_EQ(cl::health_state::warning, monitor.poll());
    clock.advance(500ms);
    CRPC_CHECK_EQ(cl::health_state::critical, monitor.poll());

    check->heartbeat(true, clock.now);
    CRPC_CHECK_EQ(cl::health_state::critical, monitor.poll());
    check->heartbeat(true, clock.now);
    CRPC_CHECK_EQ(cl::health_state::passing, monitor.poll());
}

CRPC_TEST(health_tcp_probe_interval) {
    manual_clock clock;
    cl::health_monitor monitor(make_clock(clock));

    tcp_probe_state state;
    auto* check = monitor.add_tcp("tcp", cl::endpoint{"127.0.0.1", 80}, 100ms,
                                  &tcp_probe, &state);
    CRPC_CHECK(check != nullptr);
    CRPC_CHECK_EQ(cl::health_state::critical, monitor.poll());
    CRPC_CHECK_EQ(1, state.calls);

    clock.advance(50ms);
    CRPC_CHECK_EQ(cl::health_state::critical, monitor.poll());
    CRPC_CHECK_EQ(1, state.calls);

    state.pass = true;
    clock.advance(60ms);
    CRPC_CHECK_EQ(cl::health_state::critical, monitor.poll());
    CRPC_CHECK_EQ(2, state.calls);

    clock.advance(100ms);
    CRPC_CHECK_EQ(cl::health_state::passing, monitor.poll());
}

// --- Watch ------------------------------------------------------------------

CRPC_TEST(watch_initial_snapshot_and_ordered_deltas) {
    cl::registry reg;
    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 1),
                                     make_issued_ctx(reg, "api", "n1", 1))
                     .ok());
    CRPC_CHECK(reg.register_instance(make_instance("api", "n2", 8002, 1),
                                     make_issued_ctx(reg, "api", "n2", 1))
                     .ok());

    cl::watch_hub hub(reg);
    watch_capture capture;
    const auto id = hub.watch("api", 0, &capture_callback, &capture);
    CRPC_CHECK(id.ok());
    CRPC_CHECK_EQ(std::size_t{2}, capture.snapshot_instances);
    CRPC_CHECK_EQ(std::size_t{1}, capture.kinds.size());
    CRPC_CHECK_EQ(cl::watch_event_kind::snapshot, capture.kinds[0]);
    CRPC_CHECK_EQ(reg.index(), capture.indexes[0]);

    const auto added = reg.register_instance(
        make_instance("api", "n3", 8003, 1), make_issued_ctx(reg, "api", "n3", 1));
    CRPC_CHECK(added.ok());
    CRPC_CHECK_EQ(std::size_t{1}, hub.dispatch());

    CRPC_CHECK(reg.set_state("api", "n1", cl::instance_state::critical,
                             make_issued_ctx(reg, "api", "n1", 1))
                     .ok());
    CRPC_CHECK_EQ(std::size_t{1}, hub.dispatch());

    CRPC_CHECK(reg.deregister("api", "n2",
                              make_issued_ctx(reg, "api", "n2", 1))
                     .ok());
    CRPC_CHECK_EQ(std::size_t{1}, hub.dispatch());

    CRPC_CHECK_EQ(std::size_t{3}, capture.delta_instances);
    for (std::size_t i = 1; i < capture.indexes.size(); ++i) {
        CRPC_CHECK(capture.indexes[i] > capture.indexes[i - 1]);
    }
    CRPC_CHECK(hub.cancel(id.value()).ok());
    CRPC_CHECK_EQ(std::size_t{0}, hub.subscriber_count());
}

CRPC_TEST(watch_drops_duplicate_and_old_index) {
    std::vector<cl::service_instance> snapshot;
    snapshot.push_back(make_instance("api", "n1", 8001, 1));

    watch_capture capture;
    cl::watch_stream stream("api", 10, snapshot, &capture_callback, &capture);
    CRPC_CHECK_EQ(std::size_t{1}, stream.deliver_initial());

    const auto instance = make_instance("api", "n2", 8002, 1);
    stream.on_event(10, "api", instance);
    stream.on_event(9, "api", instance);
    CRPC_CHECK_EQ(std::size_t{0}, stream.queued());
    CRPC_CHECK_EQ(std::size_t{0}, stream.dispatch());
    CRPC_CHECK_EQ(std::size_t{1}, capture.kinds.size());

    stream.on_event(11, "api", instance);
    CRPC_CHECK_EQ(std::size_t{1}, stream.queued());
    CRPC_CHECK_EQ(std::size_t{1}, stream.dispatch());
    CRPC_CHECK_EQ(cl::watch_event_kind::delta, capture.kinds.back());
}

CRPC_TEST(watch_gap_triggers_resync) {
    std::vector<cl::service_instance> snapshot;
    snapshot.push_back(make_instance("api", "n1", 8001, 1));

    watch_capture capture;
    cl::watch_stream stream("api", 10, snapshot, &capture_callback, &capture);
    CRPC_CHECK_EQ(std::size_t{1}, stream.deliver_initial());

    const auto instance = make_instance("api", "n2", 8002, 1);
    stream.on_event(12, "api", instance);
    CRPC_CHECK(stream.needs_resync());
    CRPC_CHECK_EQ(std::size_t{12}, stream.last_index());

    std::vector<cl::service_instance> fresh;
    fresh.push_back(make_instance("api", "n2", 8002, 1));
    CRPC_CHECK_EQ(std::size_t{1}, stream.apply_resync(12, fresh));
    CRPC_CHECK_FALSE(stream.needs_resync());
    CRPC_CHECK_EQ(std::size_t{1}, capture.resyncs);

    stream.on_event(13, "api", instance);
    CRPC_CHECK_EQ(std::size_t{1}, stream.dispatch());
}

CRPC_TEST(watch_hub_resync) {
    cl::registry reg;
    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 1),
                                     make_issued_ctx(reg, "api", "n1", 1))
                     .ok());

    cl::watch_hub hub(reg);
    watch_capture capture;
    const auto id = hub.watch("api", 0, &capture_callback, &capture);
    CRPC_CHECK(id.ok());
    CRPC_CHECK_EQ(std::size_t{1}, capture.snapshot_instances);

    CRPC_CHECK(reg.register_instance(make_instance("api", "n2", 8002, 1),
                                     make_issued_ctx(reg, "api", "n2", 1))
                     .ok());
    CRPC_CHECK(hub.resync(id.value()).ok());
    CRPC_CHECK_EQ(std::size_t{1}, capture.resyncs);
    CRPC_CHECK_EQ(std::size_t{3}, capture.snapshot_instances);
    CRPC_CHECK_EQ(std::size_t{0}, hub.dispatch());
}

CRPC_TEST(watch_slow_subscriber_is_dropped) {
    cl::registry reg;
    CRPC_CHECK(reg.register_instance(make_instance("api", "n1", 8001, 1),
                                     make_issued_ctx(reg, "api", "n1", 1))
                     .ok());

    cl::watch_hub hub(reg);
    watch_capture capture;
    const auto id = hub.watch("api", 0, &capture_callback, &capture, 1);
    CRPC_CHECK(id.ok());

    CRPC_CHECK(reg.register_instance(make_instance("api", "n2", 8002, 1),
                                     make_issued_ctx(reg, "api", "n2", 1))
                     .ok());
    CRPC_CHECK(reg.register_instance(make_instance("api", "n3", 8003, 1),
                                     make_issued_ctx(reg, "api", "n3", 1))
                     .ok());
    CRPC_CHECK_EQ(std::size_t{1}, hub.subscriber_count());

    const std::size_t delivered = hub.dispatch();
    CRPC_CHECK_EQ(std::size_t{1}, delivered);
    CRPC_CHECK_EQ(std::size_t{1}, capture.drops);
    CRPC_CHECK_EQ(std::size_t{0}, hub.subscriber_count());
}

// --- Concurrency ------------------------------------------------------------

CRPC_TEST(registry_concurrent_snapshots) {
    cl::registry reg;
    constexpr int writer_count = 4;
    constexpr int reader_count = 4;
    constexpr int iterations = 400;

    for (int i = 0; i < writer_count; ++i) {
        const std::string node = "n" + std::to_string(i);
        const auto instance = make_instance(
            "api", node, static_cast<crpc::u16>(9000 + i), 1);
        CRPC_CHECK(reg.register_instance(instance,
                                         make_issued_ctx(reg, "api", node, 1))
                         .ok());
    }

    std::atomic<bool> stop{false};
    std::atomic<bool> consistent{true};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(writer_count + reader_count));

    for (int i = 0; i < writer_count; ++i) {
        threads.emplace_back([&reg, &consistent, i]() {
            const std::string node = "n" + std::to_string(i);
            for (int k = 0; k < iterations; ++k) {
                const auto instance = make_instance(
                    "api", node, static_cast<crpc::u16>(9000 + i),
                    static_cast<crpc::u64>(2 + k));
                // Each iteration needs its own issuance: a token is spent by the
                // write it authorises, so a loop that reuses one is not a legal
                // caller shape under the default config.
                const auto token = reg.next_fence("api", node);
                if (!token.ok()) {
                    consistent.store(false, std::memory_order_relaxed);
                    continue;
                }
                const auto written = reg.register_instance(
                    instance, make_ctx(node, token.value(), 2 + k));
                if (!written.ok()) {
                    consistent.store(false, std::memory_order_relaxed);
                }
            }
        });
    }

    for (int i = 0; i < reader_count; ++i) {
        threads.emplace_back([&reg, &stop, &consistent]() {
            while (!stop.load(std::memory_order_acquire)) {
                const auto snapshot = reg.snapshot();
                if (snapshot == nullptr) {
                    consistent.store(false, std::memory_order_relaxed);
                    continue;
                }
                for (const auto& instance : snapshot->instances) {
                    if (instance.revision > snapshot->index ||
                        instance.service.empty() || instance.node_id.empty() ||
                        instance.incarnation == 0 ||
                        instance.address.host != "127.0.0.1" ||
                        instance.address.port < 9000 ||
                        instance.address.port > 9003 ||
                        instance.state > cl::instance_state::deregistered) {
                        consistent.store(false, std::memory_order_relaxed);
                    }
                }
                const auto queried = reg.query(cl::query_options{});
                if (!queried.ok()) {
                    consistent.store(false, std::memory_order_relaxed);
                    continue;
                }
                for (const auto& instance : queried.value().instances) {
                    if (instance.revision > queried.value().index) {
                        consistent.store(false, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (int i = 0; i < writer_count; ++i) {
        threads[static_cast<std::size_t>(i)].join();
    }
    stop.store(true, std::memory_order_release);
    for (int i = writer_count; i < writer_count + reader_count; ++i) {
        threads[static_cast<std::size_t>(i)].join();
    }

    CRPC_CHECK(consistent.load());
    const auto final_result = reg.query(cl::query_options{});
    CRPC_CHECK(final_result.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(writer_count),
                  final_result.value().instances.size());
}

CRPC_TEST_MAIN()
