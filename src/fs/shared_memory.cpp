#include <crpc/fs/shared_memory.hpp>

#include <cstring>
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

// The rules are the intersection of what Windows object names and POSIX shm names
// accept, so a name that passes here is portable.
constexpr std::size_t k_max_name = 96;

// Written into the mapping by its creator so every peer learns the length the
// creator asked for. Windows rounds the mapping to a page and has no supported way
// to recover the request afterwards; POSIX reports the ftruncate size exactly. The
// header makes both platforms answer with the creator's number.
//
// Layout: magic (u64), length (u64), then the payload. 16 bytes keeps the payload
// naturally aligned for every type a caller might place there.
constexpr u64 k_magic = 0x4352504353484D31ull; // "CRPCSHM1"
constexpr std::size_t k_header = 16;

[[nodiscard]] bool is_name_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

#if defined(CRPC_OS_WINDOWS)
[[nodiscard]] std::string object_name(std::string_view name) {
    // `Local\` scopes the object to this session; without it the name lands in the
    // global namespace and needs a privilege that a normal process may not have.
    return "Local\\" + std::string(name);
}
[[nodiscard]] std::wstring widen(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}
#else
[[nodiscard]] std::string object_name(std::string_view name) {
    return "/" + std::string(name);
}
#endif

// Sets the header on a freshly created mapping.
void write_header(std::byte* base, u64 length) noexcept {
    std::memcpy(base, &k_magic, sizeof(k_magic));
    std::memcpy(base + sizeof(k_magic), &length, sizeof(length));
}

// Reads the header; false when the mapping was not built by this class (a stale
// object left by another program under the same name), which is a real condition and
// must not be reported as a length.
[[nodiscard]] bool read_header(const std::byte* base, u64& length) noexcept {
    u64 magic = 0;
    std::memcpy(&magic, base, sizeof(magic));
    if (magic != k_magic) {
        return false;
    }
    std::memcpy(&length, base + sizeof(magic), sizeof(length));
    return true;
}

} // namespace

struct shared_memory::impl {
#if defined(CRPC_OS_WINDOWS)
    HANDLE mapping = nullptr;
    // The mapping's page-rounded size, needed to unmap and to guard the header.
    u64 mapped_size = 0;
#else
    int fd = -1;
    u64 mapped_size = 0;
#endif
};


// Defined after impl is complete so both the destructor and the move-assignment can
// use it without seeing an incomplete type.
void shared_memory::release(std::byte* base, impl* state) noexcept {
#if defined(CRPC_OS_WINDOWS)
    if (base != nullptr) {
        ::UnmapViewOfFile(base);
    }
    if (state != nullptr && state->mapping != nullptr) {
        ::CloseHandle(state->mapping);
    }
#else
    if (state != nullptr && base != nullptr) {
        ::munmap(base, static_cast<std::size_t>(state->mapped_size));
    }
    if (state != nullptr && state->fd >= 0) {
        ::close(state->fd);
    }
#endif
}

status validate_shared_name(std::string_view name) {
    if (name.empty()) {
        return rpc_error{rpc_code::invalid_argument, "a shared-memory name is empty"};
    }
    if (name.size() > k_max_name) {
        return rpc_error{rpc_code::invalid_argument,
                         "a shared-memory name is longer than 96 bytes"};
    }
    if (name.front() == '.') {
        // A leading dot is a hidden file in /dev/shm on POSIX, and "." / ".." are
        // directory entries; refusing the class keeps both platforms agreeing.
        return rpc_error{rpc_code::invalid_argument,
                         "a shared-memory name may not start with a dot"};
    }
    for (const char c : name) {
        if (!is_name_char(c)) {
            // Also rejects an embedded NUL, which would truncate the name silently at
            // the C boundary.
            return rpc_error{rpc_code::invalid_argument,
                             "a shared-memory name may contain only letters, digits, "
                             "'.', '_' and '-'"};
        }
    }
    return {};
}

bool remove_unlinks_the_name() noexcept {
#if defined(CRPC_OS_WINDOWS)
    return false;
#else
    return true;
#endif
}

shared_memory::shared_memory() = default;

shared_memory::~shared_memory() {
    if (impl_) {
        shared_memory::release(base_, impl_.get());
    }
}

