// CRPC -- TDS connection tests over a scripted server.
//
// The scenarios that cannot be reproduced against a live server on demand are the
// ones that matter: a login rejection, a reply split across packets, an error
// followed by more tokens, a reply with several result sets. Each is scripted here
// byte for byte, so the assertion is about the client's state machine rather than
// about the server's mood.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/db/connection.hpp>
#include <crpc/db/tds/connection.hpp>
#include <crpc/db/tds/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::channel;
using crpc::db::result_set;
using crpc::db::statement_guard;
using crpc::db::tds::packet_header;
using crpc::db::tds::packet_status;
using crpc::db::tds::packet_type;
using crpc::db::tds::token_type;

using bytes = std::vector<std::byte>;

[[nodiscard]] std::uint8_t byte_at(const bytes& data, std::size_t index) {
    return static_cast<std::uint8_t>(data[index]);
}

void put_u8(bytes& out, std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }

void put_u16_be(bytes& out, std::uint16_t value) {
    put_u8(out, static_cast<std::uint8_t>((value >> 8) & 0xffu));
    put_u8(out, static_cast<std::uint8_t>(value & 0xffu));
}

void put_u16_le(bytes& out, std::uint16_t value) {
    put_u8(out, static_cast<std::uint8_t>(value & 0xffu));
    put_u8(out, static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void put_u32_le(bytes& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        put_u8(out, static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
    }
}

void put_u32_be(bytes& out, std::uint32_t value) {
    put_u8(out, static_cast<std::uint8_t>((value >> 24) & 0xffu));
    put_u8(out, static_cast<std::uint8_t>((value >> 16) & 0xffu));
    put_u8(out, static_cast<std::uint8_t>((value >> 8) & 0xffu));
    put_u8(out, static_cast<std::uint8_t>(value & 0xffu));
}

void put_u64_le(bytes& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        put_u8(out, static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
    }
}

void put_utf16(bytes& out, std::string_view text) {
    const bytes encoded = crpc::db::tds::utf16le(text);
    out.insert(out.end(), encoded.begin(), encoded.end());
}

void put_b_varchar(bytes& out, std::string_view text) {
    put_u8(out, static_cast<std::uint8_t>(text.size()));
    put_utf16(out, text);
}

void put_us_varchar(bytes& out, std::string_view text) {
    put_u16_be(out, static_cast<std::uint16_t>(text.size()));
    put_utf16(out, text);
}

// Frames one message into packets of `packet_size`, setting eom on the last one.
[[nodiscard]] bytes frame(packet_type type, const bytes& payload, std::size_t packet_size) {
    bytes out;
    const std::size_t chunk_max = packet_size > 8 ? packet_size - 8 : payload.size();
    std::size_t offset = 0;
    std::uint8_t id = 1;
    do {
        const std::size_t chunk = std::min(payload.size() - offset, chunk_max);
        packet_header header;
        header.type = type;
        header.length = static_cast<std::uint16_t>(8 + chunk);
        header.packet_id = id++;
        header.status = (offset + chunk >= payload.size())
                            ? static_cast<std::uint8_t>(packet_status::eom)
                            : std::uint8_t{0};
        std::byte head[8]{};
        std::size_t written = 0;
        const crpc::status encoded =
            crpc::db::tds::encode_header(crpc::span<std::byte>(head, sizeof head), header, written);
        CRPC_CHECK(static_cast<bool>(encoded));
        CRPC_CHECK_EQ(std::size_t{8}, written);
        out.insert(out.end(), head, head + 8);
        out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(offset),
                   payload.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
        offset += chunk;
    } while (offset < payload.size());
    return out;
}

// --- token encoders (the library only decodes them) --------------------------

void put_done(bytes& out, std::uint16_t status, std::uint64_t row_count) {
    put_u8(out, static_cast<std::uint8_t>(token_type::done));
    put_u16_le(out, status);
    put_u16_le(out, 0); // current command
    put_u64_le(out, row_count);
}

void put_error(bytes& out, std::uint32_t number, std::uint8_t severity,
               std::string_view message) {
    bytes body;
    put_u32_le(body, number);
    put_u8(body, 1); // state
    put_u8(body, severity);
    put_us_varchar(body, message);
    put_b_varchar(body, "server");
    put_b_varchar(body, "");
    put_u32_le(body, 1); // line
    put_u8(out, static_cast<std::uint8_t>(token_type::error));
    put_u16_be(out, static_cast<std::uint16_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

void put_login_ack(bytes& out) {
    bytes body;
    put_u8(body, 1); // interface: SQL
    put_u32_be(body, 0x74000004u);
    put_b_varchar(body, "Microsoft SQL Server");
    put_u32_be(body, 0x0F00A1B2u); // server version 15.0.161.178
    put_u8(out, static_cast<std::uint8_t>(token_type::loginack));
    put_u16_be(out, static_cast<std::uint16_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

void put_packet_size_change(bytes& out, std::string_view new_size) {
    bytes body;
    put_u8(body, static_cast<std::uint8_t>(crpc::db::tds::env_change_type::packet_size));
    put_b_varchar(body, new_size);
    put_b_varchar(body, "4096");
    put_u8(out, static_cast<std::uint8_t>(token_type::envchange));
    put_u16_be(out, static_cast<std::uint16_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

struct column_spec {
    std::uint8_t type;
    std::uint16_t max_length;
    const char* name;
};

void put_colmetadata(bytes& out, const std::vector<column_spec>& columns) {
    put_u8(out, static_cast<std::uint8_t>(token_type::colmetadata));
    put_u16_be(out, static_cast<std::uint16_t>(columns.size()));
    for (const column_spec& column : columns) {
        put_u32_le(out, 0); // user type
        put_u16_le(out, 1); // flags
        put_u8(out, column.type);
        if (column.type == 0xE7 || column.type == 0xA7) {
            put_u16_be(out, column.max_length);
            for (int i = 0; i < 5; ++i) {
                put_u8(out, 0); // collation
            }
        }
        put_b_varchar(out, column.name);
    }
}

void put_int4_row(bytes& out, std::int32_t value) {
    put_u8(out, static_cast<std::uint8_t>(token_type::row));
    put_u32_le(out, static_cast<std::uint32_t>(value));
}

// --- scripted server ---------------------------------------------------------

class scripted_channel final : public channel {
public:
    // Returns the reply payload for one complete request message. An empty vector
    // means "no reply", which is how a silent server is modelled.
    using responder =
        std::function<bytes(packet_type, crpc::span<const std::byte>)>;

    explicit scripted_channel(responder respond) : respond_(std::move(respond)) {}

    [[nodiscard]] crpc::result<bytes> read_exact(std::size_t count) override {
        if (closed_) {
            return crpc::rpc_error{crpc::rpc_code::eof, "the scripted channel is closed"};
        }
        if (count > in_.size() - read_pos_) {
            return crpc::rpc_error{crpc::rpc_code::eof,
                                   "the scripted server has no more bytes to send"};
        }
        bytes out(in_.begin() + static_cast<std::ptrdiff_t>(read_pos_),
                  in_.begin() + static_cast<std::ptrdiff_t>(read_pos_ + count));
        read_pos_ += count;
        return out;
    }

    crpc::status write(crpc::span<const std::byte> data) override {
        if (closed_) {
            return crpc::status{crpc::rpc_code::shutting_down, "the scripted channel is closed"};
        }
        written_ += data.size();
        pending_.insert(pending_.end(), data.begin(), data.end());
        drain();
        return crpc::status{};
    }

    void close() noexcept override { closed_ = true; }

    [[nodiscard]] std::size_t bytes_written() const noexcept { return written_; }
    [[nodiscard]] bool is_closed() const noexcept { return closed_; }
    [[nodiscard]] std::size_t server_packet_size() const noexcept { return server_packet_size_; }
    void set_server_packet_size(std::size_t value) noexcept { server_packet_size_ = value; }

private:
    void drain() {
        for (;;) {
            if (pending_.size() - consumed_ < 8) {
                return;
            }
            const std::size_t base = consumed_;
            const std::uint16_t length =
                static_cast<std::uint16_t>((byte_at(pending_, base + 2) << 8) |
                                           byte_at(pending_, base + 3));
            if (pending_.size() - consumed_ < length) {
                return;
            }
            const auto type = static_cast<packet_type>(byte_at(pending_, base));
            const std::uint8_t status = byte_at(pending_, base + 1);
            message_.insert(message_.end(),
                            pending_.begin() + static_cast<std::ptrdiff_t>(base + 8),
                            pending_.begin() + static_cast<std::ptrdiff_t>(base + length));
            consumed_ += length;
            if ((status & static_cast<std::uint8_t>(packet_status::eom)) == 0) {
                continue;
            }
            const bytes reply =
                respond_(type, crpc::span<const std::byte>(message_.data(), message_.size()));
            message_.clear();
            if (!reply.empty()) {
                const bytes framed = frame(packet_type::tabular_result, reply, server_packet_size_);
                in_.insert(in_.end(), framed.begin(), framed.end());
            }
        }
    }

    responder respond_;
    bytes pending_;
    std::size_t consumed_ = 0;
    bytes message_;
    bytes in_;
    std::size_t read_pos_ = 0;
    std::size_t written_ = 0;
    std::size_t server_packet_size_ = 4096;
    bool closed_ = false;
};

[[nodiscard]] crpc::db::tds::connection_options options_for_tests() {
    crpc::db::tds::connection_options options;
    options.user = "sa";
    options.password = "pw";
    options.database = "master";
    options.packet_size = 4096;
    return options;
}

// The standard login handshake: prelogin echo plus LOGINACK, an ENVCHANGE that
// lowers the packet size, and DONE.
[[nodiscard]] scripted_channel::responder login_server() {
    return [](packet_type type, crpc::span<const std::byte>) -> bytes {
        bytes reply;
        if (type == packet_type::prelogin) {
            crpc::db::tds::prelogin answer;
            answer.encryption = crpc::db::tds::encryption_mode::not_supported;
            (void)crpc::db::tds::encode_prelogin(reply, answer);
            return reply;
        }
        if (type == packet_type::login7) {
            put_login_ack(reply);
            put_packet_size_change(reply, "8192");
            put_done(reply, 0, 0);
            return reply;
        }
        return reply;
    };
}

[[nodiscard]] std::unique_ptr<crpc::db::tds::connection> open_with(
    scripted_channel::responder respond, scripted_channel** channel_out = nullptr) {
    auto link = std::make_unique<scripted_channel>(std::move(respond));
    scripted_channel* raw = link.get();
    if (channel_out != nullptr) {
        *channel_out = raw;
    }
    return std::unique_ptr<crpc::db::tds::connection>(
        new crpc::db::tds::connection(options_for_tests(), std::move(link)));
}

} // namespace

CRPC_TEST(tds_connection_login_succeeds_and_learns_packet_size) {
    scripted_channel* server = nullptr;
    auto session = open_with(login_server(), &server);
    CRPC_CHECK(session->is_open());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                  static_cast<int>(session->connect_error().code));
    // The server lowered the packet size; framing must use the negotiated value.
    CRPC_CHECK_EQ(std::uint16_t{8192}, session->negotiated_packet_size());
    CRPC_CHECK_FALSE(session->encryption_negotiated());
    CRPC_CHECK_EQ(std::uint32_t{0x0F00A1B2u}, session->server_version());
    CRPC_CHECK(server->bytes_written() > 0);
}

CRPC_TEST(tds_connection_login_rejection_reports_the_server_message) {
    auto respond = [](packet_type type, crpc::span<const std::byte>) -> bytes {
        bytes reply;
        if (type == packet_type::prelogin) {
            crpc::db::tds::prelogin answer;
            answer.encryption = crpc::db::tds::encryption_mode::not_supported;
            (void)crpc::db::tds::encode_prelogin(reply, answer);
            return reply;
        }
        put_error(reply, 18456, 14, "Login failed for user 'sa'.");
        put_done(reply, 0x0002, 0);
        return reply;
    };
    auto session = open_with(respond);
    CRPC_CHECK_FALSE(session->is_open());
    // The message must be the server's own: a generic "login failed" hides whether
    // it was a bad password or a disabled account.
    CRPC_CHECK_EQ(std::string("Login failed for user 'sa'."), session->connect_error().message);
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::tds::map_error_number(18456)),
                  static_cast<int>(session->connect_error().code));
}

CRPC_TEST(tds_connection_refuses_a_server_that_requires_encryption) {
    auto respond = [](packet_type type, crpc::span<const std::byte>) -> bytes {
        if (type != packet_type::prelogin) {
            return {};
        }
        bytes reply;
        crpc::db::tds::prelogin answer;
        answer.encryption = crpc::db::tds::encryption_mode::required;
        (void)crpc::db::tds::encode_prelogin(reply, answer);
        return reply;
    };
    auto session = open_with(respond);
    CRPC_CHECK_FALSE(session->is_open());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(session->connect_error().code));
    CRPC_CHECK(session->connect_error().message.find("TLS") != std::string::npos);
}

CRPC_TEST(tds_connection_refuses_require_encryption_without_tls) {
    crpc::db::tds::connection_options options = options_for_tests();
    options.require_encryption = true;
    auto link = std::make_unique<scripted_channel>(login_server());
    scripted_channel* raw = link.get();
    crpc::db::tds::connection session(options, std::move(link));
    CRPC_CHECK_FALSE(session.is_open());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(session.connect_error().code));
    // Nothing may leave the client: a plaintext login the caller believes is
    // encrypted is worse than a refusal.
    CRPC_CHECK_EQ(std::size_t{0}, raw->bytes_written());
}

CRPC_TEST(tds_connection_query_returns_rows_and_metadata) {
    auto respond = login_server();
    auto session = open_with([respond](packet_type type, crpc::span<const std::byte> payload) {
        if (type != packet_type::sql_batch) {
            return respond(type, payload);
        }
        bytes reply;
        put_colmetadata(reply, {{0x38, 0, "id"}, {0xE7, 40, "name"}});
        put_u8(reply, static_cast<std::uint8_t>(token_type::row));
        put_u32_le(reply, 7);
        put_u16_be(reply, 4);
        put_utf16(reply, "ab");
        put_u8(reply, static_cast<std::uint8_t>(token_type::row));
        put_u32_le(reply, 8);
        put_u16_be(reply, 2);
        put_utf16(reply, "c");
        put_done(reply, 0, 2);
        return reply;
    });

    result_set* raw = nullptr;
    const crpc::status ran = session->query("SELECT id, name FROM t", {}, &raw);
    statement_guard guard(raw);
    CRPC_CHECK(static_cast<bool>(ran));
    CRPC_CHECK(raw != nullptr);
    CRPC_CHECK_EQ(std::size_t{2}, raw->column_count());
    CRPC_CHECK_EQ(std::string("id"), raw->column(0).name);
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::int64),
                  static_cast<int>(raw->column(0).type));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::text),
                  static_cast<int>(raw->column(1).type));

    CRPC_CHECK(raw->next());
    CRPC_CHECK_EQ(std::int64_t{7}, raw->get_int64(0).value());
    CRPC_CHECK_EQ(std::string("ab"), raw->get_text(1).value());
    CRPC_CHECK(raw->next());
    CRPC_CHECK_EQ(std::int64_t{8}, raw->get_int64(0).value());
    CRPC_CHECK_EQ(std::string("c"), raw->get_text(1).value());
    CRPC_CHECK_FALSE(raw->next());
    CRPC_CHECK_EQ(std::int64_t{2}, raw->affected_rows());
}

