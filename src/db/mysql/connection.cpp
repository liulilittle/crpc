// CRPC -- MySQL connection implementation.
#include <crpc/db/mysql/connection.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

#include <crpc/db/memory_result_set.hpp>
#include <crpc/db/mysql/auth.hpp>
#include <crpc/io.hpp>

namespace crpc {
namespace db {
namespace mysql {
namespace {

[[nodiscard]] std::uint8_t byte_at(span<const std::byte> data, std::size_t index) noexcept {
    return static_cast<std::uint8_t>(data[index]);
}

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

// Builds the library failure for a server ERR packet. `map_error_code` picks
// the abstract code; the raw provider code and the SQLSTATE are preserved as
// metadata so a caller can tell two failures that map to the same abstract
// code apart. The canonical pair is TiDB 9007 (optimistic write conflict at
// COMMIT) and MySQL 1205 (lock wait timeout): both become
// `rpc_code::unavailable`, but the metadata keeps them distinguishable and the
// TiDB retry helper (`crpc::db::tidb::is_transaction_conflict`) keys off it.
[[nodiscard]] rpc_error server_failure(const err_packet& err) {
    rpc_error failure{map_error_code(err.code), err.message};
    failure.metadata.set("sql_code", std::to_string(err.code));
    if (!err.sql_state.empty()) {
        failure.metadata.set("sql_state", err.sql_state);
    }
    return failure;
}

} // namespace

// --- connection --------------------------------------------------------------

connection::connection(connection_options options, std::unique_ptr<channel> link)
    : options_(std::move(options)), link_(std::move(link)) {
session_guard_ = std::make_shared<int>(0);
    const status opened = perform_handshake();
    if (!opened) {
        connect_error_ = opened.error();
        fail(opened);
    }
}

connection::~connection() { close(); }

void connection::fail(const status& error) noexcept {
    open_ = false;
    in_transaction_ = false;
    if (link_) {
        link_->close();
    }
    (void)error;
}

status connection::read_message(std::vector<std::byte>& payload) {
    payload.clear();
    for (;;) {
        result<std::vector<std::byte>> header = link_->read_exact(4);
        if (!header) {
            return header.error();
        }
        const std::uint32_t length = static_cast<std::uint32_t>(byte_at(
                                         span<const std::byte>(header.value().data(), 4), 0)) |
                                     (static_cast<std::uint32_t>(byte_at(
                                          span<const std::byte>(header.value().data(), 4), 1))
                                      << 8) |
                                     (static_cast<std::uint32_t>(byte_at(
                                          span<const std::byte>(header.value().data(), 4), 2))
                                      << 16);
        const std::uint8_t sequence = byte_at(span<const std::byte>(header.value().data(), 4), 3);
        // The sequence must advance by one per packet. A mismatch means the stream
        // lost or gained a packet, and continuing would decode garbage as a reply.
        if (sequence != sequence_) {
            return status{rpc_code::protocol_error, "mysql packet sequence mismatch"};
        }
        sequence_ = static_cast<std::uint8_t>(sequence_ + 1);

        if (payload.size() > options_.max_message_size ||
            length > options_.max_message_size - payload.size()) {
            return status{rpc_code::resource_exhausted, "mysql message exceeds the limit"};
        }
        result<std::vector<std::byte>> body = link_->read_exact(length);
        if (!body) {
            return body.error();
        }
        payload.insert(payload.end(), body.value().begin(), body.value().end());
        // A full packet means the message continues.
        if (length < max_packet_payload) {
            return status{};
        }
    }
}

status connection::send_message(span<const std::byte> payload, std::uint8_t sequence) {
    std::vector<std::byte> framed;
    framed.reserve(payload.size() + 8);
    std::uint8_t next = sequence;
    const status written = write_message(framed, payload, next);
    if (!written) {
        return written;
    }
    sequence_ = next;
    return link_->write(span<const std::byte>(framed.data(), framed.size()));
}

status connection::perform_handshake() {
    std::vector<std::byte> greeting_payload;
    // The greeting is packet sequence 0.
    sequence_ = 0;
    const status greeting_read = read_message(greeting_payload);
    if (!greeting_read) {
        return greeting_read;
    }
    if (!greeting_payload.empty() &&
        byte_at(span<const std::byte>(greeting_payload.data(), greeting_payload.size()), 0) ==
            0xff) {
        err_packet err;
        if (decode_err(span<const std::byte>(greeting_payload.data(), greeting_payload.size()),
                       err)
                .ok()) {
            return status{server_failure(err)};
        }
        return status{rpc_code::protocol_error, "mysql server refused the connection"};
    }

    handshake greeting;
    const status decoded =
        decode_handshake(span<const std::byte>(greeting_payload.data(), greeting_payload.size()),
                         greeting);
    if (!decoded) {
        return decoded;
    }
    server_version_ = greeting.server_version;
    server_capabilities_ = greeting.capabilities;

    const status authenticated = authenticate(greeting);
    if (!authenticated) {
        return authenticated;
    }
    open_ = true;

    // Selecting the schema after authentication keeps the handshake response
    // simple and gives a precise error (unknown database) when the name is wrong.
    if (!options_.database.empty()) {
        std::vector<std::byte> use_db = encode_command(0x02, options_.database); // COM_INIT_DB
        sequence_ = 0;
        const status sent =
            send_message(span<const std::byte>(use_db.data(), use_db.size()), 0);
        if (!sent) {
            return sent;
        }
        std::vector<std::byte> reply;
        const status read = read_message(reply);
        if (!read) {
            return read;
        }
        const span<const std::byte> view(reply.data(), reply.size());
        if (classify(view, false) == packet_kind::err) {
            err_packet err;
            (void)decode_err(view, err);
            return status{server_failure(err)};
        }
    }
    return status{};
}

status connection::authenticate(const handshake& greeting) {
    // Only capabilities both sides understand are negotiated. Asking for one the
    // server did not offer makes it reject the handshake.
    capabilities_ = options_.capabilities & greeting.capabilities;
    capabilities_ |= mask(capability::protocol_41);

    auth_plugin_ = greeting.auth_plugin_name;
    if (auth_plugin_.empty()) {
        // A server that offers no plugin name predates the plugin mechanism.
        auth_plugin_ = "mysql_native_password";
    }
    if (!is_supported_plugin(auth_plugin_)) {
        return status{rpc_code::no_such_method,
                      "unsupported mysql auth plugin: " + auth_plugin_};
    }

    const span<const std::byte> nonce(greeting.auth_plugin_data.data(),
                                      greeting.auth_plugin_data.size());
    result<std::vector<std::byte>> token =
        auth_plugin_ == "caching_sha2_password"
            ? scramble_caching_sha2_password(options_.password, nonce)
            : scramble_native_password(options_.password, nonce);
    if (!token) {
        return token.error();
    }

    handshake_response_options response;
    response.capabilities = capabilities_;
    response.charset = options_.charset;
    response.username = options_.user;
    response.auth_response = span<const std::byte>(token.value().data(), token.value().size());
    response.database = options_.database;
    response.auth_plugin = auth_plugin_;

    std::vector<std::byte> payload(handshake_response_size(response));
    std::size_t written = 0;
    const status encoded = encode_handshake_response(
        span<std::byte>(payload.data(), payload.size()), response, written);
    if (!encoded) {
        return encoded;
    }
    payload.resize(written);

    // The response continues the greeting's sequence.
    const status sent = send_message(span<const std::byte>(payload.data(), payload.size()), 1);
    if (!sent) {
        return sent;
    }

    for (;;) {
        std::vector<std::byte> reply;
        const status read = read_message(reply);
        if (!read) {
            return read;
        }
        const span<const std::byte> payload_view(reply.data(), reply.size());
        switch (classify(payload_view, false)) {
        case packet_kind::ok: {
            ok_packet ok;
            const status decoded_ok = decode_ok(payload_view, capabilities_, ok);
            if (!decoded_ok) {
                return decoded_ok;
            }
            status_flags_ = ok.status_flags;
            return status{};
        }
        case packet_kind::err: {
            err_packet err;
            const status decoded_err = decode_err(payload_view, err);
            if (!decoded_err) {
                return decoded_err;
            }
            return status{server_failure(err)};
        }
        default:
            break;
        }

        if (payload_view.empty()) {
            return status{rpc_code::protocol_error, "empty mysql auth reply"};
        }
        const std::uint8_t first = byte_at(payload_view, 0);
        if (first == 0xfe) {
            // Auth switch request: 0xFE <plugin NUL> <nonce>. A bare 0xFE means
            // "switch to the old password scheme", which this client cannot do.
            if (payload_view.size() == 1) {
                return status{rpc_code::no_such_method,
                              "mysql server asked for the pre-4.1 password scheme"};
            }
            std::size_t offset = 1;
            std::string plugin;
            std::string_view name;
            bool is_null = false;
            // The plugin name is NUL-terminated, not length-encoded.
            const auto begin = reinterpret_cast<const char*>(payload_view.data() + 1);
            std::size_t length = 0;
            while (1 + length < payload_view.size() &&
                   payload_view[1 + length] != std::byte{0}) {
                ++length;
            }
            if (1 + length >= payload_view.size()) {
                return status{rpc_code::protocol_error, "mysql auth switch has no plugin name"};
            }
            plugin.assign(begin, length);
            offset = 1 + length + 1;
            (void)name;
            (void)is_null;
            if (!is_supported_plugin(plugin)) {
                return status{rpc_code::no_such_method,
                              "unsupported mysql auth plugin: " + plugin};
            }
            const span<const std::byte> switch_nonce(payload_view.data() + offset,
                                                    payload_view.size() - offset);
            // The trailing NUL is not part of the challenge.
            span<const std::byte> trimmed = switch_nonce;
            while (!trimmed.empty() && trimmed[trimmed.size() - 1] == std::byte{0}) {
                trimmed = span<const std::byte>(trimmed.data(), trimmed.size() - 1);
            }
            result<std::vector<std::byte>> switched =
                plugin == "caching_sha2_password"
                    ? scramble_caching_sha2_password(options_.password, trimmed)
                    : scramble_native_password(options_.password, trimmed);
            if (!switched) {
                return switched.error();
            }
            auth_plugin_ = plugin;

            std::vector<std::byte> response_payload;
            encode_auth_switch_response(
                response_payload,
                span<const std::byte>(switched.value().data(), switched.value().size()),
                (capabilities_ & mask(capability::plugin_auth_lenenc_data)) != 0);
            const status resent =
                send_message(span<const std::byte>(response_payload.data(), response_payload.size()),
                             sequence_);
            if (!resent) {
                return resent;
            }
            continue;
        }
        if (first == 0x01) {
            // Auth more data. For caching_sha2: 0x03 = fast auth succeeded (an OK
            // follows), 0x04 = full authentication required, which over a plaintext
            // link needs the server's RSA key. That is the connection layer's job
            // and is not implemented here; a TLS link never needs it.
            if (payload_view.size() >= 2 && byte_at(payload_view, 1) == 0x03) {
                continue;
            }
            return status{rpc_code::failed_precondition,
                          "mysql caching_sha2 full authentication is not supported; "
                          "use TLS or a pre-hashed credential"};
        }
        return status{rpc_code::protocol_error, "unexpected mysql auth packet"};
    }
}

status connection::ping() {
    if (!open_) {
        return status{rpc_code::shutting_down, "mysql connection is closed"};
    }
    std::vector<std::byte> command = encode_command(0x0e, ""); // COM_PING
    sequence_ = 0;
    const status sent = send_message(span<const std::byte>(command.data(), command.size()), 0);
    if (!sent) {
        fail(sent);
        return sent;
    }
    std::vector<std::byte> reply;
    const status read = read_message(reply);
    if (!read) {
        fail(read);
        return read;
    }
    const span<const std::byte> payload_view(reply.data(), reply.size());
    if (classify(payload_view, false) == packet_kind::err) {
        err_packet err;
        (void)decode_err(payload_view, err);
        return status{server_failure(err)};
    }
    return status{};
}

bool connection::in_transaction() const noexcept { return open_ && in_transaction_; }

status connection::set_autocommit(bool enabled) {
    // Explicit statements rather than a capability bit: the server's
    // SERVER_STATUS_IN_TRANS is not set for `START TRANSACTION` until a statement
    // runs, so tracking it locally is the only exact answer.
    const char* sql = enabled ? "SET autocommit=1" : "SET autocommit=0";
    return run_query(sql, nullptr);
}

status connection::begin() {
    if (!open_) {
        return status{rpc_code::shutting_down, "mysql connection is closed"};
    }
    if (in_transaction_) {
        return status{rpc_code::failed_precondition, "a transaction is already open"};
    }
    const status started = run_query("START TRANSACTION", nullptr);
    if (!started) {
        return started;
    }
    in_transaction_ = true;
    return status{};
}

status connection::commit() {
    if (!open_) {
        return status{rpc_code::shutting_down, "mysql connection is closed"};
    }
    if (!in_transaction_) {
        return status{rpc_code::failed_precondition, "no transaction is open"};
    }
    const status committed = run_query("COMMIT", nullptr);
    // The transaction is over even if COMMIT failed: the server has already ended
    // it, and claiming otherwise would make the pool roll back nothing.
    in_transaction_ = false;
    return committed;
}

status connection::rollback() {
    if (!open_) {
        return status{rpc_code::shutting_down, "mysql connection is closed"};
    }
    if (!in_transaction_) {
        return status{};
    }
    const status rolled_back = run_query("ROLLBACK", nullptr);
    in_transaction_ = false;
    return rolled_back;
}

status connection::query(std::string_view sql, span<const parameter> params, result_set** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (!open_) {
        return status{rpc_code::shutting_down, "mysql connection is closed"};
    }
// Parameters always go through the prepared-statement protocol. Splicing them
    // into the SQL text is how injection happens, so there is no fallback that
    // "works" without it.
    if (!params.empty()) {
        result<prepared_statement> prepared = prepare(sql);
        if (!prepared) {
            return prepared.error();
        }
        prepared_statement handle = std::move(prepared.value());
        result<result_set*> executed = handle.execute(params);
        if (!executed) {
            return executed.error();
        }
        if (out != nullptr) {
            *out = executed.value();
        }
        // The statement is closed before returning: a pooled connection must not
        // accumulate server-side statements just because a caller used parameters.
        handle.close();
        return status{};
    }
    return run_query(sql, out);
}

status connection::run_query(std::string_view sql, result_set** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    std::vector<std::byte> command = encode_command(0x03, sql); // COM_QUERY
    sequence_ = 0;
    const status sent = send_message(span<const std::byte>(command.data(), command.size()), 0);
    if (!sent) {
        fail(sent);
        return sent;
    }

    std::vector<std::byte> reply;
    const status read = read_message(reply);
    if (!read) {
        fail(read);
        return read;
    }
    const span<const std::byte> payload(reply.data(), reply.size());

    if (classify(payload, false) == packet_kind::err) {
        err_packet err;
        const status decoded = decode_err(payload, err);
        if (!decoded) {
            return decoded;
        }
        return status{server_failure(err)};
    }
    if (classify(payload, false) == packet_kind::local_infile) {
        // LOAD DATA LOCAL INFILE asks the *client* for a file. A database client
        // that serves arbitrary local files to a server is a security hole, so it
        // is refused before any file is opened.
        return status{rpc_code::no_such_method,
                      "mysql LOAD DATA LOCAL INFILE is not supported"};
    }
    if (classify(payload, false) == packet_kind::ok) {
        ok_packet ok;
        const status decoded = decode_ok(payload, capabilities_, ok);
        if (!decoded) {
            return decoded;
        }
        status_flags_ = ok.status_flags;
        if (out != nullptr) {
            auto* set = new memory_result_set({}, static_cast<std::int64_t>(ok.affected_rows));
            *out = set;
        }
        return status{};
    }

    // A result set: column count, then the definitions, then rows.
    std::size_t offset = 0;
    std::uint64_t column_count = 0;
    bool is_null = false;
    const status count_read = read_lenenc_int(payload, offset, false, column_count, is_null);
    if (!count_read) {
        return count_read;
    }
    if (column_count == 0 || column_count > 4096) {
        return status{rpc_code::protocol_error, "mysql result set column count is implausible"};
    }

    std::vector<column_info> columns;
    std::vector<std::string> names;
    columns.reserve(static_cast<std::size_t>(column_count));
    for (std::uint64_t index = 0; index < column_count; ++index) {
        std::vector<std::byte> definition_payload;
        const status defined = read_message(definition_payload);
        if (!defined) {
            fail(defined);
            return defined;
        }
        column_definition definition;
        const status decoded = decode_column(
            span<const std::byte>(definition_payload.data(), definition_payload.size()),
            definition);
        if (!decoded) {
            return decoded;
        }
        names.push_back(definition.name);
        columns.push_back(mysql::to_column_info(definition));
    }
    const std::int64_t affected = -1;
    std::unique_ptr<memory_result_set> set(
        new memory_result_set(std::move(columns), affected));

    // Without CLIENT_DEPRECATE_EOF an EOF packet closes the column definitions; with
    // it the server omits that terminator.
    if ((capabilities_ & mask(capability::deprecate_eof)) == 0) {
        std::vector<std::byte> eof_payload;
        const status eof_read = read_message(eof_payload);
        if (!eof_read) {
            fail(eof_read);
            return eof_read;
        }
    }

    std::size_t rows = 0;
    std::size_t bytes = 0;
    for (;;) {
        std::vector<std::byte> row_payload;
        const status row_read = read_message(row_payload);
        if (!row_read) {
            fail(row_read);
            return row_read;
        }
        const span<const std::byte> row_view(row_payload.data(), row_payload.size());
        if (row_view.empty()) {
            return status{rpc_code::protocol_error, "empty mysql row packet"};
        }
        const std::uint8_t first = byte_at(row_view, 0);
        // A short 0xfe packet ends the row stream (EOF); with DEPRECATE_EOF the
        // terminator is an OK packet whose header is 0xfe and carries a status.
        if (first == 0xfe && row_view.size() < 9) {
            eof_packet eof;
            (void)decode_eof(row_view, eof);
            status_flags_ = eof.status_flags;
            break;
        }
        // With CLIENT_DEPRECATE_EOF the row stream ends with an OK packet whose
        // header byte is 0xfe. Its layout is the OK layout, so the status word sits
        // after two length-encoded integers rather than at a fixed offset.
        if ((capabilities_ & mask(capability::deprecate_eof)) != 0 && first == 0xfe) {
            std::size_t ok_offset = 1;
            std::uint64_t ignored = 0;
            bool ignored_null = false;
            (void)read_lenenc_int(row_view, ok_offset, false, ignored, ignored_null);
            (void)read_lenenc_int(row_view, ok_offset, false, ignored, ignored_null);
            if (ok_offset + 4 <= row_view.size()) {
                status_flags_ = read_u16(row_view, ok_offset);
            }
            break;
        }
        if (first == 0xff) {
            err_packet err;
            const status decoded = decode_err(row_view, err);
            if (!decoded) {
                return decoded;
            }
            // A mid-result-set error is the server's answer; the partial rows are
            // discarded rather than returned as if complete.
            return status{server_failure(err)};
        }

        text_row row;
        const status decoded = decode_text_row(row_view, row);
        if (!decoded) {
            return decoded;
        }
        if (row.values.size() != static_cast<std::size_t>(column_count)) {
            return status{rpc_code::protocol_error,
                          "mysql row does not match the column count"};
        }
        ++rows;
        if (rows > options_.max_result_rows) {
            return status{rpc_code::resource_exhausted,
                          "mysql result set exceeds max_result_rows"};
        }
        std::vector<std::string> values;
        values.reserve(row.values.size());
        for (const std::string_view value : row.values) {
            bytes += value.size();
            values.emplace_back(value.data(), value.size());
        }
        if (bytes > options_.max_result_bytes) {
            return status{rpc_code::resource_exhausted,
                          "mysql result set exceeds max_result_bytes"};
        }
        set->add_row(std::move(values), std::move(row.nulls));
    }

    if ((status_flags_ & static_cast<std::uint16_t>(server_status::more_results)) != 0) {
        // A multi-statement command produced another result set. The second set is
        // not read here, so the connection would be left mid-stream; closing it is
        // the only safe outcome.
        fail(status{rpc_code::no_such_method, "mysql multiple result sets are not supported"});
        return status{rpc_code::no_such_method, "mysql multiple result sets are not supported"};
    }

    if (out != nullptr) {
        *out = set.release();
    }
    return status{};
}

status connection::read_binary_result(std::uint64_t column_count, result_set** out,
                                           bool allow_more_results) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (column_count == 0 || column_count > 4096) {
        return status{rpc_code::protocol_error, "mysql result set column count is implausible"};
    }

