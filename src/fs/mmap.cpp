#include <crpc/fs/mmap.hpp>
#include <crpc/fs/file.hpp>

#include <cstddef>
#include <utility>

#include "os.hpp"

#if defined(CRPC_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <memoryapi.h>
#include <sysinfoapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace crpc {
namespace fs {
namespace {

struct range {
    u64 offset = 0;
    u64 size = 0;
};

// Validates and normalises [offset, size] against the file size. A zero `size`
// maps to EOF. An out-of-range request is a value error -- never a truncated
// or out-of-bounds map.
result<range> checked_range(u64 file_size, u64 offset, u64 size) {
    range out;
    out.offset = offset;
    if (offset > file_size) {
        return rpc_error(rpc_code::invalid_argument,
                         "fs: mmap: offset " + std::to_string(offset) +
                             " is beyond the file size " +
                             std::to_string(file_size));
    }
    out.size = size == 0 ? file_size - offset : size;
    if (out.size > file_size - offset) {
        return rpc_error(rpc_code::invalid_argument,
                         "fs: mmap: range [" + std::to_string(offset) + ", " +
                             std::to_string(offset + out.size) +
                             ") exceeds the file size " + std::to_string(file_size));
    }
    return out;
}

} // namespace

struct memory_mapped_file::impl {
    impl() = default;
    ~impl() {
#if defined(CRPC_OS_WINDOWS)
        if (view != nullptr) {
            (void)UnmapViewOfFile(view);
        }
        if (mapping != nullptr) {
            (void)CloseHandle(mapping);
        }
        if (file != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(file);
        }
#else
        if (mapped != nullptr) {
            (void)::munmap(mapped, mapped_len);
        }
        if (fd >= 0) {
            (void)::close(fd);
        }
#endif
    }
    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

#if defined(CRPC_OS_WINDOWS)
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    std::byte* view = nullptr;   // page-aligned base of the view
    std::size_t view_len = 0;
#else
    int fd = -1;
    std::byte* mapped = nullptr; // page-aligned base of the mapping
    std::size_t mapped_len = 0;
#endif
};

memory_mapped_file::~memory_mapped_file() = default;

memory_mapped_file::memory_mapped_file(memory_mapped_file&& other) noexcept
    : impl_(std::move(other.impl_)),
      base_(other.base_),
      size_(other.size_),
      writable_(other.writable_) {
    other.base_ = nullptr;
    other.size_ = 0;
    other.writable_ = false;
}

memory_mapped_file& memory_mapped_file::operator=(memory_mapped_file&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        base_ = other.base_;
        size_ = other.size_;
        writable_ = other.writable_;
        other.base_ = nullptr;
        other.size_ = 0;
        other.writable_ = false;
    }
    return *this;
}

result<memory_mapped_file> memory_mapped_file::open_read_only(const path& p) {
    return open_read_only(p, 0, 0);
}

result<memory_mapped_file> memory_mapped_file::open_read_only(const path& p, u64 offset,
                                                              u64 size) {
    result<file_info> info = stat(p);
    if (!info) {
        return info.error();
    }
    if (info.value().is_directory) {
        return rpc_error(rpc_code::invalid_argument,
                         "fs: mmap: cannot memory-map a directory: '" + p.str() + "'");
    }
    result<range> r = checked_range(info.value().size, offset, size);
    if (!r) {
        return r.error();
    }
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = detail::utf8_to_wide(p.str());
    if (!w) {
        return w.error();
    }
    HANDLE file = CreateFileW(w.value().c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return detail::classify_win32(GetLastError(), "mmap: open", p.str());
    }
    std::unique_ptr<impl> state(new (std::nothrow) impl());
    if (state == nullptr) {
        CloseHandle(file);
        return rpc_error(rpc_code::resource_exhausted, "fs: mmap: out of memory");
    }
    state->file = file;
    if (r.value().size == 0) {
        memory_mapped_file empty;
        empty.impl_ = std::move(state);
        empty.size_ = 0;
        return empty;
    }

    SYSTEM_INFO sys = {};
    GetSystemInfo(&sys);
    const u64 gran = sys.dwAllocationGranularity;
    const u64 aligned = r.value().offset - (r.value().offset % gran);
    const std::size_t delta = static_cast<std::size_t>(r.value().offset - aligned);
    const std::size_t map_len = static_cast<std::size_t>(r.value().size) + delta;

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        return detail::classify_win32(GetLastError(), "mmap: create mapping", p.str());
    }
    state->mapping = mapping;
    const DWORD high = static_cast<DWORD>(aligned >> 32);
    const DWORD low = static_cast<DWORD>(aligned & 0xFFFFFFFFull);
    std::byte* view =
        static_cast<std::byte*>(MapViewOfFile(mapping, FILE_MAP_READ, high, low, map_len));
    if (view == nullptr) {
        return detail::classify_win32(GetLastError(), "mmap: map view", p.str());
    }
    state->view = view;
    state->view_len = map_len;
    memory_mapped_file out;
    out.impl_ = std::move(state);
    out.base_ = view + delta;
    out.size_ = r.value().size;
    out.writable_ = false;
    return out;
