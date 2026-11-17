// CRPC -- tests for fs file operations (whole-file I/O, atomic write, rename,
// copy, attributes, timestamps).
#include <crpc/fs/fs.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::fs;
#if defined(CRPC_TESTING)
using crpc::fs::detail::zero_write_mode;
#endif

namespace {

// A private scratch directory per call site, removed on destruction.
struct scratch {
    result<temp_dir> made = temp_dir::create();
    path dir() const { return made.value().native(); }

    path file(std::string_view name) const { return dir() / path(std::string(name)); }
};

span<const std::byte> bytes_of(const std::string& s) {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

#if defined(CRPC_TESTING)
// Runs `body` on a watchdog-thread budget: if the call under test hangs (the
// zero-progress spin this file guards against), the body is abandoned and the
// case fails instead of blocking CTest forever. The watchdog is only a detector
// -- it cannot unblock a stuck thread, so the process still has to exit after
// the case returns; that is what turns "hang" into "failure with evidence".
//
// Shared state lives behind a shared_ptr captured *by value*, so a detached
// worker keeps its own reference alive past this function's return instead of
// writing through a dangling stack reference. `done`/`ok` are atomics because
// the worker writes them while the polling loop reads them.
template <class Fn>
bool runs_within(int timeout_ms, Fn&& body) {
    struct shared_state {
        std::atomic<bool> done{false};
        std::atomic<bool> ok{false};
    };
    const auto state = std::make_shared<shared_state>();
    // Own the callable too: `Fn&& body` binds the parameter, which dies with
    // this frame, so a detached worker calling through `&body` would read a
    // dead reference (stack-use-after-scope). Copying it into a shared_ptr by
    // value gives the detached thread its own object for as long as it runs.
    using body_t = std::remove_reference_t<Fn>;
    const auto owned = std::make_shared<body_t>(std::forward<Fn>(body));
    std::thread worker([state, owned]() {
        state->ok.store((*owned)(), std::memory_order_release);
        state->done.store(true, std::memory_order_release);
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!state->done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool finished = state->done.load(std::memory_order_acquire);
    if (finished) {
        worker.join();
    } else {
        // Detach: the stuck thread must not keep the process alive past main, but
        // joining it would defeat the watchdog. Its shared_ptr copy keeps the
        // state it writes alive for as long as the thread runs.
        worker.detach();
    }
    return finished && state->ok.load(std::memory_order_acquire);
}
#endif

} // namespace

CRPC_TEST(fs_file_write_read_roundtrip) {
    scratch s;
    const path p = s.file("data.bin");
    const std::string text = "hello crpc fs \xE4\xB8\x96\xE7\x95\x8C"; // UTF-8 world
    CRPC_CHECK(write_all_text(p, text).ok());
    CRPC_CHECK(exists(p).ok());
    CRPC_CHECK_EQ(true, exists(p).value());

    result<std::string> back = read_all_text(p);
    CRPC_CHECK(back.ok());
    CRPC_CHECK_EQ(text, back.value());

    result<u64> sz = file_size(p);
    CRPC_CHECK(sz.ok());
    CRPC_CHECK_EQ(static_cast<u64>(text.size()), sz.value());

    CRPC_CHECK(is_file(p).ok());
    CRPC_CHECK_EQ(true, is_file(p).value());
}

CRPC_TEST(fs_file_write_all_truncates_and_reads_bytes) {
    scratch s;
    const path p = s.file("bytes");
    const std::string first = "0123456789";
    const std::string second = "second";
    CRPC_CHECK(write_all_bytes(p, bytes_of(first)).ok());
    result<byte_buffer> big = read_all_bytes(p);
    CRPC_CHECK(big.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(10), big.value().size());
    CRPC_CHECK_EQ(std::string_view("0123456789"), big.value().to_string_view());

    // Overwrite truncates: a shorter file replaces the longer one completely.
    CRPC_CHECK(write_all_bytes(p, bytes_of(second)).ok());
    result<byte_buffer> small = read_all_bytes(p);
    CRPC_CHECK(small.ok());
    CRPC_CHECK_EQ(std::string_view("second"), small.value().to_string_view());
    CRPC_CHECK_EQ(static_cast<u64>(6), file_size(p).value());
}

CRPC_TEST(fs_file_append) {
    scratch s;
    const path p = s.file("appended");
    CRPC_CHECK(append_text(p, "a").ok());
    CRPC_CHECK(append_text(p, "b").ok());
    CRPC_CHECK(append_bytes(p, bytes_of("c")).ok());
    CRPC_CHECK_EQ(std::string("abc"), read_all_text(p).value());
}

CRPC_TEST(fs_file_stat_metadata) {
    scratch s;
    const path p = s.file("meta");
    CRPC_CHECK(write_all_text(p, "x").ok());
    result<file_info> i = stat(p);
    CRPC_CHECK(i.ok());
    CRPC_CHECK_EQ(static_cast<u64>(1), i.value().size);
    CRPC_CHECK_FALSE(i.value().is_directory);
    CRPC_CHECK_FALSE(i.value().is_read_only);
    CRPC_CHECK(i.value().last_write_time != file_time());
    CRPC_CHECK(exists(s.file("nope")).ok());
    CRPC_CHECK_EQ(false, exists(s.file("nope")).value());
}

CRPC_TEST(fs_file_missing_is_not_found) {
    scratch s;
    const path p = s.file("absent");
    CRPC_CHECK(exists(p).ok());
    CRPC_CHECK_EQ(false, exists(p).value());

    result<byte_buffer> r = read_all_bytes(p);
    CRPC_CHECK_FALSE(r.ok());
    CRPC_CHECK_EQ(rpc_code::not_found, r.code());

    result<file_info> st = stat(p);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK_EQ(rpc_code::not_found, st.code());
}

CRPC_TEST(fs_file_directory_as_file_is_invalid) {
    scratch s;
    const path dir = s.file("asdir");
    CRPC_CHECK(create_directory(dir).ok());

    result<byte_buffer> r = read_all_bytes(dir);
    CRPC_CHECK_FALSE(r.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, r.code());

    result<u64> sz = file_size(dir);
    CRPC_CHECK_FALSE(sz.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, sz.code());

    CRPC_CHECK(is_file(dir).ok());
    CRPC_CHECK_EQ(false, is_file(dir).value());
}

CRPC_TEST(fs_file_read_range) {
    scratch s;
    const path p = s.file("range");
    CRPC_CHECK(write_all_text(p, "0123456789").ok());

    std::byte buffer[6];
    result<std::size_t> n = read_range(p, 2, span<std::byte>(buffer));
    CRPC_CHECK(n.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(6), n.value());
    std::string got(reinterpret_cast<char*>(buffer), n.value());
    CRPC_CHECK_EQ(std::string("234567"), got);

    // Tail shorter than the buffer.
    std::byte tail[8];
    result<std::size_t> t = read_range(p, 7, span<std::byte>(tail));
    CRPC_CHECK(t.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), t.value());

    // Offset at EOF is a clean 0, not an error.
    result<std::size_t> e = read_range(p, 10, span<std::byte>(buffer));
    CRPC_CHECK(e.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), e.value());

    // Offset beyond EOF also 0.
    CRPC_CHECK_EQ(static_cast<std::size_t>(0),
                  read_range(p, 999, span<std::byte>(buffer)).value());

    // Empty destination.
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), read_range(p, 0, span<std::byte>()).value());
}

