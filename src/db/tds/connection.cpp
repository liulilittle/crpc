// CRPC -- TDS connection implementation.
#include <crpc/db/tds/connection.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
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
    // One message at a time. ATTENTION is written by another thread while this one is
    // blocked in a read, and interleaving it with a batch's packets would corrupt the
    // stream for both.
    const std::lock_guard<std::mutex> guard(write_mutex_);
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

// The reply loop shared by batches and RPCs. It reads until a DONE without the
// "more results" bit, collecting the first result set and any values the server wrote
// back. Anything after the first result set is decoded and discarded so the stream
// stays aligned for the next request -- a reply that is not fully drained
// desynchronises the session permanently.
status connection::read_reply(result_set** out, std::vector<return_value>* written) {
    std::unique_ptr<memory_result_set> first;
    std::unique_ptr<memory_result_set> current;
    std::int64_t affected = -1;
    rpc_error server_error{rpc_code::ok, {}};
    std::size_t row_bytes = 0;
    has_return_status_ = false;

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
                // A COLMETADATA starts a new result set; the previous one ended at its
                // DONE, so it is already complete.
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
                                                 ? std::string("the server rejected the request")
                                                 : item.error.message};
                }
                break;
            case token::kind::info:
                // PRINT and informational messages are not errors.
                break;
            case token::kind::return_status:
                last_return_status_ = item.ret_status;
                has_return_status_ = true;
                break;
            case token::kind::return_value:
                if (written != nullptr) {
                    written->push_back(item.ret);
                }
                break;
            case token::kind::done: {
                if (item.done.attention()) {
                    // The server acknowledged the ATTENTION: the request was stopped.
                    // Reporting `canceled` is the honest answer even if rows arrived
                    // before it, because the caller asked to stop.
                    attention_seen_ = true;
                }
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
                    // A cancellation is acknowledged in its own message. Stopping at the
                    // first final DONE would leave that acknowledgement in the stream,
                    // and the next request would read it as its own reply -- which is
                    // exactly what happened before this check existed.
                    finished = !attention_pending_ || attention_seen_;
                }
                break;
            }
            case token::kind::env_change:
                if (item.env.type == env_change_type::packet_size &&
                    item.env.new_packet_size >= 512) {
                    packet_size_ = static_cast<std::uint16_t>(
                        std::min<std::uint32_t>(item.env.new_packet_size, 32767u));
                }
                // A transaction started by a request hands out its descriptor in the
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

    if (attention_pending_) {
        const bool acknowledged = attention_seen_;
        attention_pending_ = false;
        attention_seen_ = false;
        if (acknowledged) {
            return status{rpc_code::canceled, "the request was cancelled"};
        }
    }
    if (server_error.code != rpc_code::ok) {
        return server_error;
    }
    if (out != nullptr) {
        if (first) {
            *out = first.release();
        } else {
            // A request with no result set (INSERT, DDL, BEGIN) still reports how many
            // rows it touched, so it gets a cursor with no columns.
            *out = new memory_result_set({}, affected);
        }
    }
    return status{};
}

status connection::run_batch(std::string_view sql, result_set** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    const std::vector<std::byte> encoded = utf16le(sql);
    // Marked before the bytes go out: a cancel arriving between the write and the flag
    // would otherwise see an idle session, skip waiting for the acknowledgement, and
    // leave it to be read as the next reply.
    in_request_.store(true);
    const status sent =
        send_message(packet_type::sql_batch, span<const std::byte>(encoded.data(), encoded.size()));
    if (!sent) {
        in_request_.store(false);
        return sent;
    }
    const status done = read_reply(out, nullptr);
    in_request_.store(false);
    return done;
}

status connection::run_rpc(const rpc_request& request, result_set** out,
                           std::vector<return_value>* written) {
    if (out != nullptr) {
        *out = nullptr;
    }
    std::vector<std::byte> body;
    const status encoded = encode_rpc(body, request);
    if (!encoded) {
        return encoded;
    }
    in_request_.store(true);
    const status sent =
        send_message(packet_type::rpc, span<const std::byte>(body.data(), body.size()));
    if (!sent) {
        in_request_.store(false);
        return sent;
    }
    const status done = read_reply(out, written);
    in_request_.store(false);
    return done;
}


