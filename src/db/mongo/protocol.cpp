// CRPC -- MongoDB wire protocol implementation.
#include <crpc/db/mongo/protocol.hpp>

#include <cstring>
#include <utility>

namespace crpc {
namespace db {
namespace mongo {
namespace {

void put_i32(std::vector<std::byte>& out, std::int32_t value) {
    const std::uint32_t bits = static_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((bits >> (8 * i)) & 0xffu));
    }
}

[[nodiscard]] std::int32_t read_i32(span<const std::byte> data, std::size_t at) noexcept {
    std::uint32_t bits = 0;
    for (int i = 0; i < 4; ++i) {
        bits |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[at + static_cast<std::size_t>(i)]))
                << (8 * i);
    }
    return static_cast<std::int32_t>(bits);
}

} // namespace

status encode_message_header(std::vector<std::byte>& out, const message_header& header) {
    put_i32(out, header.message_length);
    put_i32(out, header.request_id);
    put_i32(out, header.response_to);
    put_i32(out, header.code);
    return status{};
}

status decode_message_header(span<const std::byte> in, message_header& header) {
    if (in.size() < message_header_size) {
        return status{rpc_code::decode_error, "a MongoDB message is shorter than its header"};
    }
    header.message_length = read_i32(in, 0);
    header.request_id = read_i32(in, 4);
    header.response_to = read_i32(in, 8);
    header.code = read_i32(in, 12);
    if (header.message_length < static_cast<std::int32_t>(message_header_size)) {
        return status{rpc_code::decode_error, "a MongoDB message claims an impossible length"};
    }
    return status{};
}

result<std::vector<std::byte>> encode_op_msg(const bson_document& command,
                                             std::int32_t request_id, std::uint32_t flags) {
    result<std::vector<std::byte>> body = encode_bson(command);
    if (!body) {
        return body.error();
    }
    // Header (16) + flags (4) + section kind (1) + document.
    const std::size_t total = message_header_size + 4 + 1 + body.value().size();
    if (total > 0x7fffffffu) {
        return rpc_error{rpc_code::invalid_argument, "a MongoDB command exceeds 2 GiB"};
    }
    std::vector<std::byte> out;
    out.reserve(total);
    message_header header;
    header.message_length = static_cast<std::int32_t>(total);
    header.request_id = request_id;
    header.response_to = 0;
    header.code = static_cast<std::int32_t>(op_code::op_msg);
    (void)encode_message_header(out, header);
    put_i32(out, static_cast<std::int32_t>(flags));
    // Section kind 0: the command document follows inline.
    out.push_back(std::byte{0});
    out.insert(out.end(), body.value().begin(), body.value().end());
    return out;
}

status decode_op_msg(span<const std::byte> message, op_msg_reply& out) {
    out = op_msg_reply{};
    const status head = decode_message_header(message, out.header);
    if (!head) {
        return head;
    }
    const std::size_t declared = static_cast<std::size_t>(out.header.message_length);
    if (declared > message.size()) {
        return status{rpc_code::decode_error, "a MongoDB message runs past the buffer"};
    }
    if (!out.header.is_op_msg()) {
        return status{rpc_code::protocol_error,
                      "the server answered with an opcode this client does not read"};
    }
    if (declared < message_header_size + 4 + 5) {
        return status{rpc_code::decode_error, "an OP_MSG message is too short for a section"};
    }
    out.flags = static_cast<std::uint32_t>(read_i32(message, message_header_size));
    std::size_t offset = message_header_size + 4;
    bool saw_body = false;
    while (offset < declared) {
        const std::uint8_t kind = static_cast<std::uint8_t>(message[offset]);
        ++offset;
        if (kind == 0) {
            if (saw_body) {
                return status{rpc_code::protocol_error,
                              "an OP_MSG message carries more than one command document"};
            }
            bson_document body;
            std::size_t consumed = 0;
            const status decoded = decode_bson(message.subspan(offset), body, consumed);
            if (!decoded) {
                return decoded;
            }
            out.body = std::move(body);
            saw_body = true;
            offset += consumed;
            continue;
        }
        if (kind == 1) {
            // Document sequence: int32 size, cstring identifier, then documents. This
            // slice sends no sequences, but a server reply can contain one, and the
            // identifier is what a caller would use to find it.
            if (offset + 4 > declared) {
                return status{rpc_code::decode_error, "a document sequence has no length"};
            }
            const std::int32_t size = read_i32(message, offset);
            if (size < 5 || offset + static_cast<std::size_t>(size) > declared) {
                return status{rpc_code::decode_error,
                              "a document sequence length is impossible"};
            }
            offset += static_cast<std::size_t>(size);
            continue;
        }
        // An unknown section cannot be skipped safely: its length is not known, so
        // continuing would decode the rest as a different section.
        return status{rpc_code::protocol_error,
                      "an OP_MSG section kind this client does not understand"};
    }
    if (offset != declared) {
        return status{rpc_code::decode_error,
                      "an OP_MSG message length disagrees with its sections"};
    }
    if (!saw_body) {
        return status{rpc_code::protocol_error, "an OP_MSG message carries no command document"};
    }
    return status{};
}

