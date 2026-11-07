// CRPC -- G19 Phase 0: cluster primitive hardening tests.
//
// Each case pins one defect from the G19 semantics audit (P1-P13, W1-W4):
// grace-zero purge, tombstone high-water after purge, authority fence
// exhaustion, expired-lease renewal, field-writer separation, unfenced writes,
// grant-before-register expiry, endpoint validation, purge index monotonicity,
// TTL boundary, idempotent registration, conflicting incarnation payloads, the
// lease grant fence barrier, the watch cancel barrier, park-at-current,
// incremental replay and non-rolling resync.
//
// The behaviour-changing switches (separate_state_writers, strict_fencing,
// strict_incarnation) are ON by default (Phase 0 strict semantics); a case that
// exercises the legacy mode sets the switch to false explicitly. F3 pins the
// returned fence of a successful write (>= the key's effective high-water) and
// the health-bridge re-anchor; F4 pins the post-purge fence epoch.
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include <crpc/cluster/health.hpp>
#include <crpc/cluster/health_bridge.hpp>
#include <crpc/cluster/lease.hpp>
#include <crpc/cluster/registry.hpp>
#include <crpc/cluster/watch.hpp>

namespace cl = crpc::cluster;
using crpc::rpc_code;
using crpc::u64;

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
                                   u64 incarnation,
                                   const std::string& host = "127.0.0.1") {
    cl::service_instance instance;
    instance.service = service;
    instance.node_id = node;
    instance.address.host = host;
    instance.address.port = port;
    instance.weight = 1;
    instance.incarnation = incarnation;
    instance.state = cl::instance_state::passing;
    return instance;
}

cl::write_context make_ctx(const std::string& node, u64 fence = 0,
                           u64 incarnation = 0) {
    cl::write_context context;
    context.node_id = node;
    context.fence = fence;
    context.incarnation = incarnation;
    return context;
}

std::string reason_of(const crpc::rpc_error& error) {
    const std::string* reason = error.metadata.find("registry.reason");
    return reason != nullptr ? *reason : std::string{};
}

// Safe for both outcomes: reading error() from an ok result is a programming
// error in this library (it terminates), and a red run of these tests must
// report every failure instead of dying on the first one.
template <typename T>
std::string rejection_reason(const crpc::result<T>& outcome) {
    return outcome.ok() ? std::string("<accepted>") : reason_of(outcome.error());
}

cl::instance_state state_of(cl::registry& reg, const std::string& service,
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

struct catalog_capture {
    std::vector<u64> indexes;
    std::vector<cl::catalog_event_kind> kinds;
    std::vector<std::string> node_ids;
};

void capture_catalog_event(void* context,
                           const cl::catalog_event& event) noexcept {
    auto* capture = static_cast<catalog_capture*>(context);
    capture->indexes.push_back(event.index);
    capture->kinds.push_back(event.kind);
    capture->node_ids.push_back(event.instance.node_id);
}

struct watch_capture {
    int callbacks = 0;
    int snapshots = 0;
    int deltas = 0;
    int resyncs = 0;
    u64 last_index = 0;
    cl::watch_hub* hub = nullptr;
    u64 id = 0;
    bool cancel_on_first_delta = false;
    bool cancelled = false;
};

void watch_callback(void* context, const cl::watch_event& event) noexcept {
    auto* capture = static_cast<watch_capture*>(context);
    ++capture->callbacks;
    capture->last_index = event.index;
    switch (event.kind) {
    case cl::watch_event_kind::snapshot:
        ++capture->snapshots;
        break;
    case cl::watch_event_kind::delta:
        ++capture->deltas;
        if (capture->cancel_on_first_delta && !capture->cancelled &&
            capture->hub != nullptr && capture->id != 0) {
            capture->cancelled = true;
            (void)capture->hub->cancel(capture->id);
        }
        break;
    case cl::watch_event_kind::resync:
        ++capture->resyncs;
        break;
    case cl::watch_event_kind::dropped:
        break;
    }
}

} // namespace

// --- P1: grace == 0 means "eligible immediately" -----------------------------

