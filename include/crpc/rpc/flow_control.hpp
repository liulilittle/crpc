// CRPC -- per-connection flow control for the RPC call path.
//
// The primitives in <crpc/flow/estimator.hpp> are decision functions with no
// state of their own about a connection. This header is the object that gives
// them a connection: it owns the round-trip tracker, the estimator, the
// congestion window, the pacer and the per-lane byte accounting, and it exposes
// exactly the three questions the call path asks:
//
//   * may this call be admitted now (`can_admit`),
//   * account for an admitted call (`on_admitted`),
//   * give the bytes back when the call is done (`release`).
//
// Everything here is driven by PING/PONG probes rather than by a timer: the
// connection samples its own round trip and the window grows only on
// acknowledgements, so a silent path never causes more traffic.
//
// Threading: every member function must be called with the client's
// `map_mutex_` held. The controller takes no lock of its own, which keeps it
// out of the four-lock order documented in <crpc/rpc/client.hpp>; it is the
// innermost state, never a lock holder.
//
// Byte accounting
// ---------------
// The window is a byte window over *frames*, not calls: a call is charged the
// encoded frame size on admission and the same amount is released when it
// terminates. The release is idempotent and keyed by `(stream_id, generation)`,
// so a path that both finishes a call and fails it during teardown cannot
// release the same bytes twice, and a recycled stream id (a new generation)
// cannot be released by a stale key.
//
// An empty window admits one call unconditionally: a window that refused
// everything would never learn anything, and the first call on a fresh
// connection is what produces the first probe sample. ONE_WAY calls are never
// charged at all -- they complete as soon as they are published, so charging
// them would consume window bytes that no acknowledgement can ever return.
#ifndef CRPC_RPC_FLOW_CONTROL_HPP
#define CRPC_RPC_FLOW_CONTROL_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/flow/estimator.hpp>

namespace crpc {
namespace rpc {

// Configuration of the connection-level controller. Deliberately a separate
// value type: `rpc::client_options` is an ABI-pinned record and must not grow a
// byte, so flow control is installed through `client::set_flow_control()`.
struct flow_control_options {
    // Installs the controller when true. `set_flow_control` with a disabled
    // option set removes the controller again.
    bool enabled = true;
    // Round-trip estimator (baseline, growth, confirmation thresholds).
    flow::rtt_estimator::config estimator{};
    // Congestion window (byte window, slow start, decrease factor).
    flow::congestion_window::config window{};
    // Pacer (token bucket). Its rate is refilled from window/baseline below.
    flow::pacer::config pacer{};
    // Per-lane byte quota. Zero disables the quota.
    //
    // The unit is *encoded frame bytes*, not payload bytes: the count includes
    // the 16-byte frame header, the 16-byte ORDERED extension, the call envelope
    // and whichever optional blocks the context carries (trace, wire deadline,
    // idempotency key). `rpc::client::predicted_frame_size` returns exactly this
    // number, so a caller can size a quota against a real frame.
    //
    // The quota is what makes one ordered lane unable to consume the whole
    // connection: a lane that is full is refused locally, without a sequence
    // number being consumed, so the refusal leaves no hole in the lane's
    // sequence stream.
    u64 lane_quota_bytes = 0;
    // Number of distinct lanes the accounting table can hold at once.
    u32 lane_table_capacity = 64;
    // Number of simultaneous charges the table can index. Zero means "follow
    // the client's call-slot capacity", which is what `set_flow_control` fills
    // in. It must never be smaller than the slot capacity: an admission the
    // table cannot index would have its bytes charged and never returned, which
    // ends in a permanently full window. Admission refuses rather than leak if
    // the table is ever exhausted, but the sizing is what makes that
    // unreachable.
    u32 charge_table_capacity = 0;
};

// A read-only view of the controller. Deliberately a plain record with no
// standard-library members: it crosses the shared-library boundary by value from
// `rpc::client::flow_snapshot()`, and docs/abi.md §2 forbids standard containers
// in an exported signature. Per-lane occupancy is queried with
// `rpc::client::lane_used()`, which returns a scalar.
struct flow_state {
    bool enabled = false;
    // Congestion window in bytes, and the bytes currently charged against it.
    u64 window_bytes = 0;
    u64 in_flight_bytes = 0;
    // Round-trip estimate and the queue-delay threshold derived from it
    // (`baseline * growth_per_mille / 1000`).
    u64 baseline_ns = 0;
    u64 theta_ns = 0;
    // The pacer's current rate and available credit. The rate follows
    // `window / baseline`, so it is the window (not a constant) that decides
    // how fast the connection may send.
    u64 pacer_rate_bytes_per_second = 0;
    u64 pacer_credit_bytes = 0;
    // Counters.
    u64 admitted = 0;
    u64 rejected = 0;
    // Number of releases that actually removed a charge. It makes the fallback
    // release in `fail_all` observable: the reset that follows a teardown clears
    // every counter, so "lane_used == 0" alone cannot distinguish "released" from
    // "forgotten".
    u64 released = 0;
    u64 congestion_events = 0;
    u64 probes_sent = 0;
    // Probes whose PING actually reached the writer queue. `probes_sent` is the
    // number of attempts and only ever grows, so a monitoring rate is a plain
    // delta; `probes_staged + probes_aborted == probes_sent` always holds.
    u64 probes_staged = 0;
    u64 probe_samples = 0;
    // Probes the writer refused (write bound, closed connection). They were
    // attempted and rolled back, so they are counted here and not as staged.
    u64 probes_aborted = 0;
    u64 samples_above_theta = 0;
    // Charge-table occupancy and size. `charges_in_use == charge_capacity` means
    // the next admission is refused for lack of an index entry, which is a
    // different condition from a full window and must be distinguishable in a
    // snapshot.
    u64 charges_in_use = 0;
    u64 charge_capacity = 0;
    // Number of lanes currently holding bytes.
    u32 lanes_in_use = 0;
};

// The connection-level controller. Not exported: only `rpc::client` owns one,
// and a caller observes it through `client::can_admit()` and
// `client::flow_snapshot()`. Keeping the type out of the export table keeps the
// ABI surface at "one added pointer member plus three methods".
class flow_controller {
public:
    explicit flow_controller(const flow_control_options& options);

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // Caches the clock reading the caller already made. The controller never
    // reads a clock itself: the call path must not gain a clock read (a test
    // pins that), so the client hands over the timestamp it took anyway.
    void observe_now(u64 now_ns) noexcept { last_now_ns_ = now_ns; }

