// CRPC -- G19 step1: durable log implementation.
//
// On-disk framing (little-endian, 32-byte fixed header):
//
//   0   4  magic   'CRLG'
//   4   1  version 1
//   5   2  reserved (zero)
//   7   1  kind
//   8   8  term
//  16   8  index
//  24   4  payload_len
//  28   4  crc32c   over kind|term|index|payload_len|payload
//  32   N  payload
//
// The CRC deliberately excludes magic/version/reserved/crc so a corrupted
// header field cannot be masked by a checksum over itself; it covers every byte
// that carries meaning plus the whole payload. That is what makes a torn tail
// (short read, no error) distinguishable from a corrupted record (bad CRC,
// error).
#include <crpc/cluster/registry_log.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include <crpc/frame.hpp>

#include "../fs/os.hpp"

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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace crpc {
namespace cluster {

namespace {

constexpr u32 kMagic = 0x474C5243u; // "CRLG" in little-endian byte order
constexpr std::byte kVersion{1};
constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kCrcPrefix = 21; // kind(1) + term(8) + index(8) + len(4)

void put_u32(std::byte* p, u32 v) noexcept {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    }
}

void put_u64(std::byte* p, u64 v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFull);
    }
}

u32 get_u32(const std::byte* p) noexcept {
    u32 v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<u32>(std::to_integer<unsigned>(p[i])) << (8 * i);
    }
    return v;
}

u64 get_u64(const std::byte* p) noexcept {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<u64>(std::to_integer<unsigned>(p[i])) << (8 * i);
    }
    return v;
}

std::vector<std::byte> encode_record(const log_record& record) {
    std::vector<std::byte> crc_input(kCrcPrefix + record.payload.size());
    crc_input[0] = static_cast<std::byte>(static_cast<u8>(record.kind));
    put_u64(crc_input.data() + 1, record.term);
    put_u64(crc_input.data() + 9, record.index);
    put_u32(crc_input.data() + 17, static_cast<u32>(record.payload.size()));
    if (!record.payload.empty()) {
        std::memcpy(crc_input.data() + kCrcPrefix, record.payload.data(),
                    record.payload.size());
    }
    const u32 crc = crc32c(span<const std::byte>(crc_input.data(),
                                                 crc_input.size()));

    std::vector<std::byte> out(kHeaderSize + record.payload.size());
    put_u32(out.data(), kMagic);
    out[4] = kVersion;
    out[5] = std::byte{0};
    out[6] = std::byte{0};
    out[7] = static_cast<std::byte>(static_cast<u8>(record.kind));
    put_u64(out.data() + 8, record.term);
    put_u64(out.data() + 16, record.index);
    put_u32(out.data() + 24, static_cast<u32>(record.payload.size()));
    put_u32(out.data() + 28, crc);
    if (!record.payload.empty()) {
        std::memcpy(out.data() + kHeaderSize, record.payload.data(),
                    record.payload.size());
    }
    return out;
}

enum class parse_result { ok, torn, corrupt };

// Parses one record at `offset`. `torn` means fewer bytes than a whole record
// remain (power loss mid-write); `corrupt` means a complete record failed a
// check (magic, version or CRC).
parse_result parse_record(const std::vector<std::byte>& bytes, std::size_t offset,
                          log_record& out, std::size_t& consumed) {
    const std::size_t available = bytes.size() - offset;
    if (available < kHeaderSize) {
        return parse_result::torn;
    }
    const std::byte* p = bytes.data() + offset;
    if (get_u32(p) != kMagic) {
        return parse_result::corrupt;
    }
    if (p[4] != kVersion || p[5] != std::byte{0} || p[6] != std::byte{0}) {
        return parse_result::corrupt;
    }
    const std::size_t payload_len = get_u32(p + 24);
    if (available - kHeaderSize < payload_len) {
        return parse_result::torn;
    }

    std::vector<std::byte> crc_input(kCrcPrefix + payload_len);
    std::memcpy(crc_input.data(), p + 7, kCrcPrefix);
    if (payload_len != 0) {
        std::memcpy(crc_input.data() + kCrcPrefix, p + kHeaderSize, payload_len);
    }
    const u32 crc = crc32c(span<const std::byte>(crc_input.data(),
                                                 crc_input.size()));
    if (crc != get_u32(p + 28)) {
        return parse_result::corrupt;
    }

    out.kind = static_cast<log_record_kind>(std::to_integer<unsigned>(p[7]));
    out.term = get_u64(p + 8);
    out.index = get_u64(p + 16);
    out.payload.assign(p + kHeaderSize, p + kHeaderSize + payload_len);
    consumed = kHeaderSize + payload_len;
    return parse_result::ok;
}

