// CRPC -- MySQL connection implementation.
#include <crpc/db/mysql/connection.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

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

// Parses a decimal integer the server sent as text. The text form of an integer
// column is exactly the value, so no rounding is involved; a malformed one is a
// protocol error rather than a zero.
[[nodiscard]] result<std::int64_t> parse_int(std::string_view text) {
    if (text.empty()) {
        return rpc_error{rpc_code::decode_error, "empty integer text"};
    }
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        index = 1;
    } else if (text[0] == '+') {
        index = 1;
    }
    if (index == text.size()) {
        return rpc_error{rpc_code::decode_error, "integer text has no digits"};
    }
    std::uint64_t value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') {
            return rpc_error{rpc_code::decode_error, "integer text has a non-digit"};
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return rpc_error{rpc_code::decode_error, "integer text overflows"};
        }
        value = value * 10 + digit;
    }
    if (negative) {
        const std::uint64_t limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u;
        if (value > limit) {
            return rpc_error{rpc_code::decode_error, "negative integer text overflows"};
        }
        if (value == limit) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return -static_cast<std::int64_t>(value);
    }
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        // A BIGINT UNSIGNED above INT64_MAX is legal, so it is reported through
        // `get_uint64`; asking for a signed value is the caller's mistake.
        return rpc_error{rpc_code::invalid_argument,
                         "value exceeds int64; read it as uint64"};
    }
    return static_cast<std::int64_t>(value);
}

// A fully materialised result set.
//
// The rows are copied at query time on purpose: a cursor that lazily read from the
// socket would keep the connection busy for as long as the caller holds it, and the
// pool would have to know that. Materialising bounds the memory (via the options)
// and lets the connection go back to the pool immediately.
class memory_result_set final : public result_set {
public:
    memory_result_set(std::vector<column_info> columns, std::vector<std::string> names,
                      std::int64_t affected) noexcept
        : columns_(std::move(columns)), affected_(affected) {
        (void)names;
    }

    void add_row(std::vector<std::string> values, std::vector<bool> nulls) {
        values_.push_back(std::move(values));
        nulls_.push_back(std::move(nulls));
    }

    std::size_t column_count() const noexcept override { return columns_.size(); }

    column_info column(std::size_t index) const override {
        if (index >= columns_.size()) {
            return {};
        }
        return columns_[index];
    }

    // Forward-only cursor: `next()` moves to the following row and every accessor
    // reads that row. Before the first `next()` there is no current row, and the
    // accessors report NULL rather than reading row 0 behind the caller's back.
    bool next() override {
        if (next_ >= values_.size()) {
            has_row_ = false;
            return false;
        }
        cursor_ = next_++;
        has_row_ = true;
        return true;
    }

    bool is_null(std::size_t index) const noexcept override {
        if (!has_row_ || cursor_ >= nulls_.size() || index >= nulls_[cursor_].size()) {
            return true;
        }
        return nulls_[cursor_][index];
    }

    result<std::int64_t> get_int64(std::size_t index) override {
        const std::string_view text = value_at(index);
        if (text.data() == nullptr) {
            return rpc_error{rpc_code::invalid_argument, "column is NULL"};
        }
        return parse_int(text);
    }

    result<std::uint64_t> get_uint64(std::size_t index) override {
        const std::string_view text = value_at(index);
        if (text.data() == nullptr) {
            return rpc_error{rpc_code::invalid_argument, "column is NULL"};
        }
        if (text.empty()) {
            return rpc_error{rpc_code::decode_error, "empty integer text"};
        }
        std::uint64_t value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return rpc_error{rpc_code::decode_error, "integer text has a non-digit"};
            }
            const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                return rpc_error{rpc_code::decode_error, "integer text overflows"};
            }
            value = value * 10 + digit;
        }
        return value;
    }

    result<double> get_double(std::size_t index) override {
        const std::string_view text = value_at(index);
        if (text.data() == nullptr) {
            return rpc_error{rpc_code::invalid_argument, "column is NULL"};
        }
        // `strtod` needs a NUL-terminated buffer; the stored value is a std::string,
        // so a copy of at most one value is made here rather than parsing by hand
        // and getting the exponent/NaN rules wrong.
        const std::string copy(text);
        char* end = nullptr;
        const double value = std::strtod(copy.c_str(), &end);
        if (end == copy.c_str() || *end != '\0') {
            return rpc_error{rpc_code::decode_error, "value is not a number"};
        }
        return value;
    }

    result<bool> get_bool(std::size_t index) override {
        const std::string_view text = value_at(index);
        if (text.data() == nullptr) {
            return rpc_error{rpc_code::invalid_argument, "column is NULL"};
        }
        // MySQL's own boolean spelling is 0/1; anything else is a decode error
        // rather than a C-style truthiness, which would accept "false" as true.
        if (text == "0") {
            return false;
        }
        if (text == "1") {
            return true;
        }
        return rpc_error{rpc_code::decode_error, "value is not a boolean"};
    }

    result<std::string_view> get_text(std::size_t index) override {
        const std::string_view text = value_at(index);
        if (text.data() == nullptr) {
            return rpc_error{rpc_code::invalid_argument, "column is NULL"};
        }
        return text;
    }

    result<std::vector<std::byte>> get_blob(std::size_t index) override {
        const std::string_view text = value_at(index);
        if (text.data() == nullptr) {
            return rpc_error{rpc_code::invalid_argument, "column is NULL"};
        }
        std::vector<std::byte> out(text.size());
        std::memcpy(out.data(), text.data(), text.size());
        return out;
    }

    std::int64_t affected_rows() const noexcept override { return affected_; }

    void close() noexcept override { closed_ = true; }