CRPC_TEST(tds_connection_reads_a_reply_split_across_packets) {
    // A result taller than the packet size is ordinary. Stopping at end-of-packet
    // would truncate it, so the reader must keep going until DONE.
    auto tall = [](packet_type type, crpc::span<const std::byte> payload) -> bytes {
        if (type == packet_type::prelogin) {
            bytes reply;
            crpc::db::tds::prelogin answer;
            answer.encryption = crpc::db::tds::encryption_mode::not_supported;
            (void)crpc::db::tds::encode_prelogin(reply, answer);
            return reply;
        }
        if (type == packet_type::login7) {
            bytes reply;
            put_login_ack(reply);
            put_done(reply, 0, 0);
            return reply;
        }
        (void)payload;
        bytes reply;
        put_colmetadata(reply, {{0x38, 0, "id"}});
        for (int i = 0; i < 40; ++i) {
            put_int4_row(reply, i);
        }
        put_done(reply, 0, 40);
        return reply;
    };
    auto link = std::make_unique<scripted_channel>(tall);
    // 64 bytes per packet means the 40-row reply spans many packets.
    link->set_server_packet_size(64);
    crpc::db::tds::connection session(options_for_tests(), std::move(link));
    CRPC_CHECK(session.is_open());

    result_set* rows = nullptr;
    const crpc::status ran = session.query("SELECT id FROM t", {}, &rows);
    statement_guard row_guard(rows);
    CRPC_CHECK(static_cast<bool>(ran));
    CRPC_CHECK(rows != nullptr);
    std::int64_t count = 0;
    std::int64_t last = -1;
    while (rows->next()) {
        last = rows->get_int64(0).value();
        ++count;
    }
    CRPC_CHECK_EQ(std::int64_t{40}, count);
    CRPC_CHECK_EQ(std::int64_t{39}, last);
}

