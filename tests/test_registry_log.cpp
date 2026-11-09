// CRPC -- G19 step1: durable log tests (slice S-A).
//
// These cases pin the storage contract step2/step3 build on:
//   E11  a torn tail restores the complete prefix and reports no error
//   E12  bytes written but never synced are gone after a power loss
//   E13  synced bytes survive a power loss, and an implementation that never
//        flushes loses them -- the two-sided assertion is what has
//        discriminating power; "they survived" alone passes for both
//   E16  appending at an existing index truncates that record and the suffix
//   E17  records are self-describing: kind, term, index and payload round-trip
//
// The power loss model is `drop_unsynced_tail()`, which truncates back to the
// last successful sync offset. It is NOT an I/O-error injection: an error is
// visible to the caller, a lost page cache is not. Killing the process does not
// model it either -- the OS still writes the pages back, so "the file is still
// there after the kill" passes with AND without fsync, which is why that form
// of E13 was rejected during review.
//
// E12/E13/M4/M5 run on the file backend only. `memory_registry_log` has no
// unsynced tail by construction, so those assertions are undetectable there --
// running them against it would be a test that can never fail.
#include "test_framework.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include <crpc/cluster/registry_log.hpp>
#include <crpc/fs/file.hpp>
#include <crpc/fs/temp.hpp>

namespace cl = crpc::cluster;
using namespace crpc;

namespace {

std::vector<std::byte> bytes_of(const std::string& text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

std::string text_of(const std::vector<std::byte>& bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const std::byte b : bytes) {
        out.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
    }
    return out;
}

cl::log_record make_record(
    u64 index, const std::string& payload,
    cl::log_record_kind kind = cl::log_record_kind::registry_event,
    u64 term = 0) {
    cl::log_record record;
    record.kind = kind;
    record.term = term;
    record.index = index;
    record.payload = bytes_of(payload);
    return record;
}

// Owns the temporary directory that holds one case's log file.
struct log_dir {
    fs::temp_dir dir;

    explicit log_dir(const char* prefix) {
        auto created = fs::temp_dir::create(fs::path(), prefix);
        if (created.ok()) {
            dir = std::move(created.value());
        }
    }

    bool valid() const { return dir.valid(); }

    std::string file(const char* name) const {
        return (dir.native() / fs::path(name)).str();
    }
};

// Truncates `path` to `bytes` bytes, modelling a crash that lost the unwritten
// part of the last record. Reads the file first so the prefix is preserved.
bool truncate_file_to(const std::string& path, u64 bytes) {
    auto read = fs::read_all_bytes(fs::path(path));
    if (!read.ok()) {
        return false;
    }
    const fs::byte_buffer& all = read.value();
    if (all.size() < bytes) {
        return false;
    }
    return fs::write_all_bytes(
               fs::path(path),
               span<const std::byte>(all.data(), static_cast<std::size_t>(bytes)))
        .ok();
}

std::size_t file_bytes(const std::string& path) {
    auto size = fs::file_size(fs::path(path));
    return size.ok() ? static_cast<std::size_t>(size.value()) : 0;
}

} // namespace

// --- E17: records are self-describing ---------------------------------------

CRPC_TEST(e17_record_is_self_describing) {
    log_dir dir("crpc-loga-");
    CRPC_REQUIRE(dir.valid());
    const std::string path = dir.file("self-describing.log");

    {
        auto opened = cl::file_registry_log::open(path);
        CRPC_REQUIRE(opened.ok());
        auto log = std::move(opened.value());

        const auto appended = log->append(
            make_record(1, "vote-payload", cl::log_record_kind::raft_vote, 7));
        CRPC_REQUIRE(appended.ok());
        CRPC_CHECK_EQ(u64{1}, appended.value());
        // A second record with different values proves the four fields are not
        // merely adjacent in memory: they are independently recoverable.
        CRPC_REQUIRE(log->append(make_record(
                             2, "snap", cl::log_record_kind::raft_snapshot, 8))
                         .ok());
        CRPC_REQUIRE(log->sync().ok());
        CRPC_CHECK(log->flush_count() > 0);
    }

    auto opened = cl::file_registry_log::open(path);
    CRPC_REQUIRE(opened.ok());
    auto log = std::move(opened.value());
    const auto replayed = log->replay(0);
    CRPC_REQUIRE(replayed.ok());
    CRPC_REQUIRE(replayed.value().size() == std::size_t{2});
    const cl::log_record& record = replayed.value()[0];
    CRPC_CHECK(record.kind == cl::log_record_kind::raft_vote);
    CRPC_CHECK_EQ(u64{7}, record.term);
    CRPC_CHECK_EQ(u64{1}, record.index);
    CRPC_CHECK_EQ(std::string("vote-payload"), text_of(record.payload));
    const cl::log_record& second = replayed.value()[1];
    CRPC_CHECK(second.kind == cl::log_record_kind::raft_snapshot);
    CRPC_CHECK_EQ(u64{8}, second.term);
    CRPC_CHECK_EQ(u64{2}, second.index);
    CRPC_CHECK_EQ(std::string("snap"), text_of(second.payload));
    CRPC_CHECK_EQ(u64{2}, log->last_index());

    // The header is fixed-size and the payload is length-prefixed:
    // (32 + 12) + (32 + 4) = 80 bytes on disk.
    CRPC_CHECK_EQ(std::size_t{80}, file_bytes(path));
}