private:
    // A missing cursor position or a NULL both yield a null `data()` pointer, and
    // every accessor checks it first.
    [[nodiscard]] std::string_view value_at(std::size_t index) const {
        if (!has_row_ || cursor_ >= values_.size() || index >= values_[cursor_].size()) {
            return {};
        }
        if (index < nulls_[cursor_].size() && nulls_[cursor_][index]) {
            return {};
        }
        return values_[cursor_][index];
    }

    std::vector<column_info> columns_;
    std::vector<std::vector<std::string>> values_;
    std::vector<std::vector<bool>> nulls_;
    std::int64_t affected_ = -1;
    std::size_t next_ = 0;
    std::size_t cursor_ = 0;
    bool has_row_ = false;
    bool closed_ = false;
};

} // namespace

// --- channel -----------------------------------------------------------------

channel::~channel() = default;

socket_channel::socket_channel(crpc::net::connection_config config,
                               crpc::backend_factory& backend, crpc::runtime* shard)
    : link_(new crpc::net::connection(std::move(config), backend, shard)) {}

socket_channel::~socket_channel() { close(); }

result<std::vector<std::byte>> socket_channel::read_exact(std::size_t count) {
    if (!link_) {
        return rpc_error{rpc_code::shutting_down, "mysql channel is closed"};
    }
    return link_->read_exact(count);
}

status socket_channel::write(span<const std::byte> data) {
    if (!link_) {
        return status{rpc_code::shutting_down, "mysql channel is closed"};
    }
    return link_->write(data);
}

void socket_channel::close() noexcept {
    if (link_) {
        link_->close();
    }
}

bool socket_channel::is_open() const noexcept { return link_ != nullptr && link_->is_open(); }

// --- connection --------------------------------------------------------------

connection::connection(connection_options options, std::unique_ptr<channel> link)
    : options_(std::move(options)), link_(std::move(link)) {
    const status opened = perform_handshake();
    if (!opened) {
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
            return status{map_error_code(err.code), err.message};
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
            return status{map_error_code(err.code), err.message};
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
            return status{map_error_code(err.code), err.message};
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
        return status{map_error_code(err.code), err.message};
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
    // This backend has no prepared-statement support yet, and splicing a parameter
    // into the SQL text by hand is how injection happens. Refusing is the only
    // honest answer until COM_STMT_PREPARE is implemented.
    if (!params.empty()) {
        return status{rpc_code::no_such_method,
                      "mysql parameter binding needs prepared statements, which are not "
                      "implemented yet; use a literal query with escaping instead"};
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
        return status{map_error_code(err.code), err.message};
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
            auto* set = new memory_result_set({}, {}, static_cast<std::int64_t>(ok.affected_rows));
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
        new memory_result_set(std::move(columns), std::move(names), affected));

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
            return status{map_error_code(err.code), err.message};
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

void connection::close() noexcept {
    open_ = false;
    in_transaction_ = false;
    if (link_) {
        link_->close();
    }
}

bool connection::is_open() const noexcept { return open_; }

// --- factory -----------------------------------------------------------------

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
        return rpc_error{rpc_code::unavailable, "mysql handshake failed"};
    }
    return static_cast<db::connection*>(session.release());
}

std::string_view connection_factory::name() const noexcept { return "mysql"; }

} // namespace mysql
} // namespace db
} // namespace crpc