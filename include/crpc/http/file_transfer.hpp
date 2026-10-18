// CRPC -- HTTP upload sink and download source.
//
// `upload_sink` implements the storage half of a streaming multipart upload.
// Bytes arrive through `write`; the sink keeps them in memory until the total
// exceeds `memory_threshold` (default 1 MiB) and then spills to a uniquely
// named temporary file. `finalize` transfers ownership to a `file_part`.
//
// RAII contract:
//
//   * a temporary file created by the sink is deleted by the sink destructor
//     if `finalize` was not called (connection loss / cancellation);
//   * `file_part` owns its temporary file and deletes it on destruction unless
//     `keep()` was called;
//   * `cancel()` deletes the temporary file immediately.
//
// The client filename is metadata only. The on-disk name is generated with a
// 128-bit pseudorandom suffix and created with exclusive-create semantics, so
// two uploads cannot collide and an existing file cannot be overwritten.
//
// `download_source` streams a file or an in-memory buffer. A file is never read
// into memory as a whole; `read` fills the caller's span in chunks. Range
// parsing implements `bytes=a-b`, `bytes=a-` and `bytes=-n`; a satisfiable
// range yields status 206 and a Content-Range value, an unsatisfiable or
// syntactically invalid one yields 416 and `bytes */<size>`. Multiple ranges
// are rejected with 416 (v1 limitation). A `Range` header with a unit other
// than `bytes` is ignored (200), as RFC 9110 requires.
//
// Thread safety: one sink or source per thread. Distinct instances share no
// mutable state (the temporary-name generator is seeded per call).
//
// Allocation: `upload_sink` allocates up to the spill threshold in one growing
// string; the spill path writes through a buffered FILE. `download_source`
// allocates only the owned in-memory payload (for `from_memory`) and the
// Content-Range string. Reads perform no allocation.
#ifndef CRPC_HTTP_FILE_TRANSFER_HPP
#define CRPC_HTTP_FILE_TRANSFER_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc::http {

// --- Upload -----------------------------------------------------------------

struct upload_options {
    // At or below this many bytes the upload stays in memory. The comparison is
    // inclusive: a write that makes the total exactly `memory_threshold` stays
    // in memory; the next byte spills.
    std::size_t memory_threshold = 1u * 1024u * 1024u;
    // Inclusive per-part limit enforced by the sink.
    std::size_t max_part_bytes = 64u * 1024u * 1024u;
    // Empty means the system temporary directory. The directory is created if
    // it does not exist.
    std::filesystem::path temp_directory;
    // When true, `finalize` computes the SHA-256 of the stored bytes.
    bool compute_sha256 = false;
};

// One stored upload part. Move-only. If `temp_path` is non-empty the payload
// lives on disk; otherwise it is in `data`.
struct file_part {
    std::string name;
    std::string filename;
    std::string content_type;
    std::string sha256; // lowercase hex; empty unless requested
    std::uint64_t size = 0;
    std::string data;
    std::filesystem::path temp_path;

    file_part() = default;
    file_part(const file_part&) = delete;
    file_part& operator=(const file_part&) = delete;
    file_part(file_part&& other) noexcept;
    file_part& operator=(file_part&& other) noexcept;
    ~file_part();

    [[nodiscard]] bool in_memory() const noexcept { return temp_path.empty(); }
    [[nodiscard]] bool kept() const noexcept { return kept_; }
    [[nodiscard]] bool exists() const;

    // Keeps the temporary file after this object is destroyed. The caller then
    // owns `temp_path` and must remove it.
    void keep() noexcept { kept_ = true; }

    // Deletes the temporary file immediately (regardless of `keep`) and clears
    // the payload. Metadata (name/filename/size) is retained.
    void remove() noexcept;

private:
    friend class upload_sink;
    void drop_temp() noexcept;

    bool kept_ = false;
};

class upload_sink {
public:
    upload_sink(std::string name, std::string filename,
                std::string content_type, upload_options options = {});
    upload_sink(const upload_sink&) = delete;
    upload_sink& operator=(const upload_sink&) = delete;
    upload_sink(upload_sink&&) = delete;
    upload_sink& operator=(upload_sink&&) = delete;
    ~upload_sink();