// --- E11: a torn tail restores the complete prefix --------------------------

CRPC_TEST(e11_torn_tail_restores_complete_prefix) {
    log_dir dir("crpc-loga-");
    CRPC_REQUIRE(dir.valid());
    const std::string path = dir.file("torn.log");

    u64 durable = 0;
    {
        auto opened = cl::file_registry_log::open(path);
        CRPC_REQUIRE(opened.ok());
        auto log = std::move(opened.value());
        for (u64 index = 1; index <= 3; ++index) {
            CRPC_REQUIRE(
                log->append(make_record(index, "payload-" + std::to_string(index)))
                    .ok());
        }
        CRPC_REQUIRE(log->sync().ok());
        durable = log->durable_size();
    }
    CRPC_REQUIRE(durable > 40);

    // Cut into the third record, leaving fewer bytes than a whole record.
    CRPC_REQUIRE(truncate_file_to(path, durable - 5));

    auto opened = cl::file_registry_log::open(path);
    CRPC_REQUIRE(opened.ok());
    auto log = std::move(opened.value());
    // A torn tail is not corruption: the replay succeeds and stops at the last
    // intact record. This is the R1 half of the R1-versus-R8 distinction.
    const auto replayed = log->replay(0);
    CRPC_REQUIRE(replayed.ok());
    CRPC_CHECK_EQ(std::size_t{2}, replayed.value().size());
    CRPC_CHECK_EQ(u64{2}, replayed.value().back().index);
    CRPC_CHECK_EQ(u64{2}, log->last_index());

    // Opening discarded the unusable tail, so the file now matches the durable
    // boundary instead of carrying dead bytes forward.
    CRPC_CHECK(file_bytes(path) < static_cast<std::size_t>(durable));
}

// --- E11b: a corrupt record is an error, not a short healthy log ------------

CRPC_TEST(e11b_corrupt_record_is_rejected) {
    log_dir dir("crpc-loga-");
    CRPC_REQUIRE(dir.valid());
    const std::string path = dir.file("corrupt.log");

    {
        auto opened = cl::file_registry_log::open(path);
        CRPC_REQUIRE(opened.ok());
        auto log = std::move(opened.value());
        for (u64 index = 1; index <= 2; ++index) {
            CRPC_REQUIRE(log->append(make_record(index, "corrupt-me")).ok());
        }
        CRPC_REQUIRE(log->sync().ok());
    }

    // Flip one payload byte of the FIRST record. The length prefix still
    // parses, so only the CRC can catch it -- exactly what R8 pins.
    auto read = fs::read_all_bytes(fs::path(path));
    CRPC_REQUIRE(read.ok());
    const fs::byte_buffer& all = read.value();
    CRPC_REQUIRE(all.size() > 40);
    std::vector<std::byte> bytes(all.data(), all.data() + all.size());
    bytes[32] = static_cast<std::byte>(std::to_integer<unsigned>(bytes[32]) ^ 0xFFu);
    CRPC_REQUIRE(
        fs::write_all_bytes(fs::path(path),
                            span<const std::byte>(bytes.data(), bytes.size()))
            .ok());

    auto opened = cl::file_registry_log::open(path);
    CRPC_CHECK_FALSE(opened.ok());
    if (!opened.ok()) {
        CRPC_CHECK(opened.code() != rpc_code::ok);
    }
}

