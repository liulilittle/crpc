// CRPC -- UUID generation, parsing and formatting.
//
// Correctness bases, in order of authority:
//   * RFC 4122 section 4.1.2 fixes the 16-octet layout and section 4.1.3 the
//     version nibble; RFC 9562 (which obsoletes it) keeps both and adds v7.
//   * RFC 9562 Appendix A gives worked vectors for v1, v3, v5 and v7; the test
//     suite pins them. The v3 vector printed by RFC 4122 Appendix B is a
//     verified erratum (eid 1352), so the corrected value is used.
//   * The Unix/Gregorian offset 122192928000000000 is the value RFC 9562
//     Appendix A prints and is asserted here at compile time.
// Randomness comes from the operating system through a path deliberately
// independent of rsa.cpp's private helper: this file is not allowed to depend on
// an implementation detail of another translation unit.
#include <crpc/crypto/uuid.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>

#include <crpc/crypto/hash.hpp>
#include <crpc/crypto/md5.hpp>

#if defined(CRPC_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#elif defined(CRPC_OS_MACOS)
#include <cstdlib>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#endif

namespace crpc {
namespace crypto {
namespace {

// 100 ns intervals between the UUID epoch (1582-10-15) and the Unix epoch.
constexpr std::uint64_t gregorian_unix_offset_100ns = 122192928000000000ULL;
static_assert(gregorian_unix_offset_100ns == 0x01b21dd213814000ULL,
              "RFC 9562 Appendix A's Gregorian-to-Unix offset");

constexpr std::uint64_t timestamp_60bit_mask = 0x0fffffffffffffffull;
constexpr std::uint64_t timestamp_48bit_mask = 0x0000ffffffffffffull;

// --- Operating-system entropy -------------------------------------------------

// Fills `out` from the OS cryptographic generator. Returns false on failure;
// there is no silent predictable fallback here, the caller decides.
[[nodiscard]] bool fill_random_from_os(std::uint8_t* out, std::size_t size) noexcept {
    if (size == 0) {
        return true;
    }
#if defined(CRPC_OS_WINDOWS)
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG lets the OS pick its approved generator.
    const LONG status = ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out),
                                          static_cast<ULONG>(size),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0;
#elif defined(CRPC_OS_MACOS)
    // arc4random_buf is macOS's fork-safe OS generator; it cannot fail.
    ::arc4random_buf(out, size);
    return true;
#else
    std::size_t done = 0;
    while (done < size) {
        const ssize_t got = ::getrandom(out + done, size - done, 0);
        if (got > 0) {
            done += static_cast<std::size_t>(got);
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break; // getrandom(2) unavailable (old kernel): use /dev/urandom
    }
    if (done == size) {
        return true;
    }
    const int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    bool ok = true;
    while (done < size) {
        const ssize_t read_bytes = ::read(fd, out + done, size - done);
        if (read_bytes <= 0) {
            ok = false;
            break;
        }
        done += static_cast<std::size_t>(read_bytes);
    }
    ::close(fd);
    return ok;
#endif
}

// Degraded path, taken only when the OS generator refuses. `std::random_device`
// plus a Mersenne Twister is NOT a cryptographically secure generator: the task
// API returns a value rather than an error, so the only alternatives are this or
// returning a zero-filled UUID, which is worse. A caller that cannot tolerate
// the distinction must not use v4 on a host whose entropy source is broken.
void fill_random_degraded(std::uint8_t* out, std::size_t size) noexcept {
    thread_local std::mt19937_64 rng([] {
        std::uint64_t seed = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        try {
            std::random_device device;
            seed ^= (static_cast<std::uint64_t>(device()) << 32) ^
                    static_cast<std::uint64_t>(device());
        } catch (...) {
            // random_device is permitted to fail on a target with no entropy
            // device; the clock above is then all the seed material available.
            // This whole path is already the degraded one.
        }
        return seed;
    }());
    std::size_t done = 0;
    while (done < size) {
        const std::uint64_t word = rng();
        const std::size_t take = size - done < sizeof(word) ? size - done : sizeof(word);
        std::memcpy(out + done, &word, take);
        done += take;
    }
}

void fill_random(std::uint8_t* out, std::size_t size) noexcept {
    if (!fill_random_from_os(out, size)) {
        fill_random_degraded(out, size);
    }
}

[[nodiscard]] std::uint64_t random_word() noexcept {
    std::uint8_t bytes[sizeof(std::uint64_t)] = {};
    fill_random(bytes, sizeof(bytes));
    std::uint64_t value = 0;
    for (const std::uint8_t byte : bytes) {
        value = (value << 8) | byte;
    }
    return value;
}

// --- Field packing ------------------------------------------------------------

void write_be(std::uint8_t* out, std::uint64_t value, std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::uint8_t>((value >> (8 * (size - 1 - i))) & 0xffu);
    }
}

[[maybe_unused]] [[nodiscard]] std::uint64_t read_be(const std::uint8_t* in,
                                                     std::size_t size) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        value = (value << 8) | in[i];
    }
    return value;
}

