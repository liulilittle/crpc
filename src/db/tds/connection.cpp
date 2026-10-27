// CRPC -- TDS connection implementation.
#include <crpc/db/tds/connection.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include <crpc/db/memory_result_set.hpp>

namespace crpc {
namespace db {
namespace tds {
namespace {

[[nodiscard]] std::uint8_t byte_at(span<const std::byte> data, std::size_t index) noexcept {
    return static_cast<std::uint8_t>(data[index]);
}

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

// The header's length field is big-endian and includes the header itself.
[[nodiscard]] std::vector<std::byte> header_bytes(const packet_header& header) {
    std::vector<std::byte> out(header_size);
    std::size_t written = 0;
    const status encoded =
        encode_header(span<std::byte>(out.data(), out.size()), header, written);
    if (!encoded || written != header_size) {
        out.clear();
    }
    return out;
}

// COLMETADATA's wire type byte to the library's type family, for ORM metadata.
[[nodiscard]] db_type type_of(std::uint8_t wire) noexcept {
    switch (wire) {
    case 0x30: // INT1
    case 0x34: // INT2
    case 0x38: // INT4
    case 0x7F: // INT8
    case 0x26: // INTN
        return db_type::int64;
    case 0x32: // BIT
    case 0x68: // BITN
        return db_type::boolean;
    case 0x3B: // FLT4
    case 0x3E: // FLT8
    case 0x6D: // FLTN
        return db_type::real;
    case 0x6A: // DECIMALN
    case 0x6C: // NUMERICN
        return db_type::decimal;
    case 0x24: // GUID
        return db_type::uuid;
    case 0x28: // DATE
        return db_type::date;
    case 0x29: // TIME
        return db_type::time;
    case 0x2A: // DATETIME2
    case 0x3D: // DATETIME
        return db_type::timestamp;
    case 0xA5: // BIGVARBINARY
    case 0xAD: // BIGBINARY
        return db_type::blob;
    case 0xA7: // BIGVARCHAR
    case 0xAF: // BIGCHAR
    case 0xE7: // NVARCHAR
    case 0xEF: // NCHAR
        return db_type::text;
    default:
        return db_type::unknown;
    }
}

[[nodiscard]] constexpr std::uint32_t bit(login_option option) noexcept {
    return static_cast<std::uint32_t>(option);
}

} // namespace

connection::connection(connection_options options, std::unique_ptr<db::channel> link)
    : options_(std::move(options)), link_(std::move(link)) {
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

// --- framing -----------------------------------------------------------------

std::vector<std::byte> connection::all_headers() const {
    // Captured from sqlcmd's own traffic: the block is 22 bytes even outside a
    // transaction -- total length, the 18-byte transaction-descriptor header with an
    // all-zero descriptor, and an outstanding-request count of 1. Sending a bare
    // 4-byte length instead makes SQL Server reject the message with "the multiple
    // active result sets (MARS) TDS header is missing", and sending nothing at all
    // gives "the TDS headers contained errors": both were observed before this.
    std::vector<std::byte> out;
    constexpr std::size_t total = 4 + 4 + 2 + transaction_descriptor_size + 4;
    constexpr std::size_t header_length = 4 + 2 + transaction_descriptor_size + 4;
    out.push_back(static_cast<std::byte>(total & 0xffu));
    out.push_back(static_cast<std::byte>((total >> 8) & 0xffu));
    out.push_back(std::byte{0});
    out.push_back(std::byte{0});
    out.push_back(static_cast<std::byte>(header_length & 0xffu));
    out.push_back(static_cast<std::byte>((header_length >> 8) & 0xffu));
    out.push_back(std::byte{0});
    out.push_back(std::byte{0});
    out.push_back(std::byte{0x02}); // header type: transaction descriptor
    out.push_back(std::byte{0});
    // Inside a transaction the server's own descriptor must be echoed: sending
    // zeros gets "should come with valid transaction descriptor".
    for (const std::uint8_t byte : descriptor_) {
        out.push_back(static_cast<std::byte>(byte));
    }
    out.push_back(std::byte{1}); // outstanding request count
    out.push_back(std::byte{0});
    out.push_back(std::byte{0});
    out.push_back(std::byte{0});
    return out;
}

status connection::send_message(packet_type type, span<const std::byte> payload) {
    if (!open_ && type != packet_type::prelogin && type != packet_type::login7) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    // PRELOGIN and LOGIN7 predate the header block; everything after them carries it.
    std::vector<std::byte> framed;
    if (type == packet_type::sql_batch || type == packet_type::rpc ||
        type == packet_type::transaction_manager) {
        framed = all_headers();
        framed.insert(framed.end(), payload.begin(), payload.end());
        payload = span<const std::byte>(framed.data(), framed.size());
    }
    // The header claims the whole packet, so the body is split at the payload
    // boundary: header_size + chunk <= packet_size.
    const std::size_t chunk_max =
        packet_size_ > header_size ? static_cast<std::size_t>(packet_size_) - header_size : 0;
    if (chunk_max == 0) {
        return status{rpc_code::failed_precondition, "the negotiated packet size is too small"};
    }
    std::size_t offset = 0;
    do {
        const std::size_t remaining = payload.size() - offset;
        const std::size_t chunk = std::min(remaining, chunk_max);
        packet_header header;
        header.type = type;
        header.length = static_cast<std::uint16_t>(header_size + chunk);
        header.packet_id = packet_id_;
        header.status = (offset + chunk >= payload.size())
                            ? static_cast<std::uint8_t>(packet_status::eom)
                            : static_cast<std::uint8_t>(packet_status::normal);
        const std::vector<std::byte> prefix = header_bytes(header);
        if (prefix.size() != header_size) {
            return status{rpc_code::protocol_error, "the TDS header did not encode"};
        }
        const status head = link_->write(span<const std::byte>(prefix.data(), prefix.size()));
        if (!head) {
            fail(head);
            return head;
        }
        if (chunk != 0) {
            const status body = link_->write(
                span<const std::byte>(payload.data() + offset, chunk));
            if (!body) {
                fail(body);
                return body;
            }
        }
        offset += chunk;
        packet_id_ = static_cast<std::uint8_t>(packet_id_ + 1);
        // A zero-length payload still needs one packet, hence `do` rather than
        // `while`: the loop must run once even when there is nothing to send.
    } while (offset < payload.size());
    return status{};
}

status connection::read_message(std::vector<std::byte>& payload) {
    payload.clear();
    for (;;) {
        result<std::vector<std::byte>> head = link_->read_exact(header_size);
        if (!head) {
            fail(head.error());
            return head.error();
        }
        packet_header header;
        const status decoded =
            decode_header(span<const std::byte>(head.value().data(), head.value().size()), header);
        if (!decoded) {
            fail(decoded);
            return decoded;
        }
        if (header.length < header_size) {
            fail(status{rpc_code::protocol_error, "a TDS packet claims fewer bytes than its header"});
            return status{rpc_code::protocol_error,
                          "a TDS packet claims fewer bytes than its header"};
        }
        const std::size_t body = static_cast<std::size_t>(header.length) - header_size;
        if (body != 0) {
            result<std::vector<std::byte>> chunk = link_->read_exact(body);
            if (!chunk) {
                fail(chunk.error());
                return chunk.error();
            }
            if (payload.size() + chunk.value().size() > options_.max_message_size) {
                const status too_large{rpc_code::resource_exhausted,
                                       "a TDS message exceeds max_message_size"};
                fail(too_large);
                return too_large;
            }
            payload.insert(payload.end(), chunk.value().begin(), chunk.value().end());
        }
        if (header.is_eom()) {
            return status{};
        }
        // A reset bit means the server dropped this session's state; continuing
        // would decode a reply as if the transaction and temp tables still existed.
        if ((header.status & static_cast<std::uint8_t>(packet_status::reset_connection)) != 0) {
            const status reset{rpc_code::connection_reset,
                               "the server reset the TDS connection"};
            fail(reset);
            return reset;
        }
    }
}

// --- handshake ---------------------------------------------------------------

status connection::perform_handshake() {
    if (!link_) {
        return status{rpc_code::failed_precondition, "the connection has no channel"};
    }
    if (options_.require_encryption) {
        // Refusing beats encrypting nothing: a plaintext login that the caller
        // believes is encrypted is worse than a clear error.
        return status{rpc_code::failed_precondition,
                      "TLS is not implemented for TDS; drop require_encryption or use a "
                      "tunnel"};
    }
    packet_size_ = options_.packet_size;

    prelogin greeting;
    greeting.encryption = encryption_mode::not_supported;
    greeting.thread_id = 0;
    greeting.mars = 0;
    std::vector<std::byte> payload;
    const status encoded = encode_prelogin(payload, greeting);
    if (!encoded) {
        return encoded;
    }
    const status sent = send_message(packet_type::prelogin, span<const std::byte>(
                                                                payload.data(), payload.size()));
    if (!sent) {
        return sent;
    }
    std::vector<std::byte> reply;
    const status received = read_message(reply);
    if (!received) {
        return received;
    }
    prelogin answer;
    const status decoded =
        decode_prelogin(span<const std::byte>(reply.data(), reply.size()), answer);
    if (!decoded) {
        return decoded;
    }
    if (answer.encryption == encryption_mode::required ||
        answer.encryption == encryption_mode::on) {
        return status{rpc_code::failed_precondition,
                      "the server requires TDS encryption and this client has no TLS"};
    }
    encrypted_ = false;

    // LOGIN7. ServerName defaults to the endpoint host when the caller has none,
    // because the server records it and a mismatch shows up in audits.
    login7 login;
    login.user_name = options_.user;
    login.password = options_.password;
    login.database = options_.database;
    login.host_name = options_.host_name;
    login.application_name = options_.application_name;
    login.server_name = options_.server_name;
    login.client_interface_name = "crpc";
    login.packet_size = packet_size_;
    // OptionFlags1: fUseDB | fDatabase | fSetLang, and fByteOrder left at 0 because
    // the client is little-endian. Setting fByteOrder is not harmless: the server
    // then reads the whole login backwards and rejects it with 17832.
    login.option_flags = bit(login_option::use_db_notify) | bit(login_option::init_db_fatal) |
                         bit(login_option::set_language_on);
    // OptionFlags2: fLanguage | fODBC.
    login.option_flags |= bit(login_option::language) | bit(login_option::odbc);

    std::vector<std::byte> login_payload;
    const status login_encoded = encode_login7(login_payload, login);
    if (!login_encoded) {
        return login_encoded;
    }
    const status login_sent = send_message(
        packet_type::login7, span<const std::byte>(login_payload.data(), login_payload.size()));
    if (!login_sent) {
        return login_sent;
    }

    // The login reply is a token stream terminated by DONE. LOGINACK is the only
    // proof the session exists; anything else is an error the caller must see.
    bool acknowledged = false;
    rpc_error server_error{rpc_code::ok, {}};
    for (;;) {
        std::vector<std::byte> message;
        const status read = read_message(message);
        if (!read) {
            return read;
        }
        token_reader reader(span<const std::byte>(message.data(), message.size()));
        bool finished = false;
        for (;;) {
            token item;
            const token_reader::result step = reader.next(item);
            if (step == token_reader::result::end) {
                break;
            }
            if (step == token_reader::result::error) {
                return status{reader.error(), "a TDS login token could not be decoded"};
            }
            switch (item.type) {
            case token::kind::login_ack:
                acknowledged = true;
                if (item.ack.server_version.size() >= 4) {
                    server_version_ = (static_cast<std::uint32_t>(item.ack.server_version[0])
                                       << 24) |
                                      (static_cast<std::uint32_t>(item.ack.server_version[1])
                                       << 16) |
                                      (static_cast<std::uint32_t>(item.ack.server_version[2])
                                       << 8) |
                                      static_cast<std::uint32_t>(item.ack.server_version[3]);
                }
                break;
            case token::kind::env_change:
                if (item.env.type == env_change_type::packet_size &&
                    item.env.new_packet_size >= 512) {
                    // The negotiated size replaces the requested one for every
                    // later message, in both directions.
                    packet_size_ = static_cast<std::uint16_t>(
                        std::min<std::uint32_t>(item.env.new_packet_size, 32767u));
                }
                if (item.env.type == env_change_type::begin_transaction) {
                    descriptor_ = item.env.new_descriptor;
                    has_descriptor_ = true;
                } else if (item.env.type == env_change_type::commit_transaction ||
                           item.env.type == env_change_type::rollback_transaction) {
                    has_descriptor_ = false;
                }
                break;
            case token::kind::error:
                // Keep the first error: later ones are usually consequences.
                if (server_error.code == rpc_code::ok) {
                    server_error = rpc_error{map_error_number(item.error.number),
                                             item.error.message.empty()
                                                 ? std::string("the server rejected the login")
                                                 : item.error.message};
                }
                break;
            case token::kind::done:
                finished = true;
                break;
            default:
                break;
            }
            if (finished) {
                break;
            }
        }
        if (finished) {
            break;
        }
    }
    if (server_error.code != rpc_code::ok) {
        return server_error;
    }
    if (!acknowledged) {
        return status{rpc_code::protocol_error,
                      "the login reply had no LOGINACK; the server did not accept the session"};
    }
    server_name_ = login.server_name;
    open_ = true;
    return status{};
}

// --- batches -----------------------------------------------------------------

status connection::run_batch(std::string_view sql, result_set** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    const std::vector<std::byte> encoded = utf16le(sql);
    const status sent =
        send_message(packet_type::sql_batch, span<const std::byte>(encoded.data(), encoded.size()));
    if (!sent) {
        return sent;
    }

    // The reply is one token stream that may span several messages. `current` is
    // the result set being filled; `first` is the one the caller will get, recorded
    // when a result set completes. Everything after it is decoded and discarded so
    // the stream stays aligned for the next batch.
    std::unique_ptr<memory_result_set> first;
    std::unique_ptr<memory_result_set> current;
    std::int64_t affected = -1;
    rpc_error server_error{rpc_code::ok, {}};
    std::size_t row_bytes = 0;

    for (;;) {
        std::vector<std::byte> message;
        const status read = read_message(message);
        if (!read) {
            return read;
        }
        token_reader reader(span<const std::byte>(message.data(), message.size()));
        bool finished = false;
        for (;;) {
            token item;
            const token_reader::result step = reader.next(item);
            if (step == token_reader::result::end) {
                break;
            }
            if (step == token_reader::result::error) {
                const status broken{reader.error(), "a TDS result token could not be decoded"};
                fail(broken);
                return broken;
            }
            switch (item.type) {
            case token::kind::colmetadata: {
                std::vector<column_info> columns;
                columns.reserve(item.columns.size());
                for (const column_meta& source : item.columns) {
                    column_info info;
                    info.name = source.name;
                    info.type = type_of(source.type);
                    info.precision = source.precision;
                    info.scale = source.scale;
                    info.nullable = source.nullable;
                    columns.push_back(std::move(info));
                }
                // A COLMETADATA starts a new result set; the previous one ended at
                // its DONE, so it is already complete.
                current = std::unique_ptr<memory_result_set>(
                    new memory_result_set(std::move(columns), -1));
                break;
            }
            case token::kind::row: {
                if (!current) {
                    const status no_meta{rpc_code::protocol_error,
                                         "a TDS row arrived before its column metadata"};
                    fail(no_meta);
                    return no_meta;
                }
                for (const std::string& value : item.values) {
                    row_bytes += value.size();
                }
                if (current->row_count() >= options_.max_result_rows ||
                    row_bytes > options_.max_result_bytes) {
                    const status too_large{rpc_code::resource_exhausted,
                                           "the TDS result set exceeds the configured limit"};
                    fail(too_large);
                    return too_large;
                }
                current->add_row(item.values, item.nulls);
                break;
            }
            case token::kind::error:
                if (server_error.code == rpc_code::ok) {
                    server_error = rpc_error{map_error_number(item.error.number),
                                             item.error.message.empty()
                                                 ? std::string("the server rejected the batch")
                                                 : item.error.message};
                }
                break;
            case token::kind::info:
                // PRINT and informational messages are not errors.
                break;
            case token::kind::done: {
                if (item.done.has_row_count) {
                    affected = static_cast<std::int64_t>(std::min<std::uint64_t>(
                        item.done.row_count,
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
                } else {
                    affected = 0;
                }
                if (current) {
                    current->set_affected_rows(affected);
                    if (!first) {
                        first = std::move(current);
                    }
                }
                current.reset();
                if (!item.done.more_results()) {
                    finished = true;
                }
                break;
            }
            case token::kind::env_change:
                if (item.env.type == env_change_type::packet_size &&
                    item.env.new_packet_size >= 512) {
                    packet_size_ = static_cast<std::uint16_t>(
                        std::min<std::uint32_t>(item.env.new_packet_size, 32767u));
                }
                // A transaction started by a batch hands out its descriptor in the
                // reply. Every later message must echo it, or the server refuses the
                // request with "should come with valid transaction descriptor".
                if (item.env.type == env_change_type::begin_transaction) {
                    descriptor_ = item.env.new_descriptor;
                    has_descriptor_ = true;
                } else if (item.env.type == env_change_type::commit_transaction ||
                           item.env.type == env_change_type::rollback_transaction) {
                    has_descriptor_ = false;
                    descriptor_.fill(0);
                }
                break;
            default:
                break;
            }
            if (finished) {
                break;
            }
        }
        if (finished) {
            break;
        }
    }

    if (server_error.code != rpc_code::ok) {
        return server_error;
    }
    if (out != nullptr) {
        if (first) {
            *out = first.release();
        } else {
            // A batch with no result set (INSERT, DDL, BEGIN) still reports how many
            // rows it touched, so it gets a cursor with no columns.
            *out = new memory_result_set({}, affected);
        }
    }
    return status{};
}

// --- db::connection ----------------------------------------------------------

status connection::ping() {
    result_set* set = nullptr;
    const status ran = query("SELECT 1", {}, &set);
    statement_guard guard(set);
    return ran;
}

bool connection::in_transaction() const noexcept { return in_transaction_; }

status connection::begin() {
    result_set* set = nullptr;
    const status ran = query("BEGIN TRANSACTION", {}, &set);
    statement_guard guard(set);
    if (!ran) {
        return ran;
    }
    in_transaction_ = true;
    return status{};
}

status connection::commit() {
    result_set* set = nullptr;
    const status ran = query("COMMIT TRANSACTION", {}, &set);
    statement_guard guard(set);
    if (!ran) {
        return ran;
    }
    in_transaction_ = false;
    return status{};
}

status connection::rollback() {
    result_set* set = nullptr;
    const status ran = query("ROLLBACK TRANSACTION", {}, &set);
    statement_guard guard(set);
    if (!ran) {
        return ran;
    }
    in_transaction_ = false;
    return status{};
}

status connection::query(std::string_view sql, span<const parameter> params, result_set** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (!params.empty()) {
        // The alternative is splicing values into the SQL text, which is how
        // injection happens. sp_executesql over RPC is the correct mechanism and
        // is not encoded yet.
        return status{rpc_code::invalid_argument,
                      "the TDS adapter does not encode parameters yet; execute textual SQL "
                      "or wait for the RPC slice"};
    }
    if (!open_) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    return run_batch(sql, out);
}

void connection::close() noexcept {
    open_ = false;
    in_transaction_ = false;
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
    auto session =
        std::unique_ptr<connection>(new connection(options_, std::move(opened.value())));
    if (!session->is_open()) {
        rpc_error error = session->connect_error();
        if (error.code == rpc_code::ok) {
            error = rpc_error{rpc_code::failed_precondition, "the TDS login failed"};
        }
        return error;
    }
    return static_cast<db::connection*>(session.release());
}

std::string_view channel_factory::name() const noexcept { return "tds-injected"; }

connection_factory::connection_factory(connection_options options, crpc::backend_factory& backend,
                                       crpc::endpoint endpoint, crpc::runtime* shard)
    : options_(std::move(options)), backend_(&backend), endpoint_(std::move(endpoint)),
      shard_(shard) {}

connection_factory::~connection_factory() = default;

result<db::connection*> connection_factory::open() {
    crpc::net::connection_config config;
    config.ep = endpoint_;
    // PRELOGIN plus LOGIN7 is a round trip; the transport's operation timeout
    // bounds it on a plain thread, while a coroutine parks until the server
    // answers or the link dies.
    config.operation_timeout = std::chrono::seconds(15);

    auto session = std::unique_ptr<connection>(new connection(
        options_, std::unique_ptr<db::channel>(
                      new db::socket_channel(std::move(config), *backend_, shard_))));
    if (!session->is_open()) {
        rpc_error error = session->connect_error();
        if (error.code == rpc_code::ok) {
            error = rpc_error{rpc_code::failed_precondition, "the TDS login failed"};
        }
        return error;
    }
    return static_cast<db::connection*>(session.release());
}

std::string_view connection_factory::name() const noexcept { return "tds"; }

} // namespace tds
} // namespace db
} // namespace crpc