// --- E12 + E13: the two-sided power-loss proof ------------------------------

CRPC_TEST(e12_unsynced_tail_is_lost_after_power_loss) {
    log_dir dir("crpc-loga-");
    CRPC_REQUIRE(dir.valid());
    const std::string path = dir.file("unsynced.log");

    {
        auto opened = cl::file_registry_log::open(path);
        CRPC_REQUIRE(opened.ok());
        auto log = std::move(opened.value());

        CRPC_REQUIRE(log->append(make_record(1, "kept")).ok());
        CRPC_REQUIRE(log->sync().ok());
        const u64 after_sync = log->durable_size();
        CRPC_REQUIRE(log->append(make_record(2, "lost")).ok());

        // Appending must not move the durable boundary; if it did, the power
        // loss below would be a no-op and M5 could never be caught (D2).
        CRPC_CHECK_EQ(after_sync, log->durable_size());
        CRPC_CHECK(file_bytes(path) > static_cast<std::size_t>(after_sync));

        log->drop_unsynced_tail();
        CRPC_CHECK_EQ(after_sync, log->durable_size());
        CRPC_CHECK_EQ(u64{1}, log->last_index());
    }

    auto opened = cl::file_registry_log::open(path);
    CRPC_REQUIRE(opened.ok());
    auto log = std::move(opened.value());
    const auto replayed = log->replay(0);
    CRPC_REQUIRE(replayed.ok());
    CRPC_REQUIRE(replayed.value().size() == std::size_t{1});
    CRPC_CHECK_EQ(u64{1}, replayed.value()[0].index);
    CRPC_CHECK_EQ(std::string("kept"), text_of(replayed.value()[0].payload));
}

CRPC_TEST(e13_synced_records_survive_power_loss) {
    log_dir dir("crpc-loga-");
    CRPC_REQUIRE(dir.valid());
    const std::string path = dir.file("synced.log");

    {
        auto opened = cl::file_registry_log::open(path);
        CRPC_REQUIRE(opened.ok());
        auto log = std::move(opened.value());
        for (u64 index = 1; index <= 3; ++index) {
            CRPC_REQUIRE(
                log->append(make_record(index, "durable-" + std::to_string(index)))
                    .ok());
        }
        CRPC_REQUIRE(log->sync().ok());
        // flush_count() > 0 is part of the assertion: an implementation whose
        // sync() is a no-op would otherwise pass this case by accident.
        CRPC_CHECK(log->flush_count() > 0);
        log->drop_unsynced_tail();
        CRPC_CHECK_EQ(u64{3}, log->last_index());
    }

    auto opened = cl::file_registry_log::open(path);
    CRPC_REQUIRE(opened.ok());
    auto log = std::move(opened.value());
    const auto replayed = log->replay(0);
    CRPC_REQUIRE(replayed.ok());
    CRPC_REQUIRE(replayed.value().size() == std::size_t{3});
    for (std::size_t i = 0; i < replayed.value().size(); ++i) {
        CRPC_CHECK_EQ(static_cast<u64>(i + 1), replayed.value()[i].index);
        CRPC_CHECK_EQ(std::string("durable-") + std::to_string(i + 1),
                      text_of(replayed.value()[i].payload));
    }
}

// --- E16: a duplicate index truncates the conflicting suffix ----------------