    std::vector<column_info> columns;
    std::vector<column_type> types;
    columns.reserve(static_cast<std::size_t>(column_count));
    types.reserve(static_cast<std::size_t>(column_count));
    for (std::uint64_t index = 0; index < column_count; ++index) {
        std::vector<std::byte> definition_payload;
        const status read = read_message(definition_payload);
        if (!read) {
            fail(read);
            return read;
        }
        column_definition definition;
        const status decoded = decode_column(
            span<const std::byte>(definition_payload.data(), definition_payload.size()),
            definition);
        if (!decoded) {
            return decoded;
        }
        types.push_back(definition.type);
        columns.push_back(mysql::to_column_info(definition));
    }
    if ((capabilities_ & mask(capability::deprecate_eof)) == 0) {
        std::vector<std::byte> eof_payload;
        const status eof_read = read_message(eof_payload);
        if (!eof_read) {
            fail(eof_read);
            return eof_read;
        }
    }

    std::unique_ptr<memory_result_set> set(
        new memory_result_set(std::move(columns), -1));
    for (;;) {
        std::vector<std::byte> row_payload;
        const status read = read_message(row_payload);
        if (!read) {
            fail(read);
            return read;
        }
        const span<const std::byte> row_view(row_payload.data(), row_payload.size());
        if (row_view.empty()) {
            return status{rpc_code::protocol_error, "empty mysql binary row packet"};
        }
        const std::uint8_t first = byte_at(row_view, 0);
        if (first == 0xfe && row_view.size() < 9) {
            eof_packet eof;
            (void)decode_eof(row_view, eof);
            status_flags_ = eof.status_flags;
            break;
        }
        if ((capabilities_ & mask(capability::deprecate_eof)) != 0 && first == 0xfe) {
            std::size_t ok_offset = 1;
            std::uint64_t ignored = 0;
            bool ignored_null = false;
            (void)read_lenenc_int(row_view, ok_offset, false, ignored, ignored_null);
            (void)read_lenenc_int(row_view, ok_offset, false, ignored, ignored_null);
            if (ok_offset + 4 <= row_view.size()) {
                status_flags_ = read_u16(row_view, ok_offset);
            }
            break;
        }
        if (first == 0xff) {
            err_packet err;
            const status decoded = decode_err(row_view, err);
            if (!decoded) {
                return decoded;
            }
            return status{server_failure(err)};
        }

        binary_row row;
        const status decoded = decode_binary_row(
            row_view, span<const column_type>(types.data(), types.size()), row);
        if (!decoded) {
            return decoded;
        }
        set->add_row(std::move(row.values), std::move(row.nulls));
    }

