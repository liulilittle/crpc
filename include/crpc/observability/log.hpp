// CRPC -- high-performance asynchronous logger.
//
// The design separates the caller's hot path from the sink:
//
//   * The caller formats into a fixed stack buffer with `{}` placeholders. The
//     formatting code never allocates, never locks and never enters the kernel.
//     `make_record` reads the wall clock once (for the record timestamp).
//   * The resulting fixed-size `log_record` is copied into a per-thread
//     single-producer/single-consumer ring. The ring is wait-free in the common
//     case: one acquire load of the size, one store of the record, one release
//     increment.
//   * One background thread drains every registered ring in batches, formats
//     text lines and writes them with `WriteFile` (Windows) or `writev`
//     (POSIX). It also rotates files by size and by time.
//
// The only allocation on a thread's first log call is its ring (capacity is
// configurable, default 8192 records). After that registration a log call has
// no allocation. `logger::shutdown()` drains and joins; it is idempotent.
//
// Cost of an enabled record: formatting into a 128-byte stack buffer, one
// 512-byte record copy into the ring, two atomic operations. Cost of a disabled
// record: one atomic level load and a compare. Cost of a record above
// CRPC_LOG_MAX_LEVEL: nothing at all -- the macro expands to an empty statement.
//
// Thread safety: `logger::instance()` is the only documented mutable singleton
// in this module. `submit()` is safe from any thread. `configure()`, `start()`,
// `shutdown()` and `reset()` are lifecycle operations: call them from one
// thread, and call `reset()` only when no other thread is logging.
#ifndef CRPC_OBSERVABILITY_LOG_HPP
#define CRPC_OBSERVABILITY_LOG_HPP

#include <array>
#include <atomic>
#include <cfenv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// MXCSR is the authoritative rounding-direction register for the SSE2 double
// arithmetic on x86/x64; the intrinsic is one instruction, while UCRT's
// fegetround() costs ~12 ns per call (measured: .opencode/tmp/opt-float2).
#if (defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__)) &&                   \
    (defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) ||                     \
     (defined(__i386__) && defined(__SSE__)))
#include <xmmintrin.h>
#endif

#include <crpc/config.hpp>
#include <crpc/observability/context.hpp>

// Compile-time verbosity ceiling. Values:
//   0 trace, 1 debug, 2 info, 3 warn, 4 error, 5 off.
// A level is compiled in when CRPC_LOG_MAX_LEVEL <= its numeric value, so the
// default (0) compiles every level in and 5 (or the token `off`) compiles every
// level out.
#ifndef CRPC_LOG_MAX_LEVEL
#define CRPC_LOG_MAX_LEVEL 0
#endif