CRPC_TEST(e16_append_truncates_conflicting_suffix) {
    log_dir dir("crpc-loga-");
    CRPC_REQUIRE(dir.valid());
    const std::string path = dir.file("truncate.log");

    u64 size_after_three = 0;
    {
        auto opened = cl::file_registry_log::open(path);
        CRPC_REQUIRE(opened.ok());
        auto log = std::move(opened.value());
        for (u64 index = 1; index <= 3; ++index) {
            CRPC_REQUIRE(log->append(make_record(index, "old-" + std::to_string(index)))
                             .ok());
        }
        CRPC_REQUIRE(log->sync().ok());
        size_after_three = log->durable_size();

        // A different term at index 2 replaces index 2 AND index 3. Returning
        // an error here, or keeping two records numbered 2, both break Raft.
        const auto replaced = log->append(
            make_record(2, "new-two", cl::log_record_kind::raft_entry, 4));
        CRPC_REQUIRE(replaced.ok());
        CRPC_CHECK_EQ(u64{2}, log->last_index());
        CRPC_CHECK(file_bytes(path) < static_cast<std::size_t>(size_after_three));
    }

    auto opened = cl::file_registry_log::open(path);
    CRPC_REQUIRE(opened.ok());
    auto log = std::move(opened.value());
    const auto replayed = log->replay(0);
    CRPC_REQUIRE(replayed.ok());
    CRPC_REQUIRE(replayed.value().size() == std::size_t{2});
    CRPC_CHECK_EQ(u64{1}, replayed.value()[0].index);
    CRPC_CHECK_EQ(std::string("old-1"), text_of(replayed.value()[0].payload));
    CRPC_CHECK_EQ(u64{2}, replayed.value()[1].index);
    CRPC_CHECK_EQ(std::string("new-two"), text_of(replayed.value()[1].payload));
    CRPC_CHECK_EQ(u64{4}, replayed.value()[1].term);
}

// --- backend split: the memory log never claims durability ------------------

CRPC_TEST(memory_log_has_no_power_loss_surface) {
    cl::memory_registry_log log;
    for (u64 index = 1; index <= 3; ++index) {
        CRPC_REQUIRE(log.append(make_record(index, "mem-" + std::to_string(index))).ok());
    }
    CRPC_CHECK_EQ(u64{3}, log.last_index());

    // E16 semantics hold on both backends.
    CRPC_REQUIRE(log.append(make_record(2, "mem-replacement")).ok());
    const auto replayed = log.replay(0);
    CRPC_REQUIRE(replayed.ok());
    CRPC_REQUIRE(replayed.value().size() == std::size_t{2});
    CRPC_CHECK_EQ(std::string("mem-replacement"), text_of(replayed.value()[1].payload));

    // from_index filtering is part of the interface, not an optimisation.
    const auto tail = log.replay(2);
    CRPC_REQUIRE(tail.ok());
    CRPC_REQUIRE(tail.value().size() == std::size_t{1});
    CRPC_CHECK_EQ(u64{2}, tail.value()[0].index);
}

// --- E13b: a failed sync must not look like success -------------------------
//
// This is the only assertion that can separate "flushed" from "claimed to have
// flushed". Everything that reads `durable_size()`, `flush_count()` or even the
// file afterwards is answered by the implementation's own bookkeeping: a mutant
// whose sync() skips the OS flush and advances the boundary anyway passes all of
// them (verified: it passed the whole suite before this case existed). Reopening
// the file does not help either -- both paths read the same page cache. The
// failure path is therefore the observable the durability contract needs.

CRPC_TEST(e13b_failed_sync_must_not_advance_boundary) {
    log_dir dir("crpc-loga-");
    CRPC_REQUIRE(dir.valid());
    const std::string path = dir.file("failed-sync.log");

    auto opened = cl::file_registry_log::open(path);
    CRPC_REQUIRE(opened.ok());
    auto log = std::move(opened.value());

    CRPC_REQUIRE(log->append(make_record(1, "never-durable")).ok());
    const u64 boundary_before = log->durable_size();
    const u64 flushes_before = log->flush_count();

    log->inject_flush_failure();
    const status failed = log->sync();
    CRPC_CHECK_FALSE(failed.ok());
    if (!failed.ok()) {
        CRPC_CHECK(failed.code() != rpc_code::ok);
    }

    // Three separate facts, all of which a fake sync would violate.
    CRPC_CHECK_EQ(boundary_before, log->durable_size());
    CRPC_CHECK_EQ(flushes_before, log->flush_count());

    // A power loss now discards the record, because it never became durable.
    log->drop_unsynced_tail();
    CRPC_CHECK_EQ(u64{0}, log->last_index());

    // The injection is one-shot: the next sync succeeds and makes the record
    // durable, proving the case is not passing simply because the log is broken.
    CRPC_REQUIRE(log->append(make_record(1, "second-attempt")).ok());
    const status recovered = log->sync();
    CRPC_CHECK(recovered.ok());
    CRPC_CHECK(log->flush_count() > flushes_before);
    CRPC_CHECK(log->durable_size() > boundary_before);
    log->drop_unsynced_tail();
    CRPC_CHECK_EQ(u64{1}, log->last_index());

    const auto replayed = log->replay(0);
    CRPC_REQUIRE(replayed.ok());
    CRPC_REQUIRE(replayed.value().size() == std::size_t{1});
    CRPC_CHECK_EQ(std::string("second-attempt"), text_of(replayed.value()[0].payload));
}

