// CRPC -- explicit integer endianness primitives.
//
// The CRPC wire format is little-endian on every platform. Some external
// protocols (the memcached binary protocol, TDS, the MySQL packet header) are
// big-endian, so the same validated helpers are provided for both orders; a
// caller names the order it needs and never assumes the host order.
//
// These helpers are the only sanctioned way to move a multi-byte integer
// between memory and a wire buffer:
//
//   * no `memcpy` of a whole struct (padding and member order are not wire
//     format),
//   * no `reinterpret_cast` of a buffer to an integer type (alignment and
//     strict aliasing),
//   * no assumption about the host byte order (the load/store functions
//     byte-swap on big-endian hosts).
//
// Every function is noexcept and constexpr-friendly where the platform allows
// it. Reads are bounds-checked by the caller: these primitives trust that the
// caller has already validated that `sizeof(T)` bytes are available.
#ifndef CRPC_ENDIAN_HPP
#define CRPC_ENDIAN_HPP

#include <cstring>
#include <type_traits>

#include <crpc/config.hpp>

namespace crpc {
namespace endian {

constexpr u16 byteswap16(u16 v) noexcept {
    return static_cast<u16>((v >> 8) | (v << 8));
}

constexpr u32 byteswap32(u32 v) noexcept {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

constexpr u64 byteswap64(u64 v) noexcept {
    return (static_cast<u64>(byteswap32(static_cast<u32>(v))) << 32) |
           static_cast<u64>(byteswap32(static_cast<u32>(v >> 32)));
}

// Loads an integer of width 1/2/4/8 from `src`, which must point at exactly
// that many readable bytes. The byte order on the wire is little-endian.
template <typename T>
inline T load_le(const void* src) noexcept {
    static_assert(std::is_integral<T>::value, "load_le requires an integer type");
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "load_le supports 1, 2, 4 and 8 byte integers");
    using unsigned_t = std::make_unsigned_t<T>;
    unsigned_t raw = 0;
    std::memcpy(&raw, src, sizeof(raw));
#if defined(CRPC_HOST_BIG_ENDIAN)
    if constexpr (sizeof(T) == 2) {
        raw = byteswap16(raw);
    } else if constexpr (sizeof(T) == 4) {
        raw = byteswap32(raw);
    } else if constexpr (sizeof(T) == 8) {
        raw = byteswap64(raw);
    }
#endif
    return static_cast<T>(raw);
}

// Stores an integer of width 1/2/4/8 to `dst`, which must point at exactly that
// many writable bytes. The byte order on the wire is little-endian.
template <typename T>
inline void store_le(void* dst, T value) noexcept {
    static_assert(std::is_integral<T>::value, "store_le requires an integer type");
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "store_le supports 1, 2, 4 and 8 byte integers");
    using unsigned_t = std::make_unsigned_t<T>;
    unsigned_t raw = static_cast<unsigned_t>(value);
#if defined(CRPC_HOST_BIG_ENDIAN)
    if constexpr (sizeof(T) == 2) {
        raw = byteswap16(raw);
    } else if constexpr (sizeof(T) == 4) {
        raw = byteswap32(raw);
    } else if constexpr (sizeof(T) == 8) {
        raw = byteswap64(raw);
    }
#endif
    std::memcpy(dst, &raw, sizeof(raw));
}

// IEEE-754 bit patterns travel as fixed-width integers so that a NaN payload or
// a negative zero survives a round trip byte for byte.
inline u32 float_to_bits(float value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline float bits_to_float(u32 bits) noexcept {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline u64 double_to_bits(double value) noexcept {
    u64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline double bits_to_double(u64 bits) noexcept {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline u16 load_le16(const void* src) noexcept { return load_le<u16>(src); }
inline u32 load_le32(const void* src) noexcept { return load_le<u32>(src); }
inline u64 load_le64(const void* src) noexcept { return load_le<u64>(src); }

inline void store_le16(void* dst, u16 value) noexcept { store_le<u16>(dst, value); }
inline void store_le32(void* dst, u32 value) noexcept { store_le<u32>(dst, value); }
inline void store_le64(void* dst, u64 value) noexcept { store_le<u64>(dst, value); }

// Big-endian counterparts. Same contract: the caller guarantees the width.
template <typename T>
inline T load_be(const void* src) noexcept {
    static_assert(std::is_integral<T>::value, "load_be requires an integer type");
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "load_be supports 1, 2, 4 and 8 byte integers");
    using unsigned_t = std::make_unsigned_t<T>;
    unsigned_t raw = 0;
    std::memcpy(&raw, src, sizeof(raw));
#if !defined(CRPC_HOST_BIG_ENDIAN)
    if constexpr (sizeof(T) == 2) {
        raw = byteswap16(raw);
    } else if constexpr (sizeof(T) == 4) {
        raw = byteswap32(raw);
    } else if constexpr (sizeof(T) == 8) {
        raw = byteswap64(raw);
    }
#endif
    return static_cast<T>(raw);
}

template <typename T>
inline void store_be(void* dst, T value) noexcept {
    static_assert(std::is_integral<T>::value, "store_be requires an integer type");
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "store_be supports 1, 2, 4 and 8 byte integers");
    using unsigned_t = std::make_unsigned_t<T>;
    unsigned_t raw = static_cast<unsigned_t>(value);
#if !defined(CRPC_HOST_BIG_ENDIAN)
    if constexpr (sizeof(T) == 2) {
        raw = byteswap16(raw);
    } else if constexpr (sizeof(T) == 4) {
        raw = byteswap32(raw);
    } else if constexpr (sizeof(T) == 8) {
        raw = byteswap64(raw);
    }
#endif
    std::memcpy(dst, &raw, sizeof(raw));
}

inline u16 load_be16(const void* src) noexcept { return load_be<u16>(src); }
inline u32 load_be32(const void* src) noexcept { return load_be<u32>(src); }
inline u64 load_be64(const void* src) noexcept { return load_be<u64>(src); }

inline void store_be16(void* dst, u16 value) noexcept { store_be<u16>(dst, value); }
inline void store_be32(void* dst, u32 value) noexcept { store_be<u32>(dst, value); }
inline void store_be64(void* dst, u64 value) noexcept { store_be<u64>(dst, value); }

} // namespace endian
} // namespace crpc

#endif // CRPC_ENDIAN_HPP