    // Appends bytes. Returns `resource_exhausted` when max_part_bytes would be
    // exceeded; a failed sink stays failed.
    [[nodiscard]] status write(span<const std::byte> data);

    // Flushes and transfers ownership into a `file_part`. The file is NOT kept:
    // the returned part deletes it on destruction unless the caller calls
    // `keep()`.
    [[nodiscard]] result<file_part> finalize();

    // Deletes the temporary file and marks the sink cancelled. Idempotent.
    void cancel() noexcept;

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] bool spilled() const noexcept { return file_ != nullptr; }
    [[nodiscard]] bool finalized() const noexcept { return finalized_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] const rpc_error& error() const noexcept { return error_; }

private:
    status fail(rpc_code code, std::string message);
    status ensure_spill();
    status write_file(span<const std::byte> data);
    void close_file() noexcept;

    std::string name_;
    std::string filename_;
    std::string content_type_;
    upload_options options_;
    std::string memory_;
    std::string sha256_hex_;
    std::filesystem::path temp_path_;
    std::FILE* file_ = nullptr;
    std::uint64_t size_ = 0;
    bool finalized_ = false;
    bool canceled_ = false;
    bool failed_ = false;
    rpc_error error_;

    // SHA-256 state (opaque pointer keeps the header free of crypto details).
    struct hasher;
    hasher* hasher_ = nullptr;
};

// --- Download ---------------------------------------------------------------

enum class range_kind : std::uint8_t { none, satisfiable, unsatisfiable };

struct range_parse_result {
    range_kind kind = range_kind::none;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::uint64_t length = 0;
};

// Parses one Range header value against an entity of `entity_size` bytes.
// `none` means "serve the full entity" (absent header or a non-bytes unit).
// A malformed value, a multiple-range value, or an unsatisfiable range yields
// `unsatisfiable` (the caller sends 416).
[[nodiscard]] range_parse_result parse_range_header(std::string_view value,
                                                    std::uint64_t entity_size);

class download_source {
public:
    download_source() noexcept = default;
    download_source(const download_source&) = delete;
    download_source& operator=(const download_source&) = delete;
    download_source(download_source&& other) noexcept;
    download_source& operator=(download_source&& other) noexcept;
    ~download_source();

    // Opens a file for streaming. Missing/unreadable files yield not_found.
    [[nodiscard]] static result<download_source> open_file(
        const std::filesystem::path& path);

    // Copies the payload into an owned buffer.
    [[nodiscard]] static result<download_source> from_memory(
        std::string_view bytes);

    // Applies a Range header. Sets status_code() to 200 (ignored/no range),
    // 206 (satisfiable) or 416 (unsatisfiable).
    [[nodiscard]] status apply_range(std::string_view range_header);

    // Reads up to out.size() bytes from the current position. `count` is set
    // to the number of bytes produced; 0 means the selected entity is done.
    [[nodiscard]] status read(span<std::byte> out, std::size_t& count);

    // Restarts at the first byte of the selected range.
    [[nodiscard]] status rewind();

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint64_t remaining() const noexcept { return remaining_; }
    [[nodiscard]] std::uint64_t content_length() const noexcept { return remaining_; }
    [[nodiscard]] int status_code() const noexcept { return status_code_; }
    [[nodiscard]] const std::string& content_range() const noexcept {
        return content_range_;
    }
    [[nodiscard]] bool file_backed() const noexcept { return file_ != nullptr; }

private:
    status seek_to(std::uint64_t offset);

    std::string memory_;
    std::string content_range_;
    std::filesystem::path path_;
    std::FILE* file_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint64_t first_ = 0;
    std::uint64_t remaining_ = 0;
    std::uint64_t range_length_ = 0;
    std::uint64_t position_ = 0;
    int status_code_ = 200;
};

} // namespace crpc::http

#endif // CRPC_HTTP_FILE_TRANSFER_HPP