shared_memory::shared_memory(shared_memory&& other) noexcept
    : impl_(std::move(other.impl_)),
      base_(other.base_),
      size_(other.size_),
      name_(std::move(other.name_)) {
    other.base_ = nullptr;
    other.size_ = 0;
    other.name_.clear();
}

shared_memory& shared_memory::operator=(shared_memory&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (impl_) {
        shared_memory::release(base_, impl_.get());
    }
    impl_ = std::move(other.impl_);
    base_ = other.base_;
    size_ = other.size_;
    name_ = std::move(other.name_);
    other.base_ = nullptr;
    other.size_ = 0;
    other.name_.clear();
    return *this;
}

result<shared_memory> shared_memory::open(std::string_view name, u64 size,
                                          shared_memory_mode mode) {
    const status valid = validate_shared_name(name);
    if (!valid) {
        return valid.error();
    }
    // `size` is only consulted when creating, so it is only required there. A
    // zero-length mapping would also leave no room for the header.
    if (mode != shared_memory_mode::open_existing && size == 0) {
        return rpc_error{rpc_code::invalid_argument,
                         "a shared-memory mapping may not be created with zero size"};
    }
    // The payload plus the header, rounded up to the platform page so neither
    // platform ends up with a mapping smaller than the header.
    const u64 payload = (mode == shared_memory_mode::open_existing) ? 0 : size;

    const std::string native = object_name(name);
    shared_memory out;
    out.impl_.reset(new impl());
    out.name_.assign(name.data(), name.size());

#if defined(CRPC_OS_WINDOWS)
    SYSTEM_INFO sys{};
    ::GetSystemInfo(&sys);
    const u64 page = sys.dwPageSize != 0 ? sys.dwPageSize : 4096ull;
    const u64 request = payload == 0 ? 0 : ((k_header + payload + page - 1) / page) * page;
    const DWORD high = static_cast<DWORD>(request >> 32);
    const DWORD low = static_cast<DWORD>(request & 0xffffffffull);
    DWORD access = FILE_MAP_ALL_ACCESS;
    HANDLE mapping = nullptr;
    bool created = false;

    {
        const std::wstring wide = widen(native);
        switch (mode) {
        case shared_memory_mode::create:
            mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                           high, low, wide.c_str());
            if (mapping != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS) {
                ::CloseHandle(mapping);
                return rpc_error{rpc_code::failed_precondition,
                                 "a shared-memory mapping with this name already exists"};
            }
            created = mapping != nullptr;
            break;
        case shared_memory_mode::open_existing:
            mapping = ::OpenFileMappingW(access, FALSE, wide.c_str());
            break;
        case shared_memory_mode::open_or_create:
            mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                           high, low, wide.c_str());
            // ERROR_ALREADY_EXISTS is the ordinary path here, not a failure.
            created = mapping != nullptr && ::GetLastError() != ERROR_ALREADY_EXISTS;
            break;
        }
        if (mapping == nullptr) {
            const DWORD code = ::GetLastError();
            return detail::classify_win32(code, "open shared memory", std::string(name));
        }
    }

    void* view = ::MapViewOfFile(mapping, access, 0, 0, 0);
    if (view == nullptr) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(mapping);
        return detail::classify_win32(code, "map shared memory", std::string(name));
    }
    std::byte* base = static_cast<std::byte*>(view);

    if (created) {
        write_header(base, payload);
        out.impl_->mapped_size = request;
        out.base_ = base + k_header;
        out.size_ = payload;
    } else {
        u64 length = 0;
        if (!read_header(base, length)) {
            ::UnmapViewOfFile(view);
            ::CloseHandle(mapping);
            return rpc_error{rpc_code::failed_precondition,
                             "the existing mapping was not created by this class "
                             "(its header is missing)"};
        }
        // The page-rounded size is not stored in the mapping, so read it back from
        // the object; Windows reports the exact creation size through the handle.
        MEMORY_BASIC_INFORMATION info{};
        u64 mapped = length + k_header;
        if (::VirtualQuery(view, &info, sizeof(info)) != 0) {
            mapped = static_cast<u64>(info.RegionSize);
        }
        out.impl_->mapped_size = mapped;
        out.base_ = base + k_header;
        out.size_ = length;
    }
    out.impl_->mapping = mapping;
