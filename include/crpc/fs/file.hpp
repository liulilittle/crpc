// CRPC -- regular-file operations.
//
// The file layer is a small, total API over the OS: every operation returns
// `result`/`status` and, on failure, a `rpc_error` that classifies the native
// failure into `rpc_code` while *keeping* the native error code (GetLastError /
// errno) in the message and in `metadata["fs.native_error"]`. No system error
// is ever flattened into a generic "something failed".
//
// Error codes as used here
// -------------------------
//   not_found            the path (or its parent) does not exist
//   permission_denied    access denied, sharing violation, read-only target
//   invalid_argument     a path is a directory where a file was expected, the
//                        offset/count is out of range, or the path is too long
//   failed_precondition  the target already exists and overwrite was not allowed
//   resource_exhausted   disk full / out of handles / out of memory
//   internal             an OS error with no closer classification (native
//                        code is still preserved)
//
// Deletion semantics: `remove_file` is idempotent -- deleting a file that is
// already gone is success. Existence checks (`exists`, `stat`) report an absent
// path as `result<bool>{false}` / `not_found`, never as "deleted OK".
#ifndef CRPC_FS_FILE_HPP
#define CRPC_FS_FILE_HPP

#include <chrono>
#include <cstddef>
#include <string>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>
#include <crpc/fs/memory.hpp>
#include <crpc/fs/path.hpp>