#if defined(CRPC_OS_WINDOWS)

HANDLE as_handle(void* h) noexcept { return static_cast<HANDLE>(h); }

status win_fail(DWORD code, const char* op, const std::string& path) {
    return status(fs::detail::classify_win32(code, op, path));
}

#else

int as_fd(void* h) noexcept {
    return static_cast<int>(reinterpret_cast<std::intptr_t>(h));
}

status posix_fail(int code, const char* op, const std::string& path) {
    return status(fs::detail::classify_errno(code, op, path));
}

#endif

} // namespace

// --- file_registry_log -------------------------------------------------------

file_registry_log::~file_registry_log() {
#if defined(CRPC_OS_WINDOWS)
    if (handle_ != nullptr) {
        (void)CloseHandle(as_handle(handle_));
        handle_ = nullptr;
    }
#else
    if (handle_ != nullptr) {
        (void)::close(as_fd(handle_));
        handle_ = nullptr;
    }
#endif
}

u64 file_registry_log::current_size() const {
    if (handle_ == nullptr) {
        return 0;
    }
#if defined(CRPC_OS_WINDOWS)
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(as_handle(handle_), &size)) {
        return 0;
    }
    return static_cast<u64>(size.QuadPart);
#else
    struct stat st = {};
    if (::fstat(as_fd(handle_), &st) != 0) {
        return 0;
    }
    return static_cast<u64>(st.st_size);
#endif
}

status file_registry_log::write_at(u64 offset, const std::vector<std::byte>& bytes) {
    if (handle_ == nullptr) {
        return status(rpc_error(rpc_code::failed_precondition,
                                "registry_log: closed"));
    }
    if (bytes.empty()) {
        return status();
    }
#if defined(CRPC_OS_WINDOWS)
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(as_handle(handle_), position, nullptr, FILE_BEGIN)) {
        return win_fail(GetLastError(), "registry_log_seek", std::string());
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const std::size_t left = bytes.size() - written;
        const DWORD want = static_cast<DWORD>(
            left < (1u << 20) ? left : static_cast<std::size_t>(1u << 20));
        DWORD wrote = 0;
        if (!WriteFile(as_handle(handle_), bytes.data() + written, want, &wrote,
                       nullptr)) {
            return win_fail(GetLastError(), "registry_log_write", std::string());
        }
        if (wrote == 0) {
            return status(rpc_error(rpc_code::internal,
                                    "registry_log: short write"));
        }
        written += wrote;
    }
#else
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t n = ::pwrite(as_fd(handle_), bytes.data() + written,
                                   bytes.size() - written,
                                   static_cast<off_t>(offset + written));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return posix_fail(errno, "registry_log_write", std::string());
        }
        if (n == 0) {
            return status(rpc_error(rpc_code::internal,
                                    "registry_log: short write"));
        }
        written += static_cast<std::size_t>(n);
    }
#endif
    return status();
}

