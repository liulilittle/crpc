// CRPC -- G19 step1 S-B1: fence semantics (L-a / L-b / Z2 / A8).
//
// These cases pin the three layers of the fencing contract that the G19 audit
// separated:
//
//   L-a  comparison: below the key high-water is stale, equal is accepted only
//        as an exact replay or as the first use of an outstanding issuance,
//        above is accepted within max_fence_lead.
//   L-b  issuance attribution: only next_fence() and fence_barrier() publish a
//        token, and a write must present the token that was issued for its key,
//        principal (node id hash) and lease id. An unissued token is rejected
//        with registry.reason=not_issued. Knowing the high-water is therefore
//        not the same as being allowed to use it, which is what closes the
//        "deposed holder writes high_water+1" hole that L-a alone cannot close.
//   Z2   zero fence: rejected on every path once the authorization mode is on,
//        including the first registration of a key that has never existed.
//   A8   allocation: the lease allocator jumps from max(global, per-key floor)
//        in one step, so a fence_barrier of 1e6 does not cost 1e6 grants.
//
// Scope note: every case below turns registry_config::require_issued_fence on
// explicitly. These cases test the semantics of the switch. The historical
// default (off) keeps the Phase 0 numeric contract for in-process users, whose
// call sites live in test files outside this slice; the default flip is a
// separate slice and its own cases.
//
// Coverage note: write_context carries node_id / fence / incarnation only -- it
// has no lease field (see registry.hpp). The lease dimension of L-b is therefore
// reachable only through the register_instance(..., lease_id) overload, and
// E15b is the SINGLE end-to-end case that pins it. Deleting E15b silently drops
// a quarter of the L-b coverage (token, principal, lease, outstanding), so it
// must not be removed or folded into another case.
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <limits>
#include <string>

#include <crpc/cluster/lease.hpp>
#include <crpc/cluster/registry.hpp>

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
    context.writer_epoch = 0;  // external writer: not epoch-tracked
    return context;
}

// An epoch-tracked writer: the shape a bridge/service uses. The epoch is
// obtained from the authority, never manufactured locally.
cl::write_context make_epoch_ctx(cl::registry& /*reg*/, const std::string& node,
                                 crpc::u64 epoch, crpc::u64 fence = 0,
                                 crpc::u64 incarnation = 0) {
    cl::write_context context = make_ctx(node, fence, incarnation);
    context.writer_epoch = epoch;
    return context;
}

// The strict authorization mode: Z2 + three-way comparison + issuance checks.
cl::registry_config strict_config() {
    cl::registry_config config;
    config.require_issued_fence = true;
    return config;
}

std::string reason_of(const crpc::rpc_error& error) {
    const std::string* reason = error.metadata.find("registry.reason");
    return reason != nullptr ? *reason : std::string{};
}

// Safe for both outcomes: reading error() from an ok result terminates in this
// library, and a red run must report every failure instead of dying on the
// first one.
template <typename T>
std::string rejection_reason(const crpc::result<T>& outcome) {
    return outcome.ok() ? std::string("<accepted>") : reason_of(outcome.error());
}

} // namespace

// --- E6: an equal fence from a NEW incarnation is not an exact replay --------

CRPC_TEST(E6_equal_fence_new_incarnation_rejected) {
    cl::registry reg(strict_config());

    // The first write takes the authority allocation and fixes the high-water.
    const auto issued = reg.next_fence("svc", "n1");
    CRPC_REQUIRE(issued.ok());
    const u64 hw = issued.value();
    CRPC_REQUIRE(reg
                     .register_instance(make_instance("svc", "n1", 8001, 1),
                                        make_ctx("n1", hw, 1))
                     .ok());
    CRPC_CHECK_EQ(hw, reg.fence_floor("svc", "n1").value());

    // A new incarnation (restarted process) that only observed the high-water
    // reuses the same token. It is not a replay of the accepted write: the
    // payload and incarnation differ.
    const auto equal_fence = reg.register_instance(
        make_instance("svc", "n1", 9001, 2), make_ctx("n1", hw, 2));
    CRPC_CHECK_FALSE(equal_fence.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, equal_fence.code());
    CRPC_CHECK_EQ(std::string("stale_fence"), rejection_reason(equal_fence));

    // The accepted row is untouched - the exercised failure was a write, not a
    // read.
    cl::query_options options;
    options.service = "svc";
    options.node_id = "n1";
    const auto rows = reg.query(options);
    CRPC_REQUIRE(rows.ok());
    CRPC_REQUIRE(rows.value().instances.size() == 1);
    CRPC_CHECK_EQ(crpc::u16{8001}, rows.value().instances[0].address.port);
}

