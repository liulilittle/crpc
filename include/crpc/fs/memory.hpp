// CRPC -- growable, sliceable byte buffer.
//
// This is the framework's one owned byte container, so no subsystem needs its
// own `std::vector<std::byte>` wheel. Three properties distinguish it from a
// raw vector:
//
//   * growth is geometric (1.5x) and every allocation uses nothrow new, so a
//     huge `reserve()` fails with a value (`resource_exhausted`) instead of
//     throwing bad_alloc;
//   * slicing returns a borrowed `crpc::span` view (clamped, never UB) so a
//     parser can hand out sub-ranges of a loaded file without copies;
//   * string interop is first-class: construct/append/read as text. The bytes
//     are never assumed to be UTF-8; `to_string` is a byte-wise copy.
//
// The buffer is move-only (copying a megabytes-long blob by accident is how
// viruses are shipped). `clear()` keeps capacity for reuse.
#ifndef CRPC_FS_MEMORY_HPP
#define CRPC_FS_MEMORY_HPP

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace fs {

class CRPC_API byte_buffer {
public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    // The largest size this container will grow to. A length read from a
    // file header or a frame is untrusted, so the rejection of an absurd
    // request must be a policy, not a consequence of the allocator happening
    // to fail: on a 32-bit build `SIZE_MAX / 4` is 1 GiB, which a 32-bit
    // process can actually allocate, and the copy that follows then runs off
    // the caller's bad pointer. 512 MiB is far above any whole-file use this
    // container documents and well below what a 32-bit process can satisfy.
    // A caller that needs more must stream rather than load.
    static constexpr std::size_t default_max_capacity =
        static_cast<std::size_t>(512) * 1024 * 1024;

    byte_buffer() = default;

    // Reserves `initial_capacity` bytes up front. A failed allocation leaves
    // an empty (capacity-0) buffer; use `reserve` to observe the failure.
    explicit byte_buffer(std::size_t initial_capacity);

    byte_buffer(const char* text);
    byte_buffer(std::string text);
    byte_buffer(std::string_view text);
    explicit byte_buffer(span<const std::byte> data);

    byte_buffer(byte_buffer&&) noexcept = default;
    byte_buffer& operator=(byte_buffer&&) noexcept = default;
    byte_buffer(const byte_buffer&) = delete;
    byte_buffer& operator=(const byte_buffer&) = delete;

    std::byte* data() noexcept { return data_.get(); }
    const std::byte* data() const noexcept { return data_.get(); }
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }
    bool empty() const noexcept { return size_ == 0; }

    // Whole-buffer views.
    span<std::byte> mutable_bytes() noexcept {
        return span<std::byte>(data_.get(), size_);
    }
    span<const std::byte> bytes() const noexcept {
        return span<const std::byte>(data_.get(), size_);
    }

    // Borrowed sub-range. Out-of-range offsets yield an empty span (clamped,
    // like `crpc::span::subspan`) -- never undefined behaviour.
    span<const std::byte> slice(std::size_t offset,
                                std::size_t count = npos) const noexcept;

    // Ensure capacity for at least `new_capacity` bytes. `resource_exhausted`
    // when the allocation fails or `new_capacity` exceeds
    // `default_max_capacity` (the buffer is then left unchanged).
    status reserve(std::size_t new_capacity);

    // Appends bytes. `resource_exhausted` on allocation failure or when the
    // result would exceed `default_max_capacity` (the buffer is left
    // unchanged, and `data` is never read). `ok` means everything was
    // appended.
    status append(span<const std::byte> data);
    status append(const char* text);
    status append(std::string_view text);
    status append(std::byte value);

    // Resets size to zero, keeping capacity for reuse.
    void clear() noexcept { size_ = 0; }

    // Drops `count` bytes from the front, shifting the rest down.
    void erase_front(std::size_t count) noexcept;

    // Text interop. `to_string` copies; `to_string_view` borrows.
    std::string to_string() const;
    std::string_view to_string_view() const noexcept {
        return std::string_view(reinterpret_cast<const char*>(data_.get()), size_);
    }

    // Assigns text bytes, replacing the contents.
    status assign(std::string_view text);

    friend bool operator==(const byte_buffer& a, const byte_buffer& b) noexcept;
    friend bool operator!=(const byte_buffer& a, const byte_buffer& b) noexcept {
        return !(a == b);
    }

    void swap(byte_buffer& other) noexcept;

private:
    std::unique_ptr<std::byte[]> data_;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
};

inline void swap(byte_buffer& a, byte_buffer& b) noexcept { a.swap(b); }

} // namespace fs
} // namespace crpc

#endif // CRPC_FS_MEMORY_HPP