#include <crpc/resilience/circuit_breaker.hpp>

#include <chrono>

namespace crpc::resilience {

namespace {

constexpr u32 k_state_mask = 3;
constexpr u32 k_probes_shift = 2;
constexpr u64 k_probes_mask = (u64{1} << 14) - 1;
constexpr u32 k_successes_shift = 16;
constexpr u64 k_successes_mask = (u64{1} << 16) - 1;
constexpr u32 k_generation_shift = 32;

constexpr u64 k_bucket_locked = 1;
constexpr u64 k_bucket_ready = 2;

constexpr u64 encode_word(breaker_state state, u32 probes, u32 successes,
                          u32 generation) noexcept {
    return (static_cast<u64>(state) & k_state_mask) |
           ((static_cast<u64>(probes) & k_probes_mask) << k_probes_shift) |
           ((static_cast<u64>(successes) & k_successes_mask) << k_successes_shift) |
           (static_cast<u64>(generation) << k_generation_shift);
}

constexpr breaker_state state_of(u64 word) noexcept {
    return static_cast<breaker_state>(word & k_state_mask);
}

constexpr u32 probes_of(u64 word) noexcept {
    return static_cast<u32>((word >> k_probes_shift) & k_probes_mask);
}

constexpr u32 successes_of(u64 word) noexcept {
    return static_cast<u32>((word >> k_successes_shift) & k_successes_mask);
}

constexpr u32 generation_of(u64 word) noexcept {
    return static_cast<u32>(word >> k_generation_shift);
}

constexpr u64 with_probes(u64 word, u32 probes) noexcept {
    return (word & ~(k_probes_mask << k_probes_shift)) |
           ((static_cast<u64>(probes) & k_probes_mask) << k_probes_shift);
}

constexpr u64 with_successes(u64 word, u32 successes) noexcept {
    return (word & ~(k_successes_mask << k_successes_shift)) |
           ((static_cast<u64>(successes) & k_successes_mask) << k_successes_shift);
}

constexpr u64 bucket_word(u64 epoch, u64 flags) noexcept {
    return (epoch << 2) | flags;
}

constexpr std::chrono::nanoseconds clamp_non_negative(
    std::chrono::nanoseconds value) noexcept {
    return value.count() < 0 ? std::chrono::nanoseconds(0) : value;
}

u64 steady_now(void*) noexcept {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

// --- Clock -------------------------------------------------------------------

clock_source monotonic_clock() noexcept {
    return clock_source(&steady_now, nullptr);
}

// --- Permit ------------------------------------------------------------------

breaker_permit::breaker_permit(breaker_permit&& other) noexcept
    : breaker_(other.breaker_),
      generation_(other.generation_),
      start_ns_(other.start_ns_),
      slow_threshold_ns_(other.slow_threshold_ns_),
      probe_(other.probe_),
      recorded_(other.recorded_),
      code_(other.code_),
      retry_after_(other.retry_after_) {
    other.breaker_ = nullptr;
    other.recorded_ = true;
}

breaker_permit& breaker_permit::operator=(breaker_permit&& other) noexcept {
    if (this != &other) {
        release_unrecorded();
        breaker_ = other.breaker_;
        generation_ = other.generation_;
        start_ns_ = other.start_ns_;
        slow_threshold_ns_ = other.slow_threshold_ns_;
        probe_ = other.probe_;
        recorded_ = other.recorded_;
        code_ = other.code_;
        retry_after_ = other.retry_after_;
        other.breaker_ = nullptr;
        other.recorded_ = true;
    }
    return *this;
}

breaker_permit::~breaker_permit() {
    release_unrecorded();
}

void breaker_permit::release_unrecorded() noexcept {
    if (breaker_ != nullptr && !recorded_) {
        recorded_ = true;
        breaker_->record_permit(*this, breaker_outcome::ignored);
    }
    breaker_ = nullptr;
}

void breaker_permit::success() noexcept {
    if (breaker_ == nullptr || recorded_) {
        return;
    }
    const u64 now = breaker_->clock_.now_ns();
    const u64 elapsed = now > start_ns_ ? now - start_ns_ : 0;
    record(elapsed >= slow_threshold_ns_ ? breaker_outcome::slow
                                         : breaker_outcome::success);
}

void breaker_permit::failure() noexcept {
    record(breaker_outcome::failure);
}

void breaker_permit::slow() noexcept {
    record(breaker_outcome::slow);
}

void breaker_permit::ignore() noexcept {
    record(breaker_outcome::ignored);
}

void breaker_permit::record(breaker_outcome outcome) noexcept {
    if (breaker_ == nullptr || recorded_) {
        return;
    }
    recorded_ = true;
    breaker_->record_permit(*this, outcome);
}

// --- Breaker -----------------------------------------------------------------

circuit_breaker::circuit_breaker(const breaker_config& config, clock_source clock,
                                 state_change_fn on_change,
                                 void* on_change_context) noexcept
    : config_(config),
      clock_(clock),
      on_change_(on_change),
      on_change_context_(on_change_context) {
    u32 count = config_.buckets == 0 ? 1 : config_.buckets;
    if (count > k_max_buckets) {
        count = k_max_buckets;
    }
    bucket_count_ = count;

    const auto window_ns = config_.window.count();
    bucket_width_ns_ = window_ns > 0
                           ? static_cast<u64>(window_ns) / static_cast<u64>(count)
                           : 1;
    if (bucket_width_ns_ == 0) {
        bucket_width_ns_ = 1;
    }

    if (config_.min_requests == 0) {
        config_.min_requests = 1;
    }

    double failure_ratio = config_.failure_ratio_threshold;
    if (!(failure_ratio >= 0.0)) {
        failure_ratio = 0.0;
    }
    if (failure_ratio > 1.0) {
        failure_ratio = 1.0;
    }
    config_.failure_ratio_threshold = failure_ratio;

    double slow_ratio = config_.slow_call_ratio_threshold;
    if (!(slow_ratio >= 0.0)) {
        slow_ratio = 0.0;
    }
    if (slow_ratio > 1.0) {
        slow_ratio = 1.0;
    }
    config_.slow_call_ratio_threshold = slow_ratio;

    auto slow_ns = config_.slow_call_threshold.count();
    if (slow_ns <= 0) {
        const auto derived = config_.p99_target.count() * 5;
        slow_ns = derived > 1'000'000 ? derived : 1'000'000;
        config_.slow_call_threshold = std::chrono::nanoseconds(slow_ns);
    }
    slow_threshold_ns_ = static_cast<u64>(slow_ns);

    const auto open_ns = clamp_non_negative(config_.open_duration).count();
    config_.open_duration = std::chrono::nanoseconds(open_ns);
    open_duration_ns_ = static_cast<u64>(open_ns);

    u32 probes = config_.half_open_max_probes == 0 ? 1 : config_.half_open_max_probes;
    if (probes > static_cast<u32>(k_probes_mask)) {
        probes = static_cast<u32>(k_probes_mask);
    }
    max_probes_ = probes;
    config_.half_open_max_probes = probes;

    u32 successes = config_.half_open_successes_to_close == 0
                        ? 1
                        : config_.half_open_successes_to_close;
    if (successes > static_cast<u32>(k_successes_mask)) {
        successes = static_cast<u32>(k_successes_mask);
    }
    successes_to_close_ = successes;
    config_.half_open_successes_to_close = successes;
}

breaker_permit circuit_breaker::allow() noexcept {
    const u64 now = clock_.now_ns();
    u64 word = state_word_.load(std::memory_order_acquire);

    for (;;) {
        switch (state_of(word)) {
        case breaker_state::closed: {
            breaker_permit permit;
            permit.breaker_ = this;
            permit.generation_ = generation_of(word);
            permit.start_ns_ = now;
            permit.slow_threshold_ns_ = slow_threshold_ns_;
            permit.code_ = rpc_code::ok;
            return permit;
        }
        case breaker_state::open: {
            const u64 open_until = open_until_ns_.load(std::memory_order_acquire);
            if (now < open_until) {
                breaker_permit permit;
                const u64 remaining = open_until - now;
                permit.retry_after_ = std::chrono::milliseconds(
                    (remaining + 999'999) / 1'000'000);
                return permit;
            }

            // The open duration elapsed. The CAS winner reserves one probe and
            // publishes half-open; losers re-read the word.
            const u64 desired = encode_word(
                breaker_state::half_open, max_probes_ - 1, 0, generation_of(word) + 1);
            if (state_word_.compare_exchange_weak(word, desired,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                open_until_ns_.store(0, std::memory_order_release);
                notify(breaker_state::open, breaker_state::half_open);

                breaker_permit permit;
                permit.breaker_ = this;
                permit.generation_ = generation_of(desired);
                permit.start_ns_ = now;
                permit.slow_threshold_ns_ = slow_threshold_ns_;
                permit.probe_ = true;
                permit.code_ = rpc_code::ok;
                return permit;
            }
            break;
        }
        case breaker_state::half_open: {
            const u32 probes = probes_of(word);
            if (probes == 0) {
                return breaker_permit{};
            }
            const u64 desired = with_probes(word, probes - 1);
            if (state_word_.compare_exchange_weak(word, desired,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                breaker_permit permit;
                permit.breaker_ = this;
                permit.generation_ = generation_of(word);
                permit.start_ns_ = now;
                permit.slow_threshold_ns_ = slow_threshold_ns_;
                permit.probe_ = true;
                permit.code_ = rpc_code::ok;
                return permit;
            }
            break;
        }
        default:
            // Unknown state: fail closed. The permit's default code is
            // rpc_code::unavailable.
            return breaker_permit{};
        }
    }
}

breaker_state circuit_breaker::state() const noexcept {
    return state_of(state_word_.load(std::memory_order_acquire));
}

breaker_stats circuit_breaker::stats() const noexcept {
    const u64 now = clock_.now_ns();
    const u64 origin = window_origin_.load(std::memory_order_acquire);
    const u64 epoch = origin + now / bucket_width_ns_;
    const u64 span = bucket_count_ - 1;
    const u64 first = epoch > span ? epoch - span : 0;

    breaker_stats total;
    for (u64 current = first;; ++current) {
        const bucket& slot = buckets_[static_cast<std::size_t>(current % bucket_count_)];
        if (slot.state.load(std::memory_order_acquire) ==
            bucket_word(current, k_bucket_ready)) {
            total.success += slot.success.load(std::memory_order_relaxed);
            total.failure += slot.failure.load(std::memory_order_relaxed);
            total.slow += slot.slow.load(std::memory_order_relaxed);
        }
        if (current == epoch) {
            break;
        }
    }
    return total;
}

void circuit_breaker::reset() noexcept {
    u64 word = state_word_.load(std::memory_order_acquire);
    for (;;) {
        const breaker_state old_state = state_of(word);
        const u64 desired =
            encode_word(breaker_state::closed, 0, 0, generation_of(word) + 1);
        if (state_word_.compare_exchange_weak(word, desired, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            open_until_ns_.store(0, std::memory_order_release);
            reset_window();
            if (old_state != breaker_state::closed) {
                notify(old_state, breaker_state::closed);
            }
            return;
        }
    }
}

void circuit_breaker::record_permit(const breaker_permit& permit,
                                    breaker_outcome outcome) noexcept {
    const u64 word = state_word_.load(std::memory_order_acquire);
    if (permit.generation_ != generation_of(word)) {
        // A result from a previous generation. It must not influence the new
        // state; the transition that changed the generation owns the counters.
        return;
    }

    if (permit.probe_) {
        handle_probe(permit, outcome);
        return;
    }

    if (outcome == breaker_outcome::ignored) {
        return;
    }

    record_bucket(outcome);
    // Evaluate on every outcome: a success changes the denominator and can be
    // the record that makes a ratio threshold true.
    maybe_open();
}

void circuit_breaker::handle_probe(const breaker_permit& permit,
                                   breaker_outcome outcome) noexcept {
    if (outcome == breaker_outcome::failure || outcome == breaker_outcome::slow) {
        open_until_ns_.store(clock_.now_ns() + open_duration_ns_,
                             std::memory_order_release);
        transition_to(breaker_state::half_open, breaker_state::open, 0, 0);
        return;
    }

    if (outcome == breaker_outcome::ignored) {
        // Give the probe slot back so an abandoned probe cannot permanently
        // shrink the half-open probe budget.
        u64 word = state_word_.load(std::memory_order_acquire);
        for (;;) {
            if (state_of(word) != breaker_state::half_open ||
                generation_of(word) != permit.generation_) {
                return;
            }
            const u32 probes = probes_of(word);
            if (probes >= max_probes_) {
                return;
            }
            const u64 desired = with_probes(word, probes + 1);
            if (state_word_.compare_exchange_weak(word, desired,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                return;
            }
        }
    }

    // A success: close when enough consecutive probe successes accumulated,
    // otherwise release the probe slot for the next wave.
    u64 word = state_word_.load(std::memory_order_acquire);
    for (;;) {
        if (state_of(word) != breaker_state::half_open ||
            generation_of(word) != permit.generation_) {
            return;
        }
        const u32 successes = successes_of(word) + 1;
        if (successes >= successes_to_close_) {
            const u64 desired = encode_word(breaker_state::closed, 0, 0,
                                            generation_of(word) + 1);
            if (state_word_.compare_exchange_weak(word, desired,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                open_until_ns_.store(0, std::memory_order_release);
                reset_window();
                notify(breaker_state::half_open, breaker_state::closed);
                return;
            }
            continue;
        }

        u32 probes = probes_of(word);
        if (probes < max_probes_) {
            ++probes;
        }
        const u64 desired = with_successes(with_probes(word, probes), successes);
        if (state_word_.compare_exchange_weak(word, desired,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            return;
        }
    }
}

void circuit_breaker::record_bucket(breaker_outcome outcome) noexcept {
    const u64 now = clock_.now_ns();
    const u64 origin = window_origin_.load(std::memory_order_acquire);
    const u64 epoch = origin + now / bucket_width_ns_;
    bucket& slot = buckets_[static_cast<std::size_t>(epoch % bucket_count_)];
    if (!claim_bucket(slot, epoch)) {
        // A late record for an epoch whose slot already rotated forward. Drop
        // it instead of wiping the newer epoch's counters.
        return;
    }

    switch (outcome) {
    case breaker_outcome::success:
        slot.success.fetch_add(1, std::memory_order_relaxed);
        break;
    case breaker_outcome::failure:
        slot.failure.fetch_add(1, std::memory_order_relaxed);
        break;
    case breaker_outcome::slow:
        slot.slow.fetch_add(1, std::memory_order_relaxed);
        break;
    case breaker_outcome::ignored:
        break;
    }
}

void circuit_breaker::maybe_open() noexcept {
    const breaker_stats snapshot = stats();
    const u64 total = snapshot.total();
    if (total < config_.min_requests) {
        return;
    }

    const double denominator = static_cast<double>(total);
    const double failure_ratio = static_cast<double>(snapshot.failure) / denominator;
    const double slow_ratio = static_cast<double>(snapshot.slow) / denominator;
    if (failure_ratio < config_.failure_ratio_threshold &&
        slow_ratio < config_.slow_call_ratio_threshold) {
        return;
    }

    open_until_ns_.store(clock_.now_ns() + open_duration_ns_,
                         std::memory_order_release);
    transition_to(breaker_state::closed, breaker_state::open, 0, 0);
}

bool circuit_breaker::claim_bucket(bucket& slot, u64 epoch) noexcept {
    const u64 ready = bucket_word(epoch, k_bucket_ready);
    u64 current = slot.state.load(std::memory_order_acquire);
    for (;;) {
        if (current == ready) {
            return true;
        }
        if ((current & k_bucket_locked) != 0) {
            // Another thread is rotating this slot. The rotation is a handful
            // of stores, so a short spin is bounded.
            current = slot.state.load(std::memory_order_acquire);
            continue;
        }
        if ((current >> 2) > epoch) {
            // A newer epoch owns the slot; this record is too late to count.
            return false;
        }
        const u64 locked = bucket_word(epoch, k_bucket_locked);
        if (slot.state.compare_exchange_weak(current, locked,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            slot.success.store(0, std::memory_order_relaxed);
            slot.failure.store(0, std::memory_order_relaxed);
            slot.slow.store(0, std::memory_order_relaxed);
            slot.state.store(ready, std::memory_order_release);
            return true;
        }
    }
}

void circuit_breaker::reset_window() noexcept {
    // Move the window base far enough that every epoch still represented in
    // the ring is older than the new window. Without the full span jump an
    // immediate reset could leave old counters inside the new window.
    const u64 now = clock_.now_ns();
    const u64 elapsed_epochs = now / bucket_width_ns_;
    const u64 origin = window_origin_.load(std::memory_order_acquire);
    const u64 current_epoch = origin + elapsed_epochs;
    const u64 next = current_epoch + bucket_count_ - elapsed_epochs;
    window_origin_.store(next, std::memory_order_release);
}

bool circuit_breaker::transition_to(breaker_state expected, breaker_state next,
                                    u32 probes, u32 successes) noexcept {
    u64 word = state_word_.load(std::memory_order_acquire);
    for (;;) {
        if (state_of(word) != expected) {
            return false;
        }
        const u64 desired =
            encode_word(next, probes, successes, generation_of(word) + 1);
        if (state_word_.compare_exchange_weak(word, desired,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            if (next != breaker_state::open) {
                open_until_ns_.store(0, std::memory_order_release);
            }
            if (next == breaker_state::closed) {
                reset_window();
            }
            notify(expected, next);
            return true;
        }
    }
}

void circuit_breaker::notify(breaker_state old_state, breaker_state new_state) noexcept {
    if (on_change_ != nullptr) {
        on_change_(on_change_context_, old_state, new_state);
    }
}

} // namespace crpc::resilience
