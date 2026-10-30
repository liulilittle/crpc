// CRPC -- per-connection flow control implementation.
#include <crpc/rpc/flow_control.hpp>

namespace crpc {
namespace rpc {
namespace {

constexpr u64 kNoTimestamp = static_cast<u64>(-1);

u64 scale_per_mille(u64 value, u32 per_mille) noexcept {
    return (value / 1000u) * per_mille + ((value % 1000u) * per_mille) / 1000u;
}

} // namespace

flow_controller::flow_controller(const flow_control_options& options)
    : options_(options),
      estimator_(options.estimator),
      window_(options.window),
      pacer_(options.pacer) {
    enabled_ = true;
    const u32 lane_capacity = options_.lane_table_capacity == 0 ? 1 : options_.lane_table_capacity;
    lanes_.assign(lane_capacity, lane_entry{});
    // The charge table must be able to index every simultaneously admitted
    // call, so it is sized from the client's call-slot capacity (filled in by
    // `set_flow_control`), rounded up to a power of two for the mask below.
    // A table smaller than the slot count would let an admission go unindexed,
    // and unindexed bytes can never be released: the window would fill up and
    // the connection would stop admitting anything. The count is capped so a
    // hostile configuration cannot ask for gigabytes.
    std::size_t charge_capacity = options_.charge_table_capacity;
    if (charge_capacity == 0) {
        // Follow the client's call-slot capacity: the table must be able to
        // index every simultaneously admitted call, and the slot pool is what
        // bounds that number. set_flow_control fills this in from
        // client::options::slot_capacity; the fallback here only applies to a
        // controller built directly.
        charge_capacity = 4096;
    }
    constexpr std::size_t kMaxChargeCapacity = 1u << 20;
    if (charge_capacity > kMaxChargeCapacity) {
        charge_capacity = kMaxChargeCapacity;
    }
    std::size_t table_size = 2;
    while (table_size < charge_capacity * 2) {
        table_size <<= 1;
    }
    charges_.assign(table_size, charge_entry{});
    last_decrease_ns_ = kNoTimestamp;
    refresh_pacer_rate();
}

u64 flow_controller::theta_ns() const noexcept {
    return scale_per_mille(estimator_.baseline_ns(), options_.estimator.growth_per_mille);
}

u64 flow_controller::pacer_rate_bytes_per_second() const noexcept {
    // rate = window / baseline. A zero baseline cannot happen after the first
    // sample and is guarded anyway: the initial baseline is non-zero.
    const u64 baseline = estimator_.baseline_ns();
    if (baseline == 0) {
        return options_.pacer.initial_rate_bytes_per_second;
    }
    const u64 window_bytes = window_.bytes();
    // bytes/s = window_bytes * 1e9 / baseline_ns, split to avoid overflow.
    const u64 seconds = 1'000'000'000ull;
    u64 rate = (window_bytes / baseline) * seconds;
    rate += ((window_bytes % baseline) * seconds) / baseline;
    if (rate < options_.pacer.min_rate_bytes_per_second) {
        rate = options_.pacer.min_rate_bytes_per_second;
    }
    if (rate > options_.pacer.max_rate_bytes_per_second) {
        rate = options_.pacer.max_rate_bytes_per_second;
    }
    return rate;
}

void flow_controller::refresh_pacer_rate() noexcept {
    pacer_.set_rate(pacer_rate_bytes_per_second());
}

// --- lane accounting ---------------------------------------------------------

u64 flow_controller::lane_bytes(u64 lane_id) const noexcept {
    for (const lane_entry& entry : lanes_) {
        if (entry.used && entry.lane == lane_id) {
            return entry.bytes;
        }
    }
    return 0;
}

void flow_controller::lane_add(u64 lane_id, u64 bytes) noexcept {
    for (lane_entry& entry : lanes_) {
        if (entry.used && entry.lane == lane_id) {
            entry.bytes += bytes;
            return;
        }
    }
    for (lane_entry& entry : lanes_) {
        if (!entry.used) {
            entry.used = true;
            entry.lane = lane_id;
            entry.bytes = bytes;
            return;
        }
    }
    // The table is full. The charge still exists in the charge table (release
    // is keyed there), so the only loss is the lane quota's view of the
    // overflowing lane; admission for that lane then under-counts rather than
    // deadlocking on an accounting failure.
}

void flow_controller::lane_sub(u64 lane_id, u64 bytes) noexcept {
    for (lane_entry& entry : lanes_) {
        if (!entry.used || entry.lane != lane_id) {
            continue;
        }
        entry.bytes = entry.bytes > bytes ? entry.bytes - bytes : 0;
        if (entry.bytes == 0) {
            entry.used = false;
            entry.lane = 0;
        }
        return;
    }
}

// --- charge table (keyed by stream id, validated by generation) ---------------

flow_controller::charge_entry* flow_controller::find_charge(u32 stream_id) noexcept {
    if (charges_.empty()) {
        return nullptr;
    }
    const std::size_t mask = charges_.size() - 1;
    std::size_t index = (static_cast<std::size_t>(stream_id) * 2654435761u) & mask;
    for (std::size_t probe = 0; probe < charges_.size(); ++probe) {
        charge_entry& entry = charges_[index];
        if (!entry.used) {
            return nullptr;
        }
        if (entry.stream_id == stream_id) {
            return &entry;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

const flow_controller::charge_entry* flow_controller::find_charge(u32 stream_id) const noexcept {
    return const_cast<flow_controller*>(this)->find_charge(stream_id);
}

bool flow_controller::insert_charge(const charge_entry& entry) noexcept {
    if (charges_.empty()) {
        return false;
    }
    const std::size_t mask = charges_.size() - 1;
    std::size_t index = (static_cast<std::size_t>(entry.stream_id) * 2654435761u) & mask;
    for (std::size_t probe = 0; probe < charges_.size(); ++probe) {
        charge_entry& slot = charges_[index];
        if (slot.used && slot.stream_id == entry.stream_id) {
            slot = entry;
            return true;
        }
        if (!slot.used) {
            slot = entry;
            ++charge_count_;
            return true;
        }
        index = (index + 1) & mask;
    }
    // The table is full. Returning false is the only safe answer: charging the
    // bytes without an index entry would make them unreleasable, and the window
    // would eventually be permanently full.
    return false;
}

void flow_controller::erase_charge(u32 stream_id) noexcept {
    if (charges_.empty()) {
        return;
    }
    const std::size_t mask = charges_.size() - 1;
    std::size_t index = (static_cast<std::size_t>(stream_id) * 2654435761u) & mask;
    for (std::size_t probe = 0; probe < charges_.size(); ++probe) {
        charge_entry& entry = charges_[index];
        if (!entry.used) {
            return;
        }
        if (entry.stream_id == stream_id) {
            entry.used = false;
            if (charge_count_ > 0) {
                --charge_count_;
            }
            // Backward-shift deletion keeps every other probe chain intact.
            std::size_t hole = index;
            std::size_t next = (hole + 1) & mask;
            while (charges_[next].used) {
                const std::size_t ideal =
                    (static_cast<std::size_t>(charges_[next].stream_id) * 2654435761u) & mask;
                const bool movable =
                    (hole <= next) ? (ideal <= hole || ideal > next)
                                   : (ideal <= hole && ideal > next);
                if (movable) {
                    charges_[hole] = charges_[next];
                    charges_[next].used = false;
                    hole = next;
                }
                next = (next + 1) & mask;
            }
            return;
        }
        index = (index + 1) & mask;
    }
}

// --- admission ---------------------------------------------------------------

bool flow_controller::can_admit(u64 lane_id, std::size_t bytes) noexcept {
    if (!enabled_) {
        return true;
    }
    // The token bucket must recover as time passes, even when no call is
    // admitted: refilling only inside `on_admitted` means a drained bucket with
    // one long call in flight never regains credit and the next call is refused
    // forever.
    pacer_.refill(last_now_ns_);

    const u64 wanted = static_cast<u64>(bytes);
    if (options_.lane_quota_bytes != 0) {
        // The quota is checked for the empty lane too. Skipping the check while
        // `used == 0` (as an earlier version did) let one huge call through a
        // small quota, which is exactly what the quota exists to prevent.
        const u64 used = lane_bytes(lane_id);
        if (used + wanted > options_.lane_quota_bytes) {
            ++rejected_;
            return false;
        }
    }
    // Refuse before charging when the index table has no room: an unindexed
    // charge could never be released. Sizing makes this unreachable for a
    // correctly configured controller, but the check is what turns a
    // misconfiguration into a refusal instead of a leak.
    if (charge_count_ >= charges_.size()) {
        ++rejected_;
        return false;
    }
    const u64 burst =
        pacer_.rate_bytes_per_second() * options_.pacer.burst_ns / 1'000'000'000ull;
    const bool paced = wanted <= burst;
    if (in_flight_bytes_ == 0) {
        // An empty window admits one call unconditionally: refusing it would
        // mean the connection never learns anything about the path. This
        // bypasses the *window*, not the pacer -- a client that completes every
        // call immediately would otherwise send without any rate limit at all.
        // A call larger than the whole bucket is exempt from pacing too: it
        // cannot be split, so pacing it would refuse it forever.
        if (paced && pacer_.available() < wanted) {
            ++rejected_;
            return false;
        }
        return true;
    }
    if (in_flight_bytes_ + wanted > window_.bytes()) {
        ++rejected_;
        return false;
    }
    // Pacing shapes bursts; it never refuses a call that is larger than the
    // whole bucket, because such a call cannot be split into paced pieces.
    if (paced && pacer_.available() < wanted) {
        ++rejected_;
        return false;
    }
    return true;
}

bool flow_controller::on_admitted(u64 lane_id, std::size_t bytes, u32 stream_id,
                                  u32 generation) noexcept {
    if (!enabled_) {
        return true;
    }
    const u64 charged = static_cast<u64>(bytes);
    pacer_.refill(last_now_ns_);
    (void)pacer_.take(charged);
    charge_entry entry;
    entry.stream_id = stream_id;
    entry.generation = generation;
    entry.lane = lane_id;
    entry.bytes = charged;
    entry.used = true;
    // Insert before touching the counters: if the table cannot index this call,
    // nothing has been charged yet, so the caller can simply refuse it.
    if (!insert_charge(entry)) {
        return false;
    }
    in_flight_bytes_ += charged;
    lane_add(lane_id, charged);
    ++admitted_;
    return true;
}

void flow_controller::release(u32 stream_id, u32 generation, bool acknowledged) noexcept {
    if (!enabled_) {
        return;
    }
    charge_entry* entry = find_charge(stream_id);
    if (entry == nullptr || entry->generation != generation) {
        return;
    }
    const u64 bytes = entry->bytes;
    const u64 lane = entry->lane;
    erase_charge(stream_id);
    in_flight_bytes_ = in_flight_bytes_ > bytes ? in_flight_bytes_ - bytes : 0;
    lane_sub(lane, bytes);
    ++released_;
    // The window is self-clocked: it grows only on an acknowledgement. A
    // cancellation, a local timeout or a teardown returns the bytes but says
    // nothing about the path, so it must not grow the window.
    if (acknowledged) {
        window_.on_ack(bytes);
        refresh_pacer_rate();
    }
}

// --- probes ------------------------------------------------------------------

u64 flow_controller::begin_probe() noexcept {
    if (!enabled_) {
        return kNoTimestamp;
    }
    if (probe_outstanding_) {
        // A probe whose reply never arrives must not wedge the controller: once
        // it is older than the tracker's sample window its sample would be
        // discarded anyway, so it is re-armed. Without this a single lost PING
        // (or a PING the writer refused to stage) would stop every future probe
        // and the baseline would never update again.
        const u64 age = last_now_ns_ >= probe_sent_ns_ ? last_now_ns_ - probe_sent_ns_ : 0;
        if (age <= flow::rtt_tracker::max_sample_age_ns) {
            return kNoTimestamp;
        }
        probe_outstanding_ = false;
    }
    probe_outstanding_ = true;
    probe_token_ = tracker_.on_send(last_now_ns_);
    probe_sent_ns_ = last_now_ns_;
    ++probes_sent_;
    return probe_token_;
}

void flow_controller::abort_probe() noexcept {
    if (!enabled_ || !probe_outstanding_) {
        return;
    }
    // The PING never reached the writer queue: undo the reservation so the next
    // admission probes again. `probes_sent_` counts attempts and only grows, so
    // it is not rewound here; `probes_staged_` was never incremented, and the
    // attempt is booked as aborted. That keeps `staged + aborted == sent`.
    probe_outstanding_ = false;
    probe_token_ = 0;
    probe_sent_ns_ = 0;
    // The tracker holds exactly one outstanding token, so the abandoned one has
    // to be dropped too: otherwise the next `on_send` leaves the old token in
    // place and the following `on_ack` would compare against the wrong value.
    tracker_.reset();
    ++probes_aborted_;
}

void flow_controller::confirm_probe_staged() noexcept {
    if (!enabled_ || !probe_outstanding_) {
        return;
    }
    ++probes_staged_;
}

void flow_controller::on_probe_reply(u64 token_ns, u64 now_ns) noexcept {
    if (!enabled_) {
        return;
    }
    probe_outstanding_ = false;
    u64 sample = 0;
    if (!tracker_.on_ack(now_ns, token_ns, sample)) {
        return;
    }
    if (sample == 0) {
        return;
    }
    ++probe_samples_;

    const flow::rtt_estimator::config& cfg = estimator_.settings();
    const u64 baseline = estimator_.baseline_ns();

    if (sample >= baseline) {
        down_run_ = 0;
        down_max_ = 0;
        // The queue delay is measured against the *current* baseline, before
        // the sample is allowed to move it: a sample that is itself used to
        // raise the baseline would look like queueing on the next round
        // instead. A sample above the threshold is congestion evidence, not a
        // path measurement, so it is not fed to the estimator -- which is
        // exactly why a crushed baseline must never be possible.
        const u64 queue_delay = estimator_.queue_delay_ns(sample);
        if (queue_delay > theta_ns()) {
            ++samples_above_theta_;
            // Rate limit: at most one decrease per baseline. Without it a
            // burst of samples shrinks the window several times for one
            // congestion event.
            const bool due = (last_decrease_ns_ == kNoTimestamp) ||
                             (now_ns >= last_decrease_ns_ + baseline);
            if (due) {
                window_.on_congestion();
                last_decrease_ns_ = now_ns;
                ++congestion_events_;
                refresh_pacer_rate();
            }
            // Bottom escape: once the window sits at its floor, continued
            // congestion means the floor is wrong, not the path -- the
            // baseline is stale. After `slow_confirmations` consecutive
            // over-threshold samples the sample is accepted as the new
            // baseline so the loop can leave the floor instead of rejecting
            // every call forever.
            if (window_.bytes() <= options_.window.min_bytes) {
                ++bottom_hits_;
                if (bottom_hits_ >= cfg.slow_confirmations) {
                    bottom_hits_ = 0;
                    if (estimator_.on_sample(sample)) {
                        refresh_pacer_rate();
                    }
                }
            }
            return;
        }
        bottom_hits_ = 0;
        if (estimator_.on_sample(sample)) {
            refresh_pacer_rate();
        }
        return;
    }

    // A lower sample cannot be caused by queueing, but it can be a measurement
    // artifact. Confirm it, and accept the slowest sample of the window rather
    // than the fastest: a real path change produces a run of consistently low
    // samples, while an artifact is a lone spike that the max discards.
    //
    // Cost: `fast_confirmations` (3) self-clocked samples for a sharp drop and
    // `slow_confirmations` (4) for a small one -- the same shape, and the same
    // constant, as the upward path-change gate. The alternative (bounding each
    // step to `growth_per_mille`) would need ~60 samples for a 1000x drop.
    ++down_run_;
    if (down_max_ == 0 || sample > down_max_) {
        down_max_ = sample;
    }
    const u64 drop = baseline - sample;
    const u64 sharp = scale_per_mille(baseline, cfg.growth_per_mille);
    const u32 need = (drop >= sharp) ? cfg.fast_confirmations : cfg.slow_confirmations;
    if (down_run_ >= need) {
        const u64 accepted = down_max_;
        down_run_ = 0;
        down_max_ = 0;
        if (estimator_.on_sample(accepted)) {
            refresh_pacer_rate();
        }
    }
}

void flow_controller::reset() noexcept {
    estimator_.reset();
    window_.reset();
    tracker_.reset();
    in_flight_bytes_ = 0;
    last_decrease_ns_ = kNoTimestamp;
    bottom_hits_ = 0;
    down_run_ = 0;
    down_max_ = 0;
    probe_outstanding_ = false;
    probe_token_ = 0;
    probe_sent_ns_ = 0;
    charge_count_ = 0;
    for (lane_entry& entry : lanes_) {
        entry = lane_entry{};
    }
    for (charge_entry& entry : charges_) {
        entry = charge_entry{};
    }
    pacer_.reset(last_now_ns_);
    refresh_pacer_rate();
}

flow_state flow_controller::snapshot() const {
    flow_state out;
    out.enabled = enabled_;
    out.window_bytes = window_.bytes();
    out.in_flight_bytes = in_flight_bytes_;
    out.baseline_ns = estimator_.baseline_ns();
    out.theta_ns = theta_ns();
    out.pacer_rate_bytes_per_second = pacer_rate_bytes_per_second();
    out.pacer_credit_bytes = pacer_.available();
    out.admitted = admitted_;
    out.rejected = rejected_;
    out.released = released_;
    out.congestion_events = congestion_events_;
    out.probes_sent = probes_sent_;
    out.probes_staged = probes_staged_;
    out.probe_samples = probe_samples_;
    out.probes_aborted = probes_aborted_;
    out.samples_above_theta = samples_above_theta_;
    out.charges_in_use = charge_count_;
    out.charge_capacity = charges_.size();
    for (const lane_entry& entry : lanes_) {
        if (entry.used) {
            ++out.lanes_in_use;
        }
    }
    return out;
}

u64 flow_controller::lane_used(u64 lane_id) const noexcept { return lane_bytes(lane_id); }

} // namespace rpc
} // namespace crpc