    // A CALL answers a result set and then a trailing OK, flagged with MORE_RESULTS.
    // When the caller asked for that shape (stored procedures), the extra packet is
    // read here so the stream stays aligned; otherwise it is refused, because acting
    // on the first result while another is queued misreads the next command.
    if ((status_flags_ & static_cast<std::uint16_t>(server_status::more_results)) != 0) {
        if (!allow_more_results) {
            fail(status{rpc_code::no_such_method, "mysql multiple result sets are not supported"});
            return status{rpc_code::no_such_method,
                          "mysql multiple result sets are not supported"};
        }
        std::vector<std::byte> trailing;
        const status trailing_read = read_message(trailing);
        if (!trailing_read) {
            fail(trailing_read);
            return trailing_read;
        }
        const span<const std::byte> trailing_view(trailing.data(), trailing.size());
        if (!trailing_view.empty() &&
            static_cast<std::uint8_t>(trailing_view[0]) == 0xff) {
            err_packet err;
            const status decoded = decode_err(trailing_view, err);
            if (!decoded) {
                return decoded;
            }
            return status{server_failure(err)};
        }
        if (!trailing_view.empty() &&
            static_cast<std::uint8_t>(trailing_view[0]) == 0x00) {
            ok_packet ok;
            (void)decode_ok(trailing_view, capabilities_, ok);
            status_flags_ = ok.status_flags;
        }
    }
    if (out != nullptr) {
        *out = set.release();
    }
    return status{};
}

status connection::execute_prepared(std::uint32_t statement_id, span<const parameter> params,
                                    result_set** out, bool allow_more_results) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (!open_) {
        return status{rpc_code::shutting_down, "mysql connection is closed"};
    }
    std::vector<std::byte> command;
    const status encoded = encode_stmt_execute(command, statement_id, params);
    if (!encoded) {
        return encoded;
    }
    sequence_ = 0;
    const status sent = send_message(span<const std::byte>(command.data(), command.size()), 0);
    if (!sent) {
        fail(sent);
        return sent;
    }