namespace crpc {
namespace obs {

// --- Levels ------------------------------------------------------------------

enum class log_level : u8 { trace = 0, debug, info, warn, error, off };

namespace detail {

// Token accepted by CRPC_LOG_MAX_LEVEL for "compile every level out".
// Numeric 0..5 is equally valid; 5 has the same effect as `off`. The name
// lives in `detail` because only `compile_max_level()` expands the macro.
constexpr int off = 5;

constexpr int compile_max_level() noexcept { return CRPC_LOG_MAX_LEVEL; }

constexpr bool compile_enabled(int level_number) noexcept {
    return compile_max_level() < static_cast<int>(log_level::off) &&
           compile_max_level() <= level_number;
}

// --- %.6g double formatter ---------------------------------------------------
//
// Contract: byte-identical to snprintf("%.6g", value) for every double on this
// platform, in the SAME floating-point rounding mode. The differential suite in
// test_log_perf.cpp pins the contract against the CRT itself (explicit boundary
// table + >=1e6 fuzzed doubles in the default mode, plus 4 fenv modes x 100k),
// so any deviation is a test failure, not a silent output change.
//
// Algorithm (self-written; no third-party code): fixed-precision decimal
// generation in the spirit of Dragon4 (Steele & White, "How to Print
// Floating-Point Numbers Accurately", PLDI 1990, fixed-precision mode) with a
// Grisu-style fast/slow split (Loitsch 2010's uncertainty-window idea):
//
//   1. The exact value is |v| = m * 2^e with m a 53-bit integer. Writing
//      log10|v| = (e+52)*log10(2) + log10(mu) with mu in [1,2) (normals), the
//      decade k = floor(log10|v|) is estimated from the first term alone; the
//      estimate errs by at most one decade for normals and at most ~16 for
//      subnormals, and a bounded correction loop (multiplying/dividing x by
//      exactly 10) puts x = |v| * 10^(5-k) into [1e5, 1e6).
//   2. x is computed in double arithmetic from a split power-of-ten table;
//      the accumulated float error stays below ~1e-14 relative (< 1e-8
//      absolute on x in [1e5, 1e6)). The 7th-digit remainder frac(x)
//      therefore decides the rounding exactly whenever it is farther than
//      1e-6 from {0, 1/2, 1} -- which is to say, whenever snprintf's own
//      decimal rounding is not at (or within a hair of) a round-half-even
//      tie.
//   3. Every input the estimate cannot decide -- exact/near ties, carries
//      within the window of an integer, non-finite spellings (the CRT prints
//      "nan", "-nan(ind)" and "nan(snan)" depending on payload and sign) --
//      is dispatched back to snprintf. The window makes byte-identity total
//      by construction; it costs one CRT call for ~4e-6 of random doubles.
//
// The %f/%e selection uses the exponent AFTER rounding (C %g rule: X in
// [-4, P-1] selects %f; 999999.5 -> 1e+06, 9.999995e-5 -> 0.0001). The
// platform probe (.opencode/tmp/opt-float/probe_snprintf.cpp) pins the
// remaining syntax: at least two exponent digits ("1e-05", "1e+06", three
// from |X| >= 100), trailing fractional zeros trimmed, "-0" for negative
// zero.

// 10^i for i in [-165, 165], split as 10^(16h) * 10^j so that no table entry
// accumulates more than ~12 roundings (~1.3e-15 relative) and every factor
// used below stays inside the normal double range.
struct g6_pow10_table {
    double value[331];
};

constexpr int g6_pow10_bias = 165;

constexpr g6_pow10_table make_g6_pow10_table() {
    g6_pow10_table table{};
    double smalls[16] = {};
    smalls[0] = 1.0;
    for (int j = 1; j < 16; ++j) {
        smalls[j] = smalls[j - 1] * 10.0;  // 10^j <= 1e15: exact
    }
    double bigs[11] = {};
    bigs[0] = 1.0;
    for (int h = 1; h <= 10; ++h) {
        bigs[h] = bigs[h - 1] * 1e16;  // 1e16 is exact; <= 10 roundings total
    }
    for (int i = 0; i <= 165; ++i) {
        table.value[g6_pow10_bias + i] = bigs[i / 16] * smalls[i % 16];
    }
    for (int i = 1; i <= 165; ++i) {
        table.value[g6_pow10_bias - i] = 1.0 / table.value[g6_pow10_bias + i];
    }
    return table;
}

inline constexpr g6_pow10_table g_pow10_g6 = make_g6_pow10_table();

// Emits `value` zero-padded on the left to exactly `width` digits when width
// exceeds the digit count; `trim` drops TRAILING zeros of the emitted string
// (the C %g rule trims the fractional part). Returns the character count.
inline std::size_t g6_emit_u32(char* out, std::uint32_t value, int width, bool trim) noexcept {
    char reversed[10] = {};
    std::size_t count = 0;
    do {
        reversed[count++] = static_cast<char>('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    while (count < static_cast<std::size_t>(width)) {
        reversed[count++] = '0';  // padding lands on the high-order side
    }
    std::size_t low = 0;
    if (trim) {
        // Trailing zeros of the final big-endian string are the FRONT
        // elements of this little-endian buffer; the zero padding sits on the
        // other side and survives the trim.
        while (low < count && reversed[low] == '0') {
            ++low;
        }
    }
    std::size_t written = 0;
    for (std::size_t i = count; i > low; --i) {
        out[written++] = reversed[i - 1];
    }
    return written;
}

inline std::size_t g6_snprintf_fallback(char* out, std::size_t capacity, double value) noexcept {
    const int written = std::snprintf(out, capacity, "%.6g", value);
    if (written <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(written) < capacity ? static_cast<std::size_t>(written)
                                                        : capacity - 1;
}

// The fast path below decides one half-even rounding of the 7th significant
// digit. snprintf only behaves that way in FE_TONEAREST: under FE_UPWARD,
// FE_DOWNWARD or FE_TOWARDZERO it rounds every discarded digit in the current
// direction instead (probe: opt-float2/probe_fenv.cpp -- e.g. 123456.5 prints
// "123457" upward, and the ungated fast path disagreed with the CRT on ~50% of
// random values in each directed mode). Those modes therefore delegate to the
// CRT. The register is read directly because UCRT fegetround() costs ~12 ns,
// more than the fast path itself; MXCSR RC bits 13:14 are 00 exactly for
// round-to-nearest. Non-x86 targets use the portable query.
inline bool g6_rounding_is_nearest() noexcept {
#if (defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__)) &&                   \
    (defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) ||                     \
     (defined(__i386__) && defined(__SSE__)))
    return (_mm_getcsr() & 0x6000) == 0;
#else
    return std::fegetround() == FE_TONEAREST;
#endif
}

inline std::size_t format_double_g6(char* out, std::size_t capacity, double value) noexcept {
    if (capacity == 0) {
        return 0;
    }

    if (!g6_rounding_is_nearest()) {
        return g6_snprintf_fallback(out, capacity, value);
    }

    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint64_t magnitude_bits = bits & 0x7FFFFFFFFFFFFFFFull;

    // Zero and non-finite inputs: "-0"/"0" and the payload-dependent NaN
    // spellings are handed to the CRT; neither class is hot.
    if (magnitude_bits == 0 || magnitude_bits >= 0x7FF0000000000000ull) {
        return g6_snprintf_fallback(out, capacity, value);
    }

    const int biased = static_cast<int>(magnitude_bits >> 52);
    std::uint64_t mantissa = magnitude_bits & 0x000FFFFFFFFFFFFFull;
    int binary_exponent;
    if (biased == 0) {
        binary_exponent = -1074;  // subnormal: |v| = mantissa * 2^-1074
    } else {
        mantissa |= 0x0010000000000000ull;
        binary_exponent = biased - 1075;  // normal: |v| = mantissa * 2^e
    }

    // Decade estimate: floor((e+52)*log10(2)). Normals land in {k-1, k};
    // subnormals in [k, k+16]. The correction loop fixes either direction.
    const double decade_estimate =
        static_cast<double>(binary_exponent + 52) * 0.3010299956639812;
    int decade = static_cast<int>(decade_estimate);
    if (decade_estimate < 0.0 && static_cast<double>(decade) != decade_estimate) {
        --decade;  // floor, not truncation, for negative estimates
    }

    // x = |v| * 10^(5-decade), aimed at [1e5, 1e6). The power of ten is split
    // into two table factors; every intermediate product stays within the
    // normal double range for every reachable decade.
    const int e10 = 5 - decade;
    const int half_a = e10 >= 0 ? e10 / 2 : -((-e10) / 2);
    const int half_b = e10 - half_a;
    double magnitude;
    std::memcpy(&magnitude, &magnitude_bits, sizeof(magnitude));
    double x = magnitude * g_pow10_g6.value[g6_pow10_bias + half_a] *
               g_pow10_g6.value[g6_pow10_bias + half_b];

    // Band correction by exact factors of ten (<= ~19 steps by construction;
    // the guard is a proof-backed safety net that falls back, never guesses).
    for (int guard = 0; guard < 40; ++guard) {
        if (x >= 1e6) {
            x /= 10.0;
            ++decade;
        } else if (x < 1e5) {
            x *= 10.0;
            --decade;
        } else {
            break;
        }
    }
    if (!(x >= 1e5) || !(x < 1e6)) {
        return g6_snprintf_fallback(out, capacity, value);
    }

    const std::uint32_t integral = static_cast<std::uint32_t>(x);
    const double remainder = x - static_cast<double>(integral);  // exact (Sterbenz)

    // Rounding window: within 1e-6 of {0, 1/2, 1} the estimate (error < 1e-8)
    // cannot decide the 7th-digit rounding; dispatch to the CRT.
    constexpr double g6_window = 1e-6;
    if (remainder < g6_window || 1.0 - remainder < g6_window ||
        (remainder > 0.5 - g6_window && remainder < 0.5 + g6_window)) {
        return g6_snprintf_fallback(out, capacity, value);
    }

    const std::uint32_t rounded = integral + (remainder > 0.5 ? 1u : 0u);
    // A carry out of the 6th digit bumps the exponent (999999.5 -> 1e+06).
    std::uint32_t digits;
    int decimal_exponent;
    if (rounded == 1000000u) {
        digits = 100000u;
        decimal_exponent = decade + 1;
    } else {
        digits = rounded;
        decimal_exponent = decade;
    }

    char text[32];
    std::size_t length = 0;
    if ((bits >> 63) != 0) {
        text[length++] = '-';
    }

    if (decimal_exponent >= -4 && decimal_exponent <= 5) {
        // %f style: -4 <= X_after <= 5.
        if (decimal_exponent == 5) {
            length += g6_emit_u32(text + length, digits, 6, false);
        } else if (decimal_exponent >= 0) {
            static const std::uint32_t g6_scales[6] = {1u, 10u, 100u, 1000u, 10000u, 100000u};
            const int fraction_length = 5 - decimal_exponent;
            const std::uint32_t whole = digits / g6_scales[fraction_length];
            const std::uint32_t fraction = digits % g6_scales[fraction_length];
            length += g6_emit_u32(text + length, whole, 0, false);
            if (fraction != 0) {
                text[length++] = '.';
                length += g6_emit_u32(text + length, fraction, fraction_length, true);
            }
        } else {
            text[length++] = '0';
            text[length++] = '.';
            for (int i = 0; i < -1 - decimal_exponent; ++i) {
                text[length++] = '0';
            }
            length += g6_emit_u32(text + length, digits, 6, true);
        }
    } else {
        // %e style: d.ddddd e±XX (two exponent digits minimum, three from 100).
        const std::uint32_t leading = digits / 100000u;
        const std::uint32_t tail = digits % 100000u;
        length += g6_emit_u32(text + length, leading, 0, false);
        if (tail != 0) {
            text[length++] = '.';
            length += g6_emit_u32(text + length, tail, 5, true);
        }
        text[length++] = 'e';
        int exponent = decimal_exponent;
        if (exponent < 0) {
            text[length++] = '-';
            exponent = -exponent;
        } else {
            text[length++] = '+';
        }
        if (exponent >= 100) {
            text[length++] = static_cast<char>('0' + exponent / 100);
            text[length++] = static_cast<char>('0' + (exponent / 10) % 10);
            text[length++] = static_cast<char>('0' + exponent % 10);
        } else {
            text[length++] = static_cast<char>('0' + exponent / 10);
            text[length++] = static_cast<char>('0' + exponent % 10);
        }
    }

    const std::size_t count = length < capacity ? length : capacity - 1;
    std::memcpy(out, text, count);
    out[count] = '\0';
    return count;
}

} // namespace detail

[[nodiscard]] inline const char* to_string(log_level level) noexcept {
    switch (level) {
    case log_level::trace:
        return "TRACE";
    case log_level::debug:
        return "DEBUG";
    case log_level::info:
        return "INFO";
    case log_level::warn:
        return "WARN";
    case log_level::error:
        return "ERROR";
    case log_level::off:
        return "OFF";
    }
    return "OFF";
}

// --- Bounded stack formatter -------------------------------------------------

// A writer over a caller-supplied fixed buffer. When the buffer is full the
// excess is discarded and `truncated()` becomes true; the writer never
// allocates and never writes past the end.
class log_buffer {
public:
    log_buffer(char* out, std::size_t capacity) noexcept
        : out_(out), capacity_(capacity), size_(0), truncated_(false) {}

