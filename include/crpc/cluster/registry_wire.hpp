// CRPC -- G19 step2: registry service wire contract.
//
// Zero new frame types. The service rides the existing REQUEST/RESPONSE frame
// pair with a small integer service id and method id, exactly as the push
// control service does (docs/protocol.md:71 limits the frame-type set).
//
// Wire layout
// -----------
//   registry_frame :=
//       u16  dto_version       // must equal kRegistryDtoVersion (1)
//       u32  total_len         // MUST equal body.size() - 6      <-- layer 1
//       tag*                  // repeats until total_len bytes consumed
//
//   tag :=
//       u8   tag_id            // must be one of the ids in the tag table
//                              // below (an explicit whitelist, NOT a range:
//                              // tag 2 is retired and 0 is invalid, so an
//                              // id <= registry_tag_max_known test would
//                              // wrongly accept gaps); anything else ->
//                              // unknown_tag
//       u32  len               // MUST NOT exceed the bytes remaining   <-- layer 2
//       u8[len] value
//
// Encoding order is tag id ascending, which makes two encodings of the same
// message byte-for-byte comparable. The decoder does NOT enforce ascending
// order: it rejects an unknown or repeated id, but a non-ascending frame is
// still a well-formed frame and rejecting it would refuse a legal encoding for
// no safety gain. Comparability is a property of the writer, not a rule the
// reader imposes.
//
// Fail-closed rules (all of them; omitting any one opens a hole)
// --------------------------------------------------------------
//   1. `total_len == body.size() - 6`. A mismatch is `decode_error` with
//      reason `dto_length_mismatch`. This is the FIRST layer: it separates
//      "truncated" from "field missing".
//   2. The required tag set is declared by the version (see the per-method
//      table below). A missing tag is `decode_error` / `dto_missing_required`.
//   3. An unknown tag id is REJECTED, never skipped: a future version's field
//      must not be silently dropped by an older service. `decode_error` /
//      `dto_unknown_tag`.
//   4. Every `tag::len` must be `<=` the bytes remaining. Over-long is
//      `decode_error` / `dto_truncated_field` (not "the field ended").
//   5. A repeated tag id is `decode_error` / `dto_duplicate_tag`.
//   6. An unknown `dto_version` is `failed_precondition` /
//      `dto_unsupported_version` (a capability problem, not a format problem).
//
// `service_id` safety: the only comparisons of a service id in the codebase
// are exact equality (src/server/server.cpp:1622, src/rpc/client.cpp:697) and
// registry keys are `std::string`, so `~u64{1}` cannot collide with anything.
#ifndef CRPC_CLUSTER_REGISTRY_WIRE_HPP
#define CRPC_CLUSTER_REGISTRY_WIRE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <crpc/buffer.hpp>
#include <crpc/cluster/registry.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace cluster {

// The DTOs carry `service_instance` by value, so the complete type is
// required; registry.hpp is a public header and this one already documents the
// wire contract against it.
// --- Service and method ids --------------------------------------------------
//
// `~u64{1}` is adjacent to the PUSH control service (`~u64{0}`,
// include/crpc/push/wire.hpp:55) but not equal to it; no range test on a
// service id exists anywhere in the codebase.
inline constexpr u64 registry_service_id = ~u64{1};   // 0xFFFFFFFFFFFFFFFE
inline constexpr u64 registry_method_register   = 1;
inline constexpr u64 registry_method_deregister = 2;
inline constexpr u64 registry_method_set_state  = 3;
inline constexpr u64 registry_method_query      = 4;
inline constexpr u64 registry_method_next_fence = 5;
inline constexpr u64 registry_method_ping       = 6;
inline constexpr u64 registry_method_kv_get     = 7;
inline constexpr u64 registry_method_kv_put     = 8;
inline constexpr u64 registry_method_kv_cas     = 9;
inline constexpr u64 registry_method_kv_delete  = 10;

