// CRPC -- G19 step3 PR-3: Raft persistent state (term / votedFor).
//
// These cases pin the contract L2-4 rests on: the term and the vote survive a
// restart. The file is fail-closed -- a corrupt file is refused, never read as
// "term 0, no vote", because resetting the term is exactly the double-vote the
// persistence exists to prevent. That is why every malformed-file case asserts
// an error rather than a value.
//
// The atomicity case uses the fs layer's fault-injection seam
// (`atomic_write_stage` with `fault_after`): a write that stops mid-way must
// leave the previous value in place, not a truncated file.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <crpc/cluster/raft_state.hpp>
#include <crpc/endian.hpp>
#include <crpc/fs/directory.hpp>
#include <crpc/fs/file.hpp>
#include <crpc/fs/temp.hpp>

namespace cl = crpc::cluster;
namespace fs = crpc::fs;
using namespace crpc;

namespace {

struct state_dir {
    fs::temp_dir dir;

    explicit state_dir(const char* prefix) {
        auto created = fs::temp_dir::create(fs::path(), prefix);
        if (created.ok()) {
            dir = std::move(created.value());
        }
    }

    bool valid() const { return dir.valid(); }

    fs::path root() const { return dir.native(); }
};

std::vector<std::byte> read_bytes(const fs::path& p) {
    auto bytes = fs::read_all_bytes(p);
    if (!bytes.ok()) {
        return std::vector<std::byte>{};
    }
    const fs::byte_buffer& buf = bytes.value();
    return std::vector<std::byte>(buf.data(), buf.data() + buf.size());
}

bool write_bytes(const fs::path& p, const std::vector<std::byte>& data) {
    return fs::write_all_bytes(
               p, span<const std::byte>(data.data(), data.size()))
        .ok();
}

// Builds the on-disk shape directly, so a malformed case is not dependent on
// `save` producing the corruption itself.
std::vector<std::byte> encode(u64 term, const std::string& voted_for) {
    std::vector<std::byte> out;
    const auto put_u32 = [&out](u32 v) {
        std::byte bytes[sizeof(u32)];
        endian::store_le32(bytes, v);
        out.insert(out.end(), bytes, bytes + sizeof(bytes));
    };
    const auto put_u64 = [&out](u64 v) {
        std::byte bytes[sizeof(u64)];
        endian::store_le64(bytes, v);
        out.insert(out.end(), bytes, bytes + sizeof(bytes));
    };
    put_u64(term);
    put_u32(static_cast<u32>(voted_for.size()));
    out.insert(out.end(), reinterpret_cast<const std::byte*>(voted_for.data()),
               reinterpret_cast<const std::byte*>(voted_for.data()) +
                   voted_for.size());
    return out;
}

} // namespace

CRPC_TEST(raft_state_round_trips) {
    state_dir dir("crpc-raft-");
    CRPC_REQUIRE(dir.valid());
    const cl::raft_persistent_state value{5, "node-b"};
    CRPC_REQUIRE(cl::raft_state_store::save(dir.root(), value).ok());

    cl::raft_persistent_state out;
    const auto st = cl::raft_state_store::load(dir.root(), out);
    if (!st.ok()) {
        std::fprintf(stderr, "[diag] round_trip reason=%s\n", st.message().c_str());
    }
    CRPC_REQUIRE(st.ok());
    CRPC_CHECK_EQ(u64{5}, out.term);
    CRPC_CHECK_EQ(std::string("node-b"), out.voted_for);
}

CRPC_TEST(raft_state_absent_file_is_a_clean_zero) {
    state_dir dir("crpc-raft-");
    CRPC_REQUIRE(dir.valid());

    cl::raft_persistent_state out;
    out.term = 99;
    out.voted_for = "stale";
    const auto st = cl::raft_state_store::load(dir.root(), out);
    CRPC_REQUIRE(st.ok());
    CRPC_CHECK_EQ(u64{0}, out.term);
    CRPC_CHECK_EQ(std::string(), out.voted_for);
}

