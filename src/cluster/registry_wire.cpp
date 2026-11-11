// CRPC -- G19 step2: registry service wire codec.
//
// The decoder is fail-closed in the order VERDICT section 2.2 fixes. The order
// is not cosmetic: `total_len` is checked before any tag is looked at, so a
// body whose declared length prefix itself is truncated reports
// `dto_length_mismatch` rather than a misleading `dto_truncated_field`.
//
// Every integer is written and read through the explicit little-endian
// primitives in crpc/endian.hpp; no struct is serialised by memcpy and no
// buffer is reinterpreted as an integer, so the encoding is byte-identical on
// every supported host.
#include <cstdint>
#include <cstring>

#include <crpc/cluster/registry_wire.hpp>

#include <crpc/cluster/registry.hpp>
#include <crpc/endian.hpp>

namespace crpc {
namespace cluster {
namespace {

// The smallest possible tag is one byte of id plus four bytes of length.
constexpr std::size_t kTagHeader = 5;

void append_u8(std::vector<std::byte>& out, u8 value) {
    out.push_back(static_cast<std::byte>(value));
}

void append_u16(std::vector<std::byte>& out, u16 value) {
    const std::size_t at = out.size();
    out.resize(at + 2);
    endian::store_le16(out.data() + at, value);
}

void append_u32(std::vector<std::byte>& out, u32 value) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    endian::store_le32(out.data() + at, value);
}

void append_u64(std::vector<std::byte>& out, u64 value) {
    const std::size_t at = out.size();
    out.resize(at + 8);
    endian::store_le64(out.data() + at, value);
}

// A tag value reader. Every read is bounds-checked against the tag's own
// declared length, so a field can never run past the tag that owns it.
class tag_value_reader {
public:
    explicit tag_value_reader(span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool u8(crpc::u8& out) {
        if (bytes_.size() < 1) {
            return false;
        }
        out = static_cast<crpc::u8>(bytes_[0]);
        bytes_ = bytes_.subspan(1);
        return true;
    }

    [[nodiscard]] bool u16(crpc::u16& out) {
        if (bytes_.size() < 2) {
            return false;
        }
        out = endian::load_le16(bytes_.data());
        bytes_ = bytes_.subspan(2);
        return true;
    }

    [[nodiscard]] bool u32(crpc::u32& out) {
        if (bytes_.size() < 4) {
            return false;
        }
        out = endian::load_le32(bytes_.data());
        bytes_ = bytes_.subspan(4);
        return true;
    }

    [[nodiscard]] bool u64(crpc::u64& out) {
        if (bytes_.size() < 8) {
            return false;
        }
        out = endian::load_le64(bytes_.data());
        bytes_ = bytes_.subspan(8);
        return true;
    }

    // Length-prefixed string: a u32 length that must not run past the tag.
    [[nodiscard]] bool str(std::string& out) {
        crpc::u32 len = 0;
        if (!u32(len)) {
            return false;
        }
        if (len > bytes_.size()) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(bytes_.data()), len);
        bytes_ = bytes_.subspan(len);
        return true;
    }

    [[nodiscard]] bool at_end() const noexcept { return bytes_.empty(); }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size(); }

