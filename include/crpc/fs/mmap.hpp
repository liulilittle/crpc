// CRPC -- memory-mapped files.
//
// A `memory_mapped_file` maps a range of a file into address space. Mapping a
// file is the only way to read it with zero bytes copied by the framework, at
// the price of lifetime discipline, which this type owns for the caller:
//
//   * the map is released by the destructor (RAII), and the file handle is
//     closed with it;
//   * every open validates its range against the file size and returns
//     `invalid_argument` for an out-of-range request -- out-of-bounds access is
//     impossible by construction, not merely "documented";
//   * a read-write map of a read-only file is `permission_denied`, and writes
//     become visible to other readers only after `flush()` (or at unmap);
//   * an empty map (zero-length file, or a zero-length range at EOF) is a valid
//     empty view, not an error.
//
// The map is read-only (`bytes()`) or read-write (`writable_bytes()`); the
// writable span is requestable only on a map opened read-write.
#ifndef CRPC_FS_MMAP_HPP
#define CRPC_FS_MMAP_HPP

#include <cstddef>
#include <memory>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>
#include <crpc/fs/path.hpp>

namespace crpc {
namespace fs {

class CRPC_API memory_mapped_file {
public:
    memory_mapped_file() = default;
    ~memory_mapped_file();

    memory_mapped_file(const memory_mapped_file&) = delete;
    memory_mapped_file& operator=(const memory_mapped_file&) = delete;
    memory_mapped_file(memory_mapped_file&& other) noexcept;
    memory_mapped_file& operator=(memory_mapped_file&& other) noexcept;

    // Maps the whole file read-only.
    static result<memory_mapped_file> open_read_only(const path& p);

    // Maps `[offset, offset + size)` read-only. `size == 0` maps from `offset`
    // to EOF. `offset` may lie at EOF with a zero `size` (empty map).
    static result<memory_mapped_file> open_read_only(const path& p, u64 offset,
                                                     u64 size);

    // Maps `[offset, offset + size)` read-write (whole file when `size == 0`).
    static result<memory_mapped_file> open_read_write(const path& p, u64 offset = 0,
                                                      u64 size = 0);

    // Read-only view of the mapped range. Valid until this object is destroyed
    // or moved-from.
    span<const std::byte> bytes() const noexcept {
        return span<const std::byte>(base_, static_cast<std::size_t>(size_));
    }

    // Read-write view; available only on a read-write map (empty otherwise).
    // A write is not durable until `flush()` or unmap.
    span<std::byte> writable_bytes() noexcept {
        return span<std::byte>(base_, writable_ ? static_cast<std::size_t>(size_) : 0);
    }

    u64 size() const noexcept { return size_; }
    bool writable() const noexcept { return writable_; }
    bool valid() const noexcept { return base_ != nullptr || size_ == 0; }

    // Flushes the mapped range to stable storage. A no-op for an empty map and
    // for a read-only map (nothing to flush).
    status flush();

private:
    struct impl;
    std::unique_ptr<impl> impl_;
    std::byte* base_ = nullptr;
    u64 size_ = 0;
    bool writable_ = false;
};

} // namespace fs
} // namespace crpc

#endif // CRPC_FS_MMAP_HPP