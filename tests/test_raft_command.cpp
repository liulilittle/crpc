// CRPC -- G19 step4 PR-9: raft_command codec tests.
//
// The command is the unit the apply loop replays, so a codec bug is not a bad
// value: it is two nodes applying different things from the same log. These
// cases therefore assert exact bytes for the shapes that matter, and assert
// that every malformed shape is refused rather than half-decoded into a
// command the state machine would then apply.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "test_framework.hpp"

#include <crpc/cluster/raft_command.hpp>
#include <crpc/error.hpp>

namespace cl = crpc::cluster;
using crpc::rpc_code;

namespace {

std::vector<std::byte> bytes_of(const std::string& text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

std::string text_of(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());
}

cl::raft_command make_put(const std::string& key, const std::string& value) {
    cl::raft_command command;
    command.kind = cl::raft_command_kind::kv_put;
    command.key = key;
    command.value = bytes_of(value);
    return command;
}

cl::raft_command make_acl_upsert() {
    cl::raft_command command;
    command.kind = cl::raft_command_kind::acl_upsert;
    command.rule.token_hash = 0x0102030405060708ull;
    command.rule.principal_id = "node-A";
    command.rule.node_id = "node-A";
    command.rule.admin = true;
    command.rule.read_prefixes.push_back("");
    command.rule.read_prefixes.push_back("servers/");
    command.rule.write_prefixes.push_back("servers/");
    return command;
}

} // namespace

// --- Round trips -------------------------------------------------------------

CRPC_TEST(kv_put_round_trips) {
    const cl::raft_command original = make_put("k", "value");
    std::vector<std::byte> encoded;
    CRPC_REQUIRE(cl::encode_raft_command(original, encoded).ok());
    CRPC_CHECK(!encoded.empty());

    cl::raft_command decoded;
    CRPC_REQUIRE(cl::decode_raft_command(
                     crpc::span<const std::byte>(encoded.data(), encoded.size()),
                     decoded)
                     .ok());
    CRPC_CHECK(decoded.kind == cl::raft_command_kind::kv_put);
    CRPC_CHECK_EQ(std::string("k"), decoded.key);
    CRPC_CHECK_EQ(std::string("value"), text_of(decoded.value));
}

CRPC_TEST(kv_put_carries_binary_values_unchanged) {
    // A KV value is raw bytes, not text. A codec that ran it through a string
    // would corrupt every non-UTF-8 payload (and a NUL would truncate it).
    cl::raft_command original;
    original.kind = cl::raft_command_kind::kv_put;
    original.key = "bin";
    original.value = {std::byte{0x00}, std::byte{0xFF}, std::byte{0x80},
                      std::byte{0x00}, std::byte{0x7F}};

    std::vector<std::byte> encoded;
    CRPC_REQUIRE(cl::encode_raft_command(original, encoded).ok());
    cl::raft_command decoded;
    CRPC_REQUIRE(cl::decode_raft_command(
                     crpc::span<const std::byte>(encoded.data(), encoded.size()),
                     decoded)
                     .ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(5), decoded.value.size());
    CRPC_CHECK(decoded.value[0] == std::byte{0x00});
    CRPC_CHECK(decoded.value[1] == std::byte{0xFF});
    CRPC_CHECK(decoded.value[2] == std::byte{0x80});
    CRPC_CHECK(decoded.value[3] == std::byte{0x00});
    CRPC_CHECK(decoded.value[4] == std::byte{0x7F});
}

CRPC_TEST(kv_cas_round_trips_with_the_cas_index) {
    cl::raft_command original;
    original.kind = cl::raft_command_kind::kv_cas;
    original.key = "k";
    original.value = bytes_of("v");
    original.cas = 42;

    std::vector<std::byte> encoded;
    CRPC_REQUIRE(cl::encode_raft_command(original, encoded).ok());
    cl::raft_command decoded;
    CRPC_REQUIRE(cl::decode_raft_command(
                     crpc::span<const std::byte>(encoded.data(), encoded.size()),
                     decoded)
                     .ok());
    CRPC_CHECK(decoded.kind == cl::raft_command_kind::kv_cas);
    CRPC_CHECK_EQ(42ull, decoded.cas);
}

