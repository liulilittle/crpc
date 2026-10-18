// CRPC -- a minimal, constexpr, bounds-clamping span.
//
// C++17 has no std::span, and the public API must never expose a raw
// (pointer, length) pair: a two-argument API invites length/pointer mismatch
// and cannot carry the "borrowed, not owned" contract in the type. This type is
// the C++17 stand-in. It is deliberately small (two words), trivially copyable,
// and does no allocation.
//
// Lifetime: a span never owns its elements. Every API that takes a span
// documents how long the underlying storage must stay alive; see the transport
// contract in io.hpp for the rules the library enforces.
#ifndef CRPC_SPAN_HPP
#define CRPC_SPAN_HPP

#include <cstddef>
#include <type_traits>

#include <crpc/config.hpp>

namespace crpc {

inline constexpr std::size_t dynamic_extent = static_cast<std::size_t>(-1);

template <typename T>
class span {
public:
    using element_type = T;
    using value_type = std::remove_cv_t<T>;
    using pointer = T*;
    using reference = T&;
    using size_type = std::size_t;

    constexpr span() noexcept : data_(nullptr), size_(0) {}

    constexpr span(pointer data, size_type count) noexcept
        : data_(data), size_(count) {}

    template <std::size_t N>
    constexpr span(element_type (&array)[N]) noexcept
        : data_(array), size_(N) {}

    constexpr pointer data() const noexcept { return data_; }
    constexpr size_type size() const noexcept { return size_; }
    constexpr size_type size_bytes() const noexcept { return size_ * sizeof(element_type); }
    constexpr bool empty() const noexcept { return size_ == 0; }

    constexpr reference operator[](size_type index) const noexcept {
        return data_[index];
    }

    constexpr pointer begin() const noexcept { return data_; }
    constexpr pointer end() const noexcept { return data_ + size_; }

    // Bounds-clamping, like std::span: an out-of-range offset yields an empty
    // span rather than undefined behaviour. Callers that treat a short span as
    // an error must compare sizes explicitly.
    constexpr span subspan(size_type offset) const noexcept {
        if (offset >= size_) {
            return span();
        }
        return span(data_ + offset, size_ - offset);
    }

    constexpr span subspan(size_type offset, size_type count) const noexcept {
        if (offset >= size_) {
            return span();
        }
        const size_type available = size_ - offset;
        return span(data_ + offset, count < available ? count : available);
    }

    constexpr span first(size_type count) const noexcept {
        return subspan(0, count);
    }

    constexpr span last(size_type count) const noexcept {
        if (count >= size_) {
            return *this;
        }
        return span(data_ + (size_ - count), count);
    }

private:
    pointer data_;
    size_type size_;
};

// Reinterprets an object span as its byte representation. Reading through
// std::byte is the one aliasing form the language explicitly permits, so this
// is well-defined for trivially copyable element types and for any object whose
// bytes we are allowed to observe.
template <typename T>
constexpr span<const std::byte> as_bytes(span<const T> s) noexcept {
    static_assert(std::is_trivially_copyable<T>::value,
                  "as_bytes requires a trivially copyable element type");
    return span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()),
                                 s.size_bytes());
}

template <typename T>
constexpr span<std::byte> as_writable_bytes(span<T> s) noexcept {
    static_assert(std::is_trivially_copyable<T>::value,
                  "as_writable_bytes requires a trivially copyable element type");
    static_assert(!std::is_const<T>::value,
                  "as_writable_bytes requires a mutable element type");
    return span<std::byte>(reinterpret_cast<std::byte*>(s.data()), s.size_bytes());
}

} // namespace crpc

#endif // CRPC_SPAN_HPP
