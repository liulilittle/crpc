// CRPC -- growable byte buffer.
//
// The buffer is the library's only dynamic byte container. Two properties make
// it suitable for the hot path:
//
//   * the initial capacity is chosen by the caller; a buffer that stays under
//     its inline-free small capacity performs exactly one allocation for its
//     lifetime,
//   * `clear()` keeps capacity, so a per-connection or per-shard buffer can be
//     reused for the life of the connection without churn.
//
// Growth is geometric (1.5x, not 2x) to keep peak memory close to the live size
// while still amortising copies. A caller that knows the final size must call
// `reserve()` first; the buffer never assumes it can grow without bound, and
// every growth is checked against a caller-supplied maximum.
#ifndef CRPC_BUFFER_HPP
#define CRPC_BUFFER_HPP

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#include <crpc/config.hpp>
#include <crpc/span.hpp>

namespace crpc {

class buffer {
public:
    // The default ceiling protects against a length field that decodes to
    // something absurd before the frame codec has had a chance to validate it.
    // Callers that legitimately need more must say so explicitly.
    static constexpr std::size_t default_max_capacity = 64u * 1024u * 1024u;

    explicit buffer(std::size_t initial_capacity = 0,
                    std::size_t max_capacity = default_max_capacity)
        : max_capacity_(max_capacity) {
        if (initial_capacity > max_capacity_) {
            initial_capacity = max_capacity_;
        }
        if (initial_capacity != 0) {
            data_.reset(new (std::nothrow) std::byte[initial_capacity]);
            if (data_) {
                capacity_ = initial_capacity;
            }
        }
    }

    buffer(buffer&& other) noexcept
        : data_(std::move(other.data_)),
          size_(other.size_),
          capacity_(other.capacity_),
          max_capacity_(other.max_capacity_),
          failed_(other.failed_) {
        other.size_ = 0;
        other.capacity_ = 0;
    }

    buffer& operator=(buffer&& other) noexcept {
        if (this != &other) {
            data_ = std::move(other.data_);
            size_ = other.size_;
            capacity_ = other.capacity_;
            max_capacity_ = other.max_capacity_;
            failed_ = other.failed_;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    buffer(const buffer&) = delete;
    buffer& operator=(const buffer&) = delete;

    std::byte* data() noexcept { return data_.get(); }
    const std::byte* data() const noexcept { return data_.get(); }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }
    bool empty() const noexcept { return size_ == 0; }

    // True when an allocation has failed. The buffer stays in a consistent
    // state (its existing contents are intact); the caller must surface the
    // failure rather than silently truncating.
    bool failed() const noexcept { return failed_; }

    span<std::byte> writable() noexcept {
        if (data_ == nullptr || size_ == capacity_) {
            return span<std::byte>();
        }
        return span<std::byte>(data_.get() + size_, capacity_ - size_);
    }

    span<const std::byte> readable() const noexcept {
        return span<const std::byte>(data_.get(), size_);
    }

    void clear() noexcept { size_ = 0; }

    // Reserves room for `extra` more bytes past the current size. Returns false
    // when the request would exceed max_capacity or the allocation failed; in
    // both cases the buffer is unchanged apart from `failed()`.
    bool reserve_extra(std::size_t extra) {
        if (extra > max_capacity_ || size_ > max_capacity_ - extra) {
            failed_ = true;
            return false;
        }
        const std::size_t needed = size_ + extra;
        if (needed <= capacity_) {
            return true;
        }
        return grow_to(needed);
    }

    bool reserve(std::size_t total) { return reserve_extra(total > size_ ? total - size_ : 0); }

    // Appends bytes. Returns false (and leaves the buffer unchanged) when the
    // append would exceed the ceiling or the allocation failed.
    bool append(span<const std::byte> bytes) {
        if (bytes.empty()) {
            return true;
        }
        if (!reserve_extra(bytes.size())) {
            return false;
        }
        std::memcpy(data_.get() + size_, bytes.data(), bytes.size());
        size_ += bytes.size();
        return true;
    }

    bool append(const void* src, std::size_t count) {
        return append(span<const std::byte>(static_cast<const std::byte*>(src), count));
    }

    // Appends a single byte. Used by the varint encoder.
    bool push_back(std::byte value) {
        if (!reserve_extra(1)) {
            return false;
        }
        data_[size_++] = value;
        return true;
    }

    // Removes `count` bytes from the front. Used after a frame has been
    // consumed. Shifts the remainder down; callers that read many frames in a
    // row should instead track a read offset (see byte_reader) and call
    // `consume()` once per batch.
    void erase_front(std::size_t count) noexcept {
        if (count >= size_) {
            size_ = 0;
            return;
        }
        std::memmove(data_.get(), data_.get() + count, size_ - count);
        size_ -= count;
    }

    // Releases the heap block. The buffer returns to the empty state and may be
    // reused; this is how a pool hands memory back during an idle window.
    void release() noexcept {
        data_.reset();
        size_ = 0;
        capacity_ = 0;
        failed_ = false;
    }

private:
    bool grow_to(std::size_t needed) {
        std::size_t target = capacity_ == 0 ? 64 : capacity_ + capacity_ / 2;
        if (target < needed) {
            target = needed;
        }
        if (target > max_capacity_) {
            target = max_capacity_;
        }
        if (target < needed) {
            failed_ = true;
            return false;
        }
        std::unique_ptr<std::byte[]> next(new (std::nothrow) std::byte[target]);
        if (!next) {
            failed_ = true;
            return false;
        }
        if (size_ != 0) {
            std::memcpy(next.get(), data_.get(), size_);
        }
        data_ = std::move(next);
        capacity_ = target;
        return true;
    }

    std::unique_ptr<std::byte[]> data_;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
    std::size_t max_capacity_;
    bool failed_ = false;
};

} // namespace crpc

#endif // CRPC_BUFFER_HPP