status file_registry_log::read_all(std::vector<std::byte>& out) const {
    out.clear();
    if (handle_ == nullptr) {
        return status(rpc_error(rpc_code::failed_precondition,
                                "registry_log: closed"));
    }
    const u64 size = current_size();
    out.resize(static_cast<std::size_t>(size));
    if (size == 0) {
        return status();
    }
#if defined(CRPC_OS_WINDOWS)
    LARGE_INTEGER position{};
    position.QuadPart = 0;
    if (!SetFilePointerEx(as_handle(handle_), position, nullptr, FILE_BEGIN)) {
        return win_fail(GetLastError(), "registry_log_seek", std::string());
    }
    std::size_t read_total = 0;
    while (read_total < out.size()) {
        const std::size_t left = out.size() - read_total;
        const DWORD want = static_cast<DWORD>(
            left < (1u << 20) ? left : static_cast<std::size_t>(1u << 20));
        DWORD got = 0;
        if (!ReadFile(as_handle(handle_), out.data() + read_total, want, &got,
                      nullptr)) {
            return win_fail(GetLastError(), "registry_log_read", std::string());
        }
        if (got == 0) {
            return status(rpc_error(rpc_code::internal,
                                    "registry_log: short read"));
        }
        read_total += got;
    }
#else
    std::size_t read_total = 0;
    while (read_total < out.size()) {
        const ssize_t n = ::pread(as_fd(handle_), out.data() + read_total,
                                  out.size() - read_total,
                                  static_cast<off_t>(read_total));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return posix_fail(errno, "registry_log_read", std::string());
        }
        if (n == 0) {
            return status(rpc_error(rpc_code::internal,
                                    "registry_log: short read"));
        }
        read_total += static_cast<std::size_t>(n);
    }
#endif
    return status();
}

status file_registry_log::truncate_to(u64 size) {
    if (handle_ == nullptr) {
        return status(rpc_error(rpc_code::failed_precondition,
                                "registry_log: closed"));
    }
#if defined(CRPC_OS_WINDOWS)
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(as_handle(handle_), target, nullptr, FILE_BEGIN)) {
        return win_fail(GetLastError(), "registry_log_seek", std::string());
    }
    if (!SetEndOfFile(as_handle(handle_))) {
        return win_fail(GetLastError(), "registry_log_truncate", std::string());
    }
#else
    if (::ftruncate(as_fd(handle_), static_cast<off_t>(size)) != 0) {
        return posix_fail(errno, "registry_log_truncate", std::string());
    }
#endif
    return status();
}

result<std::unique_ptr<file_registry_log>>
file_registry_log::open(const std::string& path) {
    if (path.empty()) {
        return rpc_error(rpc_code::invalid_argument, "registry_log: empty path");
    }
    std::unique_ptr<file_registry_log> log(new file_registry_log());

#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> wide = fs::detail::utf8_to_wide(path);
    if (!wide) {
        return rpc_error(rpc_code::invalid_argument,
                         "registry_log: path is not valid UTF-8");
    }
    HANDLE h = CreateFileW(wide.value().c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return win_fail(GetLastError(), "registry_log_open", path).error();
    }
    log->handle_ = h;
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return posix_fail(errno, "registry_log_open", path).error();
    }
    log->handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
#endif

    // Adopt the intact prefix. A corrupt record is an error: recovery must
    // never make a damaged log look like a shorter healthy one.
    const result<std::vector<log_record>> all = log->replay(0);
    if (!all) {
        return all.error();
    }
    log->last_index_ = all.value().empty() ? 0 : all.value().back().index;
    // Everything the scan accepted was previously written and assumed durable
    // (it survived a process restart, which is our durability baseline).
    log->durable_size_ = log->intact_size_;
    if (log->intact_size_ != log->bytes_on_disk_) {
        // A torn tail was never durable; discard it so the file and the durable
        // boundary agree from the first append onward.
        const status trimmed = log->truncate_to(log->intact_size_);
        if (!trimmed.ok()) {
            return trimmed.error();
        }
    }
    return log;
}