// --- E6b: the same request replayed identically stays idempotent ------------

CRPC_TEST(E6b_exact_replay_of_accepted_write_is_idempotent) {
    cl::registry reg(strict_config());

    const auto issued = reg.next_fence("svc", "n1");
    CRPC_REQUIRE(issued.ok());
    const auto first = reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                            make_ctx("n1", issued.value(), 1));
    CRPC_REQUIRE(first.ok());

    // Byte-identical replay: same key, incarnation, payload and token.
    const auto replay = reg.register_instance(
        make_instance("svc", "n1", 8001, 1), make_ctx("n1", issued.value(), 1));
    CRPC_REQUIRE(replay.ok());
    CRPC_CHECK_EQ(first.value().index, replay.value().index);
    CRPC_CHECK_EQ(first.value().fence, replay.value().fence);
    CRPC_CHECK_EQ(first.value().index, reg.index());
}

// --- E7: a zero fence cannot create a key behind a barrier -------------------

CRPC_TEST(E7_zero_fence_create_after_barrier_rejected) {
    cl::registry reg(strict_config());

    CRPC_REQUIRE(reg.fence_barrier("svc", "n1", 5, make_ctx("n1")).ok());
    CRPC_CHECK_EQ(u64{5}, reg.fence_floor("svc", "n1").value());

    // The key has no visible row, so the historical contract treated it as
    // fresh and let an unfenced create take it. That is the squatter hole.
    const auto zero = reg.register_instance(make_instance("svc", "n1", 6666, 99),
                                            make_ctx("n1"));
    CRPC_CHECK_FALSE(zero.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, zero.code());
    CRPC_CHECK_EQ(std::string("fence_required"), rejection_reason(zero));

    // The key is still free for its legitimate owner: a fresh issuance above
    // the barrier is accepted and keeps the endpoint the caller asked for.
    const auto issued = reg.next_fence("svc", "n1");
    CRPC_REQUIRE(issued.ok());
    CRPC_CHECK(issued.value() > 5);
    CRPC_REQUIRE(reg
                     .register_instance(make_instance("svc", "n1", 8001, 1),
                                        make_ctx("n1", issued.value(), 1))
                     .ok());
    cl::query_options options;
    options.service = "svc";
    const auto rows = reg.query(options);
    CRPC_REQUIRE(rows.ok());
    CRPC_REQUIRE(rows.value().instances.size() == 1);
    CRPC_CHECK_EQ(crpc::u16{8001}, rows.value().instances[0].address.port);
}

// --- E7b: Z2 applies to a completely fresh key too --------------------------

CRPC_TEST(E7b_zero_fence_fresh_create_rejected) {
    cl::registry reg(strict_config());

    // No barrier, no tombstone, no fence record: the key is as fresh as it
    // gets. Z2 still requires an explicit token, because a zero fence cannot
    // be distinguished from "the caller forgot to fence".
    CRPC_CHECK_EQ(u64{0}, reg.fence_floor("never", "seen").value());

    const auto zero = reg.register_instance(
        make_instance("never", "seen", 8001, 1), make_ctx("seen"));
    CRPC_CHECK_FALSE(zero.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, zero.code());
    CRPC_CHECK_EQ(std::string("fence_required"), rejection_reason(zero));

    const auto issued = reg.next_fence("never", "seen");
    CRPC_REQUIRE(issued.ok());
    CRPC_CHECK(issued.value() > 0);
    CRPC_REQUIRE(reg
                     .register_instance(make_instance("never", "seen", 8001, 1),
                                        make_ctx("seen", issued.value(), 1))
                     .ok());
}

// --- E9: next_fence jumps over a barrier instead of walking to it -----------