    void append(const char* data, std::size_t count) noexcept {
        if (data == nullptr || count == 0) {
            return;
        }
        if (count > capacity_ - size_) {
            count = capacity_ - size_;
            truncated_ = true;
        }
        std::memcpy(out_ + size_, data, count);
        size_ += count;
    }

    void append(std::string_view text) noexcept { append(text.data(), text.size()); }

    void append(char value) noexcept {
        if (size_ == capacity_) {
            truncated_ = true;
            return;
        }
        out_[size_] = value;
        ++size_;
    }

    void append_integer(std::int64_t value) noexcept {
        using unsigned_type = std::uint64_t;
        unsigned_type magnitude;
        bool negative = false;
        if (value < 0) {
            negative = true;
            magnitude = static_cast<unsigned_type>(0) - static_cast<unsigned_type>(value);
        } else {
            magnitude = static_cast<unsigned_type>(value);
        }
        append_unsigned(magnitude, negative);
    }

    void append_integer(std::uint64_t value) noexcept { append_unsigned(value, false); }

    template <typename T,
              std::enable_if_t<std::is_integral<T>::value &&
                                   !std::is_same<T, std::int64_t>::value &&
                                   !std::is_same<T, std::uint64_t>::value,
                               int> = 0>
    void append_integer(T value) noexcept {
        if constexpr (std::is_signed<T>::value) {
            append_integer(static_cast<std::int64_t>(value));
        } else {
            append_integer(static_cast<std::uint64_t>(value));
        }
    }