#else
    const int fd = ::open(p.str().c_str(), O_RDONLY);
    if (fd < 0) {
        return detail::classify_errno(errno, "mmap: open", p.str());
    }
    std::unique_ptr<impl> state(new (std::nothrow) impl());
    if (state == nullptr) {
        (void)::close(fd);
        return rpc_error(rpc_code::resource_exhausted, "fs: mmap: out of memory");
    }
    state->fd = fd;
    if (r.value().size == 0) {
        memory_mapped_file empty;
        empty.impl_ = std::move(state);
        empty.size_ = 0;
        return empty;
    }
    const long page = ::sysconf(_SC_PAGESIZE);
    const u64 gran = page > 0 ? static_cast<u64>(page) : 4096u;
    const u64 aligned = r.value().offset - (r.value().offset % gran);
    const std::size_t delta = static_cast<std::size_t>(r.value().offset - aligned);
    const std::size_t map_len = static_cast<std::size_t>(r.value().size) + delta;
    void* mapped =
        ::mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(aligned));
    if (mapped == MAP_FAILED) {
        return detail::classify_errno(errno, "mmap: map view", p.str());
    }
    state->mapped = static_cast<std::byte*>(mapped);
    state->mapped_len = map_len;
    memory_mapped_file out;
    out.impl_ = std::move(state);
    out.base_ = static_cast<std::byte*>(mapped) + delta;
    out.size_ = r.value().size;
    out.writable_ = false;
    return out;
#endif
}

result<memory_mapped_file> memory_mapped_file::open_read_write(const path& p, u64 offset,
                                                               u64 size) {
    result<file_info> info = stat(p);
    if (!info) {
        return info.error();
    }
    if (info.value().is_directory) {
        return rpc_error(rpc_code::invalid_argument,
                         "fs: mmap: cannot memory-map a directory: '" + p.str() + "'");
    }
    if (info.value().is_read_only) {
        return rpc_error(rpc_code::permission_denied,
                         "fs: mmap: read-write map of a read-only file: '" + p.str() + "'");
    }
    result<range> r = checked_range(info.value().size, offset, size);
    if (!r) {
        return r.error();
    }
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = detail::utf8_to_wide(p.str());
    if (!w) {
        return w.error();
    }
    HANDLE file = CreateFileW(w.value().c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return detail::classify_win32(GetLastError(), "mmap: open write", p.str());
    }
    std::unique_ptr<impl> state(new (std::nothrow) impl());
    if (state == nullptr) {
        CloseHandle(file);
        return rpc_error(rpc_code::resource_exhausted, "fs: mmap: out of memory");
    }
    state->file = file;
    if (r.value().size == 0) {
        memory_mapped_file empty;
        empty.impl_ = std::move(state);
        empty.size_ = 0;
        empty.writable_ = true;
        return empty;
    }
    SYSTEM_INFO sys = {};
    GetSystemInfo(&sys);
    const u64 gran = sys.dwAllocationGranularity;
    const u64 aligned = r.value().offset - (r.value().offset % gran);
    const std::size_t delta = static_cast<std::size_t>(r.value().offset - aligned);
    const std::size_t map_len = static_cast<std::size_t>(r.value().size) + delta;
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (mapping == nullptr) {
        return detail::classify_win32(GetLastError(), "mmap: create write mapping",
                                      p.str());
    }
    state->mapping = mapping;
    const DWORD high = static_cast<DWORD>(aligned >> 32);
    const DWORD low = static_cast<DWORD>(aligned & 0xFFFFFFFFull);
    std::byte* view = static_cast<std::byte*>(
        MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, high, low, map_len));
    if (view == nullptr) {
        return detail::classify_win32(GetLastError(), "mmap: map write view", p.str());
    }
    state->view = view;
    state->view_len = map_len;
    memory_mapped_file out;
    out.impl_ = std::move(state);
    out.base_ = view + delta;
    out.size_ = r.value().size;
    out.writable_ = true;
    return out;
#else
    const int fd = ::open(p.str().c_str(), O_RDWR);
    if (fd < 0) {
        return detail::classify_errno(errno, "mmap: open write", p.str());
    }
    std::unique_ptr<impl> state(new (std::nothrow) impl());
    if (state == nullptr) {
        (void)::close(fd);
        return rpc_error(rpc_code::resource_exhausted, "fs: mmap: out of memory");
    }
    state->fd = fd;
    if (r.value().size == 0) {
        memory_mapped_file empty;
        empty.impl_ = std::move(state);
        empty.size_ = 0;
        empty.writable_ = true;
        return empty;
    }
    const long page = ::sysconf(_SC_PAGESIZE);
    const u64 gran = page > 0 ? static_cast<u64>(page) : 4096u;
    const u64 aligned = r.value().offset - (r.value().offset % gran);
    const std::size_t delta = static_cast<std::size_t>(r.value().offset - aligned);
    const std::size_t map_len = static_cast<std::size_t>(r.value().size) + delta;
    void* mapped = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                          static_cast<off_t>(aligned));
    if (mapped == MAP_FAILED) {
        return detail::classify_errno(errno, "mmap: map write view", p.str());
    }
    state->mapped = static_cast<std::byte*>(mapped);
    state->mapped_len = map_len;
    memory_mapped_file out;
    out.impl_ = std::move(state);
    out.base_ = static_cast<std::byte*>(mapped) + delta;
    out.size_ = r.value().size;
    out.writable_ = true;
    return out;
#endif
}

status memory_mapped_file::flush() {
    if (base_ == nullptr || size_ == 0 || !writable_) {
        return status(); // nothing mapped, empty view, or nothing to flush
    }
#if defined(CRPC_OS_WINDOWS)
    if (!FlushViewOfFile(impl_->view, impl_->view_len)) {
        return detail::classify_win32(GetLastError(), "mmap: flush", "");
    }
    if (!FlushFileBuffers(impl_->file)) {
        return detail::classify_win32(GetLastError(), "mmap: flush buffers", "");
    }
    return status();
#else
    if (::msync(impl_->mapped, impl_->mapped_len, MS_SYNC) != 0) {
        return detail::classify_errno(errno, "mmap: msync", "");
    }
    return status();
#endif
}

} // namespace fs
} // namespace crpc