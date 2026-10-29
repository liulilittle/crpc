// CRPC -- MongoDB connection implementation.
#include <crpc/db/mongo/connection.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iterator>
#include <limits>
#include <random>
#include <utility>

#include <crpc/crypto/base64.hpp>

namespace crpc {
namespace db {
namespace mongo {
namespace {

// The commands this client can write. Only those may carry `txnNumber`; adding one to
// a read or to `hello` would make the server refuse the command.
[[nodiscard]] bool is_write_command(std::string_view name) noexcept {
    return name == "insert" || name == "update" || name == "delete" ||
           name == "findAndModify";
}

// The commands this client can read. They are the ones that may carry $readPreference
// and readConcern; `writeConcern` is refused on them and the read fields are refused
// on writes by the strictest parsers.
[[nodiscard]] bool is_read_command(std::string_view name) noexcept {
    return name == "find" || name == "aggregate" || name == "getMore";
}

// A transport failure after the bytes were handed to the transport is unknown_side_effect
// (the server may have run the write); the other codes say the bytes never went out or
// the reply never came back. All of them make a retryable write safe to replay, because
// the txnNumber is the idempotency key.
[[nodiscard]] bool is_transport_error(rpc_code code) noexcept {
    switch (code) {
    case rpc_code::connection_refused:
    case rpc_code::connection_reset:
    case rpc_code::broken_pipe:
    case rpc_code::eof:
    case rpc_code::unavailable:
    case rpc_code::not_acknowledged:
    case rpc_code::unknown_side_effect:
        return true;
    default:
        return false;
    }
}

// Server codes whose command reply means "this node is not the right one right now",
// so the same write is worth one replay on a fresh link. The server deduplicates by
// lsid+txnNumber, so a replay cannot double the document.
[[nodiscard]] bool is_retryable_server_error(std::int32_t code) noexcept {
    switch (code) {
    case 6:      // host unreachable
    case 7:      // host not found
    case 89:     // network timeout
    case 91:     // shutdown in progress
    case 189:    // primary stepped down
    case 262:    // not master
    case 9001:   // socket exception
    case 10107:  // not writable primary
    case 11600:  // interrupted at shutdown
    case 11602:  // interrupted due to replica set change
    case 13435:  // not primary or secondary
    case 13436:  // not primary no secondary ok
        return true;
    default:
        return false;
    }
}

// The one place that decides where the extra command fields go: the command name stays
// the first field and `$db` stays the last, matching what the builders and the existing
// tests expect.
void insert_command_fields(bson_document& request,
                           std::vector<bson_document::field> fields) {
    if (fields.empty()) {
        return;
    }
    std::vector<bson_document::field>& all = request.fields();
    std::size_t at = all.size();
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].first == "$db") {
            at = i;
            break;
        }
    }
    all.insert(all.begin() + static_cast<std::ptrdiff_t>(at),
               std::make_move_iterator(fields.begin()),
               std::make_move_iterator(fields.end()));
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
    // The topology decides whether retryable writes are even legal later, so a hello
    // reply that names a malformed node list is a handshake failure, not a silent
    // half-discovered replica set.
    const status topology_parsed = parse_hello_topology(body, topology_);
    if (!topology_parsed) {
        return topology_parsed;
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

void connection::new_session_id() noexcept {
    // A session id must be a version-4 shape 16-byte UUID. The crux is the bytes, not a
    // specific bit layout the server checks, so 16 bytes of OS entropy in binary subtype
    // 4 is what the server accepts (it refuses subtype 0 with "wrong binData type
    // 'general', expected type 'UUID'").
    std::array<std::byte, 16> raw{};
    std::random_device device;
    for (std::byte& byte : raw) {
        byte = static_cast<std::byte>(device() & 0xffu);
    }
    session_id_.assign(raw.begin(), raw.end());
    txn_number_ = 0;
}

status connection::ensure_session() {
    if (!session_id_.empty()) {
        return status{};
    }
    new_session_id();
    return status{};
}

status connection::start_session() {
    if (!session_id_.empty()) {
        const status ended = end_session();
        if (!ended) {
            return ended;
        }
    }
    new_session_id();
    return status{};
}

status connection::end_session() {
    if (session_id_.empty()) {
        return status{};
    }
    std::vector<std::byte> ended_id;
    ended_id.swap(session_id_);
    txn_number_ = 0;
    if (!open_) {
        // The link is gone; the server expires the session on its own when it times out.
        return status{};
    }
    result<bson_document> reply = round_trip(make_end_sessions({ended_id}));
    if (!reply) {
        return reply.error();
    }
    if (!reply_ok(reply.value())) {
        return status{map_mongo_error(0), reply_error_message(reply.value())};
    }
    return status{};
}

status connection::reconnect() {
    if (!options_.reconnect) {
        return status{rpc_code::failed_precondition, "the connection has no reconnector"};
    }
    if (link_) {
        link_->close();
    }
    link_.reset();
    open_ = false;
    authenticated_ = false;
    result<std::unique_ptr<db::channel>> reopened = options_.reconnect();
    if (!reopened) {
        return reopened.error();
    }
    link_ = std::move(reopened.value());
    const status handshake = perform_handshake();
    if (!handshake) {
        return handshake;
    }
    const status auth = authenticate();
    if (!auth) {
        return auth;
    }
    return status{};
}

result<command_result> connection::run_command(const bson_document& command) {
    if (!open_) {
        return rpc_error{rpc_code::shutting_down, "the connection is closed"};
    }
    bson_document request = command;
    const std::string command_name =
        request.empty() ? std::string() : request.fields().front().first;
    const bool write = is_write_command(command_name);
    const bool send_txn =
        write && (options_.retryable_writes && topology_.replica_set);
    const bool auto_retry = send_txn && options_.reconnect;

    // Every application command runs inside a session. hello and authentication bypass
    // this path, so the session starts with the first real command.
    const status session_ready = ensure_session();
    if (!session_ready) {
        return session_ready.error();
    }

    // The command name is the first field and $db the last; everything else lands in
    // between.
    std::vector<bson_document::field> extra;
    if (is_read_command(command_name)) {
        if (options_.read_concern != read_concern_level::local) {
            extra.emplace_back("readConcern", bson_value::make_document(
                                                  make_read_concern_document(options_.read_concern)));
        }
        if (options_.read_preference != read_preference_mode::primary) {
            extra.emplace_back("$readPreference", bson_value::make_document(
                                                      make_read_preference_document(options_.read_preference)));
        }
    }
    if (write && options_.write_concern.configured()) {
        extra.emplace_back("writeConcern", bson_value::make_document(
                                               make_write_concern_document(options_.write_concern)));
    }
    if (!session_id_.empty()) {
        bson_document lsid;
        lsid.append("id", bson_value::make_binary(session_id_, 4));
        extra.emplace_back("lsid", bson_value::make_document(std::move(lsid)));
    }
    if (send_txn) {
        // One txnNumber per logical operation: increment here, then reuse the exact
        // value on the replay. Two different writes must never share a txnNumber or
        // the server's idempotency key would deduplicate them.
        const std::int64_t txn = ++txn_number_;
        extra.emplace_back("txnNumber", bson_value::make_int64(txn));
    }
    insert_command_fields(request, std::move(extra));
    // Every OP_MSG request must name a database, and the server refuses one without it:
    // "OP_MSG requests require a $db argument". Adding it here rather than in each builder
    // means a builder that forgets cannot produce a request the server will reject.
    if (request.find("$db") == nullptr) {
        request.append("$db", bson_value::make_text(options_.database));
    }

    std::uint32_t attempts = 1;
    result<bson_document> reply = round_trip(request);
    if (!reply && auto_retry && is_transport_error(reply.error().code)) {
        // The write may or may not have run. Replay it on a fresh link with the same
        // lsid and -- crucially -- the same txnNumber; the server deduplicates.
        const status reconnected = reconnect();
        if (reconnected) {
            // txnNumber must not advance on the replay.
            result<bson_document> replayed = round_trip(request);
            if (replayed) {
                attempts = 2;
                reply = std::move(replayed);
            } else {
                rpc_error error = replayed.error();
                error.attempts = 2;
                return error;
            }
        } else {
            rpc_error error = reply.error();
            error.attempts = 2;
            return error;
        }
    }
    if (!reply) {
        rpc_error error = reply.error();
        error.attempts = attempts;
        return error;
    }
    // A command reply that says "wrong node right now" is also worth one replay, but
    // only for a retryable write: the read path has no idempotency key to replay on.
    if (auto_retry && attempts == 1 && !reply_ok(reply.value())) {
        std::int64_t server_code = 0;
        (void)reply.value().get_int64("code", server_code);
        if (is_retryable_server_error(static_cast<std::int32_t>(server_code))) {
            const status reconnected = reconnect();
            if (reconnected) {
                result<bson_document> replayed = round_trip(request);
                if (replayed) {
                    attempts = 2;
                    reply = std::move(replayed);
                } else {
                    rpc_error error = replayed.error();
                    error.attempts = 2;
                    return error;
                }
            } else {
                rpc_error error = reply.error();
                error.attempts = 2;
                return error;
            }
        }
    }

    command_result out;
    out.retried = attempts == 2;
    out.ok = reply_ok(reply.value());
    if (!out.ok) {
        out.error_message = reply_error_message(reply.value());
    }
    // A write concern failure rides on a successful reply (ok stays 1); it is the
    // durability arm of the write, reported separately so a caller can decide whether
    // "w was not met" is the same as "the write did not happen".
    const bson_value* concern = reply.value().find("writeConcernError");
    if (concern != nullptr && concern->type == bson_type::document &&
        concern->document != nullptr) {
        out.has_write_concern_error = true;
        std::int64_t code = 0;
        (void)concern->document->get_int64("code", code);
        out.write_concern_code = static_cast<std::int32_t>(code);
        (void)concern->document->get_text("errmsg", out.write_concern_message);
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
        // The server's own code distinguishes a duplicate key from a bad value (such as
        // an illegal write concern `w`), so it is read rather than replaced.
        std::int64_t code = 0;
        (void)answered.value().reply.get_int64("code", code);
        return rpc_error{map_mongo_error(static_cast<std::int32_t>(code)),
                         answered.value().error_message};
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
    // than a document failure. run_command carries it out of band.
    if (answered.value().has_write_concern_error) {
        return rpc_error{map_mongo_error(answered.value().write_concern_code),
                         answered.value().write_concern_message.empty()
                             ? std::string("the write concern was not satisfied")
                             : answered.value().write_concern_message};
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