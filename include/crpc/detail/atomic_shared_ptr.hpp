// CRPC -- an atomically swappable `shared_ptr`.
//
// Both `cluster::registry` and `resilience::load_balancer` publish immutable
// snapshots by swapping a `shared_ptr`: readers take a strong reference with
// acquire semantics and work on a value that can never change underneath them.
//
// The storage is a plain `std::shared_ptr<T>` and the swap goes through the
// free-function API (`std::atomic_load_explicit` / `std::atomic_store_explicit`).
// That is deliberate, and it is the part that matters for ABI:
//
//   `std::atomic<std::shared_ptr<T>>` -- the C++20 replacement -- has
//   `alignof == 16` on MSVC x64 while `std::shared_ptr<T>` has `alignof == 8`.
//   Measured: `registry` 336/8 vs 336/16 and `load_balancer` 160/8 vs 176/16.
//   Embedding it would make the layout of two exported, consumer-constructible
//   classes depend on which language standard the *consumer* compiles with,
//   while the DLL is built one way. That is a silent layout mismatch for every
//   C++20 consumer of a C++17-built library, which is exactly what
//   `docs/abi.md` section 1 forbids. The free-function form keeps the layout
//   identical in C++17 and C++20 on every compiler that was measured.
//
// C++20 deprecates the free functions (MSVC: STL4029) in favour of the
// specialization, and `/W4 /WX` turns that deprecation into a hard error -- so
// without a local suppression the library does not compile in C++20 at all,
// which in turn made `tests/test_rpc_response_cpp20.cpp` a no-op in every
// configuration that existed. The suppression below is scoped to this header
// and only exists where the deprecation exists. GCC and clang do not diagnose
// this and need nothing.
//
// Semantics of the two spellings are the same for what is used here: an atomic
// load/store of the handle, where the memory order applies to the pointer and
// the reference counting is done by the control block. Neither form is
// lock-free on MSVC, so nothing is lost.
#ifndef CRPC_DETAIL_ATOMIC_SHARED_PTR_HPP
#define CRPC_DETAIL_ATOMIC_SHARED_PTR_HPP

#include <atomic>
#include <memory>
#include <utility>

#if defined(_MSC_VER)
// 4996 covers the STL deprecation warnings (STL4029 is reported under it).
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

namespace crpc {
namespace detail {

template <class T>
class atomic_shared_ptr {
public:
    atomic_shared_ptr() noexcept = default;
    explicit atomic_shared_ptr(std::shared_ptr<T> value) noexcept
        : value_(std::move(value)) {}

    atomic_shared_ptr(const atomic_shared_ptr&) = delete;
    atomic_shared_ptr& operator=(const atomic_shared_ptr&) = delete;

    [[nodiscard]] std::shared_ptr<T> load(
        std::memory_order order = std::memory_order_acquire) const noexcept {
        return std::atomic_load_explicit(&value_, order);
    }

    void store(std::shared_ptr<T> value,
               std::memory_order order = std::memory_order_release) noexcept {
        std::atomic_store_explicit(&value_, std::move(value), order);
    }

private:
    // Never change this to `std::atomic<std::shared_ptr<T>>`: see the header
    // comment. The size and alignment of the classes that embed this are part
    // of the ABI in both standards.
    mutable std::shared_ptr<T> value_;
};

} // namespace detail
} // namespace crpc

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif // CRPC_DETAIL_ATOMIC_SHARED_PTR_HPP
