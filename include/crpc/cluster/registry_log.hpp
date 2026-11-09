// CRPC -- G19 step1: durable, replayable log behind the cluster registry.
//
// Design contract (see .opencode/tmp/g19p1/verdict.md 7.1.1 / D1-D4):
//
//   * A record is self-describing: {kind, term, index, payload}. `index` is the
//     log position (1-based, strictly increasing); `term` is carried for the
//     consensus layer (step3) and is 0 for plain registry events.
//   * The on-disk framing is length-prefixed and CRC32C-protected, so a torn
//     tail is distinguishable from a corrupt record without guessing.
//   * `append()` at an index that already exists TRUNCATES that index and the
//     whole suffix before writing (Raft 4.2.2 conflict-overwrite semantics).
//     It never returns an error for a duplicate index and never writes two
//     records with the same index.
//   * Durability is explicit and sits on exactly one operation: `sync()`. An
//     append is *not* durable by itself. `sync()` flushes the file and then
//     advances the durable boundary; `drop_unsynced_tail()` truncates the file
//     back to that boundary, which is how a power loss is modelled in tests.
//     Truncating to the current file size instead would be a no-op and would
//     make the "no fsync" mutation invisible.
//
// The abstract base keeps the registry and the consensus layer independent of
// the storage backend; `file_registry_log` is the production implementation and
// `memory_registry_log` exists for tests and for embedded single-process use.
//
// Scope boundaries
// ----------------
// Two limits are inherent to this layer and are documented here rather than
// guarded in code, because no amount of checking inside this class can detect
// them:
//
//   1. A flush that *reports* success without reaching the device is
//      indistinguishable from a real one. The only durable evidence is the
//      physical medium, and both same-process and cross-process reads are
//      answered from the page cache. The defect surface is kept to a single
//      short function (`file_registry_log::flush_handle()`), which is what
//      makes a code review sufficient; so any new caller must reuse it and
//      must not issue its own flush.
//   2. Two log files concatenated into one are not distinguishable from a log
//      that was written normally: both end up CRC-valid, index-dense and
//      term-monotone. A merge can therefore replace a newer record with an
//      older one without leaving a trace, and neither last-wins folding nor a
//      term-regression check can detect it. Records carry no provenance, so
//      merging two logs must go through the compaction path (which tags
//      origin) and must never be done by concatenation.
//
// One writer per path, per process: this class holds no cross-process lock, so
// two open handles on the same file would interleave appends. The registry
// service (step2) serialises writers; a raw user of this class must not.
#ifndef CRPC_CLUSTER_REGISTRY_LOG_HPP
#define CRPC_CLUSTER_REGISTRY_LOG_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace cluster {

// What a record carries. The registry only produces `registry_event`; the
// consensus layer persists its own record kinds in the same log so that a node
// has one durability boundary instead of two.
enum class log_record_kind : u8 {
    registry_event = 0,
    raft_term = 1,
    raft_vote = 2,
    raft_snapshot = 3,
    raft_entry = 4
};

// A single self-describing log record. `payload` is opaque to this layer.
struct log_record {
    log_record_kind kind = log_record_kind::registry_event;
    u64 term = 0;
    u64 index = 0;
    std::vector<std::byte> payload;
};

// Append-only log with an explicit durability point.
class CRPC_API registry_log {
public:
    virtual ~registry_log() = default;

    // Appends `record` and returns its index. A record whose index is less than
    // or equal to the current last index truncates that index and everything
    // after it first. `record.index == 0` is `invalid_argument`: the caller owns
    // the numbering, and 0 would collide with "no records".
    virtual result<u64> append(const log_record& record) = 0;

    // Returns every intact record whose index is >= from_index, in order. A
    // torn tail (fewer bytes than a whole record) ends the replay without an
    // error; a CRC or magic mismatch on a complete record is an error, because
    // silently returning a prefix would hide corruption.
    virtual result<std::vector<log_record>> replay(u64 from_index) const = 0;

    // Flushes to the storage device and advances the durable boundary. This is
    // the only operation that makes previously appended records survive a
    // crash.
    virtual status sync() = 0;