CRPC_TEST(tds_connection_drains_an_error_and_stays_usable) {
    int batch = 0;
    auto respond = login_server();
    auto session = open_with([respond, &batch](packet_type type,
                                               crpc::span<const std::byte> payload) {
        if (type != packet_type::sql_batch) {
            return respond(type, payload);
        }
        ++batch;
        bytes reply;
        if (batch == 1) {
            put_error(reply, 208, 16, "Invalid object name 'nope'.");
            put_done(reply, 0x0002, 0);
            return reply;
        }
        put_colmetadata(reply, {{0x38, 0, "id"}});
        put_int4_row(reply, 1);
        put_done(reply, 0, 1);
        return reply;
    });

    result_set* first = nullptr;
    const crpc::status failed = session->query("SELECT * FROM nope", {}, &first);
    statement_guard first_guard(first);
    CRPC_CHECK_FALSE(static_cast<bool>(failed));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found), static_cast<int>(failed.error().code));
    CRPC_CHECK(first == nullptr);
    CRPC_CHECK(session->is_open());

    // The error was drained, so the next reply is decoded as the next reply.
    result_set* second = nullptr;
    const crpc::status ran = session->query("SELECT 1", {}, &second);
    statement_guard second_guard(second);
    CRPC_CHECK(static_cast<bool>(ran));
    CRPC_CHECK(second != nullptr);
    CRPC_CHECK(second->next());
    CRPC_CHECK_EQ(std::int64_t{1}, second->get_int64(0).value());
}

