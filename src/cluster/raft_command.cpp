// CRPC -- G19 step4 PR-9: raft_command codec.
//
// Explicit little-endian, no struct memcpy, no buffer reinterpretation -- the
// same discipline registry_wire.cpp follows, so the encoding is byte-identical
// on every host and two nodes cannot disagree about what a command says.
#include <crpc/cluster/raft_command.hpp>

#include <cstdint>
#include <cstring>

#include <crpc/cluster/registry_wire.hpp>
#include <crpc/endian.hpp>

namespace crpc {
namespace cluster {
namespace {

// The frame is: u16 version, u8 kind, then kind-specific fields. A version tag
// makes a later field addition a capability refusal rather than a misparse.
constexpr u16 kCommandVersion = 1;
constexpr std::size_t kPrefixBytes = 3;  // u16 version + u8 kind

void put_u8(std::vector<std::byte>& out, u8 value) {
    out.push_back(static_cast<std::byte>(value));
}

void put_u16(std::vector<std::byte>& out, u16 value) {
    const std::size_t at = out.size();
    out.resize(at + 2);
    endian::store_le16(out.data() + at, value);
}

void put_u32(std::vector<std::byte>& out, u32 value) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    endian::store_le32(out.data() + at, value);
}

void put_u64(std::vector<std::byte>& out, u64 value) {
    const std::size_t at = out.size();
    out.resize(at + 8);
    endian::store_le64(out.data() + at, value);
}

// A u32-length-prefixed byte string.
void put_bytes(std::vector<std::byte>& out, span<const std::byte> value) {
    put_u32(out, static_cast<u32>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void put_string(std::vector<std::byte>& out, const std::string& value) {
    put_u32(out, static_cast<u32>(value.size()));
    out.insert(out.end(),
               reinterpret_cast<const std::byte*>(value.data()),
               reinterpret_cast<const std::byte*>(value.data()) + value.size());
}

// A reader that refuses to run past the end of its buffer. Every read is
// bounds-checked; there is no "trust the length prefix" path.
class reader {
public:
    explicit reader(span<const std::byte> bytes) : bytes_(bytes) {}

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

    [[nodiscard]] bool bytes(std::vector<std::byte>& out) {
        crpc::u32 len = 0;
        if (!u32(len)) {
            return false;
        }
        if (len > bytes_.size()) {
            return false;
        }
        out.assign(bytes_.begin(), bytes_.begin() + len);
        bytes_ = bytes_.subspan(len);
        return true;
    }

    [[nodiscard]] bool string(std::string& out) {
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

private:
    span<const std::byte> bytes_;
};

} // namespace

const char* to_string(raft_command_kind kind) noexcept {
    switch (kind) {
    case raft_command_kind::kv_put:
        return "kv_put";
    case raft_command_kind::kv_cas:
        return "kv_cas";
    case raft_command_kind::kv_delete:
        return "kv_delete";
    case raft_command_kind::acl_upsert:
        return "acl_upsert";
    case raft_command_kind::acl_remove:
        return "acl_remove";
    }
    return "unknown";
}

status encode_raft_command(const raft_command& value,
                           std::vector<std::byte>& out) {
    std::vector<std::byte> body;
    put_u16(body, kCommandVersion);
    put_u8(body, static_cast<u8>(value.kind));

    switch (value.kind) {
    case raft_command_kind::kv_put:
    case raft_command_kind::kv_cas:
    case raft_command_kind::kv_delete:
        put_string(body, value.key);
        if (value.kind != raft_command_kind::kv_delete) {
            put_bytes(body, span<const std::byte>(value.value.data(),
                                                  value.value.size()));
        }
        if (value.kind == raft_command_kind::kv_cas) {
            put_u64(body, value.cas);
        }
        break;
    case raft_command_kind::acl_upsert:
        put_u64(body, value.rule.token_hash);
        put_string(body, value.rule.principal_id);
        put_string(body, value.rule.node_id);
        put_u8(body, value.rule.admin ? 1 : 0);
        put_u32(body, static_cast<u32>(value.rule.read_prefixes.size()));
        for (const std::string& prefix : value.rule.read_prefixes) {
            put_string(body, prefix);
        }
        put_u32(body, static_cast<u32>(value.rule.write_prefixes.size()));
        for (const std::string& prefix : value.rule.write_prefixes) {
            put_string(body, prefix);
        }
        break;
    case raft_command_kind::acl_remove:
        put_u64(body, value.rule.token_hash);
        break;
    default:
        return status{registry_failure(rpc_code::invalid_argument,
                                       "raft command kind is out of range",
                                       "command_kind")};
    }

    out.clear();
    out.insert(out.end(), body.begin(), body.end());
    return status{};
}

status decode_raft_command(span<const std::byte> in, raft_command& out) {
    reader input(in);
    u16 version = 0;
    if (!input.u16(version)) {
        return status{registry_failure(rpc_code::decode_error,
                                       "raft command is shorter than its prefix",
                                       registry_reason_dto_length_mismatch)};
    }
    if (version != kCommandVersion) {
        return status{registry_failure(
            rpc_code::failed_precondition, "unsupported raft command version",
            registry_reason_dto_unsupported_version)};
    }
    u8 raw_kind = 0;
    if (!input.u8(raw_kind)) {
        return status{registry_failure(rpc_code::decode_error,
                                       "raft command has no kind",
                                       registry_reason_dto_truncated_field)};
    }

    raft_command decoded;
    switch (static_cast<raft_command_kind>(raw_kind)) {
    case raft_command_kind::kv_put:
    case raft_command_kind::kv_cas:
    case raft_command_kind::kv_delete: {
        decoded.kind = static_cast<raft_command_kind>(raw_kind);
        if (!input.string(decoded.key)) {
            return status{registry_failure(rpc_code::decode_error,
                                           "raft command key is truncated",
                                           registry_reason_dto_truncated_field)};
        }
        if (decoded.kind != raft_command_kind::kv_delete) {
            if (!input.bytes(decoded.value)) {
                return status{registry_failure(
                    rpc_code::decode_error, "raft command value is truncated",
                    registry_reason_dto_truncated_field)};
            }
        }
        if (decoded.kind == raft_command_kind::kv_cas) {
            if (!input.u64(decoded.cas)) {
                return status{registry_failure(
                    rpc_code::decode_error, "raft command cas is truncated",
                    registry_reason_dto_truncated_field)};
            }
        }
        break;
    }
    case raft_command_kind::acl_upsert: {
        decoded.kind = raft_command_kind::acl_upsert;
        if (!input.u64(decoded.rule.token_hash)) {
            return status{registry_failure(
                rpc_code::decode_error, "raft acl rule token is truncated",
                registry_reason_dto_truncated_field)};
        }
        if (!input.string(decoded.rule.principal_id) ||
            !input.string(decoded.rule.node_id)) {
            return status{registry_failure(
                rpc_code::decode_error, "raft acl rule identity is truncated",
                registry_reason_dto_truncated_field)};
        }
        u8 admin = 0;
        if (!input.u8(admin)) {
            return status{registry_failure(
                rpc_code::decode_error, "raft acl rule admin flag is truncated",
                registry_reason_dto_truncated_field)};
        }
        decoded.rule.admin = admin != 0;
        u32 read_count = 0;
        if (!input.u32(read_count)) {
            return status{registry_failure(
                rpc_code::decode_error, "raft acl read prefix count is truncated",
                registry_reason_dto_truncated_field)};
        }
        for (u32 i = 0; i < read_count; ++i) {
            std::string prefix;
            if (!input.string(prefix)) {
                return status{registry_failure(
                    rpc_code::decode_error,
                    "raft acl read prefix is truncated",
                    registry_reason_dto_truncated_field)};
            }
            decoded.rule.read_prefixes.push_back(std::move(prefix));
        }
        u32 write_count = 0;
        if (!input.u32(write_count)) {
            return status{registry_failure(
                rpc_code::decode_error,
                "raft acl write prefix count is truncated",
                registry_reason_dto_truncated_field)};
        }
        for (u32 i = 0; i < write_count; ++i) {
            std::string prefix;
            if (!input.string(prefix)) {
                return status{registry_failure(
                    rpc_code::decode_error,
                    "raft acl write prefix is truncated",
                    registry_reason_dto_truncated_field)};
            }
            decoded.rule.write_prefixes.push_back(std::move(prefix));
        }
        break;
    }
    case raft_command_kind::acl_remove: {
        decoded.kind = raft_command_kind::acl_remove;
        if (!input.u64(decoded.rule.token_hash)) {
            return status{registry_failure(
                rpc_code::decode_error, "raft acl remove token is truncated",
                registry_reason_dto_truncated_field)};
        }
        break;
    }
    default:
        return status{registry_failure(rpc_code::decode_error,
                                       "raft command kind is unknown",
                                       registry_reason_dto_unknown_tag)};
    }

    // Trailing bytes are a shape error: a command this layer does not
    // understand must not be applied on the assumption that the extra bytes
    // were harmless.
    if (!input.at_end()) {
        return status{registry_failure(rpc_code::decode_error,
                                       "raft command has trailing bytes",
                                       registry_reason_dto_length_mismatch)};
    }
    out = std::move(decoded);
    return status{};
}

} // namespace cluster
} // namespace crpc