    std::vector<std::byte> reply;
    const status read = read_message(reply);
    if (!read) {
        fail(read);
        return read;
    }
    const span<const std::byte> payload(reply.data(), reply.size());
    if (classify(payload, false) == packet_kind::err) {
        err_packet err;
        const status decoded = decode_err(payload, err);
        if (!decoded) {
            return decoded;
        }
        return status{server_failure(err)};
    }
    if (classify(payload, false) == packet_kind::ok) {
        ok_packet ok;
        const status decoded = decode_ok(payload, capabilities_, ok);
        if (!decoded) {
            return decoded;
        }
        status_flags_ = ok.status_flags;
        // A procedure with no result set still ends with two OK packets when the
        // call was made through a prepared statement. Leaving the second one in the
        // stream would make the next command read it as its own answer.
        if (allow_more_results &&
            (status_flags_ & static_cast<std::uint16_t>(server_status::more_results)) != 0) {
            std::vector<std::byte> trailing;
            const status trailing_read = read_message(trailing);
            if (!trailing_read) {
                fail(trailing_read);
                return trailing_read;
            }
            const span<const std::byte> trailing_view(trailing.data(), trailing.size());
            if (!trailing_view.empty() &&
                static_cast<std::uint8_t>(trailing_view[0]) == 0xff) {
                err_packet err;
                const status err_decoded = decode_err(trailing_view, err);
                if (!err_decoded) {
                    return err_decoded;
                }
                return status{server_failure(err)};
            }
            if (!trailing_view.empty() &&
                static_cast<std::uint8_t>(trailing_view[0]) == 0x00) {
                ok_packet trailing_ok;
                (void)decode_ok(trailing_view, capabilities_, trailing_ok);
                status_flags_ = trailing_ok.status_flags;
            }
        }
        if (out != nullptr) {
            *out = new memory_result_set({}, static_cast<std::int64_t>(ok.affected_rows));
        }
        return status{};
    }