    void append_float(double value) noexcept {
        char text[40];
        const std::size_t count = detail::format_double_g6(text, sizeof(text), value);
        append(text, count);
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }
    [[nodiscard]] const char* data() const noexcept { return out_; }

    void clear() noexcept {
        size_ = 0;
        truncated_ = false;
    }

private:
    void append_unsigned(std::uint64_t value, bool negative) noexcept {
        char digits[24];
        std::size_t count = 0;
        do {
            digits[count] = static_cast<char>('0' + (value % 10));
            ++count;
            value /= 10;
        } while (value != 0);
        if (negative) {
            append('-');
        }
        while (count != 0) {
            --count;
            append(digits[count]);
        }
    }

    char* out_;
    std::size_t capacity_;
    std::size_t size_;
    bool truncated_;
};

inline void append_value(log_buffer& out, std::string_view value) noexcept {
    out.append(value);
}

inline void append_value(log_buffer& out, const std::string& value) noexcept {
    out.append(value.data(), value.size());
}

inline void append_value(log_buffer& out, const char* value) noexcept {
    if (value == nullptr) {
        out.append("(null)");
    } else {
        out.append(value);
    }
}

inline void append_value(log_buffer& out, std::nullptr_t) noexcept { out.append("(null)"); }

inline void append_value(log_buffer& out, char value) noexcept { out.append(value); }

inline void append_value(log_buffer& out, bool value) noexcept {
    out.append(value ? "true" : "false");
}

template <typename T,
          std::enable_if_t<std::is_integral<T>::value && !std::is_same<T, bool>::value &&
                               !std::is_same<T, char>::value,
                           int> = 0>
inline void append_value(log_buffer& out, T value) noexcept {
    if constexpr (std::is_signed<T>::value) {
        out.append_integer(static_cast<std::int64_t>(value));
    } else {
        out.append_integer(static_cast<std::uint64_t>(value));
    }
}

template <typename T, std::enable_if_t<std::is_floating_point<T>::value, int> = 0>
inline void append_value(log_buffer& out, T value) noexcept {
    out.append_float(static_cast<double>(value));
}

namespace detail {

template <typename Tuple, std::size_t Index = 0>
void format_impl(log_buffer& out, std::string_view format, const Tuple& args) noexcept {
    std::size_t position = 0;
    while (position < format.size()) {
        const char current = format[position];
        if (current == '{' && position + 1 < format.size() && format[position + 1] == '{') {
            out.append('{');
            position += 2;
            continue;
        }
        if (current == '}' && position + 1 < format.size() && format[position + 1] == '}') {
            out.append('}');
            position += 2;
            continue;
        }
        if (current == '{' && position + 1 < format.size() && format[position + 1] == '}') {
            if constexpr (Index < std::tuple_size<Tuple>::value) {
                append_value(out, std::get<Index>(args));
                format_impl<Tuple, Index + 1>(out, format.substr(position + 2), args);
                return;
            } else {
                out.append("{}");
                position += 2;
                continue;
            }
        }
        out.append(current);
        ++position;
    }
}

} // namespace detail

// Formats `{}` placeholders from `format` into `out`. Supports `{{` and `}}`
// as literal braces. Missing arguments leave `{}` in the output; extra
// arguments are ignored. No allocation, no exceptions.
template <typename... Args>
void format_to(log_buffer& out, std::string_view format, Args&&... args) noexcept {
    const auto tuple = std::forward_as_tuple(std::forward<Args>(args)...);
    detail::format_impl(out, format, tuple);
}

// --- Structured record -------------------------------------------------------

struct log_field {
    static constexpr std::size_t max_key = 16;
    static constexpr std::size_t max_value = 56;