CRPC_TEST(phase0_p1_grace_zero_purges_immediately) {
    cl::registry_config config;
    config.tombstone_grace = 0;
    cl::registry reg(config);

    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                       make_ctx("n1"))
                     .ok());
    CRPC_REQUIRE(reg.deregister("svc", "n1", make_ctx("n1", 0, 1)).ok());

    CRPC_CHECK_EQ(std::size_t{1}, reg.purge_expired_tombstones());

    cl::query_options all;
    all.include_deregistered = true;
    const auto rows = reg.query(all);
    CRPC_REQUIRE(rows.ok());
    CRPC_CHECK_EQ(std::size_t{0}, rows.value().instances.size());
}

// --- P2: purge keeps the tombstone incarnation high-water --------------------

CRPC_TEST(phase0_p2_purge_keeps_tombstone_high_water) {
    cl::registry_config config;
    config.tombstone_grace = 0;
    cl::registry reg(config);

    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "a", 8001, 1),
                                       make_ctx("a"))
                     .ok());
    CRPC_REQUIRE(reg.deregister("svc", "a", make_ctx("a", 0, 1)).ok());
    CRPC_REQUIRE(reg.purge_expired_tombstones() == 1);

    // The visible row is gone, but the tombstone watermark (incarnation 2)
    // must still reject every incarnation that is not strictly greater.
    const auto old_incarnation =
        reg.register_instance(make_instance("svc", "a", 8001, 1), make_ctx("a"));
    CRPC_CHECK_FALSE(old_incarnation.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, old_incarnation.code());
    CRPC_CHECK_EQ(std::string("tombstone"), rejection_reason(old_incarnation));

    const auto equal_incarnation =
        reg.register_instance(make_instance("svc", "a", 8001, 2), make_ctx("a"));
    CRPC_CHECK_FALSE(equal_incarnation.ok());
    CRPC_CHECK_EQ(std::string("tombstone"),
                  rejection_reason(equal_incarnation));

    // The visible row is purged but the tombstone watermark survives, so the
    // key is not fresh: the resurrection carries an epoch above the surviving
    // high-water (F4).
    CRPC_CHECK(reg.register_instance(make_instance("svc", "a", 8001, 3),
                                     make_ctx("a", 3))
                   .ok());
}

// --- P3: a caller fence cannot exhaust the authority fence -------------------

CRPC_TEST(phase0_p3_caller_fence_cannot_exhaust_authority) {
    cl::registry reg;
    const u64 huge = std::numeric_limits<u64>::max() - 1;

    const auto hostile =
        reg.register_instance(make_instance("svc", "hostile", 1, 1),
                              make_ctx("hostile", huge));
    CRPC_CHECK_FALSE(hostile.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, hostile.code());
    CRPC_CHECK_EQ(std::string("fence_out_of_range"),
                  rejection_reason(hostile));

    // The rejected leap must not poison later writes.
    const auto b = reg.register_instance(make_instance("svc", "b", 2, 1),
                                         make_ctx("b"));
    CRPC_CHECK(b.ok());
    const auto c = reg.register_instance(make_instance("svc", "c", 3, 1),
                                         make_ctx("c"));
    CRPC_CHECK(c.ok());
    CRPC_CHECK(b.ok() && c.ok() && c.value().fence < huge);

    // A bounded caller epoch is still honoured and the returned fence stays
    // globally strictly increasing.
    const auto epoch = reg.register_instance(make_instance("svc", "d", 4, 1),
                                             make_ctx("d", 7));
    CRPC_REQUIRE(epoch.ok());
    CRPC_CHECK_EQ(u64{7}, epoch.value().fence);
    const auto after = reg.register_instance(make_instance("svc", "e", 5, 1),
                                             make_ctx("e"));
    CRPC_REQUIRE(after.ok());
    CRPC_CHECK(after.value().fence > epoch.value().fence);
}

// --- P4: an expired active lease cannot be renewed ---------------------------