    // Borrows `count` bytes and advances past them. The caller must have
    // validated `count` against `remaining()` first.
    [[nodiscard]] span<const std::byte> borrow(std::size_t count) {
        const span<const std::byte> out = bytes_.first(count);
        bytes_ = bytes_.subspan(count);
        return out;
    }

private:
    span<const std::byte> bytes_;
};

// A scalar tag value in wire order (little-endian), so the writer can emit a
// tag without a per-type branch.
[[nodiscard]] std::vector<std::byte> scalar_tag_value_u64(u64 value) {
    std::vector<std::byte> out;
    append_u64(out, value);
    return out;
}

[[nodiscard]] std::vector<std::byte> scalar_tag_value_u32(u32 value) {
    std::vector<std::byte> out;
    append_u32(out, value);
    return out;
}

// Appends one tag (id, u32 length, payload) to the tag region.
void append_tag(std::vector<std::byte>& out, u8 id, const std::vector<std::byte>& value) {
    append_u8(out, id);
    append_u32(out, static_cast<u32>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

// Appends the frame prefix: u16 version + u32 total_len.

// Encodes a response into the DTO tag frame: u16 version, u32 total_len, tags.
[[nodiscard]] status finish_frame(const std::vector<std::byte>& tags, buffer& out) {
    std::vector<std::byte> frame;
    append_u16(frame, registry_dto_version);
    append_u32(frame, static_cast<u32>(tags.size()));
    frame.insert(frame.end(), tags.begin(), tags.end());
    out.clear();
    return out.append(frame.data(), frame.size()) ? status{}
                                                  : status{rpc_code::encode_error};
}

// Every tag id this service defines. A frame carrying any other id is
// `dto_unknown_tag`, including ids below `registry_tag_max_known`: the numeric
// range has gaps (tag 2 was retired, see the header's tag table) and a range test
// would accept them.
[[nodiscard]] bool is_known_tag(u8 id) {
    switch (id) {
    case registry_tag_token:
    case registry_tag_service:
    case registry_tag_node_id:
    case registry_tag_host:
    case registry_tag_port:
    case registry_tag_incarnation:
    case registry_tag_fence:
    case registry_tag_writer_epoch:
    case registry_tag_weight:
    case registry_tag_state:
    case registry_tag_meta:
    case registry_tag_passing_only:
    case registry_tag_index:
    case registry_tag_client_request_id:
    case registry_tag_token_issued_at:
    case registry_tag_token_nonce:
    case registry_tag_client_authority_epoch:
    case registry_tag_authority_epoch:
    case registry_tag_key:
    case registry_tag_value:
    case registry_tag_cas:
    case registry_tag_found:
    case registry_tag_create_index:
    case registry_tag_modify_index:
        return true;
    default:
        return false;
    }
}

} // namespace

rpc_error registry_failure(rpc_code code, const char* message, const char* reason) {
    rpc_error error(code, message);
    error.metadata.set("registry.reason", reason);
    return error;
}

status decode_registry_tags(span<const std::byte> body, std::vector<registry_tag>& out) {
    out.clear();
    // Rule 1 is the first layer: the fixed prefix must be intact.
    if (body.size() < registry_frame_prefix) {
        return status{registry_failure(rpc_code::decode_error,
                                       "registry frame is shorter than its prefix",
                                       registry_reason_dto_length_mismatch)};
    }
    const u16 version = endian::load_le16(body.data());
    const u32 total_len = endian::load_le32(body.data() + 2);
    // Rule 6: a version this service does not speak is a capability refusal,
    // not a malformed frame.
    if (version != registry_dto_version) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "unsupported registry dto version",
                                       registry_reason_dto_unsupported_version)};
    }
    if (total_len != body.size() - registry_frame_prefix) {
        return status{registry_failure(rpc_code::decode_error,
                                       "registry frame length mismatch",
                                       registry_reason_dto_length_mismatch)};
    }

    std::size_t pos = registry_frame_prefix;
    const std::size_t end = body.size();
    while (pos < end) {
        // Rule 4: a tag header must fit in the remaining bytes.
        if (end - pos < kTagHeader) {
            return status{registry_failure(rpc_code::decode_error,
                                           "registry tag header is truncated",
                                           registry_reason_dto_truncated_field)};
        }
        const u8 id = static_cast<u8>(body[pos]);
        const u32 len = endian::load_le32(body.data() + pos + 1);
        pos += kTagHeader;
        // Rule 4: the declared value must fit in the remaining bytes. This is
        // what distinguishes a truncated field from an absent one.
        if (len > end - pos) {
            return status{registry_failure(rpc_code::decode_error,
                                           "registry tag value is truncated",
                                           registry_reason_dto_truncated_field)};
        }
        // Rule 3: an unknown tag id is rejected, never skipped. A future
        // version's field must not be silently dropped by this service.
        //
        // The test is an explicit WHITELIST, not `id > registry_tag_max_known`.
        // An interval test silently accepts every id in a gap: tag 2 was retired
        // (the DTO version lives in the frame header, not in a tag) and an
        // interval test would keep accepting it as a meaningful id forever. A
        // whitelist makes every accepted id one this service actually defines.
        if (!is_known_tag(id)) {
            return status{registry_failure(rpc_code::decode_error,
                                           "unknown registry tag id",
                                           registry_reason_dto_unknown_tag)};
        }
        // Rule 5: a repeated tag id is rejected; a duplicate would otherwise
        // leave the winning value to the reader's iteration order.
        for (const registry_tag& existing : out) {
            if (existing.id == id) {
                return status{registry_failure(rpc_code::decode_error,
                                               "duplicate registry tag id",
                                               registry_reason_dto_duplicate_tag)};
            }
        }
        out.push_back(registry_tag{id, body.subspan(pos, len)});
        pos += len;
    }
    return status{};
}

status check_required_tags(const std::vector<registry_tag>& tags, const u8* required,
                           std::size_t count, u8& missing_id) {
    for (std::size_t i = 0; i < count; ++i) {
        const u8 wanted = required[i];
        bool found = false;
        for (const registry_tag& tag : tags) {
            if (tag.id == wanted) {
                found = true;
                break;
            }
        }
        if (!found) {
            missing_id = wanted;
            // This is the FIRST of two "missing required tag" gates, and it is
            // the only one that reports WHICH tag is absent. `find_required`
            // (the second gate, which guards the dereference) uses a generic
            // message, so a test can tell a disabled pre-gate from a working
            // one by the message rather than by a crash.
            return status{registry_failure(rpc_code::decode_error,
                                           "registry frame is missing a required tag (first gate)",
                                           registry_reason_dto_missing_required)};
        }
    }
    return status{};
}