    char key[max_key];
    char value[max_value];
    u8 key_len;
    u8 value_len;
    u8 truncated;
};

// Fixed-size record. The size is part of the memory contract: the per-thread
// ring stores records by value and never allocates per entry.
struct log_record {
    static constexpr std::size_t max_message = 128;
    static constexpr std::size_t max_fields = 4;
    static constexpr u8 flag_truncated = 0x01;

    std::uint64_t timestamp_ns;
    std::uint64_t thread_id;
    trace_id trace;
    span_id span;
    log_level level;
    u8 field_count;
    u8 flags;
    u16 message_len;
    char message[max_message];
    log_field fields[max_fields];
};

static_assert(sizeof(log_record) <= 512, "log_record must stay within one 512-byte ring slot");

// Builds a record with the current timestamp and thread id. The message is
// truncated to the inline capacity; the truncation flag is set when that
// happens.
[[nodiscard]] CRPC_API log_record make_record(log_level level, std::string_view message,
                                     const trace_context& trace = trace_context{}) noexcept;

// Appends a bounded key/value pair. Returns false when the record already
// holds `max_fields` entries. Key/value truncation is reflected in
// `log_field::truncated` and `log_record::flags`.
[[nodiscard]] CRPC_API bool record_add_field(log_record& record, std::string_view key,
                                    std::string_view value) noexcept;

template <typename T, std::enable_if_t<std::is_integral<T>::value, int> = 0>
[[nodiscard]] bool record_add_field(log_record& record, std::string_view key,
                                    T value) noexcept {
    char buffer[32];
    log_buffer out(buffer, sizeof(buffer));
    if constexpr (std::is_same<T, bool>::value) {
        out.append(value ? "true" : "false");
    } else if constexpr (std::is_signed<T>::value) {
        out.append_integer(static_cast<std::int64_t>(value));
    } else {
        out.append_integer(static_cast<std::uint64_t>(value));
    }
    return record_add_field(record, key, std::string_view(buffer, out.size()));
}

// --- Overflow policy ---------------------------------------------------------

enum class overflow_policy : u8 {
    // Drop the record and increment the ring's drop counter. Default.
    drop_and_count,
    // Spin until the sink makes room. This may stall the caller and must not
    // be used before the sink thread is running. The stall is a LIVELOCK, not
    // a delay: `block` has no timeout, so a submit() spins forever whenever
    // nobody drains the ring -- before start(), after shutdown() (the sink
    // thread is gone and the synchronous drain in shutdown() has finished), or
    // with a ring that the sink can never empty fast enough. Only legal while
    // the sink thread is verifiably running (`logger::running()`), and even
    // then a saturated sink stalls the caller indefinitely by design.
    block,
    // Yield for a bounded number of attempts, then drop and count.
    throttle
};

// --- Redaction / compression hooks ------------------------------------------

// Called for every structured field. The callback may rewrite `value` in place
// and update `*length`. `capacity` includes room for the terminating NUL but
// the length must not count it. It must not block or throw.
using redact_fn = void (*)(void* user, const char* key, std::size_t key_len, char* value,
                           std::size_t capacity, std::size_t* length) noexcept;

// Called on the sink thread with one batch of formatted bytes. Returns the
// compressed size, or 0 to leave the batch uncompressed. Off by default.
using compress_fn = std::size_t (*)(void* user, const char* input, std::size_t input_size,
                                    char* output, std::size_t output_capacity) noexcept;

// --- Configuration -----------------------------------------------------------

struct logger_config {
    // Empty disables file output; records are drained and discarded.
    std::string file_path = "crpc.log";
    // Rounded up to a power of two (minimum two records).
    std::size_t ring_capacity = 8192;
    std::size_t max_file_size = 64u * 1024u * 1024u;
    // Zero disables time-based rotation.
    std::chrono::milliseconds rotate_interval{0};
    overflow_policy overflow = overflow_policy::drop_and_count;
    bool enable_file = true;
};

// --- Logger ------------------------------------------------------------------

class log_ring;

class CRPC_API logger {
public:
    // The one documented mutable singleton of this module. Constructed on
    // first use; destroyed at process exit (its destructor calls shutdown()).
    static logger& instance() noexcept {
        static logger value;
        return value;
    }