// --- Tag ids -----------------------------------------------------------------
//
// The ids are the field table of VERDICT section 2.3. The DTO version is NOT a
// tag: it lives in the fixed frame header (the u16 at offset 0) and is checked
// there, so it needs no tag id. An earlier `registry_tag_dto_version = 2`
// constant existed for it and was never used.
inline constexpr u8 registry_tag_token        = 1;
inline constexpr u8 registry_tag_service      = 3;
inline constexpr u8 registry_tag_node_id      = 4;
inline constexpr u8 registry_tag_host         = 5;
inline constexpr u8 registry_tag_port         = 6;
inline constexpr u8 registry_tag_incarnation   = 7;
inline constexpr u8 registry_tag_fence        = 8;
inline constexpr u8 registry_tag_writer_epoch = 9;
inline constexpr u8 registry_tag_weight       = 10;
inline constexpr u8 registry_tag_state        = 11;
inline constexpr u8 registry_tag_meta         = 12;
inline constexpr u8 registry_tag_passing_only = 13;
inline constexpr u8 registry_tag_index        = 14;
inline constexpr u8 registry_tag_client_request_id = 15;
inline constexpr u8 registry_tag_token_issued_at = 16;
inline constexpr u8 registry_tag_token_nonce     = 17;

// KV tags (VERDICT section 7.4): request tags 20-22, response tags 23-25.
inline constexpr u8 registry_tag_key             = 20;
inline constexpr u8 registry_tag_value           = 21;
inline constexpr u8 registry_tag_cas             = 22;
inline constexpr u8 registry_tag_found           = 23;
inline constexpr u8 registry_tag_create_index    = 24;
inline constexpr u8 registry_tag_modify_index    = 25;

// The largest id in the tag table. NOT the decode test: the decoder uses an
// explicit whitelist (`is_known_tag` in registry_wire.cpp) because the id space
// has gaps -- tag 2 is retired. This constant survives for tests that need an
// out-of-range id.
inline constexpr u8 registry_tag_max_known    = 25;

// --- Direction-specific tags 18 / 19 -----------------------------------------
//
// The anchor is two-dimensional: `(authority_epoch, index)` (VERDICT section
// 5.1). For a long time the epoch half had NO tag of its own: the request
// carried only the index, so `authority_epoch` was always 0 and the
// dictionary-order comparison degenerated to a one-dimensional one.
//
// The response side had the opposite problem: `authority_epoch` rode in tag 9
// (`writer_epoch`) in the write response, and in tag 7 (`incarnation`) in the
// anchor/query responses. One numeric id therefore meant three different things
// depending on which encoder produced it, with nothing on the wire to say which.
//
// Tag 18 is the request half (the client's last observed authority epoch) and
// tag 19 is the response half (the service's own authority epoch). Tag 1
// (`index`) is shared by both directions because its meaning -- "a log index" --
// is the same in each; that is reuse, not a collision.
inline constexpr u8 registry_tag_client_authority_epoch = 18;
inline constexpr u8 registry_tag_authority_epoch         = 19;

// The replay window for a bearer token (VERDICT section 1.3, option (ii)).
// A token whose issue time is outside `|now - issued_at| <= window` is denied.
// This is a *time limit*, not replay prevention: a token observed inside the
// window can still be replayed. Clock synchronisation is UNVERIFIED.
inline constexpr std::chrono::milliseconds registry_token_window{30000};

inline constexpr u16 registry_dto_version = 1;

// The fixed prefix: u16 dto_version + u32 total_len.
inline constexpr std::size_t registry_frame_prefix = 6;

// --- Decoded request DTOs ----------------------------------------------------