// --- E18: index gaps are rejected on both backends --------------------------

CRPC_TEST(e18_index_gap_is_rejected) {
    log_dir dir("crpc-loga-");
    CRPC_REQUIRE(dir.valid());
    const std::string path = dir.file("gap.log");

    {
        auto opened = cl::file_registry_log::open(path);
        CRPC_REQUIRE(opened.ok());
        auto log = std::move(opened.value());
        CRPC_REQUIRE(log->append(make_record(1, "one")).ok());

        // A gap would make a lost record indistinguishable from a legitimate
        // jump, and a later append at 2 would silently cut index 5.
        const auto gapped = log->append(make_record(5, "five"));
        CRPC_CHECK_FALSE(gapped.ok());
        if (!gapped.ok()) {
            CRPC_CHECK(gapped.code() == rpc_code::invalid_argument);
            CRPC_CHECK(gapped.error().message.find("gap") != std::string::npos);
        }
        CRPC_CHECK_EQ(u64{1}, log->last_index());
        CRPC_REQUIRE(log->append(make_record(2, "two")).ok());
        CRPC_CHECK_EQ(u64{2}, log->last_index());
    }

    cl::memory_registry_log memory;
    CRPC_REQUIRE(memory.append(make_record(1, "one")).ok());
    const auto gapped = memory.append(make_record(4, "four"));
    CRPC_CHECK_FALSE(gapped.ok());
    if (!gapped.ok()) {
        CRPC_CHECK(gapped.code() == rpc_code::invalid_argument);
    }
    CRPC_CHECK_EQ(u64{1}, memory.last_index());
}

// --- F1: the replay stream is strictly increasing with no duplicate index ---
//
// This is the invariant `restore_from_log` relies on. It is asserted here,
// against the live backend, because a folding test built on a real log is
// vacuous: C2 (append truncating a conflicting suffix) already leaves only the
// newest record on disk, so "apply in order" and "fold last-wins" produce the
// same result and a first-wins mutation would stay green. F2 (a synthesised
// duplicate stream fed straight to restore_from_log) is what discriminates the
// folding strategy; F1 is what proves the invariant C2 promises.
CRPC_TEST(f1_replay_stream_is_strictly_increasing) {
    log_dir dir("crpc-loga-");
    CRPC_REQUIRE(dir.valid());
    const std::string path = dir.file("invariant.log");

    auto opened = cl::file_registry_log::open(path);
    CRPC_REQUIRE(opened.ok());
    auto log = std::move(opened.value());

    for (u64 index = 1; index <= 4; ++index) {
        CRPC_REQUIRE(log->append(make_record(index, "v" + std::to_string(index))).ok());
    }
    // Rewrite the middle: index 2 and 3 are replaced, 4 disappears.
    CRPC_REQUIRE(log->append(make_record(2, "replaced", cl::log_record_kind::raft_entry, 9)).ok());
    CRPC_REQUIRE(log->append(make_record(3, "after", cl::log_record_kind::raft_entry, 9)).ok());

    const auto replayed = log->replay(0);
    CRPC_REQUIRE(replayed.ok());
    CRPC_REQUIRE(replayed.value().size() == std::size_t{3});
    for (std::size_t i = 1; i < replayed.value().size(); ++i) {
        // Strictly increasing: no duplicate and no gap in the returned stream.
        CRPC_CHECK(replayed.value()[i].index == replayed.value()[i - 1].index + 1);
    }
    CRPC_CHECK_EQ(u64{1}, replayed.value()[0].index);
    CRPC_CHECK_EQ(u64{3}, replayed.value().back().index);
    CRPC_CHECK_EQ(std::string("replaced"), text_of(replayed.value()[1].payload));

    // from_index must not change the invariant either.
    const auto tail = log->replay(2);
    CRPC_REQUIRE(tail.ok());
    CRPC_REQUIRE(tail.value().size() == std::size_t{2});
    CRPC_CHECK_EQ(u64{2}, tail.value()[0].index);
    CRPC_CHECK_EQ(u64{3}, tail.value()[1].index);
}

CRPC_TEST_MAIN()