bson_document make_hello(std::string_view application_name) {
    bson_document hello;
    hello.append("hello", bson_value::make_int32(1));
    // The client metadata document has a fixed shape, and `driver` is itself a document
    // with `name` and `version`. Sending it as a string is refused outright:
    // "The 'driver' field is required to be a BSON document in the client metadata
    // document".
    bson_document client;
    bson_document driver;
    driver.append("name", bson_value::make_text("crpc"));
    driver.append("version", bson_value::make_text("1.0"));
    client.append("driver", bson_value::make_document(std::move(driver)));
    // `os` is required as well, with `type` mandatory. The server validates the whole
    // metadata document, so omitting a required sub-document is refused outright.
    bson_document os;
    os.append("type", bson_value::make_text("Windows"));
    client.append("os", bson_value::make_document(std::move(os)));
    if (!application_name.empty()) {
        // `application` is also a document, and only `name` is required.
        bson_document application;
        application.append("name", bson_value::make_text(std::string(application_name)));
        client.append("application", bson_value::make_document(std::move(application)));
    }
    hello.append("client", bson_value::make_document(std::move(client)));
    // The server picks the highest version both sides support; this client speaks OP_MSG
    // and reads no newer feature, so it asks for the range it understands.
    hello.append("minWireVersion", bson_value::make_int32(0));
    hello.append("maxWireVersion", bson_value::make_int32(21));
    hello.append("$db", bson_value::make_text("admin"));
    return hello;
}

bson_document make_find(const find_request& request) {
    bson_document command;
    // The command name is the first field: the server reads it before anything else.
    command.append("find", bson_value::make_text(request.collection));
    command.append("filter", bson_value::make_document(request.filter));
    if (!request.projection.empty()) {
        command.append("projection", bson_value::make_document(request.projection));
    }
    if (request.limit != 0) {
        command.append("limit", bson_value::make_int64(request.limit));
    }
    if (request.skip != 0) {
        command.append("skip", bson_value::make_int64(request.skip));
    }
    if (request.batch_size > 0) {
        command.append("batchSize", bson_value::make_int32(request.batch_size));
    }
    command.append("$db", bson_value::make_text(request.database));
    return command;
}

bson_document make_aggregate(const aggregate_request& request) {
    bson_document command;
    command.append("aggregate", bson_value::make_text(request.collection));
    std::vector<bson_value> stages;
    stages.reserve(request.pipeline.size());
    for (const bson_document& stage : request.pipeline) {
        stages.push_back(bson_value::make_document(stage));
    }
    command.append("pipeline", bson_value::make_array(std::move(stages)));
    // A missing `cursor` is refused outright, so the sub-document is always sent; only
    // `batchSize` is conditional, because 0 means "let the server choose".
    bson_document cursor_options;
    if (request.batch_size > 0) {
        cursor_options.append("batchSize", bson_value::make_int32(request.batch_size));
    }
    command.append("cursor", bson_value::make_document(std::move(cursor_options)));
    command.append("$db", bson_value::make_text(request.database));
    return command;
}

bson_document make_insert(std::string_view collection, std::vector<bson_document> documents,
                          bool ordered) {
    bson_document command;
    command.append("insert", bson_value::make_text(std::string(collection)));
    command.append("ordered", bson_value::make_boolean(ordered));
    std::vector<bson_value> values;
    values.reserve(documents.size());
    for (bson_document& document : documents) {
        values.push_back(bson_value::make_document(std::move(document)));
    }
    command.append("documents", bson_value::make_array(std::move(values)));
    return command;
}