struct registry_register_request {
    std::string token;
    std::string service;
    std::string node_id;
    std::string host;
    u16 port = 0;
    u64 incarnation = 0;
    u64 fence = 0;
    u64 writer_epoch = 0;
    u32 weight = 1;
    bool has_state = false;
    u8 state = 0;
    // The client's idempotency key (optional); see registry_register_request.
    std::string client_request_id;
    // The client's last observed anchor (optional). A write whose anchor is
    // behind the service's last accepted anchor for the key is rejected with
    // `stale_anchor` rather than silently committing an out-of-order write.
    u64 client_authority_epoch = 0;
    u64 client_index = 0;
    bool has_client_anchor = false;
    // The token's issue time (optional) and its nonce (optional). See
    // registry_token_window; a missing issue time is treated as "outside".
    std::chrono::milliseconds token_issued_at{0};
    std::string token_nonce;
    bool has_token_issued_at = false;
    // `meta` is the raw tag payload; the service parses it with
    // decode_meta_map so a malformed metadata block is a decode error rather
    // than a silently empty map.
    std::vector<std::byte> meta;
};

struct registry_deregister_request {
    std::string token;
    std::string service;
    std::string node_id;
    u64 incarnation = 0;
    u64 fence = 0;
    u64 writer_epoch = 0;
    // The client's idempotency key (optional); see registry_register_request.
    std::string client_request_id;
    u64 client_authority_epoch = 0;
    u64 client_index = 0;
    bool has_client_anchor = false;
    // Token issue time / nonce, same meaning as registry_register_request.
    std::chrono::milliseconds token_issued_at{0};
    std::string token_nonce;
    bool has_token_issued_at = false;
};

struct registry_set_state_request {
    std::string token;
    std::string service;
    std::string node_id;
    u64 incarnation = 0;
    u64 fence = 0;
    u64 writer_epoch = 0;
    u8 state = 0;
    // The client's idempotency key (optional); see registry_register_request.
    std::string client_request_id;
    u64 client_authority_epoch = 0;
    u64 client_index = 0;
    bool has_client_anchor = false;
    // Token issue time / nonce, same meaning as registry_register_request.
    std::chrono::milliseconds token_issued_at{0};
    std::string token_nonce;
    bool has_token_issued_at = false;
};

struct registry_query_request {
    std::string token;
    std::string service;
    std::string node_id;
    bool passing_only = false;
    // Token issue time / nonce, same meaning as registry_register_request.
    std::chrono::milliseconds token_issued_at{0};
    std::string token_nonce;
    bool has_token_issued_at = false;
};

struct registry_next_fence_request {
    std::string token;
    std::string service;
    std::string node_id;
    // Token issue time / nonce, same meaning as registry_register_request.
    std::chrono::milliseconds token_issued_at{0};
    std::string token_nonce;
    bool has_token_issued_at = false;
};

struct registry_ping_request {
    std::string token;
    // Token issue time / nonce, same meaning as registry_register_request.
    std::chrono::milliseconds token_issued_at{0};
    std::string token_nonce;
    bool has_token_issued_at = false;
};

// --- KV request / response DTOs (VERDICT section 7.4) ------------------------
//
// All values travel base64 on the wire; the DTO holds raw bytes and the codec
// transcodes. Every field is required except the explicitly-optional ones,
// which follow the same `has_*` flag pattern as the registry DTOs.

struct kv_get_request {
    std::string token;
    std::string key;
    std::chrono::milliseconds token_issued_at{0};
    std::string token_nonce;
    bool has_token_issued_at = false;
};

struct kv_get_response {
    u64 index = 0;
    std::vector<std::byte> value;
    u64 create_index = 0;
    u64 modify_index = 0;
    bool found = false;
};

struct kv_put_request {
    std::string token;
    std::string key;
    std::vector<std::byte> value;
    u64 cas = 0;
    bool has_cas = false;
    std::chrono::milliseconds token_issued_at{0};
    std::string token_nonce;
    bool has_token_issued_at = false;
};

struct kv_put_response {
    u64 index = 0;
    u64 modify_index = 0;
};

struct kv_delete_request {
    std::string token;
    std::string key;
    std::chrono::milliseconds token_issued_at{0};
    std::string token_nonce;
    bool has_token_issued_at = false;
};

struct kv_delete_response {
    u64 index = 0;
    bool deleted = false;
};

