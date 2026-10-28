// CRPC -- MongoDB wire protocol: OP_MSG framing and the command documents.
//
// MongoDB's wire protocol is a flat header followed by either a legacy opcode payload or
// an OP_MSG body. This slice speaks OP_MSG only (opcode 2013), which is what every
// server since 3.6 requires and the only one a new driver needs:
//
//   int32  messageLength   total bytes, including this header
//   int32  requestID       chosen by the client
//   int32  responseTo      the requestID being answered (0 in a request)
//   int32  opCode          2013
//   uint32 flagBits
//   sections               each: kind byte, then kind-specific bytes
//   [uint32 checksum]      only when flagBits says so
//
// The section that matters is kind 0: a single BSON document holding the command. The
// command's *first* field is its name (`find`, `insert`, `hello`, ...), which is why the
// document type keeps field order.
#ifndef CRPC_DB_MONGO_PROTOCOL_HPP
#define CRPC_DB_MONGO_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/mongo/bson.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace mongo {

// The opcodes this client sends or accepts. `op_msg` is the only request form.
enum class op_code : std::int32_t {
    reply = 1,
    update = 2001,
    insert = 2002,
    query = 2004,
    get_more = 2005,
    delete_ = 2006,
    kill_cursors = 2007,
    compress = 2012,
    op_msg = 2013
};

// OP_MSG flag bits. Only the ones that change what the client must do are named.
enum class message_flag : std::uint32_t {
    none = 0,
    checksum_present = 1u << 0,
    // The server answers with `moreToCome` when it will keep sending without a reply;
    // a client that ignores the bit waits for a response that never comes.
    more_to_come = 1u << 1,
    exhaust_allowed = 1u << 16
};

inline constexpr std::size_t message_header_size = 16;

struct message_header {
    std::int32_t message_length = 0;
    std::int32_t request_id = 0;
    std::int32_t response_to = 0;
    std::int32_t code = 0;

    [[nodiscard]] op_code opcode() const noexcept { return static_cast<op_code>(code); }
    [[nodiscard]] bool is_op_msg() const noexcept { return code == 2013; }
};

// Encodes the header only; the caller appends the body.
[[nodiscard]] CRPC_API status encode_message_header(std::vector<std::byte>& out,
                                                    const message_header& header);
[[nodiscard]] CRPC_API status decode_message_header(span<const std::byte> in,
                                                    message_header& header);

// One OP_MSG request: the command document, as section kind 0.
[[nodiscard]] CRPC_API result<std::vector<std::byte>> encode_op_msg(
    const bson_document& command, std::int32_t request_id, std::uint32_t flags = 0);

// A decoded OP_MSG reply. `body` is the kind-0 document; `more_to_come` says the server
// will send more without waiting to be asked.
struct op_msg_reply {
    message_header header;
    std::uint32_t flags = 0;
    bson_document body;

    [[nodiscard]] bool more_to_come() const noexcept {
        return (flags & static_cast<std::uint32_t>(message_flag::more_to_come)) != 0;
    }
};

// Decodes a complete OP_MSG message (header included). A message with an unknown section
// kind is refused rather than skipped: skipping would silently drop a document the
// caller needs.
[[nodiscard]] CRPC_API status decode_op_msg(span<const std::byte> message, op_msg_reply& out);

// --- Commands ----------------------------------------------------------------

// The client's opening handshake. `hello` replaced `isMaster`; the fields sent here are
// the ones a server needs to decide whether the client may talk to it, and the reply
// carries the maximum wire version and the maximum BSON document size.
[[nodiscard]] CRPC_API bson_document make_hello(std::string_view application_name);

// `find` with a filter. `limit` of 0 means no limit, which is the server's own default.
struct find_request {
    std::string database;
    std::string collection;
    bson_document filter;
    // Empty means "every field".
    bson_document projection;
    std::int64_t limit = 0;
    std::int64_t skip = 0;
    // Batch size for the first reply; 0 leaves it to the server.
    std::int32_t batch_size = 0;
};

[[nodiscard]] CRPC_API bson_document make_find(const find_request& request);

// `aggregate` with an ordered pipeline. The reply is a cursor whose first batch holds the
// documents the server produced, exactly like `find`.
struct aggregate_request {
    std::string database;
    std::string collection;
    // Stages in application order. Each is a one-field document such as {$match: {...}}.
    std::vector<bson_document> pipeline;
    // Documents per batch; 0 leaves the size to the server.
    std::int32_t batch_size = 0;
};

// The command always carries a `cursor` sub-document: the server refuses an aggregate
// without one ("The 'cursor' option is required"), and an empty one asks for the default.
[[nodiscard]] CRPC_API bson_document make_aggregate(const aggregate_request& request);

// `insert` with the given documents. `ordered` false tells the server to keep going after
// a duplicate key, which is what a bulk load usually wants.
[[nodiscard]] CRPC_API bson_document make_insert(std::string_view collection,
                                                 std::vector<bson_document> documents,
                                                 bool ordered = true);