namespace {

[[nodiscard]] status require_tags(const std::vector<registry_tag>& tags, const u8* ids,
                                  std::size_t count) {
    u8 missing = 0;
    return check_required_tags(tags, ids, count, missing);
}

// Fetches a required tag. This is the ONE place that decides whether a required
// tag is present, and it returns a status rather than a bare pointer: every
// decoder dereferences the result immediately (read_string(*token, ...)), so a
// null pointer here would be a dereference that takes the whole process down
// instead of the decode_error the caller can act on. A malformed frame is
// ordinary input, not an exception path.
//
// `require_tags` above it is a separate, earlier gate that reports WHICH tag is
// missing; this helper is the second, mandatory gate. Keeping both means a
// future change that loosens or skips the first gate can no longer turn a
// missing tag into a null dereference.
[[nodiscard]] status find_required(const std::vector<registry_tag>& tags, u8 id,
                                   const registry_tag** out) {
    for (const registry_tag& tag : tags) {
        if (tag.id == id) {
            *out = &tag;
            return status{};
        }
    }
    return status{registry_failure(rpc_code::decode_error,
                                   "registry frame is missing a required tag",
                                   registry_reason_dto_missing_required)};
}

[[nodiscard]] status read_string(const registry_tag& tag, std::string& out) {
    tag_value_reader reader(tag.value);
    if (!reader.str(out) || !reader.at_end()) {
        return status{registry_failure(rpc_code::decode_error,
                                       "registry string field is malformed",
                                       registry_reason_dto_truncated_field)};
    }
    return status{};
}

[[nodiscard]] status read_u8(const registry_tag& tag, u8& out) {
    if (tag.value.size() != sizeof(u8)) {
        return status{registry_failure(rpc_code::decode_error,
                                       "registry u8 field has the wrong width",
                                       registry_reason_dto_truncated_field)};
    }
    out = static_cast<u8>(tag.value[0]);
    return status{};
}

[[nodiscard]] status read_u16(const registry_tag& tag, u16& out) {
    if (tag.value.size() != sizeof(u16)) {
        return status{registry_failure(rpc_code::decode_error,
                                       "registry u16 field has the wrong width",
                                       registry_reason_dto_truncated_field)};
    }
    out = endian::load_le16(tag.value.data());
    return status{};
}

[[nodiscard]] status read_u32(const registry_tag& tag, u32& out) {
    if (tag.value.size() != sizeof(u32)) {
        return status{registry_failure(rpc_code::decode_error,
                                       "registry u32 field has the wrong width",
                                       registry_reason_dto_truncated_field)};
    }
    out = endian::load_le32(tag.value.data());
    return status{};
}

[[nodiscard]] status read_u64(const registry_tag& tag, u64& out) {
    if (tag.value.size() != sizeof(u64)) {
        return status{registry_failure(rpc_code::decode_error,
                                       "registry u64 field has the wrong width",
                                       registry_reason_dto_truncated_field)};
    }
    out = endian::load_le64(tag.value.data());
    return status{};
}

// Reads a state tag and validates the enum range.
[[nodiscard]] status read_state(const registry_tag& tag, u8& out) {
    u8 raw = 0;
    if (const status s = read_u8(tag, raw); !s.ok()) {
        return s;
    }
    if (raw > static_cast<u8>(instance_state::deregistered)) {
        return status{registry_failure(rpc_code::decode_error,
                                       "registry state is out of range",
                                       registry_reason_dto_truncated_field)};
    }
    out = raw;
    return status{};
}

// Reads the token's issue time and nonce, the DTO-level half of the
// time-limited-token window (VERDICT section 1.3).
[[nodiscard]] status read_token_window(std::vector<registry_tag>& tags,
                                       std::chrono::milliseconds& issued_at,
                                       bool& has_issued_at, std::string& nonce) {
    for (registry_tag& tag : tags) {
        switch (tag.id) {
        case registry_tag_token_issued_at: {
            u64 millis = 0;
            if (const status s = read_u64(tag, millis); !s.ok()) {
                return s;
            }
            issued_at = std::chrono::milliseconds(millis);
            has_issued_at = true;
            break;
        }
        case registry_tag_token_nonce:
            if (const status s = read_string(tag, nonce); !s.ok()) {
                return s;
            }
            break;
        default:
            break;
        }
    }
    return status{};
}

// Reads the optional identity fields every write request may carry: the
// client's idempotency key (tag 15) and its anchor index (tag 14). Shared by
// the three write decoders so the fail-closed rules are applied identically.
// Reads the client's anchor from the request: the idempotency key (tag 15), the
// index half (tag 14) and the epoch half (tag 18).
//
// `has_anchor` means "the client sent an anchor", NOT "the index it sent is
// non-zero". Deciding it from `index != 0` would silently skip the anchor check
// for a client that explicitly presents {epoch, 0}: the write would be treated
// as having no anchor and let through. The presence of either tag is what
// carries the meaning, so both are tracked separately.
[[nodiscard]] status read_optional_identity(std::vector<registry_tag>& tags,
                                            std::string& request_id, u64& index,
                                            u64& authority_epoch,
                                            bool& has_anchor) {
    bool saw_index = false;
    bool saw_epoch = false;
    for (registry_tag& tag : tags) {
        switch (tag.id) {
        case registry_tag_client_request_id:
            if (const status s = read_string(tag, request_id); !s.ok()) {
                return s;
            }
            break;
        case registry_tag_index:
            if (const status s = read_u64(tag, index); !s.ok()) {
                return s;
            }
            saw_index = true;
            break;
        case registry_tag_client_authority_epoch:
            if (const status s = read_u64(tag, authority_epoch); !s.ok()) {
                return s;
            }
            saw_epoch = true;
            break;
        default:
            break;
        }
    }
    has_anchor = saw_index || saw_epoch;
    return status{};
}

} // namespace

