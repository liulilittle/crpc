// CRPC -- trace context, W3C traceparent and baggage.
//
// This header is the single source of truth for the identifiers that travel
// with a call. Three rules shape the types below:
//
//   * identifiers are fixed-size byte arrays, never strings: a trace id that
//     allocates on the send path would defeat the whole observability budget,
//   * the context is trivially copyable so it can be copied into a frame, a
//     log record or a coroutine frame without an allocation,
//   * deadlines are relative durations. An absolute wall-clock deadline cannot
//     survive a hop between machines whose clocks differ; a relative one is
//     decremented locally and is therefore safe to propagate.
//
// Thread safety: every type here is a value type. Copies are independent and
// safe to use from any thread; no function mutates shared state.
#ifndef CRPC_OBSERVABILITY_CONTEXT_HPP
#define CRPC_OBSERVABILITY_CONTEXT_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <crpc/config.hpp>

namespace crpc {
namespace obs {

// --- Identifiers -------------------------------------------------------------

// 16-byte trace identifier. The all-zero value is invalid (W3C trace context
// rule) and `valid()` enforces it.
struct trace_id {
    std::array<std::byte, 16> bytes{};

    [[nodiscard]] bool valid() const noexcept {
        for (const std::byte b : bytes) {
            if (b != std::byte{0}) {
                return true;
            }
        }
        return false;
    }

    friend bool operator==(const trace_id& lhs, const trace_id& rhs) noexcept {
        return lhs.bytes == rhs.bytes;
    }

    friend bool operator!=(const trace_id& lhs, const trace_id& rhs) noexcept {
        return !(lhs == rhs);
    }
};

// 8-byte span identifier. The all-zero value is invalid.
struct span_id {
    std::array<std::byte, 8> bytes{};

    [[nodiscard]] bool valid() const noexcept {
        for (const std::byte b : bytes) {
            if (b != std::byte{0}) {
                return true;
            }
        }
        return false;
    }

    friend bool operator==(const span_id& lhs, const span_id& rhs) noexcept {
        return lhs.bytes == rhs.bytes;
    }

    friend bool operator!=(const span_id& lhs, const span_id& rhs) noexcept {
        return !(lhs == rhs);
    }
};

// The context carried by a call. `parent` is zero for a root span. `flags`
// uses the W3C bit layout; bit 0 is the sampled bit and every other bit is
// preserved so that future flags survive a hop.
//
// Allocation: none. Trivially copyable.
struct trace_context {
    static constexpr u8 sampled_flag = 0x01;

    trace_id trace;
    span_id span;
    span_id parent;
    u8 flags = 0;

    [[nodiscard]] bool sampled() const noexcept {
        return (flags & sampled_flag) != 0;
    }

    void set_sampled(bool value) noexcept {
        if (value) {
            flags = static_cast<u8>(flags | sampled_flag);
        } else {
            flags = static_cast<u8>(flags & static_cast<u8>(~sampled_flag));
        }
    }

    [[nodiscard]] bool valid() const noexcept { return trace.valid() && span.valid(); }
};

// --- W3C traceparent ---------------------------------------------------------

enum class traceparent_status : u8 {
    ok = 0,
    invalid_length,
    invalid_format,
    invalid_version,
    invalid_hex,
    invalid_trace_id,
    invalid_span_id
};

[[nodiscard]] CRPC_API const char* to_string(traceparent_status status) noexcept;

struct traceparent_result {
    traceparent_status status = traceparent_status::invalid_length;
    trace_context context;