bson_document make_delete(std::string_view collection, bson_document filter,
                          std::int32_t limit) {
    bson_document statement;
    statement.append("q", bson_value::make_document(std::move(filter)));
    // 0 removes every match, 1 removes one; anything else is rejected by the server.
    statement.append("limit", bson_value::make_int32(limit));
    bson_document command;
    command.append("delete", bson_value::make_text(std::string(collection)));
    command.append("deletes", bson_value::make_array({bson_value::make_document(std::move(statement))}));
    return command;
}

bson_document make_get_more(std::string_view database, std::int64_t cursor_id,
                            std::string_view collection, std::int32_t batch_size) {
    bson_document command;
    command.append("getMore", bson_value::make_int64(cursor_id));
    command.append("collection", bson_value::make_text(std::string(collection)));
    if (batch_size > 0) {
        command.append("batchSize", bson_value::make_int32(batch_size));
    }
    command.append("$db", bson_value::make_text(std::string(database)));
    return command;
}

bson_document make_kill_cursors(std::string_view collection, std::int64_t cursor_id) {
    bson_document command;
    command.append("killCursors", bson_value::make_text(std::string(collection)));
    command.append("cursors", bson_value::make_array({bson_value::make_int64(cursor_id)}));
    return command;
}

bool reply_ok(const bson_document& reply) {
    // `ok` is a number: 1 (int or double) means success, 0 means failure. A reply
    // without `ok` is not a valid command reply.
    const bson_value* ok = reply.find("ok");
    if (ok == nullptr) {
        return false;
    }
    if (ok->type == bson_type::real) {
        return ok->real != 0.0;
    }
    if (ok->type == bson_type::int32 || ok->type == bson_type::int64) {
        return ok->integer != 0;
    }
    if (ok->type == bson_type::boolean) {
        return ok->boolean;
    }
    return false;
}

std::string reply_error_message(const bson_document& reply) {
    std::string message;
    if (reply.get_text("errmsg", message)) {
        return message;
    }
    // Older servers use `$err`.
    if (reply.get_text("$err", message)) {
        return message;
    }
    return "the server rejected the command without a message";
}

rpc_code map_mongo_error(std::int32_t code) noexcept {
    switch (code) {
    case 11000: // duplicate key
    case 11001:
        return rpc_code::failed_precondition;
    case 18:    // authentication failed
    case 8000:  // auth mechanism unavailable
        return rpc_code::failed_precondition;
    case 26:    // namespace not found
    case 27:    // index not found
        return rpc_code::not_found;
    case 13:    // unauthorized
        return rpc_code::failed_precondition;
    case 6:     // host unreachable
    case 7:     // host not found
    case 89:    // network timeout
    case 91:    // shutdown in progress
        return rpc_code::unavailable;
    case 50:    // max time expired
        return rpc_code::deadline_exceeded;
    case 2:     // bad value
    case 9:     // failed to parse
    case 14:    // type mismatch
        return rpc_code::invalid_argument;
    default:
        return rpc_code::application;
    }
}

const char* to_string(read_preference_mode mode) noexcept {
    switch (mode) {
    case read_preference_mode::primary:
        return "primary";
    case read_preference_mode::primary_preferred:
        return "primaryPreferred";
    case read_preference_mode::secondary:
        return "secondary";
    case read_preference_mode::secondary_preferred:
        return "secondaryPreferred";
    case read_preference_mode::nearest:
        return "nearest";
    }
    return "primary";
}

const char* to_string(read_concern_level level) noexcept {
    switch (level) {
    case read_concern_level::local:
        return "local";
    case read_concern_level::majority:
        return "majority";
    case read_concern_level::snapshot:
        return "snapshot";
    case read_concern_level::linearizable:
        return "linearizable";
    }
    return "local";
}

bool write_concern_options::configured() const noexcept {
    return has_w || journal || wtimeout_ms > 0;
}

write_concern_options write_concern_options::majority() {
    write_concern_options options;
    options.has_w = true;
    options.w_majority = true;
    return options;
}

write_concern_options write_concern_options::count(std::int32_t value) {
    write_concern_options options;
    options.has_w = true;
    options.w = value;
    return options;
}