CRPC_TEST(fs_file_atomic_write_replaces_content) {
    scratch s;
    const path p = s.file("cfg");
    CRPC_CHECK(write_all_text_atomic(p, "v1").ok());
    CRPC_CHECK_EQ(std::string("v1"), read_all_text(p).value());
    CRPC_CHECK(write_all_text_atomic(p, "v2").ok());
    CRPC_CHECK_EQ(std::string("v2"), read_all_text(p).value());

    // No temporary clutter next to the target.
    result<std::vector<directory_entry>> entries = list_directory(s.dir());
    CRPC_CHECK(entries.ok());
    for (const directory_entry& e : entries.value()) {
        CRPC_CHECK_EQ(std::string("cfg"), e.name);
    }
}

CRPC_TEST(fs_file_interrupted_atomic_write_keeps_old_content) {
    scratch s;
    const path p = s.file("cfg");
    CRPC_CHECK(write_all_text(p, "old-content-that-must-survive").ok());

    const std::string fresh = "brand-new-content-that-is-much-longer";
    // Simulate a write that dies half-way through the staging phase.
    const status failed =
        detail::atomic_write_impl(p, bytes_of(fresh), fresh.size() / 2);
    CRPC_CHECK_FALSE(failed.ok());

    // The target still holds the old bytes -- not a torn mix of old and new.
    CRPC_CHECK_EQ(std::string("old-content-that-must-survive"),
                  read_all_text(p).value());

    // The partial temporary file was cleaned up.
    result<std::vector<directory_entry>> entries = list_directory(s.dir());
    CRPC_CHECK(entries.ok());
    for (const directory_entry& e : entries.value()) {
        CRPC_CHECK_EQ(e.name.find(".tmp."), std::string::npos);
    }
}