    logger(const logger&) = delete;
    logger& operator=(const logger&) = delete;

    // Lifecycle. `configure()` and `start()` must be called while stopped.
    void configure(const logger_config& config);
    void start();
    void shutdown() noexcept;

    // Testing/administrative reset. Stops the sink, drops every registered
    // ring and restores defaults. No other thread may be logging.
    //
    // Stronger form of that contract: no thread may be INSIDE submit() while
    // reset() runs. reset() clears the shared ring table and the *calling
    // thread's* cached ring pointer only.
    //
    // Guard: every reset() bumps a global generation counter and every cached
    // ring pointer carries the generation it was registered under. A thread
    // whose cache went stale (reset() ran on another thread while it kept
    // logging) detects the mismatch on its next submit(), drops that record,
    // counts it (see stale_dropped(), also folded into dropped_total()) and
    // never dereferences the dangling pointer; its following submit()
    // re-registers a fresh ring. That turns the contract violation from a
    // use-after-free into an observable, bounded loss. The microscopic window
    // where reset() races the one submit() that already validated its cache
    // remains the caller's responsibility -- the guard bounds the violation,
    // it does not license it.
    void reset();

    // Records dropped because the submitting thread's cached ring was stale
    // (a reset() ran on another thread against the documented contract). Zero
    // on every contract-compliant run; also included in dropped_total().
    [[nodiscard]] std::uint64_t stale_dropped() const noexcept {
        return stale_drops_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    void set_level(log_level level) noexcept {
        level_.store(static_cast<int>(level), std::memory_order_relaxed);
    }

    [[nodiscard]] log_level level() const noexcept {
        return static_cast<log_level>(level_.load(std::memory_order_relaxed));
    }

    [[nodiscard]] bool enabled(log_level level) const noexcept {
        const int value = static_cast<int>(level);
        const int runtime = level_.load(std::memory_order_relaxed);
        return value < static_cast<int>(log_level::off) && value >= runtime;
    }

    // Hot path. Copies `record` into the calling thread's ring. No allocation
    // after the thread's first call, no lock, no syscall.
    void submit(const log_record& record) noexcept;

    // Structured field helper that applies the redaction hook before the
    // bounded copy. No allocation.
    [[nodiscard]] bool add_field(log_record& record, std::string_view key,
                                 std::string_view value) noexcept;

    [[nodiscard]] std::uint64_t dropped_total() const;
    [[nodiscard]] std::uint64_t written_records() const noexcept {
        return written_.load(std::memory_order_relaxed);
    }

    void set_redactor(redact_fn function, void* user) noexcept {
        redactor_.store(function, std::memory_order_relaxed);
        redactor_user_.store(user, std::memory_order_relaxed);
    }

    void set_compressor(compress_fn function, void* user) noexcept {
        compressor_.store(function, std::memory_order_relaxed);
        compressor_user_.store(user, std::memory_order_relaxed);
    }

private:
    logger() noexcept;
    ~logger();

    [[nodiscard]] log_ring* register_ring() noexcept;

    void sink_loop() noexcept;
    void ensure_drain_buffer() noexcept;
    bool drain_all();
    bool open_file() noexcept;
    void close_file() noexcept;
    void rotate_file() noexcept;
    void write_bytes(const char* data, std::size_t size) noexcept;
    void write_batch(const char* data, std::size_t size);

    mutable std::mutex config_mutex_;
    logger_config config_;
    std::atomic<std::size_t> ring_capacity_{8192};
    std::atomic<int> overflow_{static_cast<int>(overflow_policy::drop_and_count)};
    std::atomic<int> level_{static_cast<int>(log_level::trace)};
    std::atomic<bool> running_{false};

    std::thread sink_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;

    mutable std::mutex rings_mutex_;
    std::vector<std::unique_ptr<log_ring>> rings_;
    std::atomic<std::uint64_t> allocation_drops_{0};
    std::atomic<std::uint64_t> written_{0};
    // Bumped by reset(); cached ring pointers are validated against it on the
    // hot path so a stale cache is detected without dereferencing the ring.
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> stale_drops_{0};

    std::atomic<redact_fn> redactor_{nullptr};
    std::atomic<void*> redactor_user_{nullptr};
    std::atomic<compress_fn> compressor_{nullptr};
    std::atomic<void*> compressor_user_{nullptr};

#if defined(CRPC_OS_WINDOWS)
    void* file_handle_ = nullptr;
#else
    int file_fd_ = -1;
#endif
    std::uint64_t file_size_ = 0;
    std::chrono::steady_clock::time_point file_opened_{};

    std::unique_ptr<log_record[]> drain_buffer_;
    std::size_t drain_capacity_ = 0;
    std::string batch_;
    std::string compressed_;
};

// Convenience builder for structured records. The record is submitted when the
// temporary is destroyed, so the common form is:
//
//     log_event(log_level::info, "login").field("user", name);
//
// The constructor checks the runtime level; a disabled event does no work.
class CRPC_API log_event {
public:
    log_event(log_level level, std::string_view message) noexcept;
    ~log_event();