CRPC_TEST(raft_state_rejects_a_truncated_file) {
    state_dir dir("crpc-raft-");
    CRPC_REQUIRE(dir.valid());
    const fs::path path = cl::raft_state_store::file_path(dir.root());
    // Five bytes cannot hold the 12-byte header.
    CRPC_REQUIRE(write_bytes(path, {std::byte{1}, std::byte{2}, std::byte{3},
                                    std::byte{4}, std::byte{5}}));

    cl::raft_persistent_state out;
    out.term = 7;
    const auto st = cl::raft_state_store::load(dir.root(), out);
    CRPC_CHECK_FALSE(st.ok());
    if (!st.ok()) {
        CRPC_CHECK_EQ(rpc_code::failed_precondition, st.code());
    }
    // Fail-closed: the caller's out parameter must not be left looking like a
    // valid "term 0" that a naive implementation could have written.
    CRPC_CHECK_EQ(u64{7}, out.term);
}

CRPC_TEST(raft_state_rejects_trailing_bytes) {
    state_dir dir("crpc-raft-");
    CRPC_REQUIRE(dir.valid());
    const fs::path path = cl::raft_state_store::file_path(dir.root());
    std::vector<std::byte> body = encode(3, "node-a");
    body.push_back(std::byte{0xFF});
    body.push_back(std::byte{0xFF});
    CRPC_REQUIRE(write_bytes(path, body));

    cl::raft_persistent_state out;
    const auto st = cl::raft_state_store::load(dir.root(), out);
    CRPC_CHECK_FALSE(st.ok());
    if (!st.ok()) {
        CRPC_CHECK_EQ(rpc_code::failed_precondition, st.code());
    }
}

CRPC_TEST(raft_state_rejects_an_oversized_voted_for) {
    state_dir dir("crpc-raft-");
    CRPC_REQUIRE(dir.valid());
    const fs::path path = cl::raft_state_store::file_path(dir.root());
    // A length past the cap WITH the bytes actually present. The length alone is
    // not enough to prove the cap is enforced: a file that claims 2000 bytes and
    // has none would also be refused by the truncation check, so a missing cap
    // check would go unnoticed. The 2000 bytes are real, so only the cap can
    // reject this file.
    std::vector<std::byte> body;
    const auto put_u32 = [&body](u32 v) {
        std::byte bytes[sizeof(u32)];
        endian::store_le32(bytes, v);
        body.insert(body.end(), bytes, bytes + sizeof(bytes));
    };
    const auto put_u64 = [&body](u64 v) {
        std::byte bytes[sizeof(u64)];
        endian::store_le64(bytes, v);
        body.insert(body.end(), bytes, bytes + sizeof(bytes));
    };
    put_u64(2);
    put_u32(2000);
    body.resize(body.size() + 2000, std::byte{0x41});
    CRPC_REQUIRE(write_bytes(path, body));

    cl::raft_persistent_state out;
    const auto st = cl::raft_state_store::load(dir.root(), out);
    CRPC_CHECK_FALSE(st.ok());
    if (!st.ok()) {
        CRPC_CHECK_EQ(rpc_code::failed_precondition, st.code());
    }
}

// `save` must not refuse a value it would then be unable to read back, so the
// round trip covers the empty vote too (the legitimate "no vote cast" state).
CRPC_TEST(raft_state_round_trips_an_empty_vote) {
    state_dir dir("crpc-raft-");
    CRPC_REQUIRE(dir.valid());
    CRPC_REQUIRE(cl::raft_state_store::save(dir.root(), {4, ""}).ok());

    cl::raft_persistent_state out;
    CRPC_REQUIRE(cl::raft_state_store::load(dir.root(), out).ok());
    CRPC_CHECK_EQ(u64{4}, out.term);
    CRPC_CHECK_EQ(std::string(), out.voted_for);
}

CRPC_TEST(raft_state_overwrites_a_previous_value) {
    state_dir dir("crpc-raft-");
    CRPC_REQUIRE(dir.valid());
    CRPC_REQUIRE(cl::raft_state_store::save(dir.root(), {5, "node-a"}).ok());
    CRPC_REQUIRE(cl::raft_state_store::save(dir.root(), {6, "node-b"}).ok());

    cl::raft_persistent_state out;
    CRPC_REQUIRE(cl::raft_state_store::load(dir.root(), out).ok());
    CRPC_CHECK_EQ(u64{6}, out.term);
    CRPC_CHECK_EQ(std::string("node-b"), out.voted_for);
}