CRPC_TEST(phase0_p4_expired_renew_is_rejected) {
    manual_clock clock;
    cl::registry reg;
    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                       make_ctx("n1"))
                     .ok());
    cl::lease_manager leases(reg, make_clock(clock));

    const auto granted = leases.grant("svc", "n1", 1000ms);
    CRPC_REQUIRE(granted.ok());
    clock.advance(5000ms);
    CRPC_CHECK_FALSE(leases.is_active(granted.value().id));

    const auto renewed = leases.renew(granted.value().id, 1000ms);
    CRPC_CHECK_FALSE(renewed.ok());
    CRPC_CHECK_EQ(rpc_code::deadline_exceeded, renewed.code());
    CRPC_CHECK_FALSE(leases.is_active(granted.value().id));
}

// --- P5: registration must not clobber the health/lease state ----------------

CRPC_TEST(phase0_p5_register_keeps_health_state) {
    cl::registry_config config;
    config.separate_state_writers = true;
    config.strict_incarnation = true;
    cl::registry reg(config);

    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                       make_ctx("n1"))
                     .ok());
    CRPC_REQUIRE(reg.set_state("svc", "n1", cl::instance_state::critical,
                               make_ctx("n1", 0, 1))
                     .ok());
    CRPC_CHECK_EQ(cl::instance_state::critical, state_of(reg, "svc", "n1"));

    // A new incarnation re-registers with a different endpoint. The health
    // verdict belongs to the health/lease writer and must survive.
    CRPC_CHECK(reg.register_instance(make_instance("svc", "n1", 8002, 2),
                                     make_ctx("n1"))
                   .ok());
    CRPC_CHECK_EQ(cl::instance_state::critical, state_of(reg, "svc", "n1"));

    // An identical replay is a no-op and also keeps the state.
    const u64 index_before = reg.index();
    CRPC_CHECK(reg.register_instance(make_instance("svc", "n1", 8002, 2),
                                     make_ctx("n1"))
                   .ok());
    CRPC_CHECK_EQ(index_before, reg.index());
    CRPC_CHECK_EQ(cl::instance_state::critical, state_of(reg, "svc", "n1"));
}

// --- P6: an unfenced update cannot pierce an external fencing epoch ----------

CRPC_TEST(phase0_p6_unfenced_write_after_fenced_epoch_rejected) {
    cl::registry_config config;
    config.strict_fencing = true;
    cl::registry reg(config);

    // The first create is never fenced.
    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                       make_ctx("n1"))
                     .ok());
    // An external epoch (a lease/election token) fences the key.
    CRPC_REQUIRE(reg.set_state("svc", "n1", cl::instance_state::critical,
                               make_ctx("n1", 100, 1))
                     .ok());

    const auto unfenced = reg.register_instance(
        make_instance("svc", "n1", 8002, 2), make_ctx("n1", 0));
    CRPC_CHECK_FALSE(unfenced.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, unfenced.code());
    CRPC_CHECK_EQ(std::string("fence_required"), rejection_reason(unfenced));

    CRPC_CHECK(reg.register_instance(make_instance("svc", "n1", 8002, 2),
                                     make_ctx("n1", 100))
                   .ok());

    // The default mode keeps the historical unfenced behaviour for in-process
    // users that never configured fencing.
    cl::registry_config compat_config;
    compat_config.strict_fencing = false;
    cl::registry compat(compat_config);
    CRPC_REQUIRE(compat.register_instance(make_instance("svc", "n1", 8001, 1),
                                          make_ctx("n1"))
                     .ok());
    CRPC_REQUIRE(compat.set_state("svc", "n1", cl::instance_state::critical,
                                  make_ctx("n1", 100, 1))
                     .ok());
    CRPC_CHECK(compat.register_instance(make_instance("svc", "n1", 8002, 2),
                                        make_ctx("n1"))
                   .ok());
}

// --- P7: a lease granted before registration still expires its instance ------