CRPC_TEST(kv_delete_has_no_value) {
    cl::raft_command original;
    original.kind = cl::raft_command_kind::kv_delete;
    original.key = "gone";

    std::vector<std::byte> encoded;
    CRPC_REQUIRE(cl::encode_raft_command(original, encoded).ok());
    cl::raft_command decoded;
    CRPC_REQUIRE(cl::decode_raft_command(
                     crpc::span<const std::byte>(encoded.data(), encoded.size()),
                     decoded)
                     .ok());
    CRPC_CHECK(decoded.kind == cl::raft_command_kind::kv_delete);
    CRPC_CHECK_EQ(std::string("gone"), decoded.key);
    CRPC_CHECK(decoded.value.empty());
}

CRPC_TEST(acl_upsert_round_trips_every_field) {
    const cl::raft_command original = make_acl_upsert();
    std::vector<std::byte> encoded;
    CRPC_REQUIRE(cl::encode_raft_command(original, encoded).ok());

    cl::raft_command decoded;
    CRPC_REQUIRE(cl::decode_raft_command(
                     crpc::span<const std::byte>(encoded.data(), encoded.size()),
                     decoded)
                     .ok());
    CRPC_CHECK(decoded.kind == cl::raft_command_kind::acl_upsert);
    CRPC_CHECK_EQ(0x0102030405060708ull, decoded.rule.token_hash);
    CRPC_CHECK_EQ(std::string("node-A"), decoded.rule.principal_id);
    CRPC_CHECK_EQ(std::string("node-A"), decoded.rule.node_id);
    CRPC_CHECK(decoded.rule.admin);
    CRPC_REQUIRE(decoded.rule.read_prefixes.size() == 2);
    CRPC_CHECK_EQ(std::string(""), decoded.rule.read_prefixes[0]);
    CRPC_CHECK_EQ(std::string("servers/"), decoded.rule.read_prefixes[1]);
    CRPC_REQUIRE(decoded.rule.write_prefixes.size() == 1);
    CRPC_CHECK_EQ(std::string("servers/"), decoded.rule.write_prefixes[0]);
}

CRPC_TEST(acl_remove_round_trips) {
    cl::raft_command original;
    original.kind = cl::raft_command_kind::acl_remove;
    original.rule.token_hash = 0xDEADBEEFCAFEBABEull;

    std::vector<std::byte> encoded;
    CRPC_REQUIRE(cl::encode_raft_command(original, encoded).ok());
    cl::raft_command decoded;
    CRPC_REQUIRE(cl::decode_raft_command(
                     crpc::span<const std::byte>(encoded.data(), encoded.size()),
                     decoded)
                     .ok());
    CRPC_CHECK(decoded.kind == cl::raft_command_kind::acl_remove);
    CRPC_CHECK_EQ(0xDEADBEEFCAFEBABEull, decoded.rule.token_hash);
}

CRPC_TEST(the_same_command_encodes_to_the_same_bytes) {
    // Determinism is what lets a test compare two nodes' logs byte for byte.
    std::vector<std::byte> first;
    std::vector<std::byte> second;
    CRPC_REQUIRE(cl::encode_raft_command(make_acl_upsert(), first).ok());
    CRPC_REQUIRE(cl::encode_raft_command(make_acl_upsert(), second).ok());
    CRPC_CHECK(first == second);
}

// --- Refusals ----------------------------------------------------------------

CRPC_TEST(empty_input_is_refused) {
    cl::raft_command decoded;
    const crpc::status s = cl::decode_raft_command({}, decoded);
    CRPC_CHECK(!s.ok());
    CRPC_CHECK_EQ(rpc_code::decode_error, s.code());
}

CRPC_TEST(a_truncated_prefix_is_refused) {
    const std::vector<std::byte> short_input{std::byte{0x01}};
    cl::raft_command decoded;
    const crpc::status s = cl::decode_raft_command(
        crpc::span<const std::byte>(short_input.data(), short_input.size()),
        decoded);
    CRPC_CHECK(!s.ok());
    CRPC_CHECK_EQ(rpc_code::decode_error, s.code());
}

CRPC_TEST(an_unsupported_version_is_a_capability_refusal) {
    // A future version's command must be a distinct refusal, not a misparse:
    // the node is behind, and the operator needs to know that.
    std::vector<std::byte> encoded;
    CRPC_REQUIRE(cl::encode_raft_command(make_put("k", "v"), encoded).ok());
    encoded[0] = std::byte{0x09};  // version 9
    cl::raft_command decoded;
    const crpc::status s = cl::decode_raft_command(
        crpc::span<const std::byte>(encoded.data(), encoded.size()), decoded);
    CRPC_CHECK(!s.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, s.code());
}