// "Reopen" here means a fresh `load` on the same directory, which is the
// observable a restart produces: the file is the only thing that survives.
CRPC_TEST(raft_state_survives_a_reopen) {
    state_dir dir("crpc-raft-");
    CRPC_REQUIRE(dir.valid());
    CRPC_REQUIRE(cl::raft_state_store::save(dir.root(), {9, "node-c"}).ok());

    // A first load sees the value...
    cl::raft_persistent_state first;
    CRPC_REQUIRE(cl::raft_state_store::load(dir.root(), first).ok());
    CRPC_CHECK_EQ(u64{9}, first.term);

    // ...and a second load, with no state carried between them, sees it again.
    cl::raft_persistent_state second;
    const auto st = cl::raft_state_store::load(dir.root(), second);
    CRPC_REQUIRE(st.ok());
    CRPC_CHECK_EQ(first.term, second.term);
    CRPC_CHECK_EQ(first.voted_for, second.voted_for);
}

// The write must be atomic: a reader sees the old value or the new one, never a
// partial file.
//
// Two observables are pinned here.
//
// (1) The fs layer's guarantee, exercised through its fault seam: a write that
//     stops mid-way leaves the target untouched. That is the mechanism `save`
//     relies on.
//
// (2) That `save` actually uses that mechanism. A non-atomic `write_all_bytes`
//     (CREATE_ALWAYS + WriteFile) truncates the target in place, so the file
//     keeps its creation time; the atomic path stages a new file and renames it
//     over the target, so the target is a *different* file and its creation
//     time moves. Measured on this platform: atomic -> creation time changes,
//     non-atomic -> it does not. This is the assertion that separates the two
//     implementations; without it the mutant that swaps in `write_all_bytes`
//     passes, which is exactly what happened before this case was rewritten.
CRPC_TEST(raft_state_save_uses_an_atomic_write_mechanism) {
    state_dir dir("crpc-raft-");
    CRPC_REQUIRE(dir.valid());
    const fs::path path = cl::raft_state_store::file_path(dir.root());

    CRPC_REQUIRE(cl::raft_state_store::save(dir.root(), {1, "node-a"}).ok());
    const std::vector<std::byte> before = read_bytes(path);
    CRPC_REQUIRE(!before.empty());
    const auto before_stat = fs::stat(path);
    CRPC_REQUIRE(before_stat.ok());

    // (1) An interrupted write must not disturb the target.
    const std::vector<std::byte> staged = encode(2, "node-b");
    const auto failed = fs::detail::atomic_write_impl(
        path, span<const std::byte>(staged.data(), staged.size()), 1);
    CRPC_CHECK_FALSE(failed.ok());
    const std::vector<std::byte> after = read_bytes(path);
    CRPC_CHECK_EQ(before.size(), after.size());
    CRPC_CHECK(std::memcmp(before.data(), after.data(), before.size()) == 0);

    // (2) A completed save replaces the file rather than truncating it in
    // place. The creation time is the observable: a rename installs a new file,
    // an in-place truncate keeps the old one. The clock is given room to move so
    // a coarse timestamp resolution cannot make the two look equal.
    for (volatile int spin = 0; spin < 20000000; ++spin) {
    }
    CRPC_REQUIRE(cl::raft_state_store::save(dir.root(), {2, "node-b"}).ok());
    const auto after_stat = fs::stat(path);
    CRPC_REQUIRE(after_stat.ok());
    CRPC_CHECK(before_stat.value().creation_time !=
               after_stat.value().creation_time);

    cl::raft_persistent_state out;
    CRPC_REQUIRE(cl::raft_state_store::load(dir.root(), out).ok());
    CRPC_CHECK_EQ(u64{2}, out.term);
    CRPC_CHECK_EQ(std::string("node-b"), out.voted_for);
}

// `save` must leave exactly one file behind: the target. A staging file left in
// the directory means `save` wrote but did not commit, which across elections
// grows without bound.
CRPC_TEST(raft_state_save_leaves_no_temp_file_behind) {
    state_dir dir("crpc-raft-");
    CRPC_REQUIRE(dir.valid());
    CRPC_REQUIRE(cl::raft_state_store::save(dir.root(), {3, "node-d"}).ok());

    const auto listed = fs::list_directory(dir.root());
    CRPC_REQUIRE(listed.ok());
    std::size_t state_files = 0;
    for (const auto& entry : listed.value()) {
        if (entry.name.find("registry.raftstate") != std::string::npos) {
            ++state_files;
        }
    }
    CRPC_CHECK_EQ(std::size_t{1}, state_files);
}

CRPC_TEST_MAIN()