status connection::cancel() {
    if (!open_) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    // ATTENTION is the one message that carries no AllHeaders block: it is a bare
    // header with no body, and the server answers with a DONE that has the attention
    // bit set. The acknowledgement is consumed by whoever is reading the reply.
    packet_header header;
    header.type = packet_type::attention;
    header.length = static_cast<std::uint16_t>(header_size);
    header.packet_id = packet_id_;
    header.status = static_cast<std::uint8_t>(packet_status::eom);
    const std::vector<std::byte> prefix = header_bytes(header);
    if (prefix.size() != header_size) {
        return status{rpc_code::protocol_error, "the TDS header did not encode"};
    }
    const std::lock_guard<std::mutex> guard(write_mutex_);
    // Only an in-flight request can be cancelled; an idle session has nothing to
    // acknowledge, and waiting for that acknowledgement would stall the next reply.
    attention_pending_ = in_request_.load();
    attention_seen_ = false;
    const status sent = link_->write(span<const std::byte>(prefix.data(), prefix.size()));
    if (!sent) {
        attention_pending_ = false;
        fail(sent);
        return sent;
    }
    packet_id_ = static_cast<std::uint8_t>(packet_id_ + 1);
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

namespace {

// What one parameter needs on the wire: the type info, the logical value bytes, and
// the declaration text SQL Server wants in an sp_executesql call.
struct encoded_parameter {
    column_meta info;
    std::vector<std::byte> value;
    bool is_null = false;
    std::string declaration;
};

void put_le64_bytes(std::vector<std::byte>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xffu));
    }
}

// Maps one library parameter onto TDS. A value that cannot be represented is refused
// rather than approximated: binding the wrong type makes the server convert or
// truncate silently, which is worse than a clear error.
[[nodiscard]] result<encoded_parameter> encode_parameter(const parameter& param) {
    encoded_parameter out;
    const bool sends = param.sends_value() && !param.is_null;
    switch (param.type) {
    case db_type::int64: {
        out.info.type = 0x26; // INTN
        out.info.max_length = 8;
        out.declaration = "bigint";
        if (sends) {
            put_le64_bytes(out.value, static_cast<std::uint64_t>(param.int_value));
        }
        break;
    }
    case db_type::uint64: {
        if (param.uint_value <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            out.info.type = 0x26;
            out.info.max_length = 8;
            out.declaration = "bigint";
            if (sends) {
                put_le64_bytes(out.value, param.uint_value);
            }
        } else {
            // SQL Server has no unsigned 64-bit integer; a value above INT64_MAX is
            // carried as DECIMAL(20,0), which holds every uint64 exactly.
            out.info.type = 0x6A; // DECIMALN
            out.info.precision = 20;
            out.info.scale = 0;
            out.info.max_length = 17;
            out.declaration = "decimal(20,0)";
            if (sends) {
                std::vector<std::byte> encoded;
                std::uint8_t precision = out.info.precision;
                std::uint8_t scale = out.info.scale;
                const status decimal =
                    encode_decimal_value(encoded, std::to_string(param.uint_value), precision, scale);
                if (!decimal) {
                    return decimal.error();
                }
                out.value = std::move(encoded);
            }
        }
        break;
    }
    case db_type::boolean: {
        out.info.type = 0x68; // BITN
        out.info.max_length = 1;
        out.declaration = "bit";
        if (sends) {
            out.value.push_back(static_cast<std::byte>(param.int_value != 0 ? 1 : 0));
        }
        break;
    }
    case db_type::real: {
        out.info.type = 0x6D; // FLTN
        out.info.max_length = 8;
        out.declaration = "float";
        if (sends) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &param.real_value, sizeof(bits));
            put_le64_bytes(out.value, bits);
        }
        break;
    }
    case db_type::text: {
        const std::vector<std::byte> encoded = utf16le(param.text);
        const std::size_t characters = encoded.size() / 2;
        out.info.type = 0xE7; // NVARCHAR
        out.info.collation = {0x09, 0x04, 0xD0, 0x00, 0x34};
        if (characters <= 4000) {
            out.info.max_length = static_cast<std::uint32_t>(characters * 2);
            out.declaration = "nvarchar(" + std::to_string(characters == 0 ? 1 : characters) + ")";
        } else {
            out.info.max_length = 0xFFFF; // NVARCHAR(MAX)
            out.declaration = "nvarchar(max)";
        }
        if (sends) {
            out.value = encoded;
        }
        break;
    }
    case db_type::blob: {
        out.info.type = 0xA5; // BIGVARBINARY
        if (param.blob.size() <= 8000) {
            out.info.max_length = static_cast<std::uint32_t>(param.blob.size());
            out.declaration =
                "varbinary(" + std::to_string(param.blob.empty() ? 1 : param.blob.size()) + ")";
        } else {
            out.info.max_length = 0xFFFF;
            out.declaration = "varbinary(max)";
        }
        if (sends) {
            out.value = param.blob;
        }
        break;
    }
    case db_type::decimal: {
        out.info.type = 0x6A; // DECIMALN
        if (sends) {
            const status decimal =
                encode_decimal_value(out.value, param.text, out.info.precision, out.info.scale);
            if (!decimal) {
                return decimal.error();
            }
        } else {
            out.info.precision = 38;
            out.info.scale = 0;
        }
        out.info.max_length = out.info.precision <= 9    ? 5
                              : out.info.precision <= 19 ? 9
                              : out.info.precision <= 28 ? 13
                                                         : 17;
        out.declaration = "decimal(" + std::to_string(out.info.precision) + "," +
                          std::to_string(out.info.scale) + ")";
        break;
    }
    case db_type::uuid: {
        out.info.type = 0x24; // GUID
        out.info.max_length = 16;
        out.declaration = "uniqueidentifier";
        if (sends) {
            const status guid = encode_guid_value(out.value, param.text);
            if (!guid) {
                return guid.error();
            }
        }
        break;
    }
    case db_type::date: {
        out.info.type = 0x28; // DATE
        out.info.max_length = 3;
        out.declaration = "date";
        if (sends) {
            const status date = encode_date_value(out.value, param.text);
            if (!date) {
                return date.error();
            }
        }
        break;
    }
    case db_type::time: {
        out.info.type = 0x29; // TIME
        out.info.scale = 7;
        out.info.max_length = 5;
        out.declaration = "time(7)";
        if (sends) {
            const status time = encode_time_value(out.value, param.text, out.info.scale);
            if (!time) {
                return time.error();
            }
        }
        break;
    }
    case db_type::timestamp: {
        out.info.type = 0x2A; // DATETIME2
        out.info.scale = 7;
        out.info.max_length = 8;
        out.declaration = "datetime2(7)";
        if (sends) {
            const status stamp = encode_datetime2_value(out.value, param.text, out.info.scale);
            if (!stamp) {
                return stamp.error();
            }
        }
        break;
    }
    default:
        return rpc_error{rpc_code::invalid_argument,
                         "this parameter type has no TDS representation"};
    }
    // A parameter the server writes, or one the caller left NULL, sends no value.
    out.is_null = !sends;
    return out;
}