#else
    int flags = O_RDWR;
    switch (mode) {
    case shared_memory_mode::create:
        flags |= O_CREAT | O_EXCL;
        break;
    case shared_memory_mode::open_existing:
        break;
    case shared_memory_mode::open_or_create:
        flags |= O_CREAT;
        break;
    }
    const int fd = ::shm_open(native.c_str(), flags, 0600);
    if (fd < 0) {
        return detail::classify_errno(errno, "open shared memory", std::string(name));
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int code = errno;
        ::close(fd);
        return detail::classify_errno(code, "stat shared memory", std::string(name));
    }
    const bool fresh = st.st_size == 0;
    if (fresh) {
        if (mode == shared_memory_mode::open_existing) {
            ::close(fd);
            return rpc_error{rpc_code::failed_precondition,
                             "the existing mapping has no header (zero length)"};
        }
        if (::ftruncate(fd, static_cast<off_t>(k_header + payload)) != 0) {
            const int code = errno;
            ::close(fd);
            if (mode == shared_memory_mode::create) {
                (void)::shm_unlink(native.c_str());
            }
            return detail::classify_errno(code, "size shared memory", std::string(name));
        }
    }
    const u64 mapped = fresh ? static_cast<u64>(k_header + payload)
                             : static_cast<u64>(st.st_size);
    void* view = ::mmap(nullptr, static_cast<std::size_t>(mapped),
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        const int code = errno;
        ::close(fd);
        if (mode == shared_memory_mode::create) {
            (void)::shm_unlink(native.c_str());
        }
        return detail::classify_errno(code, "map shared memory", std::string(name));
    }
    std::byte* base = static_cast<std::byte*>(view);
    if (fresh) {
        write_header(base, payload);
        out.size_ = payload;
    } else {
        u64 length = 0;
        if (!read_header(base, length)) {
            ::munmap(view, static_cast<std::size_t>(mapped));
            ::close(fd);
            return rpc_error{rpc_code::failed_precondition,
                             "the existing mapping was not created by this class "
                             "(its header is missing)"};
        }
        out.size_ = length;
    }
    out.impl_->fd = fd;
    out.impl_->mapped_size = mapped;
    out.base_ = base + k_header;
#endif

    return out;
}

status shared_memory::remove(std::string_view name) {
    const status valid = validate_shared_name(name);
    if (!valid) {
        return valid;
    }
    const std::string native = object_name(name);
#if defined(CRPC_OS_WINDOWS)
    // Windows has no name to unlink: the object lives while a handle is open, so
    // this reports whether it is there and leaves the lifetime to the handles. The
    // header says so; `remove_unlinks_the_name()` is what a caller checks when the
    // difference matters.
    const std::wstring wide = widen(native);
    HANDLE probe = ::OpenFileMappingW(FILE_MAP_READ, FALSE, wide.c_str());
    if (probe == nullptr) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return rpc_error{rpc_code::not_found,
                             "no shared-memory mapping with this name exists"};
        }
        return detail::classify_win32(code, "remove shared memory", std::string(name));
    }
    ::CloseHandle(probe);
    return {};
#else
    if (::shm_unlink(native.c_str()) != 0) {
        return detail::classify_errno(errno, "remove shared memory", std::string(name));
    }
    return {};
#endif
}

result<bool> shared_memory::exists(std::string_view name) {
    const status valid = validate_shared_name(name);
    if (!valid) {
        return valid.error();
    }
    const std::string native = object_name(name);
#if defined(CRPC_OS_WINDOWS)
    const std::wstring wide = widen(native);
    HANDLE probe = ::OpenFileMappingW(FILE_MAP_READ, FALSE, wide.c_str());
    if (probe == nullptr) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        return detail::classify_win32(code, "probe shared memory", std::string(name));
    }
    ::CloseHandle(probe);
    return true;
#else
    const int fd = ::shm_open(native.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        if (errno == ENOENT) {
            return false;
        }
        return detail::classify_errno(errno, "probe shared memory", std::string(name));
    }
    ::close(fd);
    return true;
#endif
}

} // namespace fs
} // namespace crpc
