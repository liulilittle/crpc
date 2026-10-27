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
    bson_document client;
    client.append("driver", bson_value::make_text("crpc"));
    client.append("application", bson_value::make_text(std::string(application_name)));
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

} // namespace mongo
} // namespace db
} // namespace crpc