CRPC_TEST(E9_next_fence_jumps_over_floor) {
    cl::registry reg(strict_config());

    const u64 barrier = 1000000;
    CRPC_REQUIRE(reg.fence_barrier("svc", "n1", barrier, make_ctx("n1")).ok());

    const auto issued = reg.next_fence("svc", "n1");
    CRPC_REQUIRE(issued.ok());
    // One step, not barrier-many: the allocation is max(global, floor) + 1.
    CRPC_CHECK(issued.value() > barrier);
    CRPC_CHECK_EQ(barrier + 1, issued.value());
    CRPC_CHECK(reg.fence_floor("svc", "n1").value() >= barrier);

    // The allocation is usable immediately: no "fail once to learn the floor"
    // step is needed.
    CRPC_REQUIRE(reg
                     .register_instance(make_instance("svc", "n1", 8001, 1),
                                        make_ctx("n1", issued.value(), 1))
                     .ok());

    // The global allocation moved with it, so a second key is not behind.
    const auto other = reg.next_fence("svc", "n2");
    CRPC_REQUIRE(other.ok());
    CRPC_CHECK(other.value() > issued.value());
}

// --- E9b: the lease allocator clears the floor on its FIRST grant -----------

CRPC_TEST(E9b_grant_after_barrier_succeeds_first_try) {
    manual_clock clock;
    cl::registry reg(strict_config());
    cl::lease_config config;
    config.critical_grace = 1000ms;
    cl::lease_manager leases(reg, make_clock(clock), config);

    CRPC_REQUIRE(reg.fence_barrier("svc", "n1", 1000000, make_ctx("n1")).ok());

    const auto granted = leases.grant("svc", "n1", 1000ms);
    CRPC_REQUIRE(granted.ok());
    CRPC_CHECK(granted.value().fence > 1000000);

    // The holder's FIRST write with the token its own grant published must
    // succeed. Before A8 this was the livelock: the allocator returned a token
    // below the barrier and the write was rejected as stale, repeatedly.
    CRPC_REQUIRE(reg
                     .register_instance(make_instance("svc", "n1", 8001, 1),
                                        make_ctx("n1", granted.value().fence, 1),
                                        granted.value().id)
                     .ok());
    CRPC_CHECK_EQ(cl::instance_state::passing, [&] {
        cl::query_options options;
        options.service = "svc";
        options.node_id = "n1";
        const auto rows = reg.query(options);
        return rows.ok() && !rows.value().instances.empty()
                   ? rows.value().instances.front().state
                   : cl::instance_state::deregistered;
    }());
}

// --- E10: fence_floor is per key, never the global allocation ---------------

CRPC_TEST(E10_fence_floor_reports_per_key_high_water) {
    cl::registry reg(strict_config());

    CRPC_REQUIRE(reg.fence_barrier("svc", "a", 7, make_ctx("a")).ok());
    CRPC_REQUIRE(reg.fence_barrier("svc", "b", 1000, make_ctx("b")).ok());
    CRPC_REQUIRE(reg.fence_barrier("other", "a", 3, make_ctx("a")).ok());

    // Each key reports its own high-water. A global value would make them all
    // equal, which is exactly the confusion that made the lease allocator walk
    // towards a barrier one grant at a time.
    CRPC_CHECK_EQ(u64{7}, reg.fence_floor("svc", "a").value());
    CRPC_CHECK_EQ(u64{1000}, reg.fence_floor("svc", "b").value());
    CRPC_CHECK_EQ(u64{3}, reg.fence_floor("other", "a").value());

    // An unknown key has no record; zero is the documented "no record" answer.
    CRPC_CHECK_EQ(u64{0}, reg.fence_floor("svc", "missing").value());

    // The global allocation never absorbed the barrier values, so the three
    // floors above are genuinely per key and not aliases of fence().
    CRPC_CHECK(reg.fence() < 1000);

    // An accepted write raises that key's floor only.
    const auto issued = reg.next_fence("svc", "a");
    CRPC_REQUIRE(issued.ok());
    CRPC_REQUIRE(reg
                     .register_instance(make_instance("svc", "a", 8001, 1),
                                        make_ctx("a", issued.value(), 1))
                     .ok());
    CRPC_CHECK(reg.fence_floor("svc", "a").value() >= issued.value());
    CRPC_CHECK_EQ(u64{1000}, reg.fence_floor("svc", "b").value());
}