    // Index of the last record, or 0 when the log is empty.
    [[nodiscard]] virtual u64 last_index() const noexcept = 0;
};

// File-backed log. Owns its file handle for the whole lifetime and appends with
// an explicit write at end-of-file, so a crash cannot interleave two writers.
class CRPC_API file_registry_log final : public registry_log {
public:
    ~file_registry_log() override;

    file_registry_log(const file_registry_log&) = delete;
    file_registry_log& operator=(const file_registry_log&) = delete;

    // Opens or creates `path`, scans the intact prefix and adopts its last
    // index. A corrupt record in the middle is an error: recovery must never
    // pretend a damaged log is a shorter, healthy one.
    static result<std::unique_ptr<file_registry_log>> open(const std::string& path);

    result<u64> append(const log_record& record) override;
    result<std::vector<log_record>> replay(u64 from_index) const override;
    status sync() override;
    [[nodiscard]] u64 last_index() const noexcept override { return last_index_; }

    // Byte offset of the last successful `sync()`. Everything written after it
    // is not durable yet.
    [[nodiscard]] u64 durable_size() const noexcept { return durable_size_; }

    // Number of successful flush calls. Zero means nothing was ever made
    // durable.
    [[nodiscard]] u64 flush_count() const noexcept { return flush_count_; }

    // Makes the next `sync()` fail at the device. The injection lives inside
    // the same gate the real flush uses, so a `sync()` that skips the flush and
    // advances the boundary anyway is caught: the test asserts that a failed
    // sync must move neither `durable_size()` nor `flush_count()`.
    //
    // This point exists because a "does the boundary move" assertion alone is
    // satisfied by a mutant that advances the boundary without flushing -- the
    // mutation would be self-consistent. The failure path is the only
    // observable that separates "flushed" from "claimed to have flushed".
    void inject_flush_failure() noexcept { fail_flush_ = true; }

    // Power-loss model: discards every byte written after the last `sync()`.
    // This is NOT an I/O-error injection; the distinction matters because an
    // error is visible to the caller and a lost page cache is not.
    void drop_unsynced_tail();

private:
    file_registry_log() = default;

    // The one place the OS flush is issued. Returning false means the bytes are
    // not durable; `sync()` must not advance anything in that case.
    [[nodiscard]] bool flush_handle() noexcept;

    // Where each live record starts, so a conflict overwrite can cut the file
    // back to the conflicting index without re-scanning.
    struct record_span {
        u64 index = 0;
        u64 offset = 0;
        std::size_t length = 0;
    };

    status write_at(u64 offset, const std::vector<std::byte>& bytes);
    status read_all(std::vector<std::byte>& out) const;
    status truncate_to(u64 size);
    [[nodiscard]] u64 current_size() const;

    // 0/negative when closed. Kept as void* so the public header stays free of
    // <windows.h>; on POSIX the descriptor is stored as intptr_t.
    void* handle_ = nullptr;
    u64 last_index_ = 0;
    u64 durable_size_ = 0;
    u64 flush_count_ = 0;
    bool fail_flush_ = false;
    // Rebuilt by every replay(); `open()` relies on replay() populating it, so
    // it is mutable even though replay is logically const.
    mutable std::vector<record_span> spans_;

    // Set by replay(): byte length of the intact prefix and of the whole file.
    // `open()` compares them to discard a torn tail.
    mutable u64 intact_size_ = 0;
    mutable u64 bytes_on_disk_ = 0;
};

// In-memory log with the same semantics minus durability. `sync()` is a
// successful no-op and the durable boundary equals the written size.
class CRPC_API memory_registry_log final : public registry_log {
public:
    result<u64> append(const log_record& record) override;
    result<std::vector<log_record>> replay(u64 from_index) const override;
    status sync() override { return status(); }
    [[nodiscard]] u64 last_index() const noexcept override {
        return records_.empty() ? 0 : records_.back().index;
    }

private:
    std::vector<log_record> records_;
};

} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_REGISTRY_LOG_HPP