    std::size_t offset = 0;
    std::uint64_t column_count = 0;
    bool is_null = false;
    const status count_read = read_lenenc_int(payload, offset, false, column_count, is_null);
    if (!count_read) {
        return count_read;
    }
    return read_binary_result(column_count, out, allow_more_results);
}

namespace {

// Rewrites `?` placeholders that are not inside a string literal, a quoted
// identifier or a comment. A stored procedure's OUT argument must be a session
// variable, so an output placeholder becomes `@crpc_out_<n>`; an input placeholder
// stays `?` and is bound.
//
// Rewriting SQL text is only safe because this scanner understands the quoting
// rules: replacing every '?' would corrupt a literal like 'a?b', which is exactly
// the bug that turns a stored-procedure wrapper into a data-corruption source.
status rewrite_call_placeholders(std::string_view sql, span<const parameter> params,
                                 std::string& rewritten, std::vector<std::size_t>& binds,
                                 std::vector<std::size_t>& outputs) {
    rewritten.clear();
    rewritten.reserve(sql.size() + 16);
    binds.clear();
    outputs.clear();
    std::size_t index = 0;

    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char c = sql[i];
        if (c == '\'' || c == '"' || c == '`') {
            const char quote = c;
            rewritten.push_back(c);
            ++i;
            while (i < sql.size()) {
                rewritten.push_back(sql[i]);
                if (quote != '`' && sql[i] == '\\' && i + 1 < sql.size()) {
                    ++i;
                    rewritten.push_back(sql[i]);
                } else if (sql[i] == quote) {
                    // A doubled quote is an escaped quote, not the end.
                    if (i + 1 < sql.size() && sql[i + 1] == quote) {
                        ++i;
                        rewritten.push_back(sql[i]);
                    } else {
                        break;
                    }
                }
                ++i;
            }
            continue;
        }
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') {
                rewritten.push_back(sql[i]);
                ++i;
            }
            if (i < sql.size()) {
                rewritten.push_back(sql[i]);
            }
            continue;
        }
        if (c == '#') {
            while (i < sql.size() && sql[i] != '\n') {
                rewritten.push_back(sql[i]);
                ++i;
            }
            if (i < sql.size()) {
                rewritten.push_back(sql[i]);
            }
            continue;
        }
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            rewritten.push_back('/');
            rewritten.push_back('*');
            i += 2;
            while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) {
                rewritten.push_back(sql[i]);
                ++i;
            }
            if (i + 1 < sql.size()) {
                rewritten.push_back('*');
                rewritten.push_back('/');
                ++i;
            }
            continue;
        }
        if (c == '?') {
            if (index >= params.size()) {
                return status{rpc_code::invalid_argument,
                              "the call has more placeholders than parameters"};
            }
            // Both OUT and INOUT must be session variables: MySQL does not accept a
            // bound placeholder for an argument the procedure writes. Only a pure
            // input stays ?. An INOUT value is seeded with a prepared SET before
            // the call; a pure OUT has nothing to seed.
            if (params[index].is_output()) {
                rewritten += "@crpc_out_";
                rewritten += std::to_string(index);
                outputs.push_back(index);
            } else {
                rewritten.push_back('?');
                binds.push_back(index);
            }
            ++index;
            continue;
        }
        rewritten.push_back(c);
    }

    if (index != params.size()) {
        return status{rpc_code::invalid_argument,
                      "the call has fewer placeholders than parameters"};
    }
    return status{};
}

