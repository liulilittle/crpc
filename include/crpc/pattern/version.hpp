// CRPC -- version pattern: an optimistically-concurrency-controlled entity.
//
// `versioned<T>` wraps a value with a monotonic version counter. The read
// returns both the value and the version it was read at; a write is a
// compare-and-swap: it commits only when the caller's expected version still
// matches the current one, and advances the version on success. A write based
// on a stale version is refused with `rpc_code::failed_precondition` -- the
// application-layer analogue of `UPDATE ... WHERE version = ?` affecting zero
// rows.
//
// Conflicts are retryable, but only by the version loop: the generic retry
// machinery must not blindly retry them (a conflict is not a transient network
// failure). `update()` reuses `crpc::resilience::retry_sequence` for the
// attempt cap and exponential backoff; it re-reads the record before every
// attempt and re-applies the mutator against the freshly-read value.
//
// Thread safety: read/write_if/update are safe for concurrent use. All mutation
// is serialised by an internal mutex; the version counter is checked under that
// same mutex, so the CAS is atomic. T must be copy-constructible (read returns
// a copy) and copy/move-assignable (write commits a fresh value).
#ifndef CRPC_PATTERN_VERSION_HPP
#define CRPC_PATTERN_VERSION_HPP

#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/resilience/retry.hpp>

namespace crpc {
namespace pattern {

// Builds the conflict error for a CAS write whose expected version no longer
// matches the record's current version. `retryable` stays false: a version
// conflict is not a transient transport failure, so generic retry logic must
// not touch it -- only the version pattern's own retry loop may.
[[nodiscard]] CRPC_API rpc_error make_version_conflict(u64 expected,
                                                       u64 actual) noexcept;

// The retry policy used by `update()` when the caller does not supply one:
// three attempts with the standard exponential backoff, retrying exactly
// `failed_precondition` conflicts (and nothing else).
[[nodiscard]] CRPC_API crpc::resilience::retry_policy default_version_policy() noexcept;

// Outcome of a successful `update()`.
struct version_write {
    // The committed version after the successful CAS.
    u64 version = 0;
    // How many read/modify/write attempts the loop performed in total.
    u32 attempts = 0;
    // True when at least one attempt hit a version conflict and was retried.
    bool saw_conflict = false;
};

template <typename T>
class versioned {
public:
    // One atomic read: the value plus the version it was read at.
    struct record {
        u64 version = 0;
        T value;
    };

    explicit versioned(T value) : value_(std::move(value)) {}

    versioned(const versioned&) = delete;
    versioned& operator=(const versioned&) = delete;

    // Snapshots the current value (a deep copy) together with its version.
    [[nodiscard]] record read() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return record{version_, value_};
    }

    // The current version without the payload.
    [[nodiscard]] u64 current_version() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return version_;
    }

    // Optimistic compare-and-swap. Commits `value` only when `expected` still
    // equals the record version; on success the version advances by one and the
    // new version is returned. On a stale `expected` no state changes and the
    // failure is `rpc_code::failed_precondition`.
    [[nodiscard]] result<u64> write_if(u64 expected, T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (version_ != expected) {
            return make_version_conflict(expected, version_);
        }
        ++version_;
        value_ = std::move(value);
        return version_;
    }

    // Read-modify-write with automatic conflict retry. The mutator receives the
    // value read for this attempt and returns the replacement. On a conflict a
    // fresh read is taken and the mutator is re-applied, up to `policy`
    // attempts with `policy` backoff (handled by `resilience::retry_sequence`).
    //
    // The mutator runs once per attempt and may consult the versioned entity
    // itself; it is expected to be pure over its input, because the loop
    // discards its output whenever the attempt conflicts.
    template <typename Mutator>
    [[nodiscard]] result<version_write> update(
        Mutator&& mutator,
        const crpc::resilience::retry_policy& policy = default_version_policy()) {
        crpc::resilience::retry_sequence sequence(policy);
        version_write outcome;
        for (;;) {
            const record current = read();
            T next = mutator(current.value);
            result<u64> write = write_if(current.version, std::move(next));
            ++outcome.attempts;
            if (write.ok()) {
                outcome.version = write.value();
                return outcome;
            }
            outcome.saw_conflict = true;

            std::chrono::nanoseconds delay{0};
            if (!sequence.next(write.code(), /* idempotent = */ true, delay)) {
                rpc_error error(write.error());
                error.attempts = outcome.attempts;
                return error;
            }
            if (delay.count() > 0) {
                std::this_thread::sleep_for(delay);
            }
        }
    }

private:
    mutable std::mutex mutex_;
    u64 version_ = 0;
    T value_;
};

} // namespace pattern
} // namespace crpc

#endif // CRPC_PATTERN_VERSION_HPP