status decode_register_request(span<const std::byte> body,
                               registry_register_request& out) {
    static const u8 kRequired[] = {registry_tag_token, registry_tag_service,
                                   registry_tag_node_id, registry_tag_host,
                                   registry_tag_port, registry_tag_incarnation,
                                   registry_tag_fence, registry_tag_writer_epoch};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(body, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }

    const registry_tag* token = nullptr;
    const registry_tag* service = nullptr;
    const registry_tag* node = nullptr;
    const registry_tag* host = nullptr;
    const registry_tag* port = nullptr;
    const registry_tag* incarnation = nullptr;
    const registry_tag* fence = nullptr;
    const registry_tag* epoch = nullptr;
    if (const status found = find_required(tags, registry_tag_token, &token); !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_service, &service);
        !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_node_id, &node); !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_host, &host); !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_port, &port); !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_incarnation, &incarnation);
        !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_fence, &fence); !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_writer_epoch, &epoch);
        !found.ok()) {
        return found;
    }

    if (const status s = read_string(*token, out.token); !s.ok()) {
        return s;
    }
    if (const status s = read_string(*service, out.service); !s.ok()) {
        return s;
    }
    if (const status s = read_string(*node, out.node_id); !s.ok()) {
        return s;
    }
    if (const status s = read_string(*host, out.host); !s.ok()) {
        return s;
    }
    if (const status s = read_u16(*port, out.port); !s.ok()) {
        return s;
    }
    if (const status s = read_u64(*incarnation, out.incarnation); !s.ok()) {
        return s;
    }
    if (const status s = read_u64(*fence, out.fence); !s.ok()) {
        return s;
    }
    if (const status s = read_u64(*epoch, out.writer_epoch); !s.ok()) {
        return s;
    }

    // Optional fields. An optional tag that is present but malformed is still
    // an error: skipping it would silently drop a value the sender intended.
    for (const registry_tag& tag : tags) {
        switch (tag.id) {
        case registry_tag_weight: {
            if (const status s = read_u32(tag, out.weight); !s.ok()) {
                return s;
            }
            break;
        }
        case registry_tag_state: {
            if (const status s = read_state(tag, out.state); !s.ok()) {
                return s;
            }
            out.has_state = true;
            break;
        }
        case registry_tag_meta:
            out.meta.assign(tag.value.begin(), tag.value.end());
            break;
        default:
            break;
        }
    }
    const status identity = read_optional_identity(tags, out.client_request_id,
                                                   out.client_index,
                                                   out.client_authority_epoch,
                                                   out.has_client_anchor);
    if (!identity.ok()) {
        return identity;
    }
    return read_token_window(tags, out.token_issued_at, out.has_token_issued_at,
                             out.token_nonce);
}

status decode_deregister_request(span<const std::byte> body,
                                 registry_deregister_request& out) {
    static const u8 kRequired[] = {registry_tag_token, registry_tag_service,
                                   registry_tag_node_id, registry_tag_incarnation,
                                   registry_tag_fence, registry_tag_writer_epoch};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(body, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }

    const registry_tag* token = nullptr;
    const registry_tag* service = nullptr;
    const registry_tag* node = nullptr;
    const registry_tag* incarnation = nullptr;
    const registry_tag* fence = nullptr;
    const registry_tag* epoch = nullptr;
    if (const status found = find_required(tags, registry_tag_token, &token); !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_service, &service);
        !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_node_id, &node); !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_incarnation, &incarnation);
        !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_fence, &fence); !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_writer_epoch, &epoch);
        !found.ok()) {
        return found;
    }

    if (const status s = read_string(*token, out.token); !s.ok()) {
        return s;
    }
    if (const status s = read_string(*service, out.service); !s.ok()) {
        return s;
    }
    if (const status s = read_string(*node, out.node_id); !s.ok()) {
        return s;
    }
    if (const status s = read_u64(*incarnation, out.incarnation); !s.ok()) {
        return s;
    }
    if (const status s = read_u64(*fence, out.fence); !s.ok()) {
        return s;
    }
    if (const status s = read_u64(*epoch, out.writer_epoch); !s.ok()) {
        return s;
    }
    const status identity = read_optional_identity(tags, out.client_request_id,
                                                   out.client_index,
                                                   out.client_authority_epoch,
                                                   out.has_client_anchor);
    if (!identity.ok()) {
        return identity;
    }
    return read_token_window(tags, out.token_issued_at, out.has_token_issued_at,
                             out.token_nonce);
}

status decode_set_state_request(span<const std::byte> body,
                                registry_set_state_request& out) {
    static const u8 kRequired[] = {registry_tag_token, registry_tag_service,
                                   registry_tag_node_id, registry_tag_incarnation,
                                   registry_tag_fence, registry_tag_writer_epoch,
                                   registry_tag_state};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(body, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }

    const registry_tag* token = nullptr;
    const registry_tag* service = nullptr;
    const registry_tag* node = nullptr;
    const registry_tag* incarnation = nullptr;
    const registry_tag* fence = nullptr;
    const registry_tag* epoch = nullptr;
    const registry_tag* state = nullptr;
    if (const status found = find_required(tags, registry_tag_token, &token); !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_service, &service);
        !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_node_id, &node); !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_incarnation, &incarnation);
        !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_fence, &fence); !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_writer_epoch, &epoch);
        !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_state, &state); !found.ok()) {
        return found;
    }

    if (const status s = read_string(*token, out.token); !s.ok()) {
        return s;
    }
    if (const status s = read_string(*service, out.service); !s.ok()) {
        return s;
    }
    if (const status s = read_string(*node, out.node_id); !s.ok()) {
        return s;
    }
    if (const status s = read_u64(*incarnation, out.incarnation); !s.ok()) {
        return s;
    }
    if (const status s = read_u64(*fence, out.fence); !s.ok()) {
        return s;
    }
    if (const status s = read_u64(*epoch, out.writer_epoch); !s.ok()) {
        return s;
    }
    const status state_read = read_state(*state, out.state);
    if (!state_read.ok()) {
        return state_read;
    }
    const status identity = read_optional_identity(tags, out.client_request_id,
                                                   out.client_index,
                                                   out.client_authority_epoch,
                                                   out.has_client_anchor);
    if (!identity.ok()) {
        return identity;
    }
    return read_token_window(tags, out.token_issued_at, out.has_token_issued_at,
                             out.token_nonce);
}