CRPC_TEST(tds_connection_returns_the_first_result_set_and_discards_the_rest) {
    int batch = 0;
    auto respond = login_server();
    auto session = open_with([respond, &batch](packet_type type,
                                               crpc::span<const std::byte> payload) {
        if (type != packet_type::sql_batch) {
            return respond(type, payload);
        }
        ++batch;
        bytes reply;
        if (batch == 1) {
            put_colmetadata(reply, {{0x38, 0, "a"}});
            put_int4_row(reply, 1);
            // 0x0001 is "more results": a second set follows in the same message.
            put_done(reply, 0x0001, 1);
            put_colmetadata(reply, {{0x38, 0, "b"}});
            put_int4_row(reply, 2);
            put_done(reply, 0, 1);
            return reply;
        }
        put_colmetadata(reply, {{0x38, 0, "c"}});
        put_int4_row(reply, 7);
        put_done(reply, 0, 1);
        return reply;
    });

    result_set* rows = nullptr;
    const crpc::status ran = session->query("SELECT a; SELECT b", {}, &rows);
    statement_guard guard(rows);
    CRPC_CHECK(static_cast<bool>(ran));
    CRPC_CHECK(rows != nullptr);
    CRPC_CHECK_EQ(std::size_t{1}, rows->column_count());
    CRPC_CHECK_EQ(std::string("a"), rows->column(0).name);
    CRPC_CHECK(rows->next());
    CRPC_CHECK_EQ(std::int64_t{1}, rows->get_int64(0).value());
    CRPC_CHECK_FALSE(rows->next());

    // The discarded second set was consumed too: the next reply must be read as the
    // next reply, not as a tail of the previous one.
    result_set* again = nullptr;
    const crpc::status second = session->query("SELECT c", {}, &again);
    statement_guard again_guard(again);
    CRPC_CHECK(static_cast<bool>(second));
    CRPC_CHECK(again != nullptr);
    CRPC_CHECK_EQ(std::string("c"), again->column(0).name);
    CRPC_CHECK(again->next());
    CRPC_CHECK_EQ(std::int64_t{7}, again->get_int64(0).value());
}

