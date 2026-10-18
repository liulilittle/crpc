// CRPC -- platform and compiler detection.
//
// Everything in this header is compile-time only. The library never assumes a
// 64-bit host, a little-endian host, or a particular standard library
// implementation; the checks below turn those assumptions into hard compile
// errors instead of silent misbehaviour on an unsupported target.
#ifndef CRPC_CONFIG_HPP
#define CRPC_CONFIG_HPP

#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>

// --- Target detection --------------------------------------------------------

#if defined(_WIN32)
#define CRPC_OS_WINDOWS 1
#elif defined(__linux__)
#define CRPC_OS_LINUX 1
#elif defined(__APPLE__)
#define CRPC_OS_MACOS 1
#else
#error "crpc: unsupported operating system"
#endif

#if defined(_MSC_VER)
#define CRPC_COMPILER_MSVC 1
#elif defined(__clang__)
#define CRPC_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define CRPC_COMPILER_GCC 1
#else
#error "crpc: unsupported compiler"
#endif

#if defined(_M_IX86) || defined(__i386__)
#define CRPC_ARCH_X86 1
#elif defined(_M_X64) || defined(__x86_64__)
#define CRPC_ARCH_X64 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#define CRPC_ARCH_ARM64 1
#else
#define CRPC_ARCH_UNKNOWN 1
#endif

// --- Byte order --------------------------------------------------------------
//
// The wire format is always little-endian (see crpc/endian.hpp). This macro
// only describes the host so that the load/store primitives can pick the fast
// path; correctness never depends on it.

#if defined(_MSC_VER)
// MSVC targets are little-endian for every supported architecture.
#define CRPC_HOST_LITTLE_ENDIAN 1
#elif defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define CRPC_HOST_LITTLE_ENDIAN 1
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define CRPC_HOST_BIG_ENDIAN 1
#else
#error "crpc: mixed-endian hosts are not supported"
#endif
#elif defined(__LITTLE_ENDIAN__)
#define CRPC_HOST_LITTLE_ENDIAN 1
#else
#error "crpc: cannot determine host byte order"
#endif

// --- Export / import ---------------------------------------------------------

#if defined(CRPC_SHARED)
#if defined(CRPC_BUILDING)
#if defined(CRPC_OS_WINDOWS)
#define CRPC_API __declspec(dllexport)
#else
#define CRPC_API __attribute__((visibility("default")))
#endif
#else
#if defined(CRPC_OS_WINDOWS)
#define CRPC_API __declspec(dllimport)
#else
#define CRPC_API
#endif
#endif
#else
#define CRPC_API
#endif

// --- Compile-time invariants -------------------------------------------------
//
// These are the assumptions the entire wire format and every size computation
// rest on. A target that fails one of them is rejected at compile time rather
// than producing a library that silently mis-serialises.

static_assert(CHAR_BIT == 8, "crpc requires 8-bit bytes");
static_assert(sizeof(std::uint8_t) == 1, "crpc requires an 8-bit uint8_t");
static_assert(sizeof(std::uint16_t) == 2, "crpc requires a 16-bit uint16_t");
static_assert(sizeof(std::uint32_t) == 4, "crpc requires a 32-bit uint32_t");
static_assert(sizeof(std::uint64_t) == 8, "crpc requires a 64-bit uint64_t");
static_assert(std::numeric_limits<float>::is_iec559, "crpc requires IEEE-754 float");
static_assert(std::numeric_limits<double>::is_iec559, "crpc requires IEEE-754 double");

namespace crpc {

// Size classes are named rather than spelled `long` or `size_t` so that the
// public API can state its width explicitly.
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

constexpr bool host_is_little_endian() noexcept {
#if defined(CRPC_HOST_LITTLE_ENDIAN)
    return true;
#else
    return false;
#endif
}

constexpr bool host_is_64_bit() noexcept { return sizeof(void*) >= 8; }

} // namespace crpc

#endif // CRPC_CONFIG_HPP