status decode_query_request(span<const std::byte> body, registry_query_request& out) {
    static const u8 kRequired[] = {registry_tag_token};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(body, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }

    const registry_tag* token = nullptr;
    if (const status found = find_required(tags, registry_tag_token, &token); !found.ok()) {
        return found;
    }
    if (const status s = read_string(*token, out.token); !s.ok()) {
        return s;
    }
    for (const registry_tag& tag : tags) {
        switch (tag.id) {
        case registry_tag_service:
            if (const status s = read_string(tag, out.service); !s.ok()) {
                return s;
            }
            break;
        case registry_tag_node_id:
            if (const status s = read_string(tag, out.node_id); !s.ok()) {
                return s;
            }
            break;
        case registry_tag_passing_only: {
            u8 raw = 0;
            if (const status s = read_u8(tag, raw); !s.ok()) {
                return s;
            }
            out.passing_only = raw != 0;
            break;
        }
        default:
            break;
        }
    }
    return read_token_window(tags, out.token_issued_at, out.has_token_issued_at,
                             out.token_nonce);
}

status decode_next_fence_request(span<const std::byte> body,
                                 registry_next_fence_request& out) {
    static const u8 kRequired[] = {registry_tag_token, registry_tag_service,
                                   registry_tag_node_id};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(body, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }

    const registry_tag* token = nullptr;
    const registry_tag* service = nullptr;
    const registry_tag* node = nullptr;
    if (const status found = find_required(tags, registry_tag_token, &token); !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_service, &service);
        !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_node_id, &node); !found.ok()) {
        return found;
    }
    if (const status s = read_string(*token, out.token); !s.ok()) {
        return s;
    }
    if (const status s = read_string(*service, out.service); !s.ok()) {
        return s;
    }
    const status node_read = read_string(*node, out.node_id);
    if (!node_read.ok()) {
        return node_read;
    }
    return read_token_window(tags, out.token_issued_at, out.has_token_issued_at,
                             out.token_nonce);
}

status decode_ping_request(span<const std::byte> body, registry_ping_request& out) {
    static const u8 kRequired[] = {registry_tag_token};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(body, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }
    const registry_tag* token = nullptr;
    if (const status found = find_required(tags, registry_tag_token, &token); !found.ok()) {
        return found;
    }
    const status token_read = read_string(*token, out.token);
    if (!token_read.ok()) {
        return token_read;
    }
    return read_token_window(tags, out.token_issued_at, out.has_token_issued_at,
                             out.token_nonce);
}

status decode_kv_get_request(span<const std::byte> body,
                             kv_get_request& out) {
    static const u8 kRequired[] = {registry_tag_token, registry_tag_key};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(body, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }
    const registry_tag* token = nullptr;
    const registry_tag* key = nullptr;
    if (const status found = find_required(tags, registry_tag_token, &token);
        !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_key, &key);
        !found.ok()) {
        return found;
    }
    if (const status s = read_string(*token, out.token); !s.ok()) {
        return s;
    }
    if (const status s = read_string(*key, out.key); !s.ok()) {
        return s;
    }
    return read_token_window(tags, out.token_issued_at, out.has_token_issued_at,
                             out.token_nonce);
}

status decode_kv_put_request(span<const std::byte> body,
                             kv_put_request& out) {
    static const u8 kRequired[] = {registry_tag_token, registry_tag_key,
                                   registry_tag_value};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(body, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }
    const registry_tag* token = nullptr;
    const registry_tag* key = nullptr;
    const registry_tag* value = nullptr;
    if (const status found = find_required(tags, registry_tag_token, &token);
        !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_key, &key);
        !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_value, &value);
        !found.ok()) {
        return found;
    }
    if (const status s = read_string(*token, out.token); !s.ok()) {
        return s;
    }
    if (const status s = read_string(*key, out.key); !s.ok()) {
        return s;
    }
    out.value.assign(value->value.begin(), value->value.end());
    for (const registry_tag& tag : tags) {
        if (tag.id == registry_tag_cas) {
            if (const status s = read_u64(tag, out.cas); !s.ok()) {
                return s;
            }
            out.has_cas = true;
        }
    }
    return read_token_window(tags, out.token_issued_at, out.has_token_issued_at,
                             out.token_nonce);
}

status decode_kv_delete_request(span<const std::byte> body,
                                kv_delete_request& out) {
    static const u8 kRequired[] = {registry_tag_token, registry_tag_key};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(body, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }
    const registry_tag* token = nullptr;
    const registry_tag* key = nullptr;
    if (const status found = find_required(tags, registry_tag_token, &token);
        !found.ok()) {
        return found;
    }
    if (const status found = find_required(tags, registry_tag_key, &key);
        !found.ok()) {
        return found;
    }
    if (const status s = read_string(*token, out.token); !s.ok()) {
        return s;
    }
    if (const status s = read_string(*key, out.key); !s.ok()) {
        return s;
    }
    return read_token_window(tags, out.token_issued_at, out.has_token_issued_at,
                             out.token_nonce);
}

// The authority epoch travels in tag 19, never in tag 9 (`writer_epoch`) or tag 7
// (`incarnation`). Those two ids already mean something else in the request
// direction, and reusing them on the response side made one numeric id carry
// three different meanings depending on the encoder.