void set_version_and_variant(uuid& value, std::uint8_t version) noexcept {
    value.bytes[6] = static_cast<std::uint8_t>((version << 4) | (value.bytes[6] & 0x0fu));
    value.bytes[8] = static_cast<std::uint8_t>((value.bytes[8] & 0x3fu) | 0x80u);
}

// --- Process-wide v1 state ----------------------------------------------------

struct v1_identity {
    std::array<std::uint8_t, 6> node;
    std::uint16_t clock_sequence;
};

[[nodiscard]] const v1_identity& process_identity() {
    static const v1_identity identity = [] {
        v1_identity made{};
        fill_random(made.node.data(), made.node.size());
        // RFC 4122 section 4.5: a random node sets the multicast bit (the
        // least significant bit of the first octet) so it can never collide
        // with an address a network card was assigned.
        made.node[0] = static_cast<std::uint8_t>(made.node[0] | 0x01u);
        made.clock_sequence =
            static_cast<std::uint16_t>(random_word() & 0x3fffu);
        return made;
    }();
    return identity;
}

[[nodiscard]] std::uint64_t system_timestamp_100ns() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    if (nanoseconds <= 0) {
        return gregorian_unix_offset_100ns;
    }
    return gregorian_unix_offset_100ns +
           static_cast<std::uint64_t>(nanoseconds) / 100u;
}

[[nodiscard]] std::uint64_t system_unix_ms() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return milliseconds <= 0 ? 0 : static_cast<std::uint64_t>(milliseconds);
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

// --- Version 1 ----------------------------------------------------------------

uuid uuid_v1(std::uint64_t timestamp_100ns, const uuid_v1_context& context) noexcept {
    const std::uint64_t timestamp = timestamp_100ns & timestamp_60bit_mask;
    uuid value{};
    write_be(value.bytes.data(), timestamp & 0xffffffffull, 4);
    write_be(value.bytes.data() + 4, (timestamp >> 32) & 0xffffull, 2);
    write_be(value.bytes.data() + 6, (timestamp >> 48) & 0x0fffull, 2);
    value.bytes[8] = static_cast<std::uint8_t>((context.clock_sequence >> 8) & 0x3fu);
    value.bytes[9] = static_cast<std::uint8_t>(context.clock_sequence & 0xffu);
    std::memcpy(value.bytes.data() + 10, context.node.data(), context.node.size());
    set_version_and_variant(value, 1);
    return value;
}

uuid uuid_v1() {
    const v1_identity& identity = process_identity();
    // The system clock is coarser than 100 ns, so two calls can observe the
    // same tick. RFC 4122 section 4.2.1.2 permits either stalling or
    // synthesising the low bits of the timestamp; incrementing a per-process
    // last-value is the second option and keeps the values unique.
    static std::atomic<std::uint64_t> last_timestamp{0};
    std::uint64_t observed = last_timestamp.load(std::memory_order_relaxed);
    std::uint64_t chosen = 0;
    for (;;) {
        const std::uint64_t now = system_timestamp_100ns();
        const std::uint64_t candidate =
            now > observed ? now : (observed + 1) & timestamp_60bit_mask;
        if (last_timestamp.compare_exchange_weak(observed, candidate,
                                                 std::memory_order_relaxed)) {
            chosen = candidate;
            break;
        }
    }

    uuid_v1_context context;
    context.node = identity.node;
    context.clock_sequence = identity.clock_sequence;
    return uuid_v1(chosen, context);
}

// --- Name-based (version 3 and 5) ---------------------------------------------

uuid uuid_v3(const uuid& name_space, std::string_view name) {
    const span<const std::byte> namespace_bytes(
        reinterpret_cast<const std::byte*>(name_space.bytes.data()), name_space.bytes.size());
    const span<const std::byte> name_bytes(
        reinterpret_cast<const std::byte*>(name.data()), name.size());
    const md5_digest digest = md5(namespace_bytes, name_bytes);
    uuid value{};
    std::memcpy(value.bytes.data(), digest.data(), value.bytes.size());
    set_version_and_variant(value, 3);
    return value;
}

uuid uuid_v5(const uuid& name_space, std::string_view name) {
    const span<const std::byte> namespace_bytes(
        reinterpret_cast<const std::byte*>(name_space.bytes.data()), name_space.bytes.size());
    const span<const std::byte> name_bytes(
        reinterpret_cast<const std::byte*>(name.data()), name.size());
    const sha1_digest digest = sha1(namespace_bytes, name_bytes);
    uuid value{};
    std::memcpy(value.bytes.data(), digest.data(), value.bytes.size());
    set_version_and_variant(value, 5);
    return value;
}

// --- Random (version 4) -------------------------------------------------------