bson_document make_read_preference_document(read_preference_mode mode) {
    bson_document document;
    document.append("mode", bson_value::make_text(to_string(mode)));
    return document;
}

bson_document make_read_concern_document(read_concern_level level) {
    bson_document document;
    document.append("level", bson_value::make_text(to_string(level)));
    return document;
}

bson_document make_write_concern_document(const write_concern_options& options) {
    bson_document document;
    if (options.has_w) {
        if (options.w_majority) {
            document.append("w", bson_value::make_text("majority"));
        } else {
            document.append("w", bson_value::make_int32(options.w));
        }
    }
    if (options.journal) {
        document.append("j", bson_value::make_boolean(true));
    }
    if (options.wtimeout_ms > 0) {
        document.append("wtimeout", bson_value::make_int32(options.wtimeout_ms));
    }
    return document;
}

// Reads a text-valued list into `members`. A list that is not an array of text is a
// malformed hello reply, and accepting it would leave the topology silently partial.
[[nodiscard]] status append_hosts(const bson_document& source, std::string_view key,
                                  const std::string& primary,
                                  std::vector<topology_member>& members,
                                  topology_role fallback_role) {
    if (source.find(key) == nullptr) {
        return status{};
    }
    const bson_value* value = source.find(key);
    if (value == nullptr || value->type != bson_type::array) {
        return status{rpc_code::protocol_error,
                      "a hello reply names " + std::string(key) + " as a non-array"};
    }
    for (const bson_value& host : value->array) {
        if (host.type != bson_type::text) {
            return status{rpc_code::protocol_error,
                          "a hello reply lists a non-text host in " + std::string(key)};
        }
        topology_member member;
        member.host = host.text;
        // `hosts` never carries arbiters, so the only two roles present are the primary
        // and secondaries; the primary is the host named by the `primary` field.
        member.role = member.host == primary ? topology_role::primary : fallback_role;
        members.push_back(std::move(member));
    }
    return status{};
}

status parse_hello_topology(const bson_document& hello_reply, replica_set_topology& out) {
    out = replica_set_topology{};
    std::string set_name;
    if (hello_reply.get_text("setName", set_name)) {
        out.replica_set = true;
        out.set_name = std::move(set_name);
    }
    if (!hello_reply.get_boolean("isWritablePrimary", out.writable_primary)) {
        // Servers before the rename reported the same fact as `ismaster`.
        (void)hello_reply.get_boolean("ismaster", out.writable_primary);
    }
    (void)hello_reply.get_boolean("secondary", out.secondary);
    (void)hello_reply.get_boolean("arbiterOnly", out.arbiter);
    (void)hello_reply.get_text("me", out.me);
    (void)hello_reply.get_text("primary", out.primary);
    const status hosts = append_hosts(hello_reply, "hosts", out.primary, out.members,
                                      topology_role::secondary);
    if (!hosts) {
        return hosts;
    }
    // Hidden members report themselves via `passives`, and are secondaries too.
    const status passives = append_hosts(hello_reply, "passives", out.primary, out.members,
                                         topology_role::secondary);
    if (!passives) {
        return passives;
    }
    // Arbiters are not data-bearing and are listed separately.
    const status arbiters = append_hosts(hello_reply, "arbiters", std::string{},
                                         out.members, topology_role::arbiter);
    if (!arbiters) {
        return arbiters;
    }
    return status{};
}

topology_role replica_set_topology::self_role() const noexcept {
    if (arbiter) {
        return topology_role::arbiter;
    }
    if (writable_primary) {
        return topology_role::primary;
    }
    if (secondary) {
        return topology_role::secondary;
    }
    return topology_role::unknown;
}

bson_document make_end_sessions(const std::vector<std::vector<std::byte>>& session_ids) {
    std::vector<bson_value> sessions;
    sessions.reserve(session_ids.size());
    for (const std::vector<std::byte>& id : session_ids) {
        bson_document entry;
        entry.append("id", bson_value::make_binary(id, 4));
        sessions.push_back(bson_value::make_document(std::move(entry)));
    }
    bson_document command;
    command.append("endSessions", bson_value::make_array(std::move(sessions)));
    command.append("$db", bson_value::make_text("admin"));
    return command;
}

} // namespace mongo
} // namespace db
} // namespace crpc