// --- E14: a token the authority never issued is rejected --------------------

CRPC_TEST(E14_unissued_token_rejected) {
    cl::registry reg(strict_config());

    const auto issued = reg.next_fence("svc", "n1");
    CRPC_REQUIRE(issued.ok());
    CRPC_REQUIRE(reg
                     .register_instance(make_instance("svc", "n1", 8001, 1),
                                        make_ctx("n1", issued.value(), 1))
                     .ok());
    const u64 hw = reg.fence_floor("svc", "n1").value();

    // Every one of these is in range and strictly above the high-water, so the
    // numeric comparison of L-a alone would accept them all.
    for (const u64 invented : {hw + 1, hw + 1000, hw + 1000000}) {
        const auto forged = reg.set_state("svc", "n1", cl::instance_state::critical,
                                          make_ctx("n1", invented, 1));
        CRPC_CHECK_FALSE(forged.ok());
        CRPC_CHECK_EQ(rpc_code::failed_precondition, forged.code());
        CRPC_CHECK_EQ(std::string("not_issued"), rejection_reason(forged));
    }

    // The key was not modified by any of the rejected attempts.
    CRPC_CHECK_EQ(hw, reg.fence_floor("svc", "n1").value());
}

// --- E14b: the reason is exactly "not_issued" -------------------------------

CRPC_TEST(E14b_unissued_token_reason_is_exact) {
    cl::registry reg(strict_config());

    const auto issued = reg.next_fence("svc", "n1");
    CRPC_REQUIRE(issued.ok());
    CRPC_REQUIRE(reg
                     .register_instance(make_instance("svc", "n1", 8001, 1),
                                        make_ctx("n1", issued.value(), 1))
                     .ok());
    const u64 hw = reg.fence_floor("svc", "n1").value();

    const auto forged = reg.register_instance(
        make_instance("svc", "n1", 9001, 2), make_ctx("n1", hw + 1, 2));
    CRPC_REQUIRE(!forged.ok());
    // Exact equality, not a substring and not "some failed_precondition": a
    // rejection for another reason must not be able to impersonate the
    // issuance check.
    CRPC_CHECK_EQ(std::string("not_issued"), reason_of(forged.error()));
    CRPC_CHECK(reason_of(forged.error()) != std::string("stale_fence"));
    CRPC_CHECK(reason_of(forged.error()) != std::string("fence_required"));
    CRPC_CHECK(reason_of(forged.error()) != std::string("fence_out_of_range"));
}

// --- E15: an issued token is bound to its principal -------------------------

CRPC_TEST(E15_issued_token_for_other_lease_rejected) {
    cl::registry_config config = strict_config();
    // An admin token lets a caller write on behalf of any node id, which is the
    // only way to reach check_fence() with a principal other than the key
    // owner: without it the authority check rejects the impersonation first.
    config.admin_token = "ops-secret";
    cl::registry reg(config);

    // Issued for (svc, n1), i.e. bound to principal hash("n1").
    const auto issued = reg.next_fence("svc", "n1");
    CRPC_REQUIRE(issued.ok());
    CRPC_REQUIRE(reg
                     .register_instance(make_instance("svc", "n1", 8001, 1),
                                        make_ctx("n1", issued.value(), 1))
                     .ok());
    const u64 hw = reg.fence_floor("svc", "n1").value();

    // A different principal ("n2") holding the admin token writes to n1's key.
    // It presents a token above the high-water, so the numeric L-a comparison
    // would accept it; the issuance check rejects it because the token was
    // issued to n1's principal.
    cl::write_context forged = make_ctx("n2", hw + 1000, 2);
    forged.admin_token = "ops-secret";
    const auto impersonated =
        reg.register_instance(make_instance("svc", "n1", 6666, 2), forged);
    CRPC_CHECK_FALSE(impersonated.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, impersonated.code());
    CRPC_CHECK_EQ(std::string("not_issued"), rejection_reason(impersonated));

    // The endpoint was not hijacked.
    cl::query_options options;
    options.service = "svc";
    options.node_id = "n1";
    const auto rows = reg.query(options);
    CRPC_REQUIRE(rows.ok());
    CRPC_REQUIRE(rows.value().instances.size() == 1);
    CRPC_CHECK_EQ(crpc::u16{8001}, rows.value().instances[0].address.port);

    // Without the admin token the same attempt is stopped even earlier.
    const auto no_admin = reg.register_instance(make_instance("svc", "n1", 6666, 2),
                                               make_ctx("n2", hw + 1000, 2));
    CRPC_CHECK_FALSE(no_admin.ok());
    CRPC_CHECK_EQ(std::string("authority"), rejection_reason(no_admin));
}