    // True when a call of `bytes` on `lane_id` may be admitted. An empty window
    // admits one call unconditionally (otherwise the connection could never
    // produce its first measurement); ONE_WAY callers must not call this at all
    // (the client skips the charge for them).
    //
    // The pacer is *not* bypassed by the empty-window rule: a drained bucket
    // with one long call in flight must still recover as time passes, and a
    // client that completes every call immediately must still be rate limited.
    // The one exception is a call larger than the whole bucket, which cannot be
    // split and would otherwise never be sendable.
    [[nodiscard]] bool can_admit(u64 lane_id, std::size_t bytes) noexcept;

    // Charges an admitted call. `stream_id` / `generation` are the release key.
    // Returns false when the charge table cannot index the call; the caller must
    // then treat the admission as failed rather than publish the call, because
    // bytes that are not indexed can never be released.
    [[nodiscard]] bool on_admitted(u64 lane_id, std::size_t bytes, u32 stream_id,
                                   u32 generation) noexcept;

    // Returns the bytes charged for `(stream_id, generation)`. Idempotent: a
    // second call for the same key, or a key whose generation no longer
    // matches the live entry, does nothing.
    //
    // `acknowledged` says whether the call actually completed with a response.
    // The window is self-clocked and grows only on an acknowledgement, so a
    // cancellation, a local timeout or a teardown returns the bytes without
    // growing it.
    void release(u32 stream_id, u32 generation, bool acknowledged) noexcept;

    // Token to put in a probe's payload, or `~0ull` when no probe should be
    // sent. The token is the send timestamp, so the PONG echo carries it back;
    // zero is a legal timestamp (a clock may start at zero), which is why the
    // "no probe" answer is all-ones rather than zero.
    //
    // Call contract: every successful `begin_probe` must be followed by exactly
    // one of `confirm_probe_staged` (the PING reached the writer queue) or
    // `abort_probe` (it did not). The controller cannot tell on its own, and
    // skipping both leaves the "one probe outstanding" flag set. A reply then
    // clears it through `on_probe_reply`.
    [[nodiscard]] u64 begin_probe() noexcept;

    // Reports that the probe `begin_probe` reserved could not be staged (the
    // write bound refused it, the connection closed, ...). Without this the
    // "one probe outstanding" flag would stay set forever and no probe would
    // ever be sent again, so the baseline would never update.
    void abort_probe() noexcept;