status encode_write_response(const registry_write_response& value, buffer& out) {
    std::vector<std::byte> tags;
    append_tag(tags, registry_tag_index, scalar_tag_value_u64(value.index));
    append_tag(tags, registry_tag_fence, scalar_tag_value_u64(value.fence));
    append_tag(tags, registry_tag_incarnation, scalar_tag_value_u64(value.incarnation));
    append_tag(tags, registry_tag_authority_epoch,
               scalar_tag_value_u64(value.authority_epoch));

    out.clear();
    return finish_frame(tags, out);
}

status encode_anchor_response(const registry_anchor& value, buffer& out) {
    std::vector<std::byte> tags;
    append_tag(tags, registry_tag_authority_epoch,
               scalar_tag_value_u64(value.authority_epoch));
    append_tag(tags, registry_tag_index, scalar_tag_value_u64(value.index));

    out.clear();
    return finish_frame(tags, out);
}

status encode_kv_get_response(const kv_get_response& value, buffer& out) {
    std::vector<std::byte> tags;
    append_tag(tags, registry_tag_index, scalar_tag_value_u64(value.index));
    append_tag(tags, registry_tag_found,
               scalar_tag_value_u64(value.found ? 1 : 0));
    append_tag(tags, registry_tag_create_index,
               scalar_tag_value_u64(value.create_index));
    append_tag(tags, registry_tag_modify_index,
               scalar_tag_value_u64(value.modify_index));
    append_tag(tags, registry_tag_value, value.value);
    out.clear();
    return finish_frame(tags, out);
}

status encode_kv_put_response(const kv_put_response& value, buffer& out) {
    std::vector<std::byte> tags;
    append_tag(tags, registry_tag_index, scalar_tag_value_u64(value.index));
    append_tag(tags, registry_tag_modify_index,
               scalar_tag_value_u64(value.modify_index));
    out.clear();
    return finish_frame(tags, out);
}

status encode_kv_delete_response(const kv_delete_response& value, buffer& out) {
    std::vector<std::byte> tags;
    append_tag(tags, registry_tag_index, scalar_tag_value_u64(value.index));
    append_tag(tags, registry_tag_found,
               scalar_tag_value_u64(value.deleted ? 1 : 0));
    out.clear();
    return finish_frame(tags, out);
}

// The instance body version. It matches the registry's restore payload version
// so the two decoders are the same shape (fence/incarnation header + versioned
// body); the constants stay separate so a future change to one does not
// silently change the other.
constexpr u8 kQueryInstanceVersion = 1;
constexpr std::size_t kQueryInstanceHeader = 16;

namespace {

// Encodes one instance body: version(u8) | state(u8) | service | node_id | host
// (each u32 length-prefixed) | port(u16) | weight(u32) | incarnation(u64) |
// meta_count(u32) then per entry key/value (u32 length-prefixed).
[[nodiscard]] status encode_instance_body(const service_instance& instance,
                                          std::vector<std::byte>& out) {
    out.clear();
    append_u8(out, kQueryInstanceVersion);
    append_u8(out, static_cast<u8>(instance.state));

    const auto append_str = [&out](const std::string& value) {
        append_u32(out, static_cast<u32>(value.size()));
        out.insert(out.end(), reinterpret_cast<const std::byte*>(value.data()),
                   reinterpret_cast<const std::byte*>(value.data()) + value.size());
    };
    append_str(instance.service);
    append_str(instance.node_id);
    append_str(instance.address.host);
    append_u16(out, instance.address.port);
    append_u32(out, instance.weight);
    append_u64(out, instance.incarnation);

    append_u32(out, static_cast<u32>(instance.meta.size()));
    for (const auto& entry : instance.meta) {
        append_str(entry.first);
        append_str(entry.second);
    }
    return status{};
}

} // namespace

status encode_query_instance(const service_instance& instance,
                             std::vector<std::byte>& out) {
    out.clear();
    append_u64(out, instance.revision);
    append_u64(out, instance.incarnation);
    std::vector<std::byte> body;
    const status s = encode_instance_body(instance, body);
    if (!s.ok()) {
        return s;
    }
    out.insert(out.end(), body.begin(), body.end());
    return status{};
}

status decode_query_instance(span<const std::byte> bytes, u64& fence, u64& incarnation,
                             service_instance& out) {
    if (bytes.size() < kQueryInstanceHeader) {
        return status{registry_failure(rpc_code::decode_error,
                                       "query instance is shorter than its header",
                                       registry_reason_dto_truncated_field)};
    }
    fence = endian::load_le64(bytes.data());
    incarnation = endian::load_le64(bytes.data() + 8);
    tag_value_reader reader(bytes.subspan(kQueryInstanceHeader));
    u8 version = 0;
    u8 state = 0;
    if (!reader.u8(version) || version != kQueryInstanceVersion) {
        return status{registry_failure(rpc_code::decode_error,
                                       "query instance has an unsupported version",
                                       registry_reason_dto_unsupported_version)};
    }
    if (!reader.u8(state) ||
        state > static_cast<u8>(instance_state::deregistered)) {
        return status{registry_failure(rpc_code::decode_error,
                                       "query instance state is out of range",
                                       registry_reason_dto_truncated_field)};
    }
    out.state = static_cast<instance_state>(state);
    if (!reader.str(out.service) || !reader.str(out.node_id) ||
        !reader.str(out.address.host)) {
        return status{registry_failure(rpc_code::decode_error,
                                       "query instance identity is truncated",
                                       registry_reason_dto_truncated_field)};
    }
    if (!reader.u16(out.address.port) || !reader.u32(out.weight) ||
        !reader.u64(out.incarnation)) {
        return status{registry_failure(rpc_code::decode_error,
                                       "query instance fields are truncated",
                                       registry_reason_dto_truncated_field)};
    }
    u32 meta_count = 0;
    if (!reader.u32(meta_count) || meta_count > 100000u) {
        return status{registry_failure(rpc_code::decode_error,
                                       "query instance metadata count is invalid",
                                       registry_reason_dto_truncated_field)};
    }
    out.meta = metadata_list{};
    for (u32 i = 0; i < meta_count; ++i) {
        std::string key;
        std::string value;
        if (!reader.str(key) || !reader.str(value)) {
            return status{registry_failure(rpc_code::decode_error,
                                           "query instance metadata is truncated",
                                           registry_reason_dto_truncated_field)};
        }
        out.meta.set(std::move(key), std::move(value));
    }
    if (!reader.at_end()) {
        return status{registry_failure(rpc_code::decode_error,
                                       "query instance has trailing bytes",
                                       registry_reason_dto_length_mismatch)};
    }
    return status{};
}

