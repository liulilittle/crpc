// CRPC -- acquire/release accessors and memory fences.
//
// `volatile_read` and `volatile_write` give a non-atomic object the ordering
// that .NET's `Volatile.Read` / `Volatile.Write` give a .NET field: the read
// has acquire semantics and the write has release semantics. They are the
// building block for a publication pattern -- write the payload, release-store
// a flag; acquire-load the flag, read the payload -- without making the
// payload itself atomic.
//
// Contract:
//
//   * `T` must be trivially copyable and exactly 1, 2, 4 or 8 bytes wide.
//     Anything else is rejected at compile time.
//   * The whole value is read/written in one operation, so the object must be
//     naturally aligned for its size. Fundamental integer, pointer, float and
//     double objects are. A packed or unaligned field is not a valid argument
//     (on 32-bit x86 an 8-byte value additionally requires 8-byte alignment).
//   * These functions do not make *other* accesses to the same object safe.
//     While another thread may access the object, every access must go through
//     one of these functions (or through an atomic of the same width); mixing
//     them with plain reads/writes is a data race.
//   * They are inline, header-only and never throw.
//
// Platform notes:
//
//   MSVC / Windows:
//     The access itself is the matching ISO-volatile intrinsic
//     (`__iso_volatile_load8/16/32/64` and the store forms), which the
//     compiler emits as one naturally aligned load/store -- a plain `memcpy`
//     may become two word accesses on a 32-bit target in a debug build. The
//     read then executes `std::atomic_thread_fence(acquire)` and the write
//     executes the release fence before the store. On x86 and x64 those
//     fences compile to a compiler barrier and no CPU instruction: x86
//     already orders loads against loads, loads against stores and stores
//     against stores, and an aligned 1/2/4/8-byte access is atomic. On ARM64
//     the fences emit the real `dmb`/`dmb ld` instructions.
//
//   GCC / Clang:
//     The read is `__atomic_load(..., __ATOMIC_ACQUIRE)` and the write is
//     `__atomic_store(..., __ATOMIC_RELEASE)`. The compiler emits the cheapest
//     correct sequence for the target: a plain `mov` on x86 (TSO already
//     supplies the ordering) and `ldar`/`stlr` on AArch64.
//
// `full_fence` is a sequentially consistent hardware fence
// (`std::atomic_thread_fence(seq_cst)`). `signal_fence` is the compiler-only
// fence intended for communication with a signal handler
// (`std::atomic_signal_fence(seq_cst)`); it emits no CPU instruction.
#ifndef CRPC_CONCURRENCY_VOLATILE_HPP
#define CRPC_CONCURRENCY_VOLATILE_HPP

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include <crpc/config.hpp>

#if defined(CRPC_COMPILER_MSVC)
#include <intrin.h>
#endif

namespace crpc {
namespace concurrency {

// Acquire read of an ordinary 1/2/4/8-byte object.
template <typename T>
[[nodiscard]] inline T volatile_read(const T& src) noexcept {
    static_assert(std::is_trivially_copyable<T>::value,
                  "volatile_read requires a trivially copyable type");
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 ||
                      sizeof(T) == 8,
                  "volatile_read requires a 1, 2, 4 or 8 byte type");

#if defined(CRPC_COMPILER_MSVC)
    // One access of exactly the object's width; the bit copy below only moves
    // a register value into `value`, so it cannot tear the source read.
    if constexpr (sizeof(T) == 1) {
        const char bits = __iso_volatile_load8(
            reinterpret_cast<const volatile char*>(&src));
        T value;
        std::memcpy(&value, &bits, sizeof(T));
        std::atomic_thread_fence(std::memory_order_acquire);
        return value;
    } else if constexpr (sizeof(T) == 2) {
        const std::int16_t bits = __iso_volatile_load16(
            reinterpret_cast<const volatile std::int16_t*>(&src));
        T value;
        std::memcpy(&value, &bits, sizeof(T));
        std::atomic_thread_fence(std::memory_order_acquire);
        return value;
    } else if constexpr (sizeof(T) == 4) {
        const std::int32_t bits = __iso_volatile_load32(
            reinterpret_cast<const volatile std::int32_t*>(&src));
        T value;
        std::memcpy(&value, &bits, sizeof(T));
        std::atomic_thread_fence(std::memory_order_acquire);
        return value;
    } else {
        const std::int64_t bits = __iso_volatile_load64(
            reinterpret_cast<const volatile std::int64_t*>(&src));
        T value;
        std::memcpy(&value, &bits, sizeof(T));
        std::atomic_thread_fence(std::memory_order_acquire);
        return value;
    }
#else
    T value;
    __atomic_load(&src, &value, __ATOMIC_ACQUIRE);
    return value;
#endif
}

// Release write of an ordinary 1/2/4/8-byte object.
template <typename T>
inline void volatile_write(T& dst, T value) noexcept {
    static_assert(std::is_trivially_copyable<T>::value,
                  "volatile_write requires a trivially copyable type");
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 ||
                      sizeof(T) == 8,
                  "volatile_write requires a 1, 2, 4 or 8 byte type");

#if defined(CRPC_COMPILER_MSVC)
    if constexpr (sizeof(T) == 1) {
        char bits;
        std::memcpy(&bits, &value, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        __iso_volatile_store8(reinterpret_cast<volatile char*>(&dst), bits);
    } else if constexpr (sizeof(T) == 2) {
        std::int16_t bits;
        std::memcpy(&bits, &value, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        __iso_volatile_store16(reinterpret_cast<volatile std::int16_t*>(&dst),
                               bits);
    } else if constexpr (sizeof(T) == 4) {
        std::int32_t bits;
        std::memcpy(&bits, &value, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        __iso_volatile_store32(reinterpret_cast<volatile std::int32_t*>(&dst),
                               bits);
    } else {
        std::int64_t bits;
        std::memcpy(&bits, &value, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        __iso_volatile_store64(reinterpret_cast<volatile std::int64_t*>(&dst),
                               bits);
    }
#else
    __atomic_store(&dst, &value, __ATOMIC_RELEASE);
#endif
}

// Full hardware fence (sequentially consistent).
inline void full_fence() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

// Compiler-only fence, for use in the same thread as an asynchronous signal.
inline void signal_fence() noexcept {
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

} // namespace concurrency
} // namespace crpc

#endif // CRPC_CONCURRENCY_VOLATILE_HPP