CRPC_TEST(phase0_p7_grant_before_register_expiry_applies) {
    manual_clock clock;
    cl::registry reg;
    cl::lease_config config;
    config.critical_grace = 1000ms;
    cl::lease_manager leases(reg, make_clock(clock), config);

    const auto granted = leases.grant("svc", "n1", 1000ms);
    CRPC_REQUIRE(granted.ok());
    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                       make_ctx("n1"))
                     .ok());

    clock.advance(1000ms);
    const auto sweep = leases.expire_now();
    CRPC_CHECK_EQ(crpc::u32{1}, sweep.expired);
    CRPC_CHECK_EQ(crpc::u32{0}, sweep.rejected);
    CRPC_CHECK_EQ(cl::instance_state::critical, state_of(reg, "svc", "n1"));

    clock.advance(1000ms);
    const auto second = leases.expire_now();
    CRPC_CHECK_EQ(crpc::u32{1}, second.deregistered);
    cl::query_options all;
    all.service = "svc";
    all.include_deregistered = true;
    const auto rows = reg.query(all);
    CRPC_REQUIRE(rows.ok());
    CRPC_CHECK_EQ(std::size_t{1}, rows.value().instances.size());
    CRPC_CHECK_EQ(cl::instance_state::deregistered,
                  rows.value().instances[0].state);
}

// --- P8: endpoint validation with an explicit inproc sentinel ----------------

CRPC_TEST(phase0_p8_endpoint_validation) {
    cl::registry reg;

    const auto empty_host_port0 =
        reg.register_instance(make_instance("svc", "n1", 0, 1, ""),
                              make_ctx("n1"));
    CRPC_CHECK_FALSE(empty_host_port0.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, empty_host_port0.code());

    const auto empty_host_network =
        reg.register_instance(make_instance("svc", "n2", 8001, 1, ""),
                              make_ctx("n2"));
    CRPC_CHECK_FALSE(empty_host_network.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, empty_host_network.code());

    // A non-empty host with port 0 is the inproc sentinel used by the
    // in-process distributed tests; it must not be rejected blindly.
    CRPC_CHECK(reg.register_instance(
                   make_instance("svc", "n3", 0, 1, "dist-inproc-node"),
                   make_ctx("n3"))
                   .ok());
    CRPC_CHECK(reg.register_instance(make_instance("svc", "n4", 8001, 1),
                                     make_ctx("n4"))
                   .ok());
}

// --- P9: purge advances the index and emits remove events --------------------

CRPC_TEST(phase0_p9_purge_bumps_index_and_emits_remove) {
    cl::registry_config config;
    config.tombstone_grace = 0;
    cl::registry reg(config);
    catalog_capture events;
    CRPC_REQUIRE(reg.add_sink(&capture_catalog_event, &events).ok());

    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "a", 8001, 1),
                                       make_ctx("a"))
                     .ok());
    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "b", 8002, 1),
                                       make_ctx("b"))
                     .ok());
    CRPC_REQUIRE(reg.deregister("svc", "a", make_ctx("a", 0, 1)).ok());

    const u64 before = reg.index();
    events.indexes.clear();
    events.kinds.clear();
    events.node_ids.clear();

    const std::size_t purged = reg.purge_expired_tombstones();
    CRPC_CHECK_EQ(std::size_t{1}, purged);
    CRPC_CHECK_EQ(before + 1, reg.index());
    CRPC_REQUIRE(events.indexes.size() == 1);
    CRPC_CHECK_EQ(before + 1, events.indexes[0]);
    CRPC_CHECK_EQ(cl::catalog_event_kind::remove, events.kinds[0]);
    CRPC_CHECK_EQ(std::string("a"), events.node_ids[0]);

    cl::query_options all;
    all.service = "svc";
    const auto rows = reg.query(all);
    CRPC_REQUIRE(rows.ok());
    CRPC_CHECK_EQ(std::size_t{1}, rows.value().instances.size());
    CRPC_CHECK_EQ(std::string("b"), rows.value().instances[0].node_id);
}

// --- P10: the TTL boundary is inclusive --------------------------------------

CRPC_TEST(phase0_p10_ttl_boundary_is_expired) {
    manual_clock clock;
    cl::health_monitor monitor(make_clock(clock));
    cl::ttl_health_check* check = monitor.add_ttl("ttl", 1000ms, 2);
    CRPC_REQUIRE(check != nullptr);

    clock.advance(1000ms);
    CRPC_CHECK_EQ(cl::health_state::critical, monitor.poll(clock.now));
}