CRPC_TEST(fs_file_atomic_write_stage_failure_leaves_target_absent) {
    scratch s;
    const path p = s.file("fresh");
    // Target does not exist yet; staging must fail before anything is created,
    // and must not leave a stray temp file behind.
    const status failed =
        detail::atomic_write_impl(p, bytes_of(std::string("data")), 2);
    CRPC_CHECK_FALSE(failed.ok());
    CRPC_CHECK(exists(p).ok());
    CRPC_CHECK_EQ(false, exists(p).value());
}

CRPC_TEST(fs_file_atomic_write_to_missing_parent) {
    scratch s;
    const path p = s.file("no/such/dir/file");
    status s2 = write_all_text_atomic(p, "x");
    CRPC_CHECK_FALSE(s2.ok());
    CRPC_CHECK_EQ(rpc_code::not_found, s2.code());
}

CRPC_TEST(fs_file_rename_move) {
    scratch s;
    const path from = s.file("move-me");
    const path to = s.file("moved");
    CRPC_CHECK(write_all_text(from, "payload").ok());
    CRPC_CHECK(rename(from, to).ok());
    CRPC_CHECK_EQ(false, exists(from).value());
    CRPC_CHECK(exists(to).ok());
    CRPC_CHECK_EQ(true, exists(to).value());
    CRPC_CHECK_EQ(std::string("payload"), read_all_text(to).value());

    // Overwrite an existing destination.
    const path dst = s.file("dst");
    CRPC_CHECK(write_all_text(dst, "old-dst").ok());
    CRPC_CHECK(rename(to, dst).ok());
    CRPC_CHECK_EQ(std::string("payload"), read_all_text(dst).value());
}

CRPC_TEST(fs_file_rename_missing_source) {
    scratch s;
    const status r = rename(s.file("ghost"), s.file("also-ghost"));
    CRPC_CHECK_FALSE(r.ok());
    CRPC_CHECK_EQ(rpc_code::not_found, r.code());
}

CRPC_TEST(fs_file_copy) {
    scratch s;
    const path from = s.file("src");
    const path to = s.file("dst");
    CRPC_CHECK(write_all_text(from, "copy me").ok());
    CRPC_CHECK(copy_file(from, to).ok());
    CRPC_CHECK_EQ(std::string("copy me"), read_all_text(to).value());

    // overwrite=true replaces; overwrite=false refuses.
    CRPC_CHECK(write_all_text(from, "changed").ok());
    CRPC_CHECK(copy_file(from, to, true).ok());
    CRPC_CHECK_EQ(std::string("changed"), read_all_text(to).value());
    const status refused = copy_file(from, to, false);
    CRPC_CHECK_FALSE(refused.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, refused.code());
}