namespace crpc {
namespace fs {

// Wall-clock file timestamps. Values are system-clock time points; conversion
// to/from the platform clock (FILETIME, timespec) happens inside the layer.
using file_time = std::chrono::system_clock::time_point;

struct CRPC_API file_info {
    u64 size = 0;
    bool is_directory = false;
    bool is_read_only = false;
    bool is_symlink = false;
    file_time last_write_time{};
    file_time last_access_time{};
    file_time creation_time{};
};

// --- Existence and metadata -------------------------------------------------

// Does anything (file, directory, symlink) at `p` exist? An absent path is a
// plain `false`, not an error; anything that prevents the question from being
// answered (e.g. access denied on the parent) is an error.
CRPC_API result<bool> exists(const path& p);
CRPC_API result<bool> is_file(const path& p);
CRPC_API result<bool> is_directory(const path& p);

CRPC_API result<file_info> stat(const path& p);
CRPC_API result<u64> file_size(const path& p);

// --- Whole-file I/O ---------------------------------------------------------

// Reads the whole file. A directory path is `invalid_argument` ("read a
// directory as a file"), not a silent 0-byte read.
CRPC_API result<byte_buffer> read_all_bytes(const path& p);
CRPC_API result<std::string> read_all_text(const path& p);

// Replaces the whole file (creates or truncates). This is NOT atomic; use
// `write_all_bytes_atomic` where a torn file must be impossible.
CRPC_API status write_all_bytes(const path& p, span<const std::byte> data);
CRPC_API status write_all_text(const path& p, const std::string& text);

// Appends `data` to `p` (creating the file if absent). NOT durable: a
// successful return only means the bytes reached the OS page cache (S2);
// `CloseHandle` / `::close` are not a physical-media barrier, so a power loss
// may drop them. No `FlushFileBuffers` (Windows) / `fsync` (POSIX) runs here.
// Callers needing durability must use an explicit durable channel or
// `write_all_bytes_atomic`, which flushes before the rename.
//
// An append spanning multiple `kIoChunk` slices is not atomic, and a failed
// write does not roll back: already-appended bytes may remain as a torn tail.
// A `::write` reporting zero bytes transferred for a non-empty request fails the
// call instead of spinning (same on the Windows branch).
CRPC_API status append_bytes(const path& p, span<const std::byte> data);
CRPC_API status append_text(const path& p, const std::string& text);

// Reads up to `out.size()` bytes at absolute `offset` into `out`. Returns the
// number of bytes actually read; an offset at or past EOF is a clean `0`, not
// an error. Use this for chunked reads of files too large to load whole.
CRPC_API result<std::size_t> read_range(const path& p, u64 offset,
                                        span<std::byte> out);

// --- Atomic write -----------------------------------------------------------
//
// Writes `data` to a unique temporary file next to the target, flushes it, then
// replaces the target. A failure at any point before the replace leaves the
// previous target content completely intact -- never a torn mix of old and new.
// The temporary file is removed on failure.
CRPC_API status write_all_bytes_atomic(const path& p, span<const std::byte> data);
CRPC_API status write_all_text_atomic(const path& p, const std::string& text);

// --- Mutation ---------------------------------------------------------------

// Idempotent: a missing file is a successful no-op.
CRPC_API status remove_file(const path& p);

// Move/rename; replaces an existing destination. `not_found` when `from` is
// absent. MOVEFILE_COPY_ALLOWED lets a rename span volumes.
CRPC_API status rename(const path& from, const path& to);

// Copy. With `overwrite=false`, an existing destination is
// `failed_precondition("target already exists")`.
CRPC_API status copy_file(const path& from, const path& to, bool overwrite = true);

// Sets or clears the read-only attribute.
CRPC_API status set_read_only(const path& p, bool read_only);

// Sets the last-write timestamp. Round-trips through the platform clock.
CRPC_API status set_last_write_time(const path& p, file_time time);

// --- Test seam for the atomic writer ---------------------------------------
//
// The atomic writer is split into observable phases so a fault-injection test
// can interrupt the write phase and prove the target is untouched. Production
// callers use `write_all_bytes_atomic` only.
namespace detail {

// A unique temp-file path in the target's directory.
CRPC_API path atomic_temp_path(const path& target);

// Creates an empty file with exclusive-create semantics (never truncates and
// never replaces an existing file). `failed_precondition` when the path
// already exists -- this is what makes temp_file::create race-free.
CRPC_API status create_new_empty_file(const path& p);

// Writes `data` to `temp` and closes it. With `fault_after > 0`, the write
// stops after that many bytes and fails, simulating a mid-write I/O error;
// the partial temp file is deleted before returning. `fault_after` exists
// solely so the atomic-write guarantee is testable deterministically.
CRPC_API status atomic_write_stage(const path& temp, span<const std::byte> data,
                                   std::size_t fault_after = 0);

// Replaces `target` with `temp` (the commit; atomic on both platforms).
CRPC_API status atomic_write_commit(const path& temp, const path& target);

// stage + commit, deleting the temp file on any failure. Returns the error;
// the target is untouched when it does.
CRPC_API status atomic_write_impl(const path& target, span<const std::byte> data,
                                  std::size_t fault_after = 0);

} // namespace detail

namespace detail {

// --- Test-only zero-write fault injection ------------------------------------
//
// TEST-ONLY SEAM. Do not call this from production code.
//
// A `::write` / `WriteFile` that reports success while transferring zero bytes
// makes a `left != 0` / `off < n` loop spin forever: no progress and no error.
// Regular files almost never do it, so the loop's zero-progress guard cannot be
// exercised through the published API. These hooks force that one condition,
// deterministically, in exactly the configurations where a test can name them
// (`CRPC_TESTING` is defined for the library only under CRPC_BUILD_TESTS=ON --
// CMakeLists.txt -- and for test targets only).
//
// The state is keyed by path rather than being a single global flag: a case
// sets a fault for its own scratch file and leaves every other write alone, so
// parallel cases cannot arm each other's faults.
#if defined(CRPC_TESTING)
// Arms or clears the zero-write fault for an exact path string (compared in
// generic form, so either separator spelling matches). `mode == once` fails
// only the next matching transfer; `mode == always` fails every matching
// transfer until cleared with `off`. Absent keys are inert.
//
// Scope: the fault fires on the *next transfer attempt*. For an atomic write
// the staging temp name is random per call, so arm it on the target path --
// the guarantee under test is that the target survives.
enum class zero_write_mode { off, once, always };

CRPC_API void set_zero_write_fault(const std::string& path, zero_write_mode mode);
#endif

} // namespace detail

} // namespace fs
} // namespace crpc

#endif // CRPC_FS_FILE_HPP