// --- E15b: the same principal cannot spend another lease's token ------------
CRPC_TEST(E15b_same_principal_different_lease_rejected) {
    cl::registry reg(strict_config());

    const auto issued = reg.next_fence("svc", "n1", /*lease_id=*/1);
    CRPC_REQUIRE(issued.ok());
    const u64 token = issued.value();

    // Same node id (same principal hash), different lease: rejected. Principal
    // equality alone is not enough, the token must be spent by the lease it was
    // issued to.
    const auto wrong_lease = reg.register_instance(
        make_instance("svc", "n1", 8001, 1), make_ctx("n1", token, 1),
        /*lease_id=*/2);
    CRPC_CHECK_FALSE(wrong_lease.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, wrong_lease.code());
    CRPC_CHECK_EQ(std::string("not_issued"), rejection_reason(wrong_lease));

    // The rightful lease spends it once.
    CRPC_REQUIRE(reg
                     .register_instance(make_instance("svc", "n1", 8001, 1),
                                        make_ctx("n1", token, 1), /*lease_id=*/1)
                     .ok());

    // And a second spend of the same token is stale even for that lease: the
    // issuance is consumed by the write, so a token is not a permanent ticket.
    const auto replayed = reg.register_instance(
        make_instance("svc", "n1", 9001, 2), make_ctx("n1", token, 2),
        /*lease_id=*/1);
    CRPC_CHECK_FALSE(replayed.ok());
    CRPC_CHECK_EQ(std::string("stale_fence"), rejection_reason(replayed));
}

// --- G19 step1: writer_epoch admission ---------------------------------------

CRPC_TEST(write_context_unset_epoch_is_rejected) {
    cl::registry reg(strict_config());
    const auto instance = make_instance("svc", "n1", 8001, 1);
    cl::write_context context;
    context.node_id = "n1";
    context.incarnation = 1;
    context.fence = reg.next_fence("svc", "n1").value();
    // Leave writer_epoch at its default: the sentinel means "caller did not set
    // an epoch" and must be rejected rather than silently treated as exempt.
    CRPC_CHECK_EQ((std::numeric_limits<crpc::u64>::max)(), context.writer_epoch);
    const auto outcome = reg.register_instance(instance, context);
    CRPC_CHECK_FALSE(outcome.ok());
    CRPC_CHECK_EQ(std::string("epoch_required"), rejection_reason(outcome));
}

CRPC_TEST(zero_epoch_is_accepted_and_not_recorded) {
    cl::registry reg(strict_config());
    cl::write_context context = make_ctx("n1", 0, 1);
    context.fence = reg.next_fence("svc", "n1").value();
    context.writer_epoch = 0;   // external writer: exempt, records nothing
    CRPC_CHECK(reg.register_instance(make_instance("svc", "n1", 8001, 1),
                                    context)
                    .ok());

    // A zero-epoch write must not lower the recorded floor: an epoch-tracked
    // writer that already committed a high epoch still owns the key. The
    // incarnation moves so this is a real write, not an idempotent replay (a
    // replay returns before the epoch is recorded).
    cl::write_context tracked = make_ctx("n1", 0, 2);
    tracked.fence = reg.next_fence("svc", "n1").value();
    tracked.writer_epoch = 42;
    CRPC_CHECK(reg.register_instance(make_instance("svc", "n1", 8001, 2),
                                    tracked)
                    .ok());

    cl::write_context zero_again = make_ctx("n1", 0, 3);
    zero_again.fence = reg.next_fence("svc", "n1").value();
    zero_again.writer_epoch = 0;
    CRPC_CHECK(reg.register_instance(make_instance("svc", "n1", 8001, 3),
                                    zero_again)
                    .ok());

    // The floor is still 42: a writer at 41 is stale, at 42 is accepted.
    cl::write_context stale = make_ctx("n1", 0, 4);
    stale.fence = reg.next_fence("svc", "n1").value();
    stale.writer_epoch = 41;
    CRPC_CHECK_EQ(std::string("stale_writer"),
                  rejection_reason(reg.register_instance(
                      make_instance("svc", "n1", 8001, 4), stale)));
}