// Writes a read-back value into the caller's parameter, converting the text form to
// the parameter's declared type. The type comes from the caller because a SELECT of
// a session variable reports a string.
void assign_output(parameter& target, std::string_view text, bool is_null) {
    if (is_null) {
        target.is_null = true;
        return;
    }
    target.is_null = false;
    const std::string copy(text);
    switch (target.type) {
    case db_type::boolean:
        target.int_value = (text == "1" || text == "true" || text == "TRUE") ? 1 : 0;
        return;
    case db_type::int64: {
        char* end = nullptr;
        const long long value = std::strtoll(copy.c_str(), &end, 10);
        if (end != copy.c_str()) {
            target.int_value = static_cast<std::int64_t>(value);
        }
        return;
    }
    case db_type::uint64: {
        char* end = nullptr;
        const unsigned long long value = std::strtoull(copy.c_str(), &end, 10);
        if (end != copy.c_str()) {
            target.uint_value = static_cast<std::uint64_t>(value);
        }
        return;
    }
    case db_type::real: {
        char* end = nullptr;
        const double value = std::strtod(copy.c_str(), &end);
        if (end != copy.c_str()) {
            target.real_value = value;
        }
        return;
    }
    case db_type::blob:
        target.blob.assign(reinterpret_cast<const std::byte*>(text.data()),
                           reinterpret_cast<const std::byte*>(text.data()) + text.size());
        return;
    default:
        target.text.assign(text.data(), text.size());
        return;
    }
}

} // namespace

status connection::call(std::string_view call_sql, span<parameter> params, result_set** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (!open_) {
        return status{rpc_code::shutting_down, "mysql connection is closed"};
    }

    std::string rewritten;
    std::vector<std::size_t> binds;
    std::vector<std::size_t> outputs;
    const status rewrite = rewrite_call_placeholders(
        call_sql, span<const parameter>(params.data(), params.size()), rewritten, binds, outputs);
    if (!rewrite) {
        return rewrite;
    }

    // INOUT parameters arrive as values, so the session variable is seeded first. A
    // plain OUT parameter has nothing to seed.
    for (const std::size_t index : outputs) {
        if (params[index].direction != parameter_direction::input_output) {
            continue;
        }
        const std::string set_sql =
            "SET @crpc_out_" + std::to_string(index) + " = ?";
        result<prepared_statement> setter = prepare(set_sql);
        if (!setter) {
            return setter.error();
        }
        prepared_statement set_statement = std::move(setter.value());
        const std::vector<parameter> one{params[index]};
        result<result_set*> applied =
            set_statement.execute(span<const parameter>(one.data(), one.size()));
        set_statement.close();
        if (!applied) {
            return applied.error();
        }
        delete applied.value();
    }

    std::vector<parameter> bound;
    bound.reserve(binds.size());
    for (const std::size_t index : binds) {
        bound.push_back(params[index]);
    }

    result<prepared_statement> prepared = prepare(rewritten);
    if (!prepared) {
        return prepared.error();
    }
    prepared_statement call_statement = std::move(prepared.value());
    // A procedure answers its result set and then a trailing OK, so the trailing
    // packet must be drained; refusing it would leave the stream one packet ahead.
    result<result_set*> executed = call_statement.execute_impl(
        span<const parameter>(bound.data(), bound.size()), true);
    if (!executed) {
        call_statement.close();
        return executed.error();
    }
    result_set* rows = executed.value();
    call_statement.close();

    if (!outputs.empty()) {
        std::string select = "SELECT ";
        for (std::size_t k = 0; k < outputs.size(); ++k) {
            if (k != 0) {
                select += ", ";
            }
            select += "@crpc_out_" + std::to_string(outputs[k]);
        }
        result_set* readback = nullptr;
        const status read = run_query(select, &readback);
        if (!read) {
            delete rows;
            return read;
        }
        std::unique_ptr<result_set> guard(readback);
        if (readback != nullptr && readback->next()) {
            for (std::size_t k = 0; k < outputs.size(); ++k) {
                const bool is_null = readback->is_null(k);
                std::string_view text;
                if (!is_null) {
                    result<std::string_view> value = readback->get_text(k);
                    if (!value) {
                        delete rows;
                        return value.error();
                    }
                    text = value.value();
                }
                assign_output(params[outputs[k]], text, is_null);
            }
        }
    }

    if (out != nullptr) {
        *out = rows;
    } else {
        delete rows;
    }
    return status{};
}

local_data_source::~local_data_source() = default;

result<std::size_t> slice_data_source::read(span<std::byte> buffer) {
    if (offset_ >= data_.size()) {
        return std::size_t{0};
    }
    const std::size_t take =
        std::min(buffer.size(), data_.size() - offset_);
    if (take != 0) {
        std::memcpy(buffer.data(), data_.data() + offset_, take);
    }
    offset_ += take;
    return take;
}