// --- P11: an identical registration retry is idempotent ----------------------

CRPC_TEST(phase0_p11_identical_register_is_idempotent) {
    cl::registry_config config;
    config.strict_incarnation = true;
    cl::registry reg(config);
    const auto first = reg.register_instance(
        make_instance("svc", "n1", 7001, 3), make_ctx("n1"));
    CRPC_REQUIRE(first.ok());
    CRPC_CHECK_EQ(u64{1}, reg.index());

    catalog_capture events;
    CRPC_REQUIRE(reg.add_sink(&capture_catalog_event, &events).ok());

    const auto retry = reg.register_instance(
        make_instance("svc", "n1", 7001, 3), make_ctx("n1"));
    CRPC_CHECK(retry.ok());
    CRPC_CHECK_EQ(u64{1}, reg.index());
    CRPC_CHECK_EQ(first.value().index, retry.value().index);
    CRPC_CHECK_EQ(first.value().fence, retry.value().fence);
    CRPC_CHECK_EQ(std::size_t{0}, events.indexes.size());
}

// --- P12: a conflicting payload on one incarnation is rejected ---------------

CRPC_TEST(phase0_p12_conflicting_payload_rejected) {
    cl::registry_config config;
    config.strict_incarnation = true;
    cl::registry reg(config);

    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n1", 8001, 3),
                                       make_ctx("n1"))
                     .ok());
    const u64 before = reg.index();

    const auto conflict = reg.register_instance(
        make_instance("svc", "n1", 8002, 3), make_ctx("n1"));
    CRPC_CHECK_FALSE(conflict.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, conflict.code());
    CRPC_CHECK_EQ(std::string("conflict"), rejection_reason(conflict));
    CRPC_CHECK_EQ(before, reg.index());

    CRPC_CHECK(reg.register_instance(make_instance("svc", "n1", 8001, 3),
                                     make_ctx("n1"))
                   .ok());
    CRPC_CHECK_EQ(before, reg.index());

    // The default mode keeps the historical overwrite so existing in-process
    // users that bump only the endpoint are not broken.
    cl::registry_config compat_config;
    compat_config.strict_incarnation = false;
    cl::registry compat(compat_config);
    CRPC_REQUIRE(compat.register_instance(make_instance("svc", "n1", 8001, 3),
                                          make_ctx("n1"))
                     .ok());
    CRPC_CHECK(compat.register_instance(make_instance("svc", "n1", 8002, 3),
                                        make_ctx("n1"))
                   .ok());
    cl::query_options options;
    options.service = "svc";
    const auto rows = compat.query(options);
    CRPC_REQUIRE(rows.ok());
    CRPC_REQUIRE(rows.value().instances.size() == 1);
    CRPC_CHECK_EQ(crpc::u16{8002}, rows.value().instances[0].address.port);
}

// --- P13: a replacement lease fences the old holder --------------------------

CRPC_TEST(phase0_p13_grant_fences_old_holder) {
    manual_clock clock;
    cl::registry reg;
    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                       make_ctx("n1"))
                     .ok());
    cl::lease_manager leases(reg, make_clock(clock));

    const auto old_lease = leases.grant("svc", "n1", 1000ms);
    const auto new_lease = leases.grant("svc", "n1", 1000ms);
    CRPC_REQUIRE(old_lease.ok());
    CRPC_REQUIRE(new_lease.ok());
    CRPC_CHECK(new_lease.value().fence > old_lease.value().fence);

    const auto old_write = reg.register_instance(
        make_instance("svc", "n1", 9001, 2),
        make_ctx("n1", old_lease.value().fence, 2));
    CRPC_CHECK_FALSE(old_write.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, old_write.code());
    CRPC_CHECK_EQ(std::string("stale_fence"), rejection_reason(old_write));

    CRPC_CHECK(reg.register_instance(
                   make_instance("svc", "n1", 9001, 2),
                   make_ctx("n1", new_lease.value().fence, 2))
                   .ok());
}

// --- W1: cancel is a delivery barrier ----------------------------------------

