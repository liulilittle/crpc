#include <crpc/fs/memory.hpp>

#include <cstddef>
#include <cstring>
#include <new>
#include <string>
#include <utility>

namespace crpc {
namespace fs {
namespace {

constexpr std::size_t kMinGrowth = 64;

status alloc_failed(std::size_t size) {
    return rpc_error(rpc_code::resource_exhausted,
                     "fs: byte_buffer: allocation failed (" +
                         std::to_string(size) + " bytes)");
}

} // namespace

byte_buffer::byte_buffer(std::size_t initial_capacity) {
    if (initial_capacity != 0) {
        (void)reserve(initial_capacity); // best effort; a failure leaves capacity 0
    }
}

byte_buffer::byte_buffer(const char* text) : byte_buffer(std::string_view(text)) {}

byte_buffer::byte_buffer(std::string text)
    : byte_buffer(span<const std::byte>(
          reinterpret_cast<const std::byte*>(text.data()), text.size())) {}

byte_buffer::byte_buffer(std::string_view text)
    : byte_buffer(span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                        text.size())) {}

byte_buffer::byte_buffer(span<const std::byte> data) {
    if (!data.empty()) {
        (void)append(data);
    }
}

span<const std::byte> byte_buffer::slice(std::size_t offset,
                                         std::size_t count) const noexcept {
    if (offset >= size_) {
        return span<const std::byte>();
    }
    const std::size_t available = size_ - offset;
    return span<const std::byte>(data_.get() + offset,
                                 count == npos || count > available ? available : count);
}

status byte_buffer::reserve(std::size_t new_capacity) {
    if (new_capacity <= capacity_) {
        return status();
    }
    std::unique_ptr<std::byte[]> next(new (std::nothrow) std::byte[new_capacity]);
    if (next == nullptr) {
        return alloc_failed(new_capacity);
    }
    if (size_ != 0) {
        std::memcpy(next.get(), data_.get(), size_);
    }
    data_ = std::move(next);
    capacity_ = new_capacity;
    return status();
}

status byte_buffer::append(span<const std::byte> data) {
    if (data.empty()) {
        return status();
    }
    const std::size_t needed = size_ + data.size();
    if (needed < size_) {
        return alloc_failed(data.size()); // size_t overflow guard
    }
    if (needed > capacity_) {
        // Geometric growth is the buffer's job; never below what is needed.
        std::size_t target =
            capacity_ > (std::size_t)-1 / 2 ? needed : capacity_ + capacity_ / 2;
        if (target < needed) {
            target = needed;
        }
        status grew = reserve(target);
        if (!grew) {
            return grew;
        }
    }
    std::memcpy(data_.get() + size_, data.data(), data.size());
    size_ = needed;
    return status();
}

status byte_buffer::append(const char* text) {
    if (text == nullptr) {
        return status();
    }
    return append(std::string_view(text));
}

status byte_buffer::append(std::string_view text) {
    return append(span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                        text.size()));
}

status byte_buffer::append(std::byte value) {
    if (size_ + 1 > capacity_) {
        status grew = reserve(capacity_ == 0 ? kMinGrowth : capacity_ + capacity_ / 2);
        if (!grew) {
            return grew;
        }
    }
    data_[size_++] = value;
    return status();
}

void byte_buffer::erase_front(std::size_t count) noexcept {
    if (count >= size_) {
        size_ = 0;
        return;
    }
    std::memmove(data_.get(), data_.get() + count, size_ - count);
    size_ -= count;
}

std::string byte_buffer::to_string() const {
    return std::string(reinterpret_cast<const char*>(data_.get()), size_);
}

status byte_buffer::assign(std::string_view text) {
    clear();
    return append(text);
}

bool operator==(const byte_buffer& a, const byte_buffer& b) noexcept {
    if (a.size_ != b.size_) {
        return false;
    }
    return a.size_ == 0 ||
           std::memcmp(a.data_.get(), b.data_.get(), a.size_) == 0;
}

void byte_buffer::swap(byte_buffer& other) noexcept {
    using std::swap;
    swap(data_, other.data_);
    swap(size_, other.size_);
    swap(capacity_, other.capacity_);
}

} // namespace fs
} // namespace crpc