CRPC_TEST(fs_file_copy_missing) {
    scratch s;
    const status r = copy_file(s.file("ghost"), s.file("dst"));
    CRPC_CHECK_FALSE(r.ok());
    CRPC_CHECK_EQ(rpc_code::not_found, r.code());
}

CRPC_TEST(fs_file_remove_is_idempotent) {
    scratch s;
    const path p = s.file("delete-me");
    CRPC_CHECK(write_all_text(p, "bye").ok());
    CRPC_CHECK(remove_file(p).ok());
    CRPC_CHECK_EQ(false, exists(p).value());
    CRPC_CHECK(remove_file(p).ok()); // deleting again is fine
}

CRPC_TEST(fs_file_read_only_blocks_writes) {
    scratch s;
    const path p = s.file("locked");
    CRPC_CHECK(write_all_text(p, "content").ok());
    CRPC_CHECK(set_read_only(p, true).ok());

    result<file_info> i = stat(p);
    CRPC_CHECK(i.ok());
    CRPC_CHECK(i.value().is_read_only);

    status write = write_all_text(p, "should be refused");
    CRPC_CHECK_FALSE(write.ok());
    CRPC_CHECK_EQ(rpc_code::permission_denied, write.code());

    // The old content is untouched (the open failed before truncate).
    CRPC_CHECK_EQ(std::string("content"), read_all_text(p).value());

    CRPC_CHECK(set_read_only(p, false).ok());
    CRPC_CHECK(write_all_text(p, "back to writable").ok());
    CRPC_CHECK_EQ(std::string("back to writable"), read_all_text(p).value());
}