CRPC_TEST(phase0_w1_cancel_is_a_delivery_barrier) {
    cl::registry reg;
    cl::watch_hub hub(reg);
    watch_capture capture;
    capture.hub = &hub;
    capture.cancel_on_first_delta = true;

    const auto id = hub.watch("svc", 0, &watch_callback, &capture);
    CRPC_REQUIRE(id.ok());
    capture.id = id.value();

    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                       make_ctx("n1"))
                     .ok());
    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n2", 8002, 1),
                                       make_ctx("n2"))
                     .ok());

    (void)hub.dispatch();
    CRPC_CHECK_EQ(1, capture.deltas);
    CRPC_CHECK_EQ(std::size_t{0}, hub.subscriber_count());
    CRPC_CHECK_EQ(std::size_t{0}, hub.dispatch());
    CRPC_CHECK_EQ(1, capture.deltas);
}

// --- W2: a watch at the current index parks ----------------------------------

CRPC_TEST(phase0_w2_watch_at_current_parks) {
    cl::registry reg;
    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                       make_ctx("n1"))
                     .ok());
    cl::watch_hub hub(reg);
    watch_capture capture;

    const auto id = hub.watch("svc", reg.index(), &watch_callback, &capture);
    CRPC_REQUIRE(id.ok());
    CRPC_CHECK_EQ(0, capture.callbacks);
    CRPC_CHECK_EQ(std::size_t{0}, hub.dispatch());
    CRPC_CHECK_EQ(0, capture.callbacks);

    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n2", 8002, 1),
                                       make_ctx("n2"))
                     .ok());
    CRPC_CHECK_EQ(std::size_t{1}, hub.dispatch());
    CRPC_CHECK_EQ(1, capture.deltas);
    CRPC_CHECK_EQ(0, capture.snapshots);
    CRPC_CHECK_EQ(reg.index(), capture.last_index);
}

// --- W3: a reconnect one index behind replays the missed delta ---------------

CRPC_TEST(phase0_w3_incremental_replay) {
    cl::registry reg;
    cl::watch_hub hub(reg, 1024, 64);
    watch_capture first;
    const auto id = hub.watch("svc", 0, &watch_callback, &first);
    CRPC_REQUIRE(id.ok());

    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                       make_ctx("n1"))
                     .ok());
    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n2", 8002, 1),
                                       make_ctx("n2"))
                     .ok());
    CRPC_REQUIRE(hub.dispatch() == 2);
    CRPC_REQUIRE(hub.cancel(id.value()).ok());

    watch_capture replay;
    const auto reconnected = hub.watch("svc", 1, &watch_callback, &replay);
    CRPC_REQUIRE(reconnected.ok());
    CRPC_CHECK_EQ(0, replay.snapshots);
    CRPC_CHECK_EQ(std::size_t{1}, hub.dispatch());
    CRPC_CHECK_EQ(1, replay.deltas);
    CRPC_CHECK_EQ(u64{2}, replay.last_index);

    // A reconnect the ring can no longer cover falls back to a resync
    // snapshot.
    watch_capture beyond;
    CRPC_REQUIRE(hub.cancel(reconnected.value()).ok());
    const auto cold = hub.watch("svc", 0, &watch_callback, &beyond);
    CRPC_REQUIRE(cold.ok());
    CRPC_CHECK_EQ(1, beyond.snapshots);
}

// --- W4: a stale resync snapshot cannot roll the anchor back ----------------

CRPC_TEST(phase0_w4_apply_resync_does_not_roll_back) {
    cl::watch_stream stream("svc", 5, {}, nullptr, nullptr);
    const cl::service_instance newer = make_instance("svc", "n1", 8001, 1);
    stream.on_event(6, "svc", newer);
    CRPC_CHECK_EQ(u64{6}, stream.last_index());
    CRPC_CHECK_EQ(std::size_t{1}, stream.queued());

    const std::size_t delivered = stream.apply_resync(4, {});
    CRPC_CHECK_EQ(std::size_t{0}, delivered);
    CRPC_CHECK_EQ(u64{6}, stream.last_index());
    CRPC_CHECK_EQ(std::size_t{1}, stream.queued());
}