struct registry_anchor {
    u64 authority_epoch = 0;
    u64 index = 0;
};

// True when `observed` is at or ahead of `anchor` in dictionary order.
[[nodiscard]] inline bool not_stale(const registry_anchor& anchor,
                                    const registry_anchor& observed) {
    if (observed.authority_epoch != anchor.authority_epoch) {
        return observed.authority_epoch > anchor.authority_epoch;
    }
    return observed.index >= anchor.index;
}

// --- Response DTOs -----------------------------------------------------------

struct registry_write_response {
    u64 index = 0;
    u64 fence = 0;
    u64 incarnation = 0;
    u64 authority_epoch = 0;
};

// A catalog row as the wire carries it: the registry index at which the row
// last changed, plus the row itself. The service encodes it with
// encode_query_instance / decode_query_instance.
struct registry_query_row {
    u64 revision = 0;
    service_instance instance;
};

struct registry_query_response {
    registry_anchor anchor{};
    std::vector<registry_query_row> rows;
};

// --- Tag decoding ------------------------------------------------------------

// One decoded tag. `value` borrows `body` (the full frame body), so a tag
// reader must not outlive the buffer it was given.
struct registry_tag {
    u8 id = 0;
    span<const std::byte> value{};
};

// Walks the tag region of `body` (the whole frame body: 6-byte prefix + tags)
// and returns every tag. Applies fail-closed rules 1, 3, 4 and 5; the
// required-set rule 2 is applied by the per-method decoder because it is
// method-specific. Rule 6 is applied here because the version is in the prefix.
//
// On failure the returned status carries `registry.reason` in its metadata:
//   dto_length_mismatch / dto_unknown_tag / dto_truncated_field /
//   dto_duplicate_tag / dto_unsupported_version.
[[nodiscard]] CRPC_API status decode_registry_tags(span<const std::byte> body,
                                                   std::vector<registry_tag>& out);

// Rule 2: every id in `required` must be present exactly once. Reports
// `dto_missing_required` naming the first missing id in `missing_id`.
[[nodiscard]] CRPC_API status check_required_tags(const std::vector<registry_tag>& tags,
                                                  const u8* required, std::size_t count,
                                                  u8& missing_id);

// --- Per-method request decoders --------------------------------------------
//
// Each returns a status carrying `registry.reason` on failure. Rule 2's
// required set is taken from VERDICT section 2.3.

[[nodiscard]] CRPC_API status decode_register_request(
    span<const std::byte> body, registry_register_request& out);
[[nodiscard]] CRPC_API status decode_deregister_request(
    span<const std::byte> body, registry_deregister_request& out);
[[nodiscard]] CRPC_API status decode_set_state_request(
    span<const std::byte> body, registry_set_state_request& out);
[[nodiscard]] CRPC_API status decode_query_request(
    span<const std::byte> body, registry_query_request& out);
[[nodiscard]] CRPC_API status decode_next_fence_request(
    span<const std::byte> body, registry_next_fence_request& out);
[[nodiscard]] CRPC_API status decode_ping_request(
    span<const std::byte> body, registry_ping_request& out);

// --- KV per-method request decoders (VERDICT section 7.4) ---------------------

[[nodiscard]] CRPC_API status decode_kv_get_request(
    span<const std::byte> body, kv_get_request& out);
[[nodiscard]] CRPC_API status decode_kv_put_request(
    span<const std::byte> body, kv_put_request& out);
[[nodiscard]] CRPC_API status decode_kv_delete_request(
    span<const std::byte> body, kv_delete_request& out);

// --- KV response encoders / decoders ------------------------------------------

[[nodiscard]] CRPC_API status encode_kv_get_response(
    const kv_get_response& value, buffer& out);
[[nodiscard]] CRPC_API status decode_kv_get_response(
    span<const std::byte> in, kv_get_response& out);
[[nodiscard]] CRPC_API status encode_kv_put_response(
    const kv_put_response& value, buffer& out);