    // Reports that the reserved probe's PING was staged successfully. The
    // controller cannot tell on its own: staging happens in the client's writer.
    //
    // Call contract: every successful `begin_probe` must be followed by exactly
    // one of `confirm_probe_staged` (staged) or `abort_probe` (not staged). If
    // neither is called, `probes_staged` stays at zero and the counters look as
    // if no probe was ever staged, even though one was sent. `rpc::client` pairs
    // them at its only call site; a caller that drives `flow_controller`
    // directly has to do the same.
    void confirm_probe_staged() noexcept;

    // Feeds the round trip measured for `token_ns`.
    void on_probe_reply(u64 token_ns, u64 now_ns) noexcept;

    void reset() noexcept;

    [[nodiscard]] flow_state snapshot() const;

    // Bytes currently charged to `lane_id`, or zero when the lane holds
    // nothing. A scalar accessor rather than a list keeps `flow_snapshot` free
    // of standard-library members (it crosses the ABI boundary by value).
    [[nodiscard]] u64 lane_used(u64 lane_id) const noexcept;

    // `window / baseline`, clamped to the pacer's configured bounds. Exposed so
    // the wiring test can assert the pacer is fed by the window rather than by
    // the pacer's own 1 MiB/s default.
    [[nodiscard]] u64 pacer_rate_bytes_per_second() const noexcept;

private:
    struct lane_entry {
        u64 lane = 0;
        u64 bytes = 0;
        bool used = false;
    };

    // One charged call, keyed by stream id (open addressing over a fixed array,
    // so admission and release allocate nothing).
    struct charge_entry {
        u32 stream_id = 0;
        u32 generation = 0;
        u64 lane = 0;
        u64 bytes = 0;
        bool used = false;
    };

    [[nodiscard]] u64 lane_bytes(u64 lane_id) const noexcept;
    void lane_add(u64 lane_id, u64 bytes) noexcept;
    void lane_sub(u64 lane_id, u64 bytes) noexcept;
    [[nodiscard]] charge_entry* find_charge(u32 stream_id) noexcept;
    [[nodiscard]] const charge_entry* find_charge(u32 stream_id) const noexcept;
    // Returns false when the table has no room: the caller must then refuse the
    // admission instead of charging bytes that could never be released.
    [[nodiscard]] bool insert_charge(const charge_entry& entry) noexcept;
    void erase_charge(u32 stream_id) noexcept;
    [[nodiscard]] u64 theta_ns() const noexcept;
    void refresh_pacer_rate() noexcept;

    flow_control_options options_;
    bool enabled_ = false;
    flow::rtt_estimator estimator_;
    flow::congestion_window window_;
    flow::pacer pacer_;
    flow::rtt_tracker tracker_;

    u64 in_flight_bytes_ = 0;
    u64 last_now_ns_ = 0;
    u64 last_decrease_ns_ = 0;
    u64 probes_sent_ = 0;
    u64 probes_staged_ = 0;
    u64 probe_samples_ = 0;
    u64 probes_aborted_ = 0;
    u64 samples_above_theta_ = 0;
    u64 congestion_events_ = 0;
    u64 admitted_ = 0;
    u64 rejected_ = 0;
    u64 released_ = 0;
    u32 bottom_hits_ = 0;
    // Downward confirmation window. A single fast sample can be an artifact
    // (a stalled or coalesced ACK), so a drop below the baseline is confirmed
    // like an upward shift is, and the *slowest* sample of the window is the
    // one that is accepted. Feeding the minimum instead would let one artifact
    // poison the baseline permanently: with the decided rule that
    // over-threshold samples are not fed to the estimator, a crushed baseline
    // makes every honest sample over-threshold, so it could never recover.
    u32 down_run_ = 0;
    u64 down_max_ = 0;
    // Set while a probe is on the wire; a second probe is never sent until the
    // first one is answered or abandoned, which bounds probe traffic by the
    // round trip rather than by the call rate.
    bool probe_outstanding_ = false;
    u64 probe_token_ = 0;
    // When the outstanding probe was sent. A probe whose reply never arrives (a
    // lost frame, a peer that stopped answering) must not wedge the controller,
    // so it is re-armed once it is older than the tracker's sample window --
    // the same bound that makes an older sample useless anyway.
    u64 probe_sent_ns_ = 0;
    // Number of live entries in the charge table. Admission refuses once it
    // reaches the table size, so an unindexed charge (which could never be
    // released) is impossible rather than merely unlikely.
    std::size_t charge_count_ = 0;

    std::vector<lane_entry> lanes_;
    std::vector<charge_entry> charges_;
};

} // namespace rpc
} // namespace crpc

#endif // CRPC_RPC_FLOW_CONTROL_HPP