    [[nodiscard]] bool ok() const noexcept { return status == traceparent_status::ok; }
};

// Parses `00-<32 lowercase hex>-<16 lowercase hex>-<2 lowercase hex>`.
//
// The accepted grammar is deliberately strict:
//   * exactly 55 bytes,
//   * lowercase hexadecimal only (W3C requires lowercase),
//   * version `ff` is invalid; versions `00`..`fe` are accepted so that a
//     future version with the same 55-byte shape still propagates,
//   * an all-zero trace id or span id is rejected.
//
// Allocation: none. No exceptions.
[[nodiscard]] CRPC_API traceparent_result parse_traceparent(std::string_view text) noexcept;

// Writes the version-`00` form into `out` and returns the number of bytes
// written (always 55). Returns 0 when the buffer is too small or either id is
// invalid. A terminating NUL is written when `capacity > 55`; use the returned
// length rather than relying on it.
//
// Allocation: none. No exceptions.
[[nodiscard]] CRPC_API std::size_t format_traceparent(const trace_context& context, char* out,
                                             std::size_t capacity) noexcept;

// --- Deadline ----------------------------------------------------------------

// A deadline is a *relative* duration. It is decremented as the call advances
// and is never converted to an absolute wall-clock time: two hosts with skewed
// clocks still agree on "500 ms left".
//
// Allocation: none. Trivially copyable.
struct deadline {
    std::chrono::nanoseconds remaining{0};
    bool infinite = true;

    [[nodiscard]] static deadline infinite_deadline() noexcept { return deadline{}; }

    [[nodiscard]] static deadline after(std::chrono::nanoseconds duration) noexcept {
        deadline value;
        value.remaining = duration;
        value.infinite = false;
        return value;
    }

    [[nodiscard]] bool has_deadline() const noexcept { return !infinite; }
    [[nodiscard]] bool expired() const noexcept { return !infinite && remaining.count() <= 0; }

    // Subtracts local elapsed time. Values below zero are clamped so a late
    // wake-up cannot wrap the duration into a future deadline.
    void consume(std::chrono::nanoseconds elapsed) noexcept {
        if (infinite || elapsed.count() <= 0) {
            return;
        }
        remaining -= elapsed;
        if (remaining.count() < 0) {
            remaining = std::chrono::nanoseconds{0};
        }
    }
};

// --- Baggage -----------------------------------------------------------------

// Ordered, bounded key/value list.
//
// The storage is an inline 8 KiB arena plus a fixed descriptor table, so a
// baggage copy never allocates. Insertion is rejected when the entry count or
// the byte budget is exhausted; rejection is never silent: `truncated()` turns
// true and the dropped counters record the loss.
//
// Thread safety: value type, not synchronised. Allocation: none, ever.
class CRPC_API baggage {
public:
    static constexpr std::size_t max_entries = 32;
    static constexpr std::size_t max_bytes = 8u * 1024u;
    static constexpr std::size_t max_key_size = 128;
    static constexpr std::size_t max_value_size = 4096;

    struct entry_view {
        std::string_view key;
        std::string_view value;
    };

    baggage() noexcept = default;

    // Inserts or replaces a key. Returns false (and records the truncation)
    // when the key/value is too long or the bounded storage is full.
    [[nodiscard]] bool set(std::string_view key, std::string_view value) noexcept;

    // Returns nullptr when absent. When `length` is non-null it receives the
    // value length. The returned pointer is valid until the next mutation.
    [[nodiscard]] const char* get(std::string_view key,
                                  std::size_t* length = nullptr) const noexcept;

    [[nodiscard]] bool remove(std::string_view key) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] std::size_t bytes_used() const noexcept { return data_size_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

    // True once at least one insertion was rejected.
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }
    [[nodiscard]] std::size_t dropped_entries() const noexcept { return dropped_entries_; }
    [[nodiscard]] std::size_t dropped_bytes() const noexcept { return dropped_bytes_; }

    // Precondition: index < size(). No bounds check in release builds.
    [[nodiscard]] entry_view at(std::size_t index) const noexcept;

private:
    struct entry {
        u16 key_offset;
        u16 key_len;
        u16 value_offset;
        u16 value_len;
    };

    [[nodiscard]] std::size_t find_index(std::string_view key) const noexcept;
    bool append(std::string_view key, std::string_view value) noexcept;
    void remove_at(std::size_t index) noexcept;

    std::array<char, max_bytes> data_{};
    std::array<entry, max_entries> entries_{};
    std::size_t count_ = 0;
    std::size_t data_size_ = 0;
    bool truncated_ = false;
    std::size_t dropped_entries_ = 0;
    std::size_t dropped_bytes_ = 0;
};

} // namespace obs

// Compatibility alias for callers that spell the module name out.
namespace observability = obs;

} // namespace crpc

#endif // CRPC_OBSERVABILITY_CONTEXT_HPP