// `delete` with a filter and a limit: 0 means "every matching document", 1 means one.
[[nodiscard]] CRPC_API bson_document make_delete(std::string_view collection,
                                                 bson_document filter, std::int32_t limit);

// `getMore` continues a cursor the server left open.
[[nodiscard]] CRPC_API bson_document make_get_more(std::string_view database,
                                                   std::int64_t cursor_id,
                                                   std::string_view collection,
                                                   std::int32_t batch_size = 0);

// `killCursors` releases a cursor the client will not read to the end. Sending it is not
// optional politeness: an abandoned cursor holds server resources until it times out.
[[nodiscard]] CRPC_API bson_document make_kill_cursors(std::string_view collection,
                                                       std::int64_t cursor_id);

// Reads the `ok` field. A command that failed sets `ok` to 0 and carries `errmsg`; the
// error is built from those rather than from the transport state.
[[nodiscard]] CRPC_API bool reply_ok(const bson_document& reply);
[[nodiscard]] CRPC_API std::string reply_error_message(const bson_document& reply);

// Maps a server error code to the library code. Duplicate key, auth failure and
// namespace-not-found are the ones a caller reacts to differently.
[[nodiscard]] CRPC_API rpc_code map_mongo_error(std::int32_t code) noexcept;

// --- Distributed (replica set) semantics -------------------------------------

// How a read command prefers its nodes. The names below are the exact strings the
// wire protocol uses, so they double as the `name` document value. `primary` is the
// mode that sends no `$readPreference` at all, because it is the server's default.
enum class read_preference_mode : std::uint8_t {
    primary,
    primary_preferred,
    secondary,
    secondary_preferred,
    nearest
};

[[nodiscard]] CRPC_API const char* to_string(read_preference_mode mode) noexcept;

// How fresh a read must be. `local` is the default and sends no `readConcern`.
enum class read_concern_level : std::uint8_t {
    local,
    majority,
    snapshot,
    linearizable
};

[[nodiscard]] CRPC_API const char* to_string(read_concern_level level) noexcept;

// A write concern. `has_w` distinguishes "w explicitly 0" (unacknowledged, which
// already changes semantics) from "w not requested" (the server default applies).
struct CRPC_API write_concern_options {
    bool has_w = false;
    // Writes must be acknowledged by a majority; `has_w` is set as well.
    bool w_majority = false;
    std::int32_t w = 0;
    bool journal = false;
    std::int32_t wtimeout_ms = 0;

    // True when any field differs from "leave it to the server".
    [[nodiscard]] bool configured() const noexcept;

    [[nodiscard]] static write_concern_options majority();
    [[nodiscard]] static write_concern_options count(std::int32_t value);
};

// The value of the `$readPreference` top-level command field: {mode: "..."}.
[[nodiscard]] CRPC_API bson_document make_read_preference_document(read_preference_mode mode);

// The value of the `readConcern` top-level command field: {level: "..."}. Unlike the
// read preference the wire field has no leading dollar sign; the server refuses
// `$readConcern` with "unknown field".
[[nodiscard]] CRPC_API bson_document make_read_concern_document(read_concern_level level);

// The value of the `writeConcern` top-level command field: {w, j, wtimeout}.
[[nodiscard]] CRPC_API bson_document make_write_concern_document(
    const write_concern_options& options);

// The role a node plays in its replica set. Unknown is "the hello reply did not pin
// it down".
enum class topology_role : std::uint8_t { unknown, primary, secondary, arbiter };

// One node the hello reply named, with the role inferred from the reply.
struct CRPC_API topology_member {
    std::string host;
    topology_role role = topology_role::unknown;
};

// What one `hello` reply said about its replica set. A standalone answers without
// `setName`, which is exactly how `replica_set` stays false and the client knows not
// to attempt retryable writes (a standalone refuses `txnNumber` outright).
struct CRPC_API replica_set_topology {
    bool replica_set = false;
    std::string set_name;
    // The reply's own fields.
    bool writable_primary = false;
    bool secondary = false;
    bool arbiter = false;
    std::string me;
    std::string primary;
    // `hosts` (data-bearing, non-hidden, non-arbiter) plus `arbiters`. The host named
    // by `primary` is the primary; every other host is a secondary.
    std::vector<topology_member> members;

    // The role of the node this client talked to, from its own flags.
    [[nodiscard]] topology_role self_role() const noexcept;
};

// Parses a `hello` reply. A malformed `hosts`/`arbiters` array is a protocol error
// rather than a silently partial topology.
[[nodiscard]] CRPC_API status parse_hello_topology(const bson_document& hello_reply,
                                                   replica_set_topology& out);

// `endSessions` for the given session ids. Each id is exactly sixteen bytes and is
// sent as binary subtype 4 (UUID), which the server enforces: a session id stored as
// subtype 0 is refused with "wrong binData type 'general', expected type 'UUID'".
[[nodiscard]] CRPC_API bson_document make_end_sessions(
    const std::vector<std::vector<std::byte>>& session_ids);

} // namespace mongo
} // namespace db
} // namespace crpc

#endif // CRPC_DB_MONGO_PROTOCOL_HPP