status encode_query_response(const registry_query_response& value, buffer& out) {
    std::vector<std::byte> body;
    append_u32(body, static_cast<u32>(value.rows.size()));
    for (const registry_query_row& row : value.rows) {
        std::vector<std::byte> encoded;
        const status s = encode_query_instance(row.instance, encoded);
        if (!s.ok()) {
            return s;
        }
        append_u32(body, static_cast<u32>(encoded.size()));
        body.insert(body.end(), encoded.begin(), encoded.end());
    }

    std::vector<std::byte> tags;
    append_tag(tags, registry_tag_authority_epoch,
               scalar_tag_value_u64(value.anchor.authority_epoch));
    append_tag(tags, registry_tag_index, scalar_tag_value_u64(value.anchor.index));
    append_tag(tags, registry_tag_meta, body);

    out.clear();
    return finish_frame(tags, out);
}

// The response decoders are fail-closed for the same reason the request decoders
// are: a response the service never produced (a truncated frame, a wrong
// endpoint, a bug) used to decode "successfully" into a zeroed struct, and the
// client then stored `{0, 0}` as its anchor -- a valid-looking anchor pointing at
// nothing, which makes every later write look stale with no way to recover.
// The required set mirrors exactly what the matching encoder writes.

status decode_write_response(span<const std::byte> in, registry_write_response& out) {
    static const u8 kRequired[] = {registry_tag_index, registry_tag_fence,
                                   registry_tag_incarnation,
                                   registry_tag_authority_epoch};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(in, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }
    for (const registry_tag& tag : tags) {
        if (tag.value.size() != sizeof(u64)) {
            continue;
        }
        const u64 value = endian::load_le64(tag.value.data());
        switch (tag.id) {
        case registry_tag_index:
            out.index = value;
            break;
        case registry_tag_fence:
            out.fence = value;
            break;
        case registry_tag_incarnation:
            out.incarnation = value;
            break;
        case registry_tag_authority_epoch:
            out.authority_epoch = value;
            break;
        default:
            break;
        }
    }
    return status{};
}

status decode_query_response(span<const std::byte> in, registry_query_response& out) {
// Mirrors `encode_query_response`: the anchor's epoch (tag 19) and index
// (tag 14, `registry_tag_index`) are both always written, so both are required
// here.
    static const u8 kRequired[] = {registry_tag_authority_epoch, registry_tag_index};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(in, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }
    for (const registry_tag& tag : tags) {
        if (tag.id == registry_tag_authority_epoch && tag.value.size() == sizeof(u64)) {
            out.anchor.authority_epoch = endian::load_le64(tag.value.data());
        } else if (tag.id == registry_tag_index && tag.value.size() == sizeof(u64)) {
            out.anchor.index = endian::load_le64(tag.value.data());
        } else if (tag.id == registry_tag_meta) {
            tag_value_reader reader(tag.value);
            u32 count = 0;
            if (!reader.u32(count)) {
                return status{registry_failure(rpc_code::decode_error,
                                               "query response count is truncated",
                                               registry_reason_dto_truncated_field)};
            }
            for (u32 i = 0; i < count; ++i) {
                u32 length = 0;
                if (!reader.u32(length) || length > reader.remaining()) {
                    return status{registry_failure(
                        rpc_code::decode_error, "query response row is truncated",
                        registry_reason_dto_truncated_field)};
                }
                const span<const std::byte> row = reader.borrow(length);
                u64 fence = 0;
                u64 incarnation = 0;
                registry_query_row decoded_row;
                const status s = decode_query_instance(row, fence, incarnation,
                                                       decoded_row.instance);
                if (!s.ok()) {
                    return s;
                }
                decoded_row.revision = fence;
                out.rows.push_back(std::move(decoded_row));
            }
        }
    }
    return status{};
}