namespace {

// True when the statement is a LOAD DATA LOCAL INFILE, ignoring leading whitespace
// and case. The statement text comes from the caller, so this is a sanity check
// rather than a security boundary; the boundary is that the data source is the
// caller's.
[[nodiscard]] bool is_load_data_local(std::string_view sql) noexcept {
    std::size_t index = 0;
    while (index < sql.size() && (sql[index] == ' ' || sql[index] == '\\t' ||
                                  sql[index] == '\\r' || sql[index] == '\\n')) {
        ++index;
    }
    constexpr std::string_view prefix = "LOAD DATA LOCAL INFILE";
    if (sql.size() - index < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        char c = sql[index + i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        if (c != prefix[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

status connection::load_data(std::string_view load_sql, local_data_source& source,
                             result_set** out, std::size_t chunk_size) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (!open_) {
        return status{rpc_code::shutting_down, "mysql connection is closed"};
    }
    if (!is_load_data_local(load_sql)) {
        return status{rpc_code::invalid_argument,
                      "load_data needs a LOAD DATA LOCAL INFILE statement"};
    }
    if (chunk_size == 0) {
        return status{rpc_code::invalid_argument, "the chunk size must be at least 1"};
    }

    std::vector<std::byte> command = encode_command(0x03, load_sql); // COM_QUERY
    sequence_ = 0;
    const status sent = send_message(span<const std::byte>(command.data(), command.size()), 0);
    if (!sent) {
        fail(sent);
        return sent;
    }

    std::vector<std::byte> reply;
    const status read = read_message(reply);
    if (!read) {
        fail(read);
        return read;
    }
    const span<const std::byte> payload(reply.data(), reply.size());
    if (!payload.empty() && static_cast<std::uint8_t>(payload[0]) == 0xff) {
        err_packet err;
        const status decoded = decode_err(payload, err);
        if (!decoded) {
            return decoded;
        }
        return status{server_failure(err)};
    }
    if (!payload.empty() && static_cast<std::uint8_t>(payload[0]) == 0x00) {
        // The server answered without asking for data (LOCAL disabled on its side,
        // or the statement needed none). That is a success, not an error.
        ok_packet ok;
        const status decoded = decode_ok(payload, capabilities_, ok);
        if (!decoded) {
            return decoded;
        }
        status_flags_ = ok.status_flags;
        if (out != nullptr) {
            *out = new memory_result_set({}, static_cast<std::int64_t>(ok.affected_rows));
        }
        return status{};
    }
    if (payload.empty() || static_cast<std::uint8_t>(payload[0]) != 0xfb) {
        return status{rpc_code::protocol_error,
                      "mysql did not request the local file data"};
    }

    // The server asked; stream exactly what the caller supplies, then an empty
    // packet to mark the end. Nothing here opens a path.
    std::vector<std::byte> buffer(chunk_size);
    for (;;) {
        result<std::size_t> produced = source.read(span<std::byte>(buffer.data(), buffer.size()));
        if (!produced) {
            // A partial file has already been sent, so the stream is mid-operation:
            // the only safe outcome is to drop the connection.
            fail(produced.error());
            return produced.error();
        }
        const std::size_t count = produced.value();
        if (count == 0) {
            break;
        }
        if (count > buffer.size()) {
            fail(status{rpc_code::protocol_error, "the data source returned too many bytes"});
            return status{rpc_code::protocol_error, "the data source returned too many bytes"};
        }
        const status chunk_sent =
            send_message(span<const std::byte>(buffer.data(), count), sequence_);
        if (!chunk_sent) {
            fail(chunk_sent);
            return chunk_sent;
        }
    }
    // An empty packet terminates the data stream.
    const status terminated = send_message(span<const std::byte>(), sequence_);
    if (!terminated) {
        fail(terminated);
        return terminated;
    }

    std::vector<std::byte> final_reply;
    const status final_read = read_message(final_reply);
    if (!final_read) {
        fail(final_read);
        return final_read;
    }
    const span<const std::byte> final_view(final_reply.data(), final_reply.size());
    if (!final_view.empty() && static_cast<std::uint8_t>(final_view[0]) == 0xff) {
        err_packet err;
        const status decoded = decode_err(final_view, err);
        if (!decoded) {
            return decoded;
        }
        return status{server_failure(err)};
    }
    ok_packet ok;
    const status decoded = decode_ok(final_view, capabilities_, ok);
    if (!decoded) {
        return decoded;
    }
    status_flags_ = ok.status_flags;
    if (out != nullptr) {
        *out = new memory_result_set({}, static_cast<std::int64_t>(ok.affected_rows));
    }
    return status{};
}

status connection::bulk_insert(const db::insert_target& target,
                                 span<const std::vector<parameter>> rows,
                                 const db::bulk_options& options, std::uint64_t& affected) {
    affected = 0;
    if (!open_) {
        return status{rpc_code::shutting_down, "mysql connection is closed"};
    }
    if (rows.empty()) {
        return status{};
    }
    for (const std::vector<parameter>& row : rows) {
        if (row.size() != target.columns.size()) {
            return status{rpc_code::invalid_argument,
                          "a bulk row does not match the column count"};
        }
    }

    result<std::vector<std::size_t>> plan =
        db::plan_insert_batches(target, rows.size(), options);
    if (!plan) {
        return plan.error();
    }

    std::size_t offset = 0;
    for (const std::size_t batch : plan.value()) {
        result<std::string> sql = db::make_insert_sql(target, batch);
        if (!sql) {
            return sql.error();
        }
        result<prepared_statement> prepared = prepare(sql.value());
        if (!prepared) {
            return prepared.error();
        }
        prepared_statement statement = std::move(prepared.value());

        std::vector<parameter> bound;
        bound.reserve(batch * target.columns.size());
        for (std::size_t row = 0; row < batch; ++row) {
            for (const parameter& value : rows[offset + row]) {
                bound.push_back(value);
            }
        }

        result<result_set*> executed =
            statement.execute(span<const parameter>(bound.data(), bound.size()));
        statement.close();
        if (!executed) {
            return executed.error();
        }
        std::unique_ptr<result_set> guard(executed.value());
        if (guard != nullptr && guard->affected_rows() > 0) {
            affected += static_cast<std::uint64_t>(guard->affected_rows());
        }
        offset += batch;
    }
    return status{};
}

result<prepared_statement> connection::prepare(std::string_view sql) {
    if (!open_) {
        return rpc_error{rpc_code::shutting_down, "mysql connection is closed"};
    }
    std::vector<std::byte> command = encode_command(com_stmt_prepare, sql);
    sequence_ = 0;
    const status sent = send_message(span<const std::byte>(command.data(), command.size()), 0);
    if (!sent) {
        fail(sent);
        return sent.error();
    }

    std::vector<std::byte> reply;
    const status read = read_message(reply);
    if (!read) {
        fail(read);
        return read.error();
    }
    const span<const std::byte> payload(reply.data(), reply.size());
    if (classify(payload, false) == packet_kind::err) {
        err_packet err;
        const status decoded = decode_err(payload, err);
        if (!decoded) {
            return decoded.error();
        }
        return server_failure(err);
    }

    prepare_ok ok;
    const status decoded = decode_prepare_ok(payload, ok);
    if (!decoded) {
        return decoded.error();
    }

    // The reply is followed by the parameter definitions and the column
    // definitions, each closed by an EOF. Skipping them by count (rather than by
    // scanning for EOF) is what keeps the stream aligned: a definition packet can
    // contain any bytes.
    if (ok.param_count > 0) {
        for (std::uint16_t index = 0; index < ok.param_count; ++index) {
            std::vector<std::byte> ignored;
            const status skipped = read_message(ignored);
            if (!skipped) {
                fail(skipped);
                return skipped.error();
            }
        }
        if ((capabilities_ & mask(capability::deprecate_eof)) == 0) {
            std::vector<std::byte> eof_payload;
            const status eof_read = read_message(eof_payload);
            if (!eof_read) {
                fail(eof_read);
                return eof_read.error();
            }
        }
    }
    if (ok.column_count > 0) {
        for (std::uint16_t index = 0; index < ok.column_count; ++index) {
            std::vector<std::byte> ignored;
            const status skipped = read_message(ignored);
            if (!skipped) {
                fail(skipped);
                return skipped.error();
            }
        }
        if ((capabilities_ & mask(capability::deprecate_eof)) == 0) {
            std::vector<std::byte> eof_payload;
            const status eof_read = read_message(eof_payload);
            if (!eof_read) {
                fail(eof_read);
                return eof_read.error();
            }
        }
    }
    return prepared_statement(this, session_guard_, ok.statement_id, ok.param_count,
                              ok.column_count);
}

void connection::release_statement(std::uint32_t statement_id) noexcept {
    if (!open_ || statement_id == 0) {
        return;
    }
    std::vector<std::byte> command = encode_stmt_close(statement_id);
    sequence_ = 0;
    // COM_STMT_CLOSE has no reply. Reading one here would consume the next command's
    // answer, so a failure is simply terminal for the session.
    const status sent = send_message(span<const std::byte>(command.data(), command.size()), 0);
    if (!sent) {
        fail(sent);
    }
}

// --- prepared_statement ------------------------------------------------------

prepared_statement::prepared_statement(connection* owner, std::weak_ptr<void> guard,
                                       std::uint32_t statement_id, std::size_t params,
                                       std::size_t columns) noexcept
    : owner_(owner),
      guard_(std::move(guard)),
      statement_id_(statement_id),
      param_count_(params),
      column_count_(columns) {}

prepared_statement::~prepared_statement() { close(); }

prepared_statement::prepared_statement(prepared_statement&& other) noexcept
    : owner_(other.owner_),
      guard_(std::move(other.guard_)),
      statement_id_(other.statement_id_),
      param_count_(other.param_count_),
      column_count_(other.column_count_) {
    // The moved-from handle must not close the statement the moved-to handle owns.
    other.owner_ = nullptr;
    other.statement_id_ = 0;
}

prepared_statement& prepared_statement::operator=(prepared_statement&& other) noexcept {
    if (this != &other) {
        close();
        owner_ = other.owner_;
        guard_ = std::move(other.guard_);
        statement_id_ = other.statement_id_;
        param_count_ = other.param_count_;
        column_count_ = other.column_count_;
        other.owner_ = nullptr;
        other.statement_id_ = 0;
    }
    return *this;
}

result<result_set*> prepared_statement::execute(span<const parameter> params) {
    return execute_impl(params, false);
}

result<result_set*> prepared_statement::execute_impl(span<const parameter> params,
                                                     bool allow_more_results) {
    if (owner_ == nullptr || guard_.expired()) {
        return rpc_error{rpc_code::failed_precondition, "the statement is not valid"};
    }
    if (params.size() != param_count_) {
        // A count mismatch would bind values to the wrong placeholders; refusing is
        // the only safe answer.
        return rpc_error{rpc_code::invalid_argument,
                         "the statement expects a different number of parameters"};
    }
    result_set* set = nullptr;
    const status ran =
        owner_->execute_prepared(statement_id_, params, &set, allow_more_results);
    if (!ran) {
        return ran.error();
    }
    return set;
}

void prepared_statement::close() noexcept {
    if (owner_ == nullptr) {
        return;
    }
    // If the session is gone, the server already dropped every statement; there is
    // nothing to send and the owner pointer must not be dereferenced.
    if (guard_.expired()) {
        owner_ = nullptr;
        statement_id_ = 0;
        return;
    }
    connection* owner = owner_;
    const std::uint32_t id = statement_id_;
    owner_ = nullptr;
    statement_id_ = 0;
    param_count_ = 0;
    column_count_ = 0;
    owner->release_statement(id);
}

void connection::close() noexcept {
    open_ = false;
    in_transaction_ = false;
    // Expires every prepared-statement handle without touching them: the server
    // drops all statements when the session ends, and a handle must not call into a
    // connection that is being destroyed.
    session_guard_.reset();
    if (link_) {
        link_->close();
    }
}

bool connection::is_open() const noexcept { return open_; }

// --- factory -----------------------------------------------------------------

channel_factory::channel_factory(connection_options options, opener open)
    : options_(std::move(options)), open_(std::move(open)) {}

channel_factory::~channel_factory() = default;

result<db::connection*> channel_factory::open() {
    if (!open_) {
        return rpc_error{rpc_code::failed_precondition, "the channel factory has no opener"};
    }
    result<std::unique_ptr<channel>> opened = open_();
    if (!opened) {
        return opened.error();
    }
    auto session = std::unique_ptr<connection>(
        new connection(options_, std::move(opened.value())));
    if (!session->is_open()) {
        rpc_error error = session->connect_error();
        if (error.code == rpc_code::ok) {
            // Report the server's own message: "handshake failed" alone hides
            // whether it was an auth rejection, a missing database, or a plugin.
            error = rpc_error{rpc_code::unavailable, "mysql handshake failed"};
        }
        return error;
    }
    return static_cast<db::connection*>(session.release());
}

std::string_view channel_factory::name() const noexcept { return "mysql-injected"; }
connection_factory::connection_factory(connection_options options, crpc::backend_factory& backend,
                                       crpc::endpoint endpoint, crpc::runtime* shard)
    : options_(std::move(options)),
      backend_(&backend),
      endpoint_(std::move(endpoint)),
      shard_(shard) {}

connection_factory::~connection_factory() = default;

result<db::connection*> connection_factory::open() {
    crpc::net::connection_config config;
    config.ep = endpoint_;
    // The MySQL handshake is a round trip; the transport's operation timeout
    // bounds it on a plain thread, while a coroutine parks until the server
    // answers or the link dies.
    config.operation_timeout = std::chrono::seconds(15);

    auto session = std::unique_ptr<connection>(new connection(
        options_, std::unique_ptr<channel>(
                      new socket_channel(std::move(config), *backend_, shard_))));
    if (!session->is_open()) {
        rpc_error error = session->connect_error();
        if (error.code == rpc_code::ok) {
            // Report the server's own message: "handshake failed" alone hides
            // whether it was an auth rejection, a missing database, or a plugin.
            error = rpc_error{rpc_code::unavailable, "mysql handshake failed"};
        }
        return error;
    }
    return static_cast<db::connection*>(session.release());
}

std::string_view connection_factory::name() const noexcept { return "mysql"; }

} // namespace mysql
} // namespace db
} // namespace crpc