CRPC_TEST(stale_writer_cannot_overwrite_newer_epoch) {
    cl::registry reg(strict_config());

    cl::write_context older = make_ctx("n1", 0, 1);
    older.fence = reg.next_fence("svc", "n1").value();
    older.writer_epoch = 5;
    CRPC_CHECK(reg.register_instance(make_instance("svc", "n1", 8001, 1), older)
                    .ok());

    // A newer epoch takes over. It needs a fresh issuance like any other write,
    // and a distinct incarnation so the write is not an idempotent replay.
    cl::write_context newer = make_ctx("n1", 0, 2);
    newer.fence = reg.next_fence("svc", "n1").value();
    newer.writer_epoch = 6;
    CRPC_CHECK(reg.register_instance(make_instance("svc", "n1", 8001, 2), newer)
                    .ok());

    // The old epoch is now fenced out even though it can still obtain a fresh
    // fence token: epoch is checked before fence, so the rejection reason is
    // stale_writer, not stale_fence. That distinction is the whole point: a
    // stale_fence tells the writer to re-anchor and keep writing.
    cl::write_context back = make_ctx("n1", 0, 3);
    back.fence = reg.next_fence("svc", "n1").value();
    back.writer_epoch = 5;
    CRPC_CHECK_EQ(std::string("stale_writer"),
                  rejection_reason(reg.register_instance(
                      make_instance("svc", "n1", 8001, 3), back)));
}

CRPC_TEST(epoch_is_checked_before_fence) {
    cl::registry reg(strict_config());

    cl::write_context newer = make_ctx("n1", 0, 1);
    newer.fence = reg.next_fence("svc", "n1").value();
    newer.writer_epoch = 9;
    CRPC_CHECK(reg.register_instance(make_instance("svc", "n1", 8001, 1), newer)
                    .ok());

    // Both the epoch AND the fence are wrong here. If fence were checked first
    // the reason would be stale_fence; the contract says the epoch verdict wins
    // because a deposed writer must yield instead of re-anchoring.
    cl::write_context both_wrong = make_ctx("n1", /*fence=*/1, 2);
    both_wrong.writer_epoch = 3;
    CRPC_CHECK_EQ(std::string("stale_writer"),
                  rejection_reason(reg.register_instance(
                      make_instance("svc", "n1", 8001, 2), both_wrong)));
}

CRPC_TEST(same_epoch_may_write_again) {
    cl::registry reg(strict_config());

    cl::write_context first = make_ctx("n1", 0, 1);
    first.fence = reg.next_fence("svc", "n1").value();
    first.writer_epoch = 7;
    CRPC_CHECK(reg.register_instance(make_instance("svc", "n1", 8001, 1), first)
                    .ok());

    // The same writer keeps writing with the same epoch; only a strictly older
    // epoch is stale. Turning the comparison into <= would freeze every writer
    // after its first write.
    cl::write_context second = make_ctx("n1", 0, 2);
    second.fence = reg.next_fence("svc", "n1").value();
    second.writer_epoch = 7;
    CRPC_CHECK(reg.register_instance(make_instance("svc", "n1", 8001, 2), second)
                    .ok());
}

CRPC_TEST(epoch_grant_is_unconditional) {
    // next_writer_epoch() is a monotonic counter, NOT a capability: it is
    // handed to any caller with no identity or liveness check. This case pins
    // that fact so nobody mistakes an epoch for proof of ownership. It is why
    // health_bridge's yield must be permanent (see test_health_bridge.cpp).
    cl::registry reg;
    crpc::u64 previous = 0;
    for (int i = 0; i < 64; ++i) {
        const auto granted = reg.next_writer_epoch();
        CRPC_REQUIRE(granted.ok());
        CRPC_CHECK(granted.value() > previous);
        CRPC_CHECK(granted.value() != (std::numeric_limits<crpc::u64>::max)());
        previous = granted.value();
    }
}

CRPC_TEST_MAIN()
