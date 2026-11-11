// CRPC -- G19 step4 PR-7: unit tests for cluster::kv_store.
//
// The store is exercised directly (no service, no Raft, no ACL): each case
// asserts the exact `kv_result_kind` and the index accounting, not just
// "it worked". A store that returned the right kind but the wrong index would
// let a later `cas=N` observe a stale modify_index, which is exactly the
// conflict the spec forbids.
#include <cstdint>
#include <string>
#include <vector>

#include "test_framework.hpp"

#include <crpc/cluster/kv_store.hpp>
#include <crpc/error.hpp>

namespace cl = crpc::cluster;

namespace {

// A byte vector from a string, for readable cases.
std::vector<std::byte> bytes_of(const std::string& text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

std::string text_of(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} // namespace

CRPC_TEST(kv_put_get_round_trips) {
    cl::kv_store store;
    const cl::kv_write_result written = store.put("name", bytes_of("alice"), 1);
    CRPC_CHECK_EQ(cl::kv_result_kind::ok, written.kind);
    CRPC_CHECK_EQ(1ull, written.index);
    CRPC_CHECK_EQ(1ull, written.modify_index);

    const crpc::result<cl::kv_entry> got = store.get("name");
    CRPC_CHECK(static_cast<bool>(got));
    CRPC_CHECK_EQ("alice", text_of(got.value().value));
    CRPC_CHECK_EQ(1ull, got.value().create_index);
    CRPC_CHECK_EQ(1ull, got.value().modify_index);
}

CRPC_TEST(kv_get_missing_key_is_not_found) {
    cl::kv_store store;
    const crpc::result<cl::kv_entry> got = store.get("absent");
    CRPC_CHECK_FALSE(static_cast<bool>(got));
    CRPC_CHECK_EQ(crpc::rpc_code::not_found, got.code());
}

CRPC_TEST(kv_put_overwrites_and_advances_modify_index) {
    cl::kv_store store;
    const cl::kv_write_result first = store.put("k", bytes_of("v1"), 1);
    CRPC_CHECK_EQ(1ull, first.modify_index);
    const cl::kv_write_result second = store.put("k", bytes_of("v2"), 7);
    CRPC_CHECK_EQ(7ull, second.modify_index);
    const crpc::result<cl::kv_entry> got = store.get("k");
    CRPC_CHECK(static_cast<bool>(got));
    CRPC_CHECK_EQ("v2", text_of(got.value().value));
    CRPC_CHECK_EQ(1ull, got.value().create_index);
    CRPC_CHECK_EQ(7ull, got.value().modify_index);
}

CRPC_TEST(kv_cas_zero_creates_only) {
    cl::kv_store store;
    const cl::kv_write_result created = store.cas("k", bytes_of("v"), 0, 3);
    CRPC_CHECK_EQ(cl::kv_result_kind::ok, created.kind);
    CRPC_CHECK_EQ(3ull, created.modify_index);
    const crpc::result<cl::kv_entry> got = store.get("k");
    CRPC_CHECK(static_cast<bool>(got));
    CRPC_CHECK_EQ("v", text_of(got.value().value));
    CRPC_CHECK_EQ(3ull, got.value().create_index);
    CRPC_CHECK_EQ(3ull, got.value().modify_index);
}

CRPC_TEST(kv_cas_zero_on_existing_key_conflicts) {
    cl::kv_store store;
    const cl::kv_write_result first = store.put("k", bytes_of("v1"), 1);
    CRPC_CHECK_EQ(cl::kv_result_kind::ok, first.kind);
    const cl::kv_write_result conflict = store.cas("k", bytes_of("v2"), 0, 2);
    CRPC_CHECK_EQ(cl::kv_result_kind::cas_conflict, conflict.kind);
    const crpc::result<cl::kv_entry> got = store.get("k");
    CRPC_CHECK(static_cast<bool>(got));
    CRPC_CHECK_EQ("v1", text_of(got.value().value));
    CRPC_CHECK_EQ(1ull, got.value().modify_index);
}

CRPC_TEST(kv_cas_zero_conflicts_when_modify_index_is_zero) {
    cl::kv_store store;
    // A write at index 0 leaves modify_index == 0. cas=0 is create-only and
    // must still conflict: the explicit cas_index==0 guard cannot be replaced
    // by the modify_index comparison alone.
    const cl::kv_write_result first = store.put("k", bytes_of("v1"), 0);
    CRPC_CHECK_EQ(cl::kv_result_kind::ok, first.kind);
    CRPC_CHECK_EQ(0ull, first.modify_index);
    const cl::kv_write_result conflict = store.cas("k", bytes_of("v2"), 0, 5);
    CRPC_CHECK_EQ(cl::kv_result_kind::cas_conflict, conflict.kind);
    const crpc::result<cl::kv_entry> got = store.get("k");
    CRPC_CHECK(static_cast<bool>(got));
    CRPC_CHECK_EQ("v1", text_of(got.value().value));
    CRPC_CHECK_EQ(0ull, got.value().modify_index);
}

CRPC_TEST(kv_cas_matching_index_succeeds) {
    cl::kv_store store;
    const cl::kv_write_result first = store.put("k", bytes_of("v1"), 1);
    CRPC_CHECK_EQ(1ull, first.modify_index);
    const cl::kv_write_result updated = store.cas("k", bytes_of("v2"), 1, 5);
    CRPC_CHECK_EQ(cl::kv_result_kind::ok, updated.kind);
    CRPC_CHECK_EQ(5ull, updated.modify_index);
    const crpc::result<cl::kv_entry> got = store.get("k");
    CRPC_CHECK(static_cast<bool>(got));
    CRPC_CHECK_EQ("v2", text_of(got.value().value));
    CRPC_CHECK_EQ(1ull, got.value().create_index);
    CRPC_CHECK_EQ(5ull, got.value().modify_index);
}

CRPC_TEST(kv_cas_stale_index_conflicts) {
    cl::kv_store store;
    const cl::kv_write_result first = store.put("k", bytes_of("v1"), 1);
    CRPC_CHECK_EQ(1ull, first.modify_index);
    const cl::kv_write_result second = store.put("k", bytes_of("v2"), 4);
    CRPC_CHECK_EQ(4ull, second.modify_index);
    // cas=N where N is no longer the entry's modify_index must conflict.
    const cl::kv_write_result conflict = store.cas("k", bytes_of("v3"), 1, 6);
    CRPC_CHECK_EQ(cl::kv_result_kind::cas_conflict, conflict.kind);
    const crpc::result<cl::kv_entry> got = store.get("k");
    CRPC_CHECK(static_cast<bool>(got));
    CRPC_CHECK_EQ("v2", text_of(got.value().value));
    CRPC_CHECK_EQ(4ull, got.value().modify_index);
}

CRPC_TEST(kv_delete_removes_and_returns_index) {
    cl::kv_store store;
    const cl::kv_write_result put = store.put("k", bytes_of("v"), 2);
    CRPC_CHECK_EQ(cl::kv_result_kind::ok, put.kind);
    const cl::kv_write_result removed = store.erase("k", 9);
    CRPC_CHECK_EQ(cl::kv_result_kind::ok, removed.kind);
    CRPC_CHECK_EQ(9ull, removed.index);
    CRPC_CHECK_EQ(9ull, removed.modify_index);
    const crpc::result<cl::kv_entry> got = store.get("k");
    CRPC_CHECK_FALSE(static_cast<bool>(got));
    CRPC_CHECK_EQ(crpc::rpc_code::not_found, got.code());
}

CRPC_TEST(kv_delete_missing_key_is_not_found) {
    cl::kv_store store;
    const cl::kv_write_result removed = store.erase("absent", 5);
    CRPC_CHECK_EQ(cl::kv_result_kind::not_found, removed.kind);
}

CRPC_TEST(kv_invalid_key_is_rejected) {
    cl::kv_store store;
    // Empty key.
    CRPC_CHECK_FALSE(cl::kv_store::valid_key(""));
    // Control characters.
    CRPC_CHECK_FALSE(cl::kv_store::valid_key("a\tb"));
    CRPC_CHECK_FALSE(cl::kv_store::valid_key("a\nb"));
    CRPC_CHECK_FALSE(cl::kv_store::valid_key("\x7f"));
    // Over 512 bytes.
    const std::string too_long(513, 'a');
    CRPC_CHECK_FALSE(cl::kv_store::valid_key(too_long));
    // Exactly 512 bytes is valid.
    const std::string exactly_512(512, 'a');
    CRPC_CHECK(cl::kv_store::valid_key(exactly_512));
    // A normal UTF-8 key is valid.
    CRPC_CHECK(cl::kv_store::valid_key("servers/eu-01"));

    const cl::kv_write_result put_bad = store.put("", bytes_of("v"), 1);
    CRPC_CHECK_EQ(cl::kv_result_kind::invalid_key, put_bad.kind);
    const cl::kv_write_result cas_bad = store.cas("", bytes_of("v"), 0, 1);
    CRPC_CHECK_EQ(cl::kv_result_kind::invalid_key, cas_bad.kind);
    const cl::kv_write_result erase_bad = store.erase("", 1);
    CRPC_CHECK_EQ(cl::kv_result_kind::invalid_key, erase_bad.kind);
    const crpc::result<cl::kv_entry> get_bad = store.get("");
    CRPC_CHECK_FALSE(static_cast<bool>(get_bad));
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument, get_bad.code());
}

CRPC_TEST(kv_keys_with_prefix_is_sorted) {
    cl::kv_store store;
    (void)store.put("servers/eu-02", bytes_of("b"), 1);
    (void)store.put("servers/eu-01", bytes_of("a"), 2);
    (void)store.put("services/db", bytes_of("c"), 3);
    const std::vector<std::string> keys = store.keys_with_prefix("servers/");
    CRPC_CHECK_EQ(2ul, keys.size());
    if (keys.size() != 2) {
        // Element checks below would index out of range; report and stop.
        return;
    }
    CRPC_CHECK(std::string("servers/eu-01") == keys[0]);
    CRPC_CHECK(std::string("servers/eu-02") == keys[1]);
}

CRPC_TEST(kv_highest_index_tracks_writes) {
    cl::kv_store store;
    CRPC_CHECK_EQ(0ull, store.highest_index());
    (void)store.put("a", bytes_of("1"), 3);
    CRPC_CHECK_EQ(3ull, store.highest_index());
    (void)store.put("b", bytes_of("2"), 1);
    CRPC_CHECK_EQ(3ull, store.highest_index());
    (void)store.put("c", bytes_of("3"), 10);
    CRPC_CHECK_EQ(10ull, store.highest_index());
    (void)store.cas("d", bytes_of("4"), 0, 2);
    CRPC_CHECK_EQ(10ull, store.highest_index());
}

CRPC_TEST_MAIN()