CRPC_TEST(an_unknown_kind_is_refused) {
    std::vector<std::byte> encoded;
    CRPC_REQUIRE(cl::encode_raft_command(make_put("k", "v"), encoded).ok());
    encoded[2] = std::byte{0x7F};  // kind 127
    cl::raft_command decoded;
    const crpc::status s = cl::decode_raft_command(
        crpc::span<const std::byte>(encoded.data(), encoded.size()), decoded);
    CRPC_CHECK(!s.ok());
    CRPC_CHECK_EQ(rpc_code::decode_error, s.code());
}

CRPC_TEST(trailing_bytes_are_refused) {
    // Extra bytes mean this node does not understand the whole command. Applying
    // the part it does understand would let it drift from the leader silently.
    std::vector<std::byte> encoded;
    CRPC_REQUIRE(cl::encode_raft_command(make_put("k", "v"), encoded).ok());
    encoded.push_back(std::byte{0xAB});
    cl::raft_command decoded;
    const crpc::status s = cl::decode_raft_command(
        crpc::span<const std::byte>(encoded.data(), encoded.size()), decoded);
    CRPC_CHECK(!s.ok());
    CRPC_CHECK_EQ(rpc_code::decode_error, s.code());
}

CRPC_TEST(a_key_length_that_runs_past_the_end_is_refused) {
    std::vector<std::byte> encoded;
    CRPC_REQUIRE(cl::encode_raft_command(make_put("key", "v"), encoded).ok());
    // The key length is the u32 right after the 3-byte prefix. Make it absurd.
    encoded[3] = std::byte{0xFF};
    encoded[4] = std::byte{0xFF};
    encoded[5] = std::byte{0xFF};
    encoded[6] = std::byte{0x7F};
    cl::raft_command decoded;
    const crpc::status s = cl::decode_raft_command(
        crpc::span<const std::byte>(encoded.data(), encoded.size()), decoded);
    CRPC_CHECK(!s.ok());
    CRPC_CHECK_EQ(rpc_code::decode_error, s.code());
}

CRPC_TEST(a_failed_decode_leaves_the_output_untouched) {
    // The apply loop reuses one `raft_command`; a half-decoded command must not
    // survive a failed decode.
    cl::raft_command decoded;
    decoded.kind = cl::raft_command_kind::kv_delete;
    decoded.key = "sentinel";
    const crpc::status s = cl::decode_raft_command({}, decoded);
    CRPC_CHECK(!s.ok());
    CRPC_CHECK(decoded.kind == cl::raft_command_kind::kv_delete);
    CRPC_CHECK_EQ(std::string("sentinel"), decoded.key);
}

CRPC_TEST(a_value_length_that_runs_past_the_end_is_refused) {
    std::vector<std::byte> encoded;
    CRPC_REQUIRE(cl::encode_raft_command(make_put("k", "value"), encoded).ok());
    // The value length follows the key: 3-byte prefix + 4-byte key length +
    // 1-byte key. Corrupt its high byte.
    const std::size_t value_len_at = 3 + 4 + 1;
    encoded[value_len_at] = std::byte{0xFF};
    encoded[value_len_at + 1] = std::byte{0xFF};
    encoded[value_len_at + 2] = std::byte{0xFF};
    encoded[value_len_at + 3] = std::byte{0x7F};
    cl::raft_command decoded;
    const crpc::status s = cl::decode_raft_command(
        crpc::span<const std::byte>(encoded.data(), encoded.size()), decoded);
    CRPC_CHECK(!s.ok());
    CRPC_CHECK_EQ(rpc_code::decode_error, s.code());
}

CRPC_TEST(to_string_names_every_kind) {
    CRPC_CHECK_EQ(std::string("kv_put"),
                  std::string(cl::to_string(cl::raft_command_kind::kv_put)));
    CRPC_CHECK_EQ(std::string("kv_cas"),
                  std::string(cl::to_string(cl::raft_command_kind::kv_cas)));
    CRPC_CHECK_EQ(std::string("kv_delete"),
                  std::string(cl::to_string(cl::raft_command_kind::kv_delete)));
    CRPC_CHECK_EQ(std::string("acl_upsert"),
                  std::string(cl::to_string(cl::raft_command_kind::acl_upsert)));
    CRPC_CHECK_EQ(std::string("acl_remove"),
                  std::string(cl::to_string(cl::raft_command_kind::acl_remove)));
}

CRPC_TEST_MAIN()