CRPC_TEST(fs_file_set_last_write_time_roundtrip) {
    scratch s;
    const path p = s.file("stamp");
    CRPC_CHECK(write_all_text(p, "t").ok());
    const file_time target =
        file_time(std::chrono::system_clock::from_time_t(1'600'000'000LL));
    CRPC_CHECK(set_last_write_time(p, target).ok());
    result<file_info> i = stat(p);
    CRPC_CHECK(i.ok());
    const auto diff = std::chrono::duration_cast<std::chrono::seconds>(
                          i.value().last_write_time - target)
                          .count();
    CRPC_CHECK(diff >= -1 && diff <= 1);
}

CRPC_TEST(fs_file_write_under_a_file_parent_fails) {
    scratch s;
    const path blocker = s.file("just-a-file");
    CRPC_CHECK(write_all_text(blocker, "x").ok());
    const status r = write_all_text(blocker / path("child"), "y");
    CRPC_CHECK_FALSE(r.ok());
}

CRPC_TEST(fs_file_overlong_path_is_reported) {
    scratch s;
    // Without the \\?\ prefix Win32 refuses paths beyond MAX_PATH. Depending
    // on the system's "long paths" policy it may instead succeed; both are
    // correct behaviour for the layer, so the probe only asserts it does not
    // crash and behaves consistently.
    std::string long_component(220, 'a');
    const path p = s.dir() / path(long_component);
    status r = write_all_text(p, "x");
    if (r.ok()) {
        // The system allows long paths; leave a real file but clean it up.
        CRPC_CHECK(is_file(p).ok());
        CRPC_CHECK_EQ(true, is_file(p).value());
        CRPC_CHECK(remove_file(p).ok());
    } else {
        CRPC_CHECK_FALSE(r.ok());
    }
}

#if defined(CRPC_TESTING) && !defined(CRPC_OS_WINDOWS)
// --- Zero-progress write: a `write` that reports success but moves no bytes ---
//
// A loop that advances by the transferred count cannot make progress when the
// OS reports 0 for a non-empty request, so it spins forever -- or, worse,
// returns "success" having written nothing at all. These cases arm the
// test-only seam (`detail::set_zero_write_fault`) to make one transfer return
// zero and assert each public API answers with `internal` and leaves no torn
// output behind.
//
// POSIX only: the Windows branches are separate loops that already carry their
// own `wrote == 0` guard (and Windows `copy_file` is CopyFileW, not a loop), so
// the zero-progress condition this defect is about does not exist there. The
// seam hooks compile on both platforms but are only armed by these cases.

namespace {

void clear_zero_write_fault(const path& p) {
    detail::set_zero_write_fault(p.str(), zero_write_mode::off);
}

} // namespace

CRPC_TEST(fs_file_write_all_zero_progress_fails_and_writes_nothing) {
    scratch s;
    const path p = s.file("zero.bin");
    const std::string text = "payload-that-must-not-land";

    detail::set_zero_write_fault(p.str(), zero_write_mode::always);
    status failed = status();
    bool finished = false;
    finished = runs_within(2000, [&]() {
        failed = write_all_bytes(p, bytes_of(text));
        return true;
    });
    clear_zero_write_fault(p);
    CRPC_CHECK(finished);
    CRPC_CHECK_FALSE(failed.ok());
    CRPC_CHECK_EQ(rpc_code::internal, failed.code());
    result<byte_buffer> after = read_all_bytes(p);
    CRPC_CHECK(after.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), after.value().size());
}

CRPC_TEST(fs_file_append_zero_progress_fails_without_torn_tail) {
    scratch s;
    const path p = s.file("appended");
    CRPC_CHECK(write_all_text(p, "head").ok());

    detail::set_zero_write_fault(p.str(), zero_write_mode::once);
    bool finished = runs_within(2000, [&]() {
        const status r = append_text(p, "tail-that-must-not-land");
        return !r.ok();
    });
    clear_zero_write_fault(p);

    CRPC_CHECK(finished);
    CRPC_CHECK_EQ(std::string("head"), read_all_text(p).value());
}

CRPC_TEST(fs_file_atomic_write_zero_progress_keeps_target) {
    scratch s;
    const path p = s.file("cfg");
    CRPC_CHECK(write_all_text(p, "old-content-that-must-survive").ok());

    // The staging temp name is random per call, so the fault is armed on the
    // target instead (see detail::set_zero_write_fault): the guarantee under
    // test is that the target survives.
    detail::set_zero_write_fault(p.str(), zero_write_mode::once);

    const std::string fresh = "brand-new-content-that-is-much-longer";
    bool finished = runs_within(2000, [&]() {
        const status r = write_all_bytes_atomic(p, bytes_of(fresh));
        return !r.ok();
    });
    clear_zero_write_fault(p);
    CRPC_CHECK(finished);

    // The target still holds the old bytes -- never a torn mix of old and new.
    CRPC_CHECK_EQ(std::string("old-content-that-must-survive"),
                  read_all_text(p).value());

    // No temporary clutter left next to the target.
    result<std::vector<directory_entry>> entries = list_directory(s.dir());
    CRPC_CHECK(entries.ok());
    for (const directory_entry& e : entries.value()) {
        CRPC_CHECK_EQ(e.name.find(".tmp."), std::string::npos);
    }
}

#if !defined(CRPC_OS_WINDOWS)
// Windows `copy_file` is CopyFileW, not a write loop, so the zero-progress
// condition does not exist there; this case covers the POSIX loop only.
CRPC_TEST(fs_file_copy_zero_progress_removes_partial_destination) {
    scratch s;
    const path from = s.file("src");
    const path to = s.file("dst");
    CRPC_CHECK(write_all_text(from, "copy me, then fail on the first transfer")
                   .ok());

    detail::set_zero_write_fault(to.str(), zero_write_mode::once);
    bool finished = runs_within(2000, [&]() {
        const status r = copy_file(from, to);
        return !r.ok();
    });
    clear_zero_write_fault(to);
    CRPC_CHECK(finished);

    // The partial destination is removed rather than left half-written.
    result<byte_buffer> after = read_all_bytes(to);
    CRPC_CHECK_FALSE(after.ok());
    // The seam cleared, the same copy succeeds.
    CRPC_CHECK(copy_file(from, to).ok());
    CRPC_CHECK_EQ(std::string("copy me, then fail on the first transfer"),
                  read_all_text(to).value());
}
#endif
#endif

CRPC_TEST_MAIN()

