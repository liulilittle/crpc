// CRPC -- MongoDB connection implementation.
#include <crpc/db/mongo/connection.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

#include <crpc/crypto/base64.hpp>

namespace crpc {
namespace db {
namespace mongo {
namespace {

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

// A BSON binary value carrying a UTF-8 SASL payload, which is what saslStart/saslContinue
// take. The subtype is 0 ("generic").
[[nodiscard]] bson_value sasl_payload(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bson_value::make_binary(std::move(bytes), 0);
}

// Reads a BSON binary value as text. A SASL payload is opaque bytes, but every server
// sends ASCII attributes, so a strict copy is safe and keeps the NUL out.
[[nodiscard]] result<std::string> binary_text(const bson_value& value) {
    if (value.type != bson_type::binary) {
        return rpc_error{rpc_code::protocol_error, "a SASL payload is not binary"};
    }
    std::string out(value.bytes.size(), '\0');
    if (!value.bytes.empty()) {
        std::memcpy(out.data(), value.bytes.data(), value.bytes.size());
    }
    return out;
}

// `find` and `aggregate` answer with the same cursor document { id, ns, firstBatch }. The
// collection is kept on the cursor because getMore and killCursors have to name it.
[[nodiscard]] result<cursor> first_batch(const bson_document& reply, std::string collection,
                                         std::string_view command_name) {
    const bson_document* cursor_doc = reply.get_document("cursor");
    if (cursor_doc == nullptr) {
        return rpc_error{rpc_code::protocol_error, "a " + std::string(command_name) +
                                                       " reply carries no cursor document"};
    }
    cursor out;
    out.collection = std::move(collection);
    std::int64_t id = 0;
    if (cursor_doc->get_int64("id", id)) {
        out.id = id;
    }
    const bson_value* batch = cursor_doc->find("firstBatch");
    if (batch != nullptr) {
        for (const bson_value& item : batch->array) {
            if (item.document) {
                out.batch.push_back(*item.document);
            }
        }
    }
    return out;
}

} // namespace

connection::connection(connection_options options, std::unique_ptr<db::channel> link)
    : options_(std::move(options)), link_(std::move(link)) {
    const status opened = perform_handshake();
    if (!opened) {
        connect_error_ = opened.error();
        fail(opened);
        return;
    }
    // Authentication is a separate phase: a server that allows anonymous access answers
    // the handshake and then rejects the first command, so failing here is what turns a
    // silent later failure into a connection error the caller can act on.
    const status authenticated = authenticate();
    if (!authenticated) {
        connect_error_ = authenticated.error();
        fail(authenticated);
    }
}

connection::~connection() { close(); }

void connection::fail(const status& error) noexcept {
    open_ = false;
    if (link_) {
        link_->close();
    }
    (void)error;
}

status connection::send_message(span<const std::byte> message) {
    if (!link_) {
        return status{rpc_code::shutting_down, "the connection has no channel"};
    }
    return link_->write(message);
}

result<std::vector<std::byte>> connection::read_message() {
    // The header is fixed, but its length field is what says how much follows, so the
    // body cannot be read until the header has been parsed.
    result<std::vector<std::byte>> head = link_->read_exact(message_header_size);
    if (!head) {
        return head.error();
    }
    message_header header;
    const status decoded = decode_message_header(
        span<const std::byte>(head.value().data(), head.value().size()), header);
    if (!decoded) {
        return decoded.error();
    }
    const std::size_t total = static_cast<std::size_t>(header.message_length);
    if (total > options_.max_message_size) {
        return rpc_error{rpc_code::resource_exhausted,
                         "a MongoDB message exceeds max_message_size"};
    }
    std::vector<std::byte> message = std::move(head.value());
    if (total > message_header_size) {
        result<std::vector<std::byte>> rest = link_->read_exact(total - message_header_size);
        if (!rest) {
            return rest.error();
        }
        message.insert(message.end(), rest.value().begin(), rest.value().end());
    }
    return message;
}

result<bson_document> connection::round_trip(const bson_document& command) {
    const std::int32_t request_id = next_request_id_;
    // The counter wraps at the top of the range rather than overflowing into a negative
    // id, which some servers reject.
    next_request_id_ = request_id >= std::numeric_limits<std::int32_t>::max() ? 1
                                                                            : request_id + 1;
    result<std::vector<std::byte>> encoded = encode_op_msg(command, request_id);
    if (!encoded) {
        return encoded.error();
    }
    const status sent =
        send_message(span<const std::byte>(encoded.value().data(), encoded.value().size()));
    if (!sent) {
        fail(sent);
        return sent.error();
    }

    for (;;) {
        result<std::vector<std::byte>> message = read_message();
        if (!message) {
            fail(message.error());
            return message.error();
        }
        op_msg_reply reply;
        const status decoded = decode_op_msg(
            span<const std::byte>(message.value().data(), message.value().size()), reply);
        if (!decoded) {
            fail(decoded);
            return decoded.error();
        }
        // A reply that does not answer this request belongs to another conversation on the
        // same link, which cannot happen without the client multiplexing; accepting it
        // would attribute another command's result to this one.
        if (reply.header.response_to != request_id) {
            const status mismatch{rpc_code::protocol_error,
                                  "a MongoDB reply answers a different request"};
            fail(mismatch);
            return mismatch.error();
        }
        if (reply.more_to_come()) {
            // The server will keep sending without being asked. Nothing in this client
            // requests that, so the remaining messages are drained and the last one is
            // used as the answer, rather than leaving them to be read as the next reply.
            continue;
        }
        return reply.body;
    }
}

status connection::perform_handshake() {
    if (!link_) {
        return status{rpc_code::failed_precondition, "the connection has no channel"};
    }
    // `hello` is the modern handshake; it reports maxWireVersion, which decides whether
    // OP_MSG and the newer authentication mechanisms are available at all.
    result<bson_document> reply = round_trip(make_hello(options_.application_name));
    if (!reply) {
        return reply.error();
    }
    const bson_document& body = reply.value();
    if (!reply_ok(body)) {
        return status{map_mongo_error(0), reply_error_message(body)};
    }
    std::int64_t value = 0;
    if (body.get_int64("maxWireVersion", value)) {
        max_wire_version_ = static_cast<std::int32_t>(value);
    }
    if (body.get_int64("maxBsonObjectSize", value)) {
        max_bson_size_ = static_cast<std::int32_t>(value);
    }
    std::string version;
    if (body.get_text("version", version)) {
        server_version_ = std::move(version);
    }
    if (max_wire_version_ < 6) {
        // OP_MSG arrived in wire version 6; a server older than that cannot be talked to
        // by this client at all, and saying so is better than failing on the first query.
        return status{rpc_code::failed_precondition,
                      "the server is too old for OP_MSG; it reports maxWireVersion " +
                          std::to_string(max_wire_version_)};
    }
    open_ = true;
    return status{};
}

status connection::authenticate() {
    if (options_.user.empty()) {
        // No credentials means the server must allow anonymous access; if it does not,
        // the first real command fails and reports the server's own message.
        return status{};
    }
    scram_client client(options_.user, options_.password, options_.mechanism);
    mechanism_used_ = to_string(options_.mechanism);

    bson_document start;
    start.append("saslStart", bson_value::make_int32(1));
    start.append("mechanism", bson_value::make_text(to_string(options_.mechanism)));
    start.append("payload", sasl_payload(client.client_first()));
    start.append("autoAuthorize", bson_value::make_int32(1));
    start.append("$db", bson_value::make_text(options_.auth_database));

    result<bson_document> first = round_trip(start);
    if (!first) {
        return first.error();
    }
    if (!reply_ok(first.value())) {
        return status{map_mongo_error(0), reply_error_message(first.value())};
    }
    // The conversation id is echoed back exactly as received. The server is strict about
    // its type -- sending int64 where it sent int32 is refused with "is the wrong type
    // 'long', expected type 'int'" -- so converting it would be a self-inflicted failure.
    const bson_value* conversation = first.value().find("conversationId");
    if (conversation == nullptr ||
        (conversation->type != bson_type::int32 && conversation->type != bson_type::int64)) {
        return status{rpc_code::protocol_error, "a saslStart reply has no conversationId"};
    }
    const bson_value conversation_id = *conversation;
    const bson_value* payload = first.value().find("payload");
    if (payload == nullptr) {
        return status{rpc_code::protocol_error, "a saslStart reply has no payload"};
    }
    result<std::string> server_first = binary_text(*payload);
    if (!server_first) {
        return server_first.error();
    }
    result<std::string> client_final = client.on_server_first(server_first.value());
    if (!client_final) {
        return client_final.error();
    }

    bson_document cont;
    cont.append("saslContinue", bson_value::make_int32(1));
    cont.append("conversationId", conversation_id);
    cont.append("payload", sasl_payload(client_final.value()));
    cont.append("$db", bson_value::make_text(options_.auth_database));

    result<bson_document> second = round_trip(cont);
    if (!second) {
        return second.error();
    }
    if (!reply_ok(second.value())) {
        return status{map_mongo_error(0), reply_error_message(second.value())};
    }
    const bson_value* final_payload = second.value().find("payload");
    if (final_payload != nullptr) {
        result<std::string> server_final = binary_text(*final_payload);
        if (!server_final) {
            return server_final.error();
        }
        // Verifying the server signature is what makes this mutual authentication: a
        // client that skips it will hand a proof to anyone who asks for one.
        if (!server_final.value().empty()) {
            const status verified = client.on_server_final(server_final.value());
            if (!verified) {
                return verified;
            }
        }
    }
    // A server may need one more empty saslContinue to mark the conversation done. The
    // `done` flag says whether it does.
    bool done = false;
    // A reply without `done` leaves it false, which means one more empty saslContinue is
    // expected; the value is read rather than assumed.
    (void)second.value().get_boolean("done", done);
    if (!done) {
        bson_document finish;
        finish.append("saslContinue", bson_value::make_int32(1));
        finish.append("conversationId", conversation_id);
        finish.append("payload", sasl_payload(std::string_view{}));
        finish.append("$db", bson_value::make_text(options_.auth_database));
        result<bson_document> third = round_trip(finish);
        if (!third) {
            return third.error();
        }
        if (!reply_ok(third.value())) {
            return status{map_mongo_error(0), reply_error_message(third.value())};
        }
    }
    authenticated_ = true;
    return status{};
}

result<command_result> connection::run_command(const bson_document& command) {
    if (!open_) {
        return rpc_error{rpc_code::shutting_down, "the connection is closed"};
    }
    // Every OP_MSG request must name a database, and the server refuses one without it:
    // "OP_MSG requests require a $db argument". Adding it here rather than in each builder
    // means a builder that forgets cannot produce a request the server will reject.
    bson_document request = command;
    if (request.find("$db") == nullptr) {
        request.append("$db", bson_value::make_text(options_.database));
    }
    result<bson_document> reply = round_trip(request);
    if (!reply) {
        return reply.error();
    }
    command_result out;
    out.ok = reply_ok(reply.value());
    if (!out.ok) {
        out.error_message = reply_error_message(reply.value());
    }
    out.reply = std::move(reply.value());
    return out;
}

result<cursor> connection::find(const find_request& request) {
    result<command_result> answered = run_command(make_find(request));
    if (!answered) {
        return answered.error();
    }
    if (!answered.value().ok) {
        return rpc_error{rpc_code::not_found, answered.value().error_message};
    }
    return first_batch(answered.value().reply, request.collection, "find");
}

result<cursor> connection::aggregate(const aggregate_request& request) {
    result<command_result> answered = run_command(make_aggregate(request));
    if (!answered) {
        return answered.error();
    }
    if (!answered.value().ok) {
        // The server's own code is what distinguishes a bad pipeline from a missing
        // collection, so it is read rather than replaced with a generic failure.
        std::int64_t code = 0;
        (void)answered.value().reply.get_int64("code", code);
        return rpc_error{map_mongo_error(static_cast<std::int32_t>(code)),
                         answered.value().error_message};
    }
    return first_batch(answered.value().reply, request.collection, "aggregate");
}

result<bool> connection::next_batch(cursor& state) {
    if (state.id == 0) {
        return false;
    }
    result<command_result> answered = run_command(
        make_get_more(options_.database, state.id, state.collection));
    if (!answered) {
        return answered.error();
    }
    if (!answered.value().ok) {
        return rpc_error{map_mongo_error(0), answered.value().error_message};
    }
    const bson_document* cursor_doc = answered.value().reply.get_document("cursor");
    if (cursor_doc == nullptr) {
        return rpc_error{rpc_code::protocol_error, "a getMore reply carries no cursor document"};
    }
    std::int64_t id = 0;
    if (cursor_doc->get_int64("id", id)) {
        state.id = id;
    }
    state.batch.clear();
    state.position = 0;
    const bson_value* batch = cursor_doc->find("nextBatch");
    if (batch != nullptr) {
        for (const bson_value& item : batch->array) {
            if (item.document) {
                state.batch.push_back(*item.document);
            }
        }
    }
    // An exhausted cursor is released by the server itself (id 0), so nothing more is
    // sent; reporting false here is what stops the caller's loop.
    return !state.batch.empty();
}

status connection::close_cursor(cursor& state) {
    if (state.id == 0) {
        return status{};
    }
    const std::int64_t id = state.id;
    state.id = 0;
    result<command_result> answered =
        run_command(make_kill_cursors(state.collection, id));
    if (!answered) {
        return answered.error();
    }
    if (!answered.value().ok) {
        return status{map_mongo_error(0), answered.value().error_message};
    }
    return status{};
}

result<std::int64_t> connection::insert(std::string_view collection,
                                        std::vector<bson_document> documents, bool ordered) {
    if (documents.empty()) {
        return std::int64_t{0};
    }
    // One document larger than the server's limit is refused here rather than after a
    // round trip that is guaranteed to fail.
    for (const bson_document& document : documents) {
        if (document.encoded_size() > static_cast<std::size_t>(max_bson_size_)) {
            return rpc_error{rpc_code::invalid_argument,
                             "a document exceeds the server's maxBsonObjectSize"};
        }
    }
    result<command_result> answered =
        run_command(make_insert(collection, std::move(documents), ordered));
    if (!answered) {
        return answered.error();
    }
    if (!answered.value().ok) {
        return rpc_error{map_mongo_error(0), answered.value().error_message};
    }
    // A write command reports a failed document in `writeErrors` while `ok` stays 1: the
    // command succeeded, the write did not. Checking only `ok` would report a duplicate key
    // as a successful insert, which is the kind of silent data loss a caller cannot see.
    const bson_value* write_errors = answered.value().reply.find("writeErrors");
    if (write_errors != nullptr && !write_errors->array.empty()) {
        const bson_value& first = write_errors->array.front();
        std::int64_t code = 0;
        std::string message;
        if (first.document != nullptr) {
            (void)first.document->get_int64("code", code);
            (void)first.document->get_text("errmsg", message);
        }
        if (message.empty()) {
            message = "the server rejected a document without a message";
        }
        return rpc_error{map_mongo_error(static_cast<std::int32_t>(code)), message};
    }
    // The same applies to a write concern failure, which is a durability failure rather
    // than a document failure.
    const bson_value* concern = answered.value().reply.find("writeConcernError");
    if (concern != nullptr && concern->type == bson_type::document &&
        concern->document != nullptr) {
        std::int64_t code = 0;
        std::string message;
        (void)concern->document->get_int64("code", code);
        (void)concern->document->get_text("errmsg", message);
        return rpc_error{map_mongo_error(static_cast<std::int32_t>(code)),
                         message.empty() ? std::string("the write concern was not satisfied")
                                         : message};
    }
    std::int64_t count = 0;
    if (!answered.value().reply.get_int64("n", count)) {
        return rpc_error{rpc_code::protocol_error, "an insert reply carries no count"};
    }
    return count;
}

status connection::ping() {
    bson_document command;
    command.append("ping", bson_value::make_int32(1));
    command.append("$db", bson_value::make_text(options_.database));
    result<command_result> answered = run_command(command);
    if (!answered) {
        return answered.error();
    }
    if (!answered.value().ok) {
        return status{map_mongo_error(0), answered.value().error_message};
    }
    return status{};
}

status connection::begin() {
    // MongoDB has no multi-statement transaction over this interface; sessions and
    // transactions exist but need a session id this client does not create.
    return status{rpc_code::no_such_method,
                  "MongoDB transactions need a session; this client does not create one"};
}

status connection::commit() { return begin(); }

status connection::rollback() { return begin(); }

status connection::query(std::string_view sql, span<const parameter> params, result_set** out) {
    (void)sql;
    (void)params;
    if (out != nullptr) {
        *out = nullptr;
    }
    // A MongoDB query is a command document, not text. Accepting a string here would mean
    // guessing which command was meant.
    return status{rpc_code::no_such_method,
                  "MongoDB takes command documents; use find() or run_command()"};
}

void connection::close() noexcept {
    open_ = false;
    if (link_) {
        link_->close();
    }
}

bool connection::is_open() const noexcept { return open_; }

// --- factories ---------------------------------------------------------------

channel_factory::channel_factory(connection_options options, opener open)
    : options_(std::move(options)), open_(std::move(open)) {}

channel_factory::~channel_factory() = default;

result<db::connection*> channel_factory::open() {
    if (!open_) {
        return rpc_error{rpc_code::failed_precondition, "the channel factory has no opener"};
    }
    result<std::unique_ptr<db::channel>> opened = open_();
    if (!opened) {
        return opened.error();
    }
    auto session = std::unique_ptr<connection>(
        new connection(options_, std::move(opened.value())));
    if (!session->is_open()) {
        rpc_error error = session->connect_error();
        if (error.code == rpc_code::ok) {
            error = rpc_error{rpc_code::failed_precondition, "the MongoDB handshake failed"};
        }
        return error;
    }
    return static_cast<db::connection*>(session.release());
}

std::string_view channel_factory::name() const noexcept { return "mongo-injected"; }

connection_factory::connection_factory(connection_options options, crpc::backend_factory& backend,
                                       crpc::endpoint endpoint, crpc::runtime* shard)
    : options_(std::move(options)), backend_(&backend), endpoint_(std::move(endpoint)),
      shard_(shard) {}

connection_factory::~connection_factory() = default;

result<db::connection*> connection_factory::open() {
    crpc::net::connection_config config;
    config.ep = endpoint_;
    // The handshake plus SCRAM is several round trips; the transport's operation timeout
    // bounds each one.
    config.operation_timeout = std::chrono::seconds(15);

    auto session = std::unique_ptr<connection>(new connection(
        options_, std::unique_ptr<db::channel>(
                      new db::socket_channel(std::move(config), *backend_, shard_))));
    if (!session->is_open()) {
        rpc_error error = session->connect_error();
        if (error.code == rpc_code::ok) {
            error = rpc_error{rpc_code::failed_precondition, "the MongoDB handshake failed"};
        }
        return error;
    }
    return static_cast<db::connection*>(session.release());
}

std::string_view connection_factory::name() const noexcept { return "mongo"; }

} // namespace mongo
} // namespace db
} // namespace crpc