status decode_kv_get_response(span<const std::byte> in, kv_get_response& out) {
    static const u8 kRequired[] = {registry_tag_index, registry_tag_found,
                                   registry_tag_create_index,
                                   registry_tag_modify_index, registry_tag_value};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(in, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }
    for (const registry_tag& tag : tags) {
        switch (tag.id) {
        case registry_tag_index:
            if (tag.value.size() != sizeof(u64)) {
                return status{registry_failure(rpc_code::decode_error,
                                               "kv index field has the wrong width",
                                               registry_reason_dto_truncated_field)};
            }
            out.index = endian::load_le64(tag.value.data());
            break;
        case registry_tag_found:
            if (tag.value.size() != sizeof(u64)) {
                return status{registry_failure(rpc_code::decode_error,
                                               "kv found field has the wrong width",
                                               registry_reason_dto_truncated_field)};
            }
            out.found = endian::load_le64(tag.value.data()) != 0;
            break;
        case registry_tag_create_index:
            if (tag.value.size() != sizeof(u64)) {
                return status{registry_failure(rpc_code::decode_error,
                                               "kv create_index field has the wrong width",
                                               registry_reason_dto_truncated_field)};
            }
            out.create_index = endian::load_le64(tag.value.data());
            break;
        case registry_tag_modify_index:
            if (tag.value.size() != sizeof(u64)) {
                return status{registry_failure(rpc_code::decode_error,
                                               "kv modify_index field has the wrong width",
                                               registry_reason_dto_truncated_field)};
            }
            out.modify_index = endian::load_le64(tag.value.data());
            break;
        case registry_tag_value:
            out.value.assign(tag.value.begin(), tag.value.end());
            break;
        default:
            break;
        }
    }
    return status{};
}

status decode_kv_put_response(span<const std::byte> in, kv_put_response& out) {
    static const u8 kRequired[] = {registry_tag_index, registry_tag_modify_index};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(in, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }
    for (const registry_tag& tag : tags) {
        switch (tag.id) {
        case registry_tag_index:
            if (tag.value.size() != sizeof(u64)) {
                return status{registry_failure(rpc_code::decode_error,
                                               "kv index field has the wrong width",
                                               registry_reason_dto_truncated_field)};
            }
            out.index = endian::load_le64(tag.value.data());
            break;
        case registry_tag_modify_index:
            if (tag.value.size() != sizeof(u64)) {
                return status{registry_failure(rpc_code::decode_error,
                                               "kv modify_index field has the wrong width",
                                               registry_reason_dto_truncated_field)};
            }
            out.modify_index = endian::load_le64(tag.value.data());
            break;
        default:
            break;
        }
    }
    return status{};
}

status decode_kv_delete_response(span<const std::byte> in, kv_delete_response& out) {
    static const u8 kRequired[] = {registry_tag_index, registry_tag_found};
    std::vector<registry_tag> tags;
    const status walked = decode_registry_tags(in, tags);
    if (!walked.ok()) {
        return walked;
    }
    const status required = require_tags(tags, kRequired, std::size(kRequired));
    if (!required.ok()) {
        return required;
    }
    for (const registry_tag& tag : tags) {
        switch (tag.id) {
        case registry_tag_index:
            if (tag.value.size() != sizeof(u64)) {
                return status{registry_failure(rpc_code::decode_error,
                                               "kv index field has the wrong width",
                                               registry_reason_dto_truncated_field)};
            }
            out.index = endian::load_le64(tag.value.data());
            break;
        case registry_tag_found:
            if (tag.value.size() != sizeof(u64)) {
                return status{registry_failure(rpc_code::decode_error,
                                               "kv found field has the wrong width",
                                               registry_reason_dto_truncated_field)};
            }
            out.deleted = endian::load_le64(tag.value.data()) != 0;
            break;
        default:
            break;
        }
    }
    return status{};
}

status decode_meta_map(span<const std::byte> bytes, metadata_list& out) {
    out = metadata_list{};
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        if (bytes.size() - pos < 4) {
            return status{registry_failure(rpc_code::decode_error,
                                           "metadata block is truncated",
                                           registry_reason_dto_truncated_field)};
        }
        const u32 key_len = endian::load_le32(bytes.data() + pos);
        pos += 4;
        if (key_len > bytes.size() - pos) {
            return status{registry_failure(rpc_code::decode_error,
                                           "metadata key is truncated",
                                           registry_reason_dto_truncated_field)};
        }
        const std::string key(reinterpret_cast<const char*>(bytes.data()) + pos, key_len);
        pos += key_len;
        if (bytes.size() - pos < 4) {
            return status{registry_failure(rpc_code::decode_error,
                                           "metadata value length is truncated",
                                           registry_reason_dto_truncated_field)};
        }
        const u32 value_len = endian::load_le32(bytes.data() + pos);
        pos += 4;
        if (value_len > bytes.size() - pos) {
            return status{registry_failure(rpc_code::decode_error,
                                           "metadata value is truncated",
                                           registry_reason_dto_truncated_field)};
        }
        const std::string value(reinterpret_cast<const char*>(bytes.data()) + pos,
                                value_len);
        pos += value_len;
        // Fail closed on a duplicate key: two values for one key would make the
        // observed catalog depend on iteration order.
        if (out.find(key) != nullptr) {
            return status{registry_failure(rpc_code::decode_error,
                                           "duplicate metadata key",
                                           registry_reason_dto_duplicate_tag)};
        }
        out.set(std::move(key), std::move(value));
    }
    return status{};
}

status encode_meta_map(const metadata_list& meta, std::vector<std::byte>& out) {
    out.clear();
    for (const auto& entry : meta) {
        append_u32(out, static_cast<u32>(entry.first.size()));
        out.insert(out.end(), reinterpret_cast<const std::byte*>(entry.first.data()),
                   reinterpret_cast<const std::byte*>(entry.first.data()) +
                       entry.first.size());
        append_u32(out, static_cast<u32>(entry.second.size()));
        out.insert(out.end(), reinterpret_cast<const std::byte*>(entry.second.data()),
                   reinterpret_cast<const std::byte*>(entry.second.data()) +
                       entry.second.size());
    }
    return status{};
}

} // namespace cluster
} // namespace crpc