[[nodiscard]] CRPC_API status decode_kv_put_response(
    span<const std::byte> in, kv_put_response& out);
[[nodiscard]] CRPC_API status encode_kv_delete_response(
    const kv_delete_response& value, buffer& out);
[[nodiscard]] CRPC_API status decode_kv_delete_response(
    span<const std::byte> in, kv_delete_response& out);

// --- Encoders ----------------------------------------------------------------

[[nodiscard]] CRPC_API status encode_write_response(
    const registry_write_response& value, buffer& out);
[[nodiscard]] CRPC_API status encode_query_response(
    const registry_query_response& value, buffer& out);
// Decodes a write/anchor response body (the whole frame, 6-byte prefix + tags).
[[nodiscard]] CRPC_API status decode_write_response(span<const std::byte> in,
                                                    registry_write_response& out);
[[nodiscard]] CRPC_API status decode_query_response(span<const std::byte> in,
                                                    registry_query_response& out);
[[nodiscard]] CRPC_API status encode_anchor_response(
    const registry_anchor& value, buffer& out);

// --- Query instance codec (response tag 12) ----------------------------------
//
// The instance list is a self-describing block so the response stays a single
// versioned tag frame. Its shape mirrors the registry's restore payload
// (tests/test_registry_restore.cpp documents it): the first 16 bytes carry the
// fence and incarnation, the rest is the instance body. A reader can therefore
// reuse one bounds-checked length-prefixed parser for both.
[[nodiscard]] CRPC_API status encode_query_instance(const service_instance& instance,
                                                    std::vector<std::byte>& out);
[[nodiscard]] CRPC_API status decode_query_instance(span<const std::byte> bytes,
                                                    u64& fence, u64& incarnation,
                                                    service_instance& out);

// --- Metadata map codec (request tag 12) -------------------------------------
//
// A length-prefixed list of key/value pairs, both `u32`-length-prefixed. It is
// the same shape as the registry's restore payload so a malformed block is a
// decode error instead of an empty map. Rejects a duplicate key (fail closed:
// the caller could otherwise observe one of two values nondeterministically).
[[nodiscard]] CRPC_API status decode_meta_map(span<const std::byte> bytes,
                                              metadata_list& out);
[[nodiscard]] CRPC_API status encode_meta_map(const metadata_list& meta,
                                              std::vector<std::byte>& out);

// --- Reason helpers ----------------------------------------------------------

// Sets `registry.reason` (and optionally `registry.index` / `registry.leader`)
// on `error`. Every failure the service returns goes through here so the
// metadata keys are spelled identically everywhere.
[[nodiscard]] CRPC_API rpc_error registry_failure(rpc_code code, const char* message,
                                                  const char* reason);

inline constexpr const char* registry_reason_acl_denied = "acl_denied";
inline constexpr const char* registry_reason_stale_anchor = "stale_anchor";
// The persisted `last_anchor` file exists but cannot be parsed. This is a
// startup failure, never a silent empty map.
inline constexpr const char* registry_reason_anchor_unreadable =
    "anchor_unreadable";
inline constexpr const char* registry_reason_epoch_required = "epoch_required";
inline constexpr const char* registry_reason_epoch_not_persisted =
    "epoch_not_persisted";
inline constexpr const char* registry_reason_dto_length_mismatch =
    "dto_length_mismatch";
inline constexpr const char* registry_reason_dto_missing_required =
    "dto_missing_required";
inline constexpr const char* registry_reason_dto_unknown_tag = "dto_unknown_tag";
inline constexpr const char* registry_reason_dto_truncated_field =
    "dto_truncated_field";
inline constexpr const char* registry_reason_dto_duplicate_tag =
    "dto_duplicate_tag";
inline constexpr const char* registry_reason_dto_unsupported_version =
    "dto_unsupported_version";
inline constexpr const char* registry_reason_cas_conflict = "cas_conflict";


} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_REGISTRY_WIRE_HPP
