// CRPC -- G19 step2: registry wire DTO tests.
//
// Each fail-closed rule of VERDICT section 2.2 has its own case, and each case
// asserts the exact `registry.reason` string, not just "it failed": a decoder
// that rejected the wrong input for the wrong reason would still be fail-closed
// but would tell a future maintainer the wrong story.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "test_framework.hpp"

#include <crpc/cluster/registry_wire.hpp>
#include <crpc/endian.hpp>

namespace cl = crpc::cluster;

namespace {

// --- Test-side encoder -------------------------------------------------------
//
// The tests build frames by hand so they can produce exactly the malformed
// input a rule is about. Using the production encoder would make a "truncated"
// case impossible to express.

void put_u8(std::vector<std::byte>& out, crpc::u8 value) {
    out.push_back(static_cast<std::byte>(value));
}

void put_u16(std::vector<std::byte>& out, crpc::u16 value) {
    const std::size_t at = out.size();
    out.resize(at + 2);
    crpc::endian::store_le16(out.data() + at, value);
}

void put_u32(std::vector<std::byte>& out, crpc::u32 value) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    crpc::endian::store_le32(out.data() + at, value);
}

void put_u64(std::vector<std::byte>& out, crpc::u64 value) {
    const std::size_t at = out.size();
    out.resize(at + 8);
    crpc::endian::store_le64(out.data() + at, value);
}

void put_tag(std::vector<std::byte>& out, crpc::u8 id, const std::vector<std::byte>& value) {
    put_u8(out, id);
    put_u32(out, static_cast<crpc::u32>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void put_str_tag(std::vector<std::byte>& out, crpc::u8 id, const std::string& value) {
    std::vector<std::byte> bytes;
    put_u32(bytes, static_cast<crpc::u32>(value.size()));
    bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(value.data()),
                 reinterpret_cast<const std::byte*>(value.data()) + value.size());
    put_tag(out, id, bytes);
}

void put_u64_tag(std::vector<std::byte>& out, crpc::u8 id, crpc::u64 value) {
    std::vector<std::byte> bytes;
    put_u64(bytes, value);
    put_tag(out, id, bytes);
}

// Builds a complete frame with a correct total_len.
std::vector<std::byte> frame_with(const std::vector<std::byte>& tags, crpc::u16 version = 1) {
    std::vector<std::byte> out;
    put_u16(out, version);
    put_u32(out, static_cast<crpc::u32>(tags.size()));
    out.insert(out.end(), tags.begin(), tags.end());
    return out;
}

// Builds a complete register frame with all required tags.
std::vector<std::byte> valid_register_frame(const std::string& token = "tok",
                                            const std::string& service = "svc",
                                            const std::string& node = "node-a") {
    std::vector<std::byte> tags;
    put_str_tag(tags, cl::registry_tag_token, token);
    put_str_tag(tags, cl::registry_tag_service, service);
    put_str_tag(tags, cl::registry_tag_node_id, node);
    put_str_tag(tags, cl::registry_tag_host, "127.0.0.1");
    std::vector<std::byte> port;
    put_u16(port, 8080);
    put_tag(tags, cl::registry_tag_port, port);
    put_u64_tag(tags, cl::registry_tag_incarnation, 1);
    put_u64_tag(tags, cl::registry_tag_fence, 1);
    put_u64_tag(tags, cl::registry_tag_writer_epoch, 7);
    return frame_with(tags);
}

std::string reason_of(const crpc::status& s) {
    const std::string* found = s.error().metadata.find("registry.reason");
    return found != nullptr ? *found : std::string{};
}

// `registry.reason` alone cannot tell the two "missing required tag" layers
// apart: `check_required_tags` (the pre-gate that names the missing tag) and
// `find_required` (the mandatory gate that guards the dereference) both use
// `dto_missing_required`. Asserting only the reason makes a test blind to a
// mutant that disables the pre-gate, because the second gate returns the same
// reason. The message differs, so the tests pin it too.
std::string message_of(const crpc::status& s) {
    return s.error().message;
}

crpc::span<const std::byte> bytes_of(const crpc::span<const std::byte> value) {
    return value;
}

crpc::span<const std::byte> bytes_of(const std::vector<std::byte>& value) {
    return crpc::span<const std::byte>(value.data(), value.size());
}

} // namespace