result<u64> file_registry_log::append(const log_record& record) {
    if (record.index == 0) {
        return rpc_error(rpc_code::invalid_argument,
                         "registry_log: record index must not be zero");
    }
    if (handle_ == nullptr) {
        return rpc_error(rpc_code::failed_precondition, "registry_log: closed");
    }
    if (record.payload.size() > std::numeric_limits<u32>::max()) {
        return rpc_error(rpc_code::invalid_argument,
                         "registry_log: payload is too large");
    }

    // Contiguity is a contract, not a convenience. If a gap were accepted, a
    // replay could not tell "records 2-4 were lost" from "5 legitimately comes
    // after 1", and a later append at 2 would silently truncate index 5. Raft
    // also requires a dense index space, so step3 gets this for free.
    if (record.index > last_index_ + 1) {
        return rpc_error(rpc_code::invalid_argument,
                         "registry_log: index gap (expected " +
                             std::to_string(last_index_ + 1) + ", got " +
                             std::to_string(record.index) + ")");
    }

    // Conflict overwrite: an index that is not greater than the last one cuts
    // that record and everything after it. This is the Raft rule, and it is why
    // a duplicate index can never leave two records alive under one number.
    if (!spans_.empty() && record.index <= last_index_) {
        std::size_t cut = 0;
        while (cut < spans_.size() && spans_[cut].index < record.index) {
            ++cut;
        }
        if (cut < spans_.size()) {
            const u64 base = spans_[cut].offset;
            spans_.resize(cut);
            const status trimmed = truncate_to(base);
            if (!trimmed.ok()) {
                return trimmed.error();
            }
            if (durable_size_ > base) {
                durable_size_ = base;
            }
            last_index_ = spans_.empty() ? 0 : spans_.back().index;
        }
    }

    const std::vector<std::byte> encoded = encode_record(record);
    const u64 base = current_size();
    const status written = write_at(base, encoded);
    if (!written.ok()) {
        return written.error();
    }
    spans_.push_back(record_span{record.index, base, encoded.size()});
    last_index_ = record.index;
    return record.index;
}

result<std::vector<log_record>> file_registry_log::replay(u64 from_index) const {
    if (handle_ == nullptr) {
        return rpc_error(rpc_code::failed_precondition, "registry_log: closed");
    }

    std::vector<std::byte> bytes;
    const status read = read_all(bytes);
    if (!read.ok()) {
        return read.error();
    }
    bytes_on_disk_ = bytes.size();

    std::vector<log_record> out;
    std::vector<record_span> spans;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        log_record record;
        std::size_t consumed = 0;
        const parse_result parsed = parse_record(bytes, offset, record, consumed);
        if (parsed == parse_result::torn) {
            break; // power loss left a partial record; the prefix is intact
        }
        if (parsed == parse_result::corrupt) {
            return rpc_error(rpc_code::internal,
                             "registry_log: corrupt record at offset " +
                                 std::to_string(offset));
        }
        spans.push_back(record_span{record.index, offset, consumed});
        offset += consumed;
        if (record.index >= from_index) {
            out.push_back(std::move(record));
        }
    }
    intact_size_ = offset;
    spans_ = std::move(spans);
    return out;
}

status file_registry_log::sync() {
    if (handle_ == nullptr) {
        return status(rpc_error(rpc_code::failed_precondition,
                                "registry_log: closed"));
    }
    if (!flush_handle()) {
        // Nothing became durable. Advancing the boundary or the counter here
        // would let a "sync that never flushed" implementation look correct to
        // every assertion that reads this object's own state.
        return status(rpc_error(rpc_code::internal,
                                "registry_log: flush failed"));
    }
    // Only here do the appended bytes become durable. Advancing the boundary
    // anywhere else (inside append, for instance) would make a missing flush
    // undetectable, which is exactly the defect the V3 contract guards.
    durable_size_ = current_size();
    ++flush_count_;
    return status();
}

bool file_registry_log::flush_handle() noexcept {
    if (fail_flush_) {
        fail_flush_ = false; // one-shot: the next attempt may succeed
        return false;
    }
#if defined(CRPC_OS_WINDOWS)
    return FlushFileBuffers(as_handle(handle_)) != 0;
#else
    return ::fsync(as_fd(handle_)) == 0;
#endif
}

void file_registry_log::drop_unsynced_tail() {
    if (handle_ == nullptr) {
        return;
    }
    const u64 target = durable_size_;
    (void)truncate_to(target);
    while (!spans_.empty() && spans_.back().offset >= target) {
        spans_.pop_back();
    }
    last_index_ = spans_.empty() ? 0 : spans_.back().index;
    bytes_on_disk_ = target;
    intact_size_ = target;
}