CRPC_TEST(tds_connection_refuses_parameters) {
    auto session = open_with(login_server());
    crpc::db::parameter param = crpc::db::parameter::null();
    result_set* rows = nullptr;
    const crpc::span<const crpc::db::parameter> params(&param, 1);
    const crpc::status ran = session->query("SELECT ?", params, &rows);
    CRPC_CHECK_FALSE(static_cast<bool>(ran));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(ran.error().code));
    CRPC_CHECK(rows == nullptr);
}

CRPC_TEST(tds_connection_tracks_transactions_locally) {
    auto respond = login_server();
    auto session = open_with([respond](packet_type type, crpc::span<const std::byte> payload) {
        if (type != packet_type::sql_batch) {
            return respond(type, payload);
        }
        bytes reply;
        put_done(reply, 0, 0);
        return reply;
    });
    CRPC_CHECK_FALSE(session->in_transaction());
    CRPC_CHECK(static_cast<bool>(session->begin()));
    CRPC_CHECK(session->in_transaction());
    CRPC_CHECK(static_cast<bool>(session->commit()));
    CRPC_CHECK_FALSE(session->in_transaction());
    CRPC_CHECK(static_cast<bool>(session->begin()));
    CRPC_CHECK(static_cast<bool>(session->rollback()));
    CRPC_CHECK_FALSE(session->in_transaction());
    CRPC_CHECK(static_cast<bool>(session->ping()));
}