// Rewrites each top-level `?` into @P1, @P2, ... so one statement can go to
// sp_executesql. The scanner understands SQL quoting: replacing every '?' would
// corrupt a literal like 'a?b', turning a bound statement into a data bug.
[[nodiscard]] status bind_placeholders(std::string_view sql, std::size_t count,
                                       std::string& out) {
    out.clear();
    out.reserve(sql.size() + 8 * count);
    std::size_t index = 0;
    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char c = sql[i];
        if (c == '\'' || c == '"' || c == '[') {
            const char closing = c == '[' ? ']' : c;
            out.push_back(c);
            ++i;
            while (i < sql.size()) {
                out.push_back(sql[i]);
                if (c != '[' && sql[i] == '\\' && i + 1 < sql.size()) {
                    ++i;
                    out.push_back(sql[i]);
                } else if (sql[i] == closing) {
                    if (i + 1 < sql.size() && sql[i + 1] == closing) {
                        ++i;
                        out.push_back(sql[i]);
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
                out.push_back(sql[i]);
                ++i;
            }
            if (i < sql.size()) {
                out.push_back(sql[i]);
            }
            continue;
        }
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            out += "/*";
            i += 2;
            while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) {
                out.push_back(sql[i]);
                ++i;
            }
            if (i + 1 < sql.size()) {
                out += "*/";
                ++i;
            }
            continue;
        }
        if (c == '?') {
            if (index >= count) {
                return status{rpc_code::invalid_argument,
                              "the statement has more placeholders than parameters"};
            }
            out += "@P";
            out += std::to_string(index + 1);
            ++index;
            continue;
        }
        out.push_back(c);
    }
    if (index != count) {
        return status{rpc_code::invalid_argument,
                      "the statement has fewer placeholders than parameters"};
    }
    return status{};
}

// Applies a value the server wrote back to the caller's parameter, in the type the
// caller declared.
void apply_return_value(const return_value& value, parameter& target) {
    target.is_null = value.is_null;
    if (value.is_null) {
        return;
    }
    switch (target.type) {
    case db_type::int64:
    case db_type::uint64:
    case db_type::boolean: {
        char* end = nullptr;
        const long long parsed = std::strtoll(value.value.c_str(), &end, 10);
        target.int_value = parsed;
        target.uint_value = static_cast<std::uint64_t>(parsed);
        break;
    }
    case db_type::real: {
        char* end = nullptr;
        target.real_value = std::strtod(value.value.c_str(), &end);
        break;
    }
    case db_type::blob:
        target.blob.assign(reinterpret_cast<const std::byte*>(value.value.data()),
                           reinterpret_cast<const std::byte*>(value.value.data()) +
                               value.value.size());
        break;
    default:
        target.text = value.value;
        break;
    }
}

} // namespace