// --- F3: a successful write reports the key's effective high-water ----------

CRPC_TEST(phase0_f3_write_result_covers_key_high_water) {
    cl::registry reg;

    // A lease barrier can raise the key high-water before the first
    // registration; the create itself stays unfenced (grant-before-register).
    CRPC_REQUIRE(reg.fence_barrier("svc", "n1", 5, make_ctx("n1")).ok());

    const auto created = reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                               make_ctx("n1"));
    CRPC_REQUIRE(created.ok());
    CRPC_CHECK(created.value().fence >= 5);

    // write_result::fence stays globally strictly increasing even though the
    // barrier raised a key above the global allocation.
    const auto other = reg.register_instance(make_instance("svc", "n2", 8002, 1),
                                             make_ctx("n2"));
    CRPC_REQUIRE(other.ok());
    CRPC_CHECK(other.value().fence > created.value().fence);

    // The returned token is directly usable: a follower write with it must not
    // be rejected as stale.
    const auto next = reg.set_state("svc", "n1", cl::instance_state::critical,
                                    make_ctx("n1", created.value().fence, 1));
    CRPC_CHECK(next.ok());
    CRPC_CHECK(next.value().fence >= created.value().fence);
}

// --- F3: the health bridge re-anchors above the key high-water ---------------

CRPC_TEST(phase0_f3_health_bridge_reanchors_above_key_high_water) {
    manual_clock clock;
    cl::registry reg;
    CRPC_REQUIRE(reg.register_instance(make_instance("api", "n1", 8001, 1),
                                       make_ctx("n1"))
                     .ok());

    cl::health_bridge bridge(reg, 0ms, 1, 1, 16, make_clock(clock));
    CRPC_CHECK(bridge.report("api", "n1", false));
    CRPC_REQUIRE(bridge.flush() == 1);
    CRPC_CHECK_EQ(cl::instance_state::critical, state_of(reg, "api", "n1"));

    // Lease barriers raise the key high-water without moving the global fence.
    cl::lease_manager leases(reg, make_clock(clock));
    for (int i = 0; i < 3; ++i) {
        CRPC_REQUIRE(leases.grant("api", "n1", 1000ms).ok());
    }

    // The bridge's next token is now below the key high-water. Re-anchoring
    // from the global fence would retry the same stale token forever; the
    // rejection reports the key high-water so the next flush succeeds.
    CRPC_CHECK(bridge.report("api", "n1", true));
    CRPC_CHECK_EQ(crpc::u64{0}, bridge.flush());
    CRPC_CHECK_EQ(crpc::u64{1}, bridge.flush());
    CRPC_CHECK_EQ(cl::instance_state::passing, state_of(reg, "api", "n1"));
    CRPC_CHECK_EQ(cl::health_state::passing, bridge.state("api", "n1"));
}

// --- F4: a purged key is not fresh -------------------------------------------

CRPC_TEST(phase0_f4_purged_key_requires_fence) {
    cl::registry_config config;
    config.tombstone_grace = 0;
    cl::registry reg(config);

    CRPC_REQUIRE(reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                       make_ctx("n1"))
                     .ok());
    CRPC_REQUIRE(reg.deregister("svc", "n1", make_ctx("n1", 0, 1)).ok());
    CRPC_REQUIRE(reg.purge_expired_tombstones() == 1);

    // The fence epoch survives the purge, so a zero-fence create would reset
    // it. Strict fencing rejects the resurrection.
    const auto unfenced =
        reg.register_instance(make_instance("svc", "n1", 8001, 3), make_ctx("n1"));
    CRPC_CHECK_FALSE(unfenced.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, unfenced.code());
    CRPC_CHECK_EQ(std::string("fence_required"), rejection_reason(unfenced));

    // An explicit epoch above the surviving high-water is accepted.
    const auto fenced = reg.register_instance(make_instance("svc", "n1", 8001, 3),
                                              make_ctx("n1", 3));
    CRPC_CHECK(fenced.ok());
}

CRPC_TEST_MAIN()