// --- truncate_suffix (base default) ------------------------------------------
//
// The default refuses rather than silently doing nothing. A no-op truncation is
// exactly the failure the Raft rule prevents: the follower would keep a suffix
// the leader has already overwritten and could acknowledge entries it does not
// hold. Refusing makes the limitation visible to the caller.
status registry_log::truncate_suffix(u64 from_index) {
    (void)from_index;
    // `unimplemented` is deliberately absent from `rpc_code`: design.md:1779
    // (O1) records that adding it was ruled out, so the capability gap is
    // expressed with the closest existing code. `failed_precondition` says the
    // operation is not available on this backend rather than that the caller
    // did something wrong, and `unavailable` would wrongly suggest a retry
    // could help.
    return status(rpc_code::failed_precondition,
                  "registry_log: this backend cannot truncate");
}

// --- truncate_suffix (file backend) ------------------------------------------
status file_registry_log::truncate_suffix(u64 from_index) {
    if (from_index == 0) {
        return status(rpc_code::invalid_argument,
                      "registry_log: truncate index must not be zero");
    }
    if (handle_ == nullptr) {
        return status(rpc_code::failed_precondition, "registry_log: closed");
    }

    // `spans_` is rebuilt by replay(), not by append(). A caller that opens a
    // log and truncates without replaying first would otherwise see an empty
    // span list and be told "nothing to do" while records remain on disk.
    // Replaying also re-validates every frame's CRC, so a corrupt log is
    // reported here rather than truncated around.
    const auto replayed = replay(0);
    if (!replayed.ok()) {
        return replayed.error();
    }

    // Idempotent: at or past the end there is no suffix to remove.
    if (from_index > last_index_) {
        return status();
    }

    // First span whose index is >= from_index; everything from there on goes.
    std::size_t cut = 0;
    while (cut < spans_.size() && spans_[cut].index < from_index) {
        ++cut;
    }
    if (cut == spans_.size()) {
        // Defensive: last_index_ said the record exists but the span list does
        // not carry it. Nothing to cut, and reporting success would hide the
        // inconsistency.
        return status();
    }

    const u64 base = spans_[cut].offset;
    const status trimmed = truncate_to(base);
    if (!trimmed.ok()) {
        return trimmed;
    }
    spans_.resize(cut);
    if (durable_size_ > base) {
        durable_size_ = base;
    }
    last_index_ = spans_.empty() ? 0 : spans_.back().index;
    bytes_on_disk_ = base;
    intact_size_ = base;

    // Raft requires the truncation to be durable: a follower that truncated in
    // memory only would replay the stale suffix after a restart.
    return sync();
}

// --- memory_registry_log -----------------------------------------------------
result<u64> memory_registry_log::append(const log_record& record) {
    if (record.index == 0) {
        return rpc_error(rpc_code::invalid_argument,
                         "registry_log: record index must not be zero");
    }
    if (record.index > last_index() + 1) {
        return rpc_error(rpc_code::invalid_argument,
                         "registry_log: index gap (expected " +
                             std::to_string(last_index() + 1) + ", got " +
                             std::to_string(record.index) + ")");
    }
    while (!records_.empty() && records_.back().index >= record.index) {
        records_.pop_back();
    }
    records_.push_back(record);
    return record.index;
}

status memory_registry_log::truncate_suffix(u64 from_index) {
    if (from_index == 0) {
        return status(rpc_code::invalid_argument,
                      "registry_log: truncate index must not be zero");
    }
    // Idempotent: a from_index past the end pops nothing.
    while (!records_.empty() && records_.back().index >= from_index) {
        records_.pop_back();
    }
    return status();
}

result<std::vector<log_record>> memory_registry_log::replay(u64 from_index) const {
    std::vector<log_record> out;
    for (const log_record& record : records_) {
        if (record.index >= from_index) {
            out.push_back(record);
        }
    }
    return out;
}

} // namespace cluster
} // namespace crpc