uuid uuid_v4() {
    uuid value{};
    fill_random(value.bytes.data(), value.bytes.size());
    set_version_and_variant(value, 4);
    return value;
}

// --- Time-ordered (version 7) -------------------------------------------------

uuid uuid_v7(std::uint64_t unix_ms) {
    // The counter lives in rand_a (RFC 9562 section 5.7 and section 6.2
    // Method 1). Packing it with the timestamp in one atomic word lets a CAS
    // both keep the pair consistent and keep the sequence monotonic without a
    // lock. Counter overflow borrows the next millisecond, so the value stays
    // monotonic and the timestamp field still means "at or after unix_ms".
    static std::atomic<std::uint64_t> last_state{0};
    std::uint64_t observed = last_state.load(std::memory_order_relaxed);
    std::uint64_t timestamp = 0;
    std::uint32_t counter = 0;
    for (;;) {
        const std::uint64_t observed_ms = observed >> 12;
        const std::uint32_t observed_counter =
            static_cast<std::uint32_t>(observed & 0xfffu);
        std::uint64_t candidate_ms = unix_ms;
        std::uint32_t candidate_counter = 0;
        if (candidate_ms <= observed_ms) {
            candidate_ms = observed_ms;
            if (observed_counter == 0xfffu) {
                ++candidate_ms;
                candidate_counter = 0;
            } else {
                candidate_counter = observed_counter + 1;
            }
        }
        const std::uint64_t desired = (candidate_ms << 12) | candidate_counter;
        if (last_state.compare_exchange_weak(observed, desired,
                                             std::memory_order_relaxed)) {
            timestamp = candidate_ms;
            counter = candidate_counter;
            break;
        }
    }

    uuid value{};
    write_be(value.bytes.data(), timestamp & timestamp_48bit_mask, 6);
    // rand_a occupies the low 12 bits of octets 6-7; the version nibble sits
    // above it in octet 6. rand_b is 62 bits: octets 9-15 plus the low 6 bits
    // of octet 8, with the variant in the top 2.
    std::uint8_t random_tail[8] = {};
    fill_random(random_tail, sizeof(random_tail));
    value.bytes[6] = static_cast<std::uint8_t>((counter >> 8) & 0x0fu);
    value.bytes[7] = static_cast<std::uint8_t>(counter & 0xffu);
    value.bytes[8] = random_tail[0];
    std::memcpy(value.bytes.data() + 9, random_tail + 1, 7);
    set_version_and_variant(value, 7);
    return value;
}

uuid uuid_v7() { return uuid_v7(system_unix_ms()); }

// --- Text and bytes -----------------------------------------------------------

result<uuid> parse_uuid(std::string_view text) {
    // Two accepted shapes: 32 bare hex digits, or 8-4-4-4-12. Anything else is
    // refused rather than guessed at; see the header comment on notation.
    static constexpr std::size_t hyphens[4] = {8, 13, 18, 23};
    const bool canonical = text.size() == 36;
    if (!canonical && text.size() != 32) {
        return rpc_error{rpc_code::decode_error,
                         "a UUID is 32 hex digits, optionally in 8-4-4-4-12 form"};
    }
    if (canonical) {
        for (const std::size_t position : hyphens) {
            if (text[position] != '-') {
                return rpc_error{rpc_code::decode_error,
                                 "hyphen at an unexpected position in a UUID"};
            }
        }
    }

    uuid value{};
    std::size_t written = 0;
    int high = -1;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (canonical && (i == 8 || i == 13 || i == 18 || i == 23)) {
            continue;
        }
        const int digit = hex_value(text[i]);
        if (digit < 0) {
            return rpc_error{rpc_code::decode_error,
                             "a UUID character is not a hexadecimal digit"};
        }
        if (high < 0) {
            high = digit;
        } else {
            value.bytes[written] = static_cast<std::uint8_t>((high << 4) | digit);
            ++written;
            high = -1;
        }
    }
    // Both accepted lengths are even, so the loop always ends on a nibble
    // boundary; the check is here so a later length change cannot silently
    // return a half-filled value.
    if (written != value.bytes.size()) {
        return rpc_error{rpc_code::decode_error, "a UUID must decode to exactly 16 octets"};
    }
    return value;
}

std::string to_string(const uuid& value) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < value.bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(digits[value.bytes[i] >> 4]);
        out.push_back(digits[value.bytes[i] & 0x0fu]);
    }
    return out;
}

result<uuid> uuid_from_bytes(span<const std::uint8_t> bytes) {
    if (bytes.size() != 16) {
        return rpc_error{rpc_code::decode_error,
                         "a UUID is exactly 16 octets"};
    }
    uuid value{};
    std::memcpy(value.bytes.data(), bytes.data(), value.bytes.size());
    return value;
}

uuid_bytes to_bytes(const uuid& value) noexcept { return value.bytes; }

} // namespace crypto
} // namespace crpc