status connection::call_impl(std::string_view sql, span<const parameter> params,
                             result_set** out, bool procedure,
                             std::vector<return_value>& written) {
    std::vector<encoded_parameter> encoded;
    encoded.reserve(params.size());
    for (const parameter& param : params) {
        result<encoded_parameter> one = encode_parameter(param);
        if (!one) {
            return one.error();
        }
        encoded.push_back(std::move(one.value()));
    }

    rpc_request request;
    if (procedure) {
        request.proc_name = std::string(sql);
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            rpc_parameter wire;
            // TDS parameter names are @-prefixed, and the server matches on the name
            // it declared: a caller writing "sum" means "@sum".
            wire.name = params[i].name;
            if (!wire.name.empty() && wire.name[0] != '@') {
                wire.name.insert(wire.name.begin(), '@');
            }
            wire.status = params[i].is_output() ? 0x01 : 0x00;
            wire.info = encoded[i].info;
            wire.value = std::move(encoded[i].value);
            wire.is_null = encoded[i].is_null;
            request.params.push_back(std::move(wire));
        }
    } else {
        // sp_executesql: @stmt, @params, then the bound values. The statement's `?`
        // markers become @P1..@Pn so the text and the declaration agree.
        std::string statement;
        const status bound = bind_placeholders(sql, params.size(), statement);
        if (!bound) {
            return bound;
        }
        std::string declaration;
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            if (i != 0) {
                declaration.push_back(',');
            }
            declaration += "@P";
            declaration += std::to_string(i + 1);
            declaration.push_back(' ');
            declaration += encoded[i].declaration;
            if (params[i].is_output()) {
                declaration += " OUTPUT";
            }
        }

        request.proc_name = "sp_executesql";

        rpc_parameter stmt;
        stmt.info.type = 0xE7;
        stmt.info.max_length = 0xFFFF;
        stmt.info.collation = {0x09, 0x04, 0xD0, 0x00, 0x34};
        stmt.value = utf16le(statement);
        request.params.push_back(std::move(stmt));

        rpc_parameter decl;
        decl.info.type = 0xE7;
        decl.info.max_length = 0xFFFF;
        decl.info.collation = {0x09, 0x04, 0xD0, 0x00, 0x34};
        decl.value = utf16le(declaration);
        request.params.push_back(std::move(decl));

        for (std::size_t i = 0; i < encoded.size(); ++i) {
            rpc_parameter wire;
            wire.name = "@P" + std::to_string(i + 1);
            wire.status = params[i].is_output() ? 0x01 : 0x00;
            wire.info = encoded[i].info;
            wire.value = std::move(encoded[i].value);
            wire.is_null = encoded[i].is_null;
            request.params.push_back(std::move(wire));
        }
    }

    return run_rpc(request, out, &written);
}

status connection::query(std::string_view sql, span<const parameter> params, result_set** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (!open_) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    if (params.empty()) {
        return run_batch(sql, out);
    }
    // A query's parameters are const by contract, so an OUTPUT marker is rejected
    // instead of being silently treated as input.
    for (const parameter& param : params) {
        if (param.is_output()) {
            return status{rpc_code::invalid_argument,
                          "query() cannot return an output parameter; use call()"};
        }
    }
    std::vector<return_value> ignored;
    return call_impl(sql, params, out, false, ignored);
}

status connection::call(std::string_view procedure, span<parameter> params, result_set** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (!open_) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    std::vector<return_value> written;
    const status ran = call_impl(procedure, span<const parameter>(params.data(), params.size()),
                                out, true, written);
    if (!ran || written.empty()) {
        return ran;
    }
    // Match by name first: the server echoes the name it was given, and the wire
    // ordinal is zero-based in practice (a three-parameter call returned 2 for the
    // third parameter), so a fixed +-1 assumption is wrong either way.
    const auto strip_at = [](std::string_view name) {
        return !name.empty() && name.front() == '@' ? name.substr(1) : name;
    };
    for (const return_value& value : written) {
        std::size_t match = params.size();
        if (!value.name.empty()) {
            for (std::size_t i = 0; i < params.size(); ++i) {
                if (!params[i].name.empty() &&
                    strip_at(params[i].name) == strip_at(value.name)) {
                    match = i;
                    break;
                }
            }
        }
        if (match == params.size()) {
            // Fall back to the ordinal, treating it as zero-based when that lands on
            // a parameter and one-based otherwise.
            if (value.ordinal < params.size()) {
                match = value.ordinal;
            } else if (value.ordinal >= 1 && value.ordinal <= params.size()) {
                match = static_cast<std::size_t>(value.ordinal) - 1;
            }
        }
        if (match >= params.size()) {
            continue;
        }
        apply_return_value(value, params[match]);
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