    log_event(const log_event&) = delete;
    log_event& operator=(const log_event&) = delete;

    log_event& field(std::string_view key, std::string_view value) noexcept;
    log_event& field(std::string_view key, std::int64_t value) noexcept;
    log_event& field(std::string_view key, std::uint64_t value) noexcept;

    template <typename T, std::enable_if_t<std::is_integral<T>::value, int> = 0>
    log_event& field(std::string_view key, T value) noexcept {
        if (!submit_) {
            return *this;
        }
        char buffer[32];
        log_buffer out(buffer, sizeof(buffer));
        if constexpr (std::is_same<T, bool>::value) {
            out.append(value ? "true" : "false");
        } else if constexpr (std::is_signed<T>::value) {
            out.append_integer(static_cast<std::int64_t>(value));
        } else {
            out.append_integer(static_cast<std::uint64_t>(value));
        }
        return field(key, std::string_view(buffer, out.size()));
    }

private:
    log_record record_;
    bool submit_;
};

namespace detail {

template <typename... Args>
void log_at(log_level level, std::string_view format, Args&&... args) noexcept {
    if constexpr (compile_max_level() >= static_cast<int>(log_level::off)) {
        (void)level;
        (void)format;
        ((void)args, ...);
    } else {
        if (static_cast<int>(level) < compile_max_level()) {
            return;
        }
        logger& sink = logger::instance();
        if (!sink.enabled(level)) {
            return;
        }

        log_record record = make_record(level, std::string_view{});
        log_buffer buffer(record.message, log_record::max_message);
        format_to(buffer, format, std::forward<Args>(args)...);
        record.message_len = static_cast<u16>(buffer.size());
        if (buffer.truncated()) {
            record.flags = static_cast<u8>(record.flags | log_record::flag_truncated);
        }
        sink.submit(record);
    }
}

} // namespace detail
} // namespace obs
} // namespace crpc

// Logging macros. The compile-time ceiling is evaluated with `if constexpr`, so
// a level above the ceiling is not even type-checked into the call.
#define CRPC_LOG_IMPL(level_value, level_number, ...)                                  \
    do {                                                                               \
        if constexpr (::crpc::obs::detail::compile_enabled(level_number)) {           \
            ::crpc::obs::detail::log_at((level_value), __VA_ARGS__);                   \
        }                                                                              \
    } while (0)

#define CRPC_LOG_TRACE(...) CRPC_LOG_IMPL(::crpc::obs::log_level::trace, 0, __VA_ARGS__)
#define CRPC_LOG_DEBUG(...) CRPC_LOG_IMPL(::crpc::obs::log_level::debug, 1, __VA_ARGS__)
#define CRPC_LOG_INFO(...) CRPC_LOG_IMPL(::crpc::obs::log_level::info, 2, __VA_ARGS__)
#define CRPC_LOG_WARN(...) CRPC_LOG_IMPL(::crpc::obs::log_level::warn, 3, __VA_ARGS__)
#define CRPC_LOG_ERROR(...) CRPC_LOG_IMPL(::crpc::obs::log_level::error, 4, __VA_ARGS__)

#endif // CRPC_OBSERVABILITY_LOG_HPP