// --- Rule 1: total_len is the first layer ------------------------------------

CRPC_TEST(dto_length_mismatch_when_body_is_short) {
    std::vector<std::byte> tags;
    put_u64_tag(tags, cl::registry_tag_incarnation, 1);
    std::vector<std::byte> body = frame_with(tags);
    // Drop one byte: total_len now disagrees with the body.
    body.pop_back();

    cl::registry_register_request decoded;
    const crpc::status s = cl::decode_register_request(bytes_of(body), decoded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_length_mismatch"), reason_of(s));
}

CRPC_TEST(dto_length_mismatch_when_prefix_itself_is_truncated) {
    // The prefix is 6 bytes. A 3-byte body cannot even carry it, so the report
    // must be a length mismatch, not a truncated field.
    const std::vector<std::byte> body{static_cast<std::byte>(1),
                                      static_cast<std::byte>(0),
                                      static_cast<std::byte>(9)};

    cl::registry_register_request decoded;
    const crpc::status s = cl::decode_register_request(bytes_of(body), decoded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_length_mismatch"), reason_of(s));
}

// --- Rule 4: a tag's len must not exceed the remaining bytes -----------------

CRPC_TEST(dto_truncated_field_when_tag_len_overruns) {
    std::vector<std::byte> body;
    put_u16(body, 1);
    put_u32(body, 100);   // total_len agrees with the body below
    put_u8(body, cl::registry_tag_incarnation);
    put_u32(body, 999);   // declares 999 bytes, only 0 remain
    // total_len counts the tag header (5) only; the declared value is missing.
    // Rebuild with the matching total_len so rule 4, not rule 1, fires.
    std::vector<std::byte> rebuilt;
    put_u16(rebuilt, 1);
    put_u32(rebuilt, 5);
    put_u8(rebuilt, cl::registry_tag_incarnation);
    put_u32(rebuilt, 999);
    (void)body;

    std::vector<cl::registry_tag> tags;
    const crpc::status s = cl::decode_registry_tags(bytes_of(rebuilt), tags);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_truncated_field"), reason_of(s));
}

CRPC_TEST(dto_truncated_field_when_tag_header_is_incomplete) {
    // A tag header is 5 bytes; a trailing 2 bytes are not a header.
    std::vector<std::byte> body;
    put_u16(body, 1);
    put_u32(body, 2);
    put_u8(body, cl::registry_tag_incarnation);
    put_u8(body, 0);

    std::vector<cl::registry_tag> tags;
    const crpc::status s = cl::decode_registry_tags(bytes_of(body), tags);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_truncated_field"), reason_of(s));
}

// --- Rule 3: an unknown tag id is rejected, not skipped ----------------------

CRPC_TEST(dto_unknown_tag_rejected) {
    std::vector<std::byte> tags;
    std::vector<std::byte> value;
    put_u64(value, 1);
    put_tag(tags, cl::registry_tag_max_known + 1, value);

    std::vector<cl::registry_tag> decoded;
    const crpc::status s = cl::decode_registry_tags(bytes_of(frame_with(tags)), decoded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_unknown_tag"), reason_of(s));
}

CRPC_TEST(dto_zero_tag_id_rejected) {
    std::vector<std::byte> tags;
    std::vector<std::byte> value;
    put_tag(tags, 0, value);

    std::vector<cl::registry_tag> decoded;
    const crpc::status s = cl::decode_registry_tags(bytes_of(frame_with(tags)), decoded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_unknown_tag"), reason_of(s));
}

// --- Rule 5: a repeated tag id is rejected -----------------------------------

CRPC_TEST(dto_duplicate_tag_rejected) {
    std::vector<std::byte> tags;
    put_u64_tag(tags, cl::registry_tag_incarnation, 1);
    put_u64_tag(tags, cl::registry_tag_incarnation, 2);

    std::vector<cl::registry_tag> decoded;
    const crpc::status s = cl::decode_registry_tags(bytes_of(frame_with(tags)), decoded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_duplicate_tag"), reason_of(s));
}

// --- Rule 2: a missing required tag is rejected ------------------------------

CRPC_TEST(dto_missing_required_tag_rejected) {
    // A valid register frame minus the fence tag.
    std::vector<std::byte> tags;
    put_str_tag(tags, cl::registry_tag_token, "tok");
    put_str_tag(tags, cl::registry_tag_service, "svc");
    put_str_tag(tags, cl::registry_tag_node_id, "node-a");
    put_str_tag(tags, cl::registry_tag_host, "127.0.0.1");
    std::vector<std::byte> port;
    put_u16(port, 8080);
    put_tag(tags, cl::registry_tag_port, port);
    put_u64_tag(tags, cl::registry_tag_incarnation, 1);
    put_u64_tag(tags, cl::registry_tag_writer_epoch, 7);

    cl::registry_register_request decoded;
    const crpc::status s = cl::decode_register_request(bytes_of(frame_with(tags)), decoded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_missing_required"), reason_of(s));
    // The pre-gate is the one that reports WHICH tag was missing; pin its
    // message so a mutant that disables it (leaving only `find_required`) is
    // caught here rather than by a crash in production. The frame above omits
    // `registry_tag_fence`.
    CRPC_CHECK_EQ(std::string("registry frame is missing a required tag (first gate)"),
                  message_of(s));
}

// --- Rule 6: an unknown version is a capability refusal ---------------------

CRPC_TEST(dto_unsupported_version_rejected) {
    std::vector<std::byte> tags;
    put_str_tag(tags, cl::registry_tag_token, "tok");

    cl::registry_ping_request decoded;
    const crpc::status s = cl::decode_ping_request(bytes_of(frame_with(tags, 99)), decoded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_unsupported_version"), reason_of(s));
    // A version problem is failed_precondition (12), not decode_error (15).
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(s.code()));
}

// --- The happy path, so the rules above are not just "everything fails" -----

CRPC_TEST(register_frame_decodes_all_fields) {
    const std::vector<std::byte> body = valid_register_frame("tok", "svc", "node-a");
    cl::registry_register_request decoded;
    const crpc::status s = cl::decode_register_request(bytes_of(body), decoded);
    CRPC_CHECK(s.ok());
    if (!s.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("tok"), decoded.token);
    CRPC_CHECK_EQ(std::string("svc"), decoded.service);
    CRPC_CHECK_EQ(std::string("node-a"), decoded.node_id);
    CRPC_CHECK_EQ(std::string("127.0.0.1"), decoded.host);
    CRPC_CHECK_EQ(static_cast<crpc::u16>(8080), decoded.port);
    CRPC_CHECK_EQ(crpc::u64{1}, decoded.incarnation);
    CRPC_CHECK_EQ(crpc::u64{1}, decoded.fence);
    CRPC_CHECK_EQ(crpc::u64{7}, decoded.writer_epoch);
    CRPC_CHECK_FALSE(decoded.has_state);
}

CRPC_TEST(query_frame_decodes_optional_filters) {
    std::vector<std::byte> tags;
    put_str_tag(tags, cl::registry_tag_token, "tok");
    put_str_tag(tags, cl::registry_tag_service, "svc");
    put_str_tag(tags, cl::registry_tag_node_id, "node-a");
    std::vector<std::byte> flag;
    put_u8(flag, 1);
    put_tag(tags, cl::registry_tag_passing_only, flag);

    cl::registry_query_request decoded;
    const crpc::status s = cl::decode_query_request(bytes_of(frame_with(tags)), decoded);
    CRPC_CHECK(s.ok());
    if (!s.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("tok"), decoded.token);
    CRPC_CHECK_EQ(std::string("svc"), decoded.service);
    CRPC_CHECK_EQ(std::string("node-a"), decoded.node_id);
    CRPC_CHECK(decoded.passing_only);
}

// --- Tag 2 is retired, and a retired id must not be accepted ------------------
//
// The DTO version lives in the fixed frame header, not in a tag. Tag 2 used to be
// `registry_tag_dto_version` and was removed, leaving a gap in the numeric range.
// An interval test (`id > registry_tag_max_known`) would keep accepting tag 2
// forever, so the check is an explicit whitelist of the ids this service defines.

CRPC_TEST(a_frame_with_the_retired_tag_2_is_rejected) {
    std::vector<std::byte> tags;
    std::vector<std::byte> value;
    put_u64(value, 1);
    put_tag(tags, 2, value);

    std::vector<cl::registry_tag> decoded;
    const crpc::status s = cl::decode_registry_tags(bytes_of(frame_with(tags)), decoded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_unknown_tag"), reason_of(s));
}

// --- Encoders round-trip -----------------------------------------------------

CRPC_TEST(write_response_round_trip) {
    cl::registry_write_response value;
    value.index = 42;
    value.fence = 7;
    value.incarnation = 3;
    value.authority_epoch = 9;

    crpc::buffer out;
    const crpc::status encoded = cl::encode_write_response(value, out);
    CRPC_CHECK(encoded.ok());
    if (!encoded.ok()) {
        return;
    }

    // The response frame must walk as a valid tag frame.
    std::vector<cl::registry_tag> tags;
    const crpc::status walked =
        cl::decode_registry_tags(bytes_of(out.readable()), tags);
    CRPC_CHECK(walked.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), tags.size());
}

CRPC_TEST(meta_map_round_trip) {
    crpc::metadata_list meta;
    meta.set("a", "1");
    meta.set("b", "2");

    std::vector<std::byte> encoded;
    const crpc::status wrote = cl::encode_meta_map(meta, encoded);
    CRPC_CHECK(wrote.ok());

    crpc::metadata_list decoded;
    const crpc::status read =
        cl::decode_meta_map(crpc::span<const std::byte>(encoded.data(), encoded.size()),
                            decoded);
    CRPC_CHECK(read.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), decoded.size());
    const std::string* a = decoded.find("a");
    const std::string* b = decoded.find("b");
    CRPC_CHECK(a != nullptr && *a == "1");
    CRPC_CHECK(b != nullptr && *b == "2");
}

CRPC_TEST(meta_map_duplicate_key_is_rejected) {
    // key "a" twice; the second must be a decode error, not a silent overwrite.
    std::vector<std::byte> block;
    put_u32(block, 1);
    block.push_back(static_cast<std::byte>('a'));
    put_u32(block, 1);
    block.push_back(static_cast<std::byte>('1'));
    put_u32(block, 1);
    block.push_back(static_cast<std::byte>('a'));
    put_u32(block, 1);
    block.push_back(static_cast<std::byte>('2'));

    crpc::metadata_list decoded;
    const crpc::status s =
        cl::decode_meta_map(crpc::span<const std::byte>(block.data(), block.size()),
                            decoded);
    CRPC_CHECK_FALSE(s.ok());
}

// --- The anchor predicate itself --------------------------------------------
//
// These cases pin `not_stale` directly, because the service-level anchor tests
// go through a client that re-anchors on a stale answer; a `>` instead of `>=`
// would be invisible there (both reject a strictly-behind observation). The
// equal-index case is the one that separates the two.

CRPC_TEST(not_stale_accepts_an_equal_index) {
    const cl::registry_anchor anchor{1, 5};
    const cl::registry_anchor observed{1, 5};
    CRPC_CHECK(cl::not_stale(anchor, observed));
}

CRPC_TEST(not_stale_accepts_a_newer_index) {
    const cl::registry_anchor anchor{1, 5};
    const cl::registry_anchor observed{1, 6};
    CRPC_CHECK(cl::not_stale(anchor, observed));
}

CRPC_TEST(not_stale_rejects_an_older_index) {
    const cl::registry_anchor anchor{1, 5};
    const cl::registry_anchor observed{1, 4};
    CRPC_CHECK_FALSE(cl::not_stale(anchor, observed));
}

CRPC_TEST(not_stale_compares_epoch_before_index) {
    const cl::registry_anchor anchor{1, 9};
    const cl::registry_anchor newer_epoch{2, 0};
    CRPC_CHECK(cl::not_stale(anchor, newer_epoch));

    const cl::registry_anchor older_epoch{0, 999};
    CRPC_CHECK_FALSE(cl::not_stale(anchor, older_epoch));
}

// --- The response decoders are fail-closed too --------------------------------
//
// `decode_write_response` and `decode_query_response` used to walk the tag list
// and silently accept whatever was present, so a response missing `index` or
// `fence` decoded "successfully" into a zeroed struct. The client then stored
// `{0, 0}` as its anchor, which is a valid-looking anchor pointing at nothing --
// a later write would be rejected as stale forever, with no way to recover. A
// response the service never produced is malformed input, not an empty result.

CRPC_TEST(write_response_rejects_an_empty_body) {
    const std::vector<std::byte> empty;
    cl::registry_write_response decoded;
    const crpc::status s =
        cl::decode_write_response(crpc::span<const std::byte>(empty.data(), empty.size()),
                                  decoded);
    CRPC_CHECK_FALSE(s.ok());
    // An empty body cannot even carry the 6-byte frame prefix.
    CRPC_CHECK_EQ(std::string("dto_length_mismatch"), reason_of(s));
}

CRPC_TEST(write_response_rejects_a_prefix_only_body) {
    // 6 bytes: version + total_len, and no tags at all. This is the shape a
    // truncated or wrong-endpoint response would have.
    std::vector<std::byte> body;
    put_u16(body, 1);
    put_u32(body, 0);

    cl::registry_write_response decoded;
    const crpc::status s = cl::decode_write_response(bytes_of(body), decoded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_missing_required"), reason_of(s));
}

CRPC_TEST(write_response_rejects_a_missing_index_tag) {
    // fence + incarnation + authority_epoch are present; `index` is not.
    std::vector<std::byte> tags;
    put_u64_tag(tags, cl::registry_tag_fence, 7);
    put_u64_tag(tags, cl::registry_tag_incarnation, 3);
    put_u64_tag(tags, cl::registry_tag_authority_epoch, 9);

    cl::registry_write_response decoded;
    const crpc::status s =
        cl::decode_write_response(bytes_of(frame_with(tags)), decoded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_missing_required"), reason_of(s));
}

CRPC_TEST(write_response_rejects_a_missing_fence_tag) {
    std::vector<std::byte> tags;
    put_u64_tag(tags, cl::registry_tag_index, 42);
    put_u64_tag(tags, cl::registry_tag_incarnation, 3);
    put_u64_tag(tags, cl::registry_tag_authority_epoch, 9);

    cl::registry_write_response decoded;
    const crpc::status s =
        cl::decode_write_response(bytes_of(frame_with(tags)), decoded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_missing_required"), reason_of(s));
}

CRPC_TEST(write_response_rejects_a_missing_authority_epoch_tag) {
    // The tag that carries the authority epoch is tag 19. Before it existed the
    // value rode in tag 9 (writer_epoch), which is a different concept, so this
    // case also pins the tag identity: a tag-9-only response must NOT satisfy
    // the authority_epoch requirement.
    std::vector<std::byte> tags;
    put_u64_tag(tags, cl::registry_tag_index, 42);
    put_u64_tag(tags, cl::registry_tag_fence, 7);
    put_u64_tag(tags, cl::registry_tag_incarnation, 3);
    put_u64_tag(tags, cl::registry_tag_writer_epoch, 9);

    cl::registry_write_response decoded;
    const crpc::status s =
        cl::decode_write_response(bytes_of(frame_with(tags)), decoded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_missing_required"), reason_of(s));
}

CRPC_TEST(query_response_rejects_a_missing_authority_epoch_tag) {
    // Only the index is present. `encode_anchor_response` writes both.
    std::vector<std::byte> tags;
    put_u64_tag(tags, cl::registry_tag_index, 42);

    cl::registry_query_response decoded;
    const crpc::status s =
        cl::decode_query_response(bytes_of(frame_with(tags)), decoded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("dto_missing_required"), reason_of(s));
}

// --- tag 19 carries the authority epoch, and round-trips ----------------------
//
// This is the regression test for the semantic collision: `encode_write_response`
// used to put `authority_epoch` in tag 9 (`writer_epoch`), and
// `encode_anchor_response`/`encode_query_response` used tag 7 (`incarnation`).
// The same numeric tag id therefore meant three different things depending on
// which encoder produced it. Tag 19 is dedicated to the authority epoch.

CRPC_TEST(authority_epoch_round_trips_through_tag_19) {
    cl::registry_write_response value;
    value.index = 42;
    value.fence = 7;
    value.incarnation = 3;
    value.authority_epoch = 9;

    crpc::buffer out;
    const crpc::status encoded = cl::encode_write_response(value, out);
    CRPC_REQUIRE(encoded.ok());

    // The frame must contain tag 19, and must NOT carry the authority epoch in
    // tag 9 any more.
    std::vector<cl::registry_tag> tags;
    const crpc::status walked =
        cl::decode_registry_tags(bytes_of(out.readable()), tags);
    CRPC_REQUIRE(walked.ok());
    bool has_tag_19 = false;
    bool has_tag_9 = false;
    for (const auto& tag : tags) {
        if (tag.id == cl::registry_tag_authority_epoch) {
            has_tag_19 = true;
        }
        if (tag.id == cl::registry_tag_writer_epoch) {
            has_tag_9 = true;
        }
    }
    CRPC_CHECK(has_tag_19);
    CRPC_CHECK_FALSE(has_tag_9);

    cl::registry_write_response decoded;
    const crpc::status s =
        cl::decode_write_response(bytes_of(out.readable()), decoded);
    CRPC_REQUIRE(s.ok());
    CRPC_CHECK_EQ(crpc::u64{9}, decoded.authority_epoch);
    CRPC_CHECK_EQ(crpc::u64{42}, decoded.index);
    CRPC_CHECK_EQ(crpc::u64{7}, decoded.fence);
    CRPC_CHECK_EQ(crpc::u64{3}, decoded.incarnation);
}

CRPC_TEST(anchor_response_round_trips_through_tag_19) {
    const cl::registry_anchor value{9, 42};

    crpc::buffer out;
    const crpc::status encoded = cl::encode_anchor_response(value, out);
    CRPC_REQUIRE(encoded.ok());

    std::vector<cl::registry_tag> tags;
    const crpc::status walked =
        cl::decode_registry_tags(bytes_of(out.readable()), tags);
    CRPC_REQUIRE(walked.ok());
    bool has_tag_19 = false;
    for (const auto& tag : tags) {
        if (tag.id == cl::registry_tag_authority_epoch) {
            has_tag_19 = true;
        }
    }
    CRPC_CHECK(has_tag_19);

    cl::registry_query_response decoded;
    const crpc::status s =
        cl::decode_query_response(bytes_of(out.readable()), decoded);
    CRPC_REQUIRE(s.ok());
    CRPC_CHECK_EQ(crpc::u64{9}, decoded.anchor.authority_epoch);
    CRPC_CHECK_EQ(crpc::u64{42}, decoded.anchor.index);
}

// --- tag 18 carries the client's anchor epoch ---------------------------------
//
// `read_optional_identity` reads the client's anchor from the request. Before
// tag 18 existed the request had no way to carry the epoch half of the anchor,
// so `registry_anchor::authority_epoch` was always 0 on the request path and the
// two-dimensional `not_stale` degenerated to a one-dimensional comparison.

CRPC_TEST(register_frame_decodes_the_client_anchor_epoch) {
    std::vector<std::byte> tags;
    put_str_tag(tags, cl::registry_tag_token, "tok");
    put_str_tag(tags, cl::registry_tag_service, "svc");
    put_str_tag(tags, cl::registry_tag_node_id, "node-a");
    put_str_tag(tags, cl::registry_tag_host, "127.0.0.1");
    std::vector<std::byte> port;
    put_u16(port, 8080);
    put_tag(tags, cl::registry_tag_port, port);
    put_u64_tag(tags, cl::registry_tag_incarnation, 1);
    put_u64_tag(tags, cl::registry_tag_fence, 1);
    put_u64_tag(tags, cl::registry_tag_writer_epoch, 7);
    put_u64_tag(tags, cl::registry_tag_index, 12);
    put_u64_tag(tags, cl::registry_tag_client_authority_epoch, 9);

    cl::registry_register_request decoded;
    const crpc::status s = cl::decode_register_request(bytes_of(frame_with(tags)), decoded);
    CRPC_REQUIRE(s.ok());
    CRPC_CHECK(decoded.has_client_anchor);
    CRPC_CHECK_EQ(crpc::u64{12}, decoded.client_index);
    CRPC_CHECK_EQ(crpc::u64{9}, decoded.client_authority_epoch);
}

CRPC_TEST(deregister_frame_decodes_the_client_anchor_epoch) {
    std::vector<std::byte> tags;
    put_str_tag(tags, cl::registry_tag_token, "tok");
    put_str_tag(tags, cl::registry_tag_service, "svc");
    put_str_tag(tags, cl::registry_tag_node_id, "node-a");
    put_u64_tag(tags, cl::registry_tag_incarnation, 1);
    put_u64_tag(tags, cl::registry_tag_fence, 1);
    put_u64_tag(tags, cl::registry_tag_writer_epoch, 7);
    put_u64_tag(tags, cl::registry_tag_index, 12);
    put_u64_tag(tags, cl::registry_tag_client_authority_epoch, 9);

    cl::registry_deregister_request decoded;
    const crpc::status s = cl::decode_deregister_request(bytes_of(frame_with(tags)), decoded);
    CRPC_REQUIRE(s.ok());
    CRPC_CHECK(decoded.has_client_anchor);
    CRPC_CHECK_EQ(crpc::u64{12}, decoded.client_index);
    CRPC_CHECK_EQ(crpc::u64{9}, decoded.client_authority_epoch);
}

CRPC_TEST(set_state_frame_decodes_the_client_anchor_epoch) {
    std::vector<std::byte> tags;
    put_str_tag(tags, cl::registry_tag_token, "tok");
    put_str_tag(tags, cl::registry_tag_service, "svc");
    put_str_tag(tags, cl::registry_tag_node_id, "node-a");
    put_u64_tag(tags, cl::registry_tag_incarnation, 1);
    put_u64_tag(tags, cl::registry_tag_fence, 1);
    put_u64_tag(tags, cl::registry_tag_writer_epoch, 7);
    std::vector<std::byte> state;
    put_u8(state, 2);
    put_tag(tags, cl::registry_tag_state, state);
    put_u64_tag(tags, cl::registry_tag_index, 12);
    put_u64_tag(tags, cl::registry_tag_client_authority_epoch, 9);

    cl::registry_set_state_request decoded;
    const crpc::status s = cl::decode_set_state_request(bytes_of(frame_with(tags)), decoded);
    CRPC_REQUIRE(s.ok());
    CRPC_CHECK(decoded.has_client_anchor);
    CRPC_CHECK_EQ(crpc::u64{12}, decoded.client_index);
    CRPC_CHECK_EQ(crpc::u64{9}, decoded.client_authority_epoch);
}

CRPC_TEST(client_anchor_epoch_absent_is_zero_and_not_an_anchor) {
    // No tag 14 and no tag 18: `has_client_anchor` must stay false so the
    // service treats the write as having no anchor at all, rather than as an
    // anchor of {0, 0} (which would be "ahead of" nothing and always stale).
    const std::vector<std::byte> body = valid_register_frame("tok", "svc", "node-a");
    cl::registry_register_request decoded;
    const crpc::status s = cl::decode_register_request(bytes_of(body), decoded);
    CRPC_REQUIRE(s.ok());
    CRPC_CHECK_FALSE(decoded.has_client_anchor);
    CRPC_CHECK_EQ(crpc::u64{0}, decoded.client_authority_epoch);
    CRPC_CHECK_EQ(crpc::u64{0}, decoded.client_index);
}

// `has_client_anchor` must mean "the client sent an anchor", not "the index it
// sent is non-zero". The catalog's index starts at 1 (registry.cpp: `next_index
// = index_ + 1`), so a zero index never legitimately occurs -- but if a client
// ever DID send {epoch, 0} explicitly, deciding "no anchor" from `index != 0`
// would silently skip the anchor check and let the write through. The presence
// of the tags is what carries the meaning.

CRPC_TEST(an_explicit_zero_index_anchor_is_distinguishable) {
    // Tag 14 present with value 0, and tag 18 present with a non-zero epoch.
    std::vector<std::byte> tags;
    put_str_tag(tags, cl::registry_tag_token, "tok");
    put_str_tag(tags, cl::registry_tag_service, "svc");
    put_str_tag(tags, cl::registry_tag_node_id, "node-a");
    put_str_tag(tags, cl::registry_tag_host, "127.0.0.1");
    std::vector<std::byte> port;
    put_u16(port, 8080);
    put_tag(tags, cl::registry_tag_port, port);
    put_u64_tag(tags, cl::registry_tag_incarnation, 1);
    put_u64_tag(tags, cl::registry_tag_fence, 1);
    put_u64_tag(tags, cl::registry_tag_writer_epoch, 7);
    put_u64_tag(tags, cl::registry_tag_index, 0);
    put_u64_tag(tags, cl::registry_tag_client_authority_epoch, 5);

    cl::registry_register_request decoded;
    const crpc::status s = cl::decode_register_request(bytes_of(frame_with(tags)), decoded);
    CRPC_REQUIRE(s.ok());
    CRPC_CHECK(decoded.has_client_anchor);
    CRPC_CHECK_EQ(crpc::u64{0}, decoded.client_index);
    CRPC_CHECK_EQ(crpc::u64{5}, decoded.client_authority_epoch);
}

CRPC_TEST(an_epoch_without_an_index_is_still_an_anchor) {
    // Tag 18 alone must also count as an anchor: the epoch half is the half
    // that survives a restart, so treating "no tag 14" as "no anchor" would
    // make a restarted service unable to be pinned by epoch.
    std::vector<std::byte> tags;
    put_str_tag(tags, cl::registry_tag_token, "tok");
    put_str_tag(tags, cl::registry_tag_service, "svc");
    put_str_tag(tags, cl::registry_tag_node_id, "node-a");
    put_str_tag(tags, cl::registry_tag_host, "127.0.0.1");
    std::vector<std::byte> port;
    put_u16(port, 8080);
    put_tag(tags, cl::registry_tag_port, port);
    put_u64_tag(tags, cl::registry_tag_incarnation, 1);
    put_u64_tag(tags, cl::registry_tag_fence, 1);
    put_u64_tag(tags, cl::registry_tag_writer_epoch, 7);
    put_u64_tag(tags, cl::registry_tag_client_authority_epoch, 5);

    cl::registry_register_request decoded;
    const crpc::status s = cl::decode_register_request(bytes_of(frame_with(tags)), decoded);
    CRPC_REQUIRE(s.ok());
    CRPC_CHECK(decoded.has_client_anchor);
    CRPC_CHECK_EQ(crpc::u64{5}, decoded.client_authority_epoch);
    CRPC_CHECK_EQ(crpc::u64{0}, decoded.client_index);
}

CRPC_TEST_MAIN()