CRPC_TEST(tds_connection_reports_closed_after_close) {
    auto session = open_with(login_server());
    CRPC_CHECK(session->is_open());
    session->close();
    CRPC_CHECK_FALSE(session->is_open());
    CRPC_CHECK_FALSE(session->in_transaction());
    result_set* rows = nullptr;
    const crpc::status ran = session->query("SELECT 1", {}, &rows);
    CRPC_CHECK_FALSE(static_cast<bool>(ran));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down),
                  static_cast<int>(ran.error().code));
}

CRPC_TEST(tds_connection_factory_reports_the_login_failure) {
    auto factory = crpc::db::tds::channel_factory(options_for_tests(), [] {
        auto link = std::make_unique<scripted_channel>(
            [](packet_type type, crpc::span<const std::byte>) -> bytes {
                bytes reply;
                if (type == packet_type::prelogin) {
                    crpc::db::tds::prelogin answer;
                    answer.encryption = crpc::db::tds::encryption_mode::not_supported;
                    (void)crpc::db::tds::encode_prelogin(reply, answer);
                    return reply;
                }
                put_error(reply, 18456, 14, "Login failed for user 'sa'.");
                put_done(reply, 0x0002, 0);
                return reply;
            });
        return crpc::result<std::unique_ptr<channel>>(std::move(link));
    });
    auto opened = factory.open();
    CRPC_CHECK_FALSE(static_cast<bool>(opened));
    CRPC_CHECK_EQ(std::string("Login failed for user 'sa'."), opened.error().message);
    CRPC_CHECK_EQ(std::string_view("tds-injected"), factory.name());
}

CRPC_TEST_MAIN()