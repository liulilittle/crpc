// CRPC -- LOAD DATA LOCAL INFILE streaming tests.
//
// The security property is the point: the client streams bytes it was given and
// never opens the path the server named. The tests also pin the packet sequence,
// because a data stream that is one packet short hangs the server and one packet
// long corrupts the next command.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/db/mysql/channel.hpp>
#include <crpc/db/mysql/connection.hpp>
#include <crpc/db/mysql/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::mysql::capability;
using crpc::db::mysql::connection;
using crpc::db::mysql::connection_options;
using crpc::db::mysql::local_data_source;
using crpc::db::mysql::mask;
using crpc::db::mysql::slice_data_source;

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xffu));
}

void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xffu));
    }
}

void append_text(std::vector<std::byte>& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
}

[[nodiscard]] std::vector<std::byte> packet(const std::vector<std::byte>& payload,
                                            std::uint8_t sequence) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(payload.size() & 0xffu));
    out.push_back(static_cast<std::byte>((payload.size() >> 8) & 0xffu));
    out.push_back(static_cast<std::byte>((payload.size() >> 16) & 0xffu));
    out.push_back(static_cast<std::byte>(sequence));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

[[nodiscard]] std::vector<std::byte> ok_payload(std::uint64_t affected) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0});
    crpc::db::mysql::append_lenenc_int(payload, affected);
    crpc::db::mysql::append_lenenc_int(payload, 0);
    append_u16(payload, 2);
    append_u16(payload, 0);
    return payload;
}

[[nodiscard]] std::vector<std::byte> err_payload(std::uint16_t code, std::string_view message) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0xff});
    append_u16(payload, code);
    payload.push_back(std::byte{'#'});
    append_text(payload, "HY000");
    append_text(payload, message);
    return payload;
}

constexpr std::uint32_t server_capabilities =
    mask(capability::protocol_41) | mask(capability::secure_connection) |
    mask(capability::plugin_auth) | mask(capability::transactions);

[[nodiscard]] std::vector<std::byte> greeting_payload() {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{10});
    append_text(payload, "8.0.36");
    payload.push_back(std::byte{0});
    append_u32(payload, 42);
    for (int i = 0; i < 8; ++i) {
        payload.push_back(static_cast<std::byte>(i + 1));
    }
    payload.push_back(std::byte{0});
    append_u16(payload, static_cast<std::uint16_t>(server_capabilities & 0xffffu));
    payload.push_back(std::byte{45});
    append_u16(payload, 2);
    append_u16(payload, static_cast<std::uint16_t>((server_capabilities >> 16) & 0xffffu));
    payload.push_back(std::byte{21});
    for (int i = 0; i < 10; ++i) {
        payload.push_back(std::byte{0});
    }
    for (int i = 0; i < 12; ++i) {
        payload.push_back(static_cast<std::byte>(i + 9));
    }
    payload.push_back(std::byte{0});
    append_text(payload, "caching_sha2_password");
    payload.push_back(std::byte{0});
    return payload;
}

class scripted_channel final : public crpc::db::mysql::channel {
public:
    std::deque<std::vector<std::byte>> incoming;
    std::vector<std::vector<std::byte>> writes;
    bool closed = false;

    crpc::result<std::vector<std::byte>> read_exact(std::size_t count) override {
        std::vector<std::byte> out;
        out.reserve(count);
        while (out.size() < count) {
            if (incoming.empty()) {
                return crpc::rpc_error{crpc::rpc_code::eof, "scripted channel ran out"};
            }
            std::vector<std::byte>& front = incoming.front();
            const std::size_t take = std::min(count - out.size(), front.size());
            out.insert(out.end(), front.begin(),
                       front.begin() + static_cast<std::ptrdiff_t>(take));
            front.erase(front.begin(), front.begin() + static_cast<std::ptrdiff_t>(take));
            if (front.empty()) {
                incoming.pop_front();
            }
        }
        return out;
    }

    crpc::status write(crpc::span<const std::byte> data) override {
        writes.emplace_back(data.begin(), data.end());
        return crpc::status{};
    }

    void close() noexcept override { closed = true; }
    void push(std::vector<std::byte> data) { incoming.push_back(std::move(data)); }

    [[nodiscard]] std::uint8_t command_of(std::size_t index) const {
        return static_cast<std::uint8_t>(writes[index][4]);
    }
    // Payload length from the packet header, and the sequence byte.
    [[nodiscard]] std::size_t payload_size_of(std::size_t index) const {
        const std::vector<std::byte>& frame = writes[index];
        return static_cast<std::size_t>(frame[0]) |
               (static_cast<std::size_t>(frame[1]) << 8) |
               (static_cast<std::size_t>(frame[2]) << 16);
    }
    [[nodiscard]] std::uint8_t sequence_of(std::size_t index) const {
        return static_cast<std::uint8_t>(writes[index][3]);
    }
    [[nodiscard]] std::string payload_of(std::size_t index) const {
        const std::vector<std::byte>& frame = writes[index];
        return std::string(reinterpret_cast<const char*>(frame.data() + 4), frame.size() - 4);
    }
};

struct harness {
    scripted_channel* raw = nullptr;
    std::unique_ptr<connection> session;

    harness() {
        auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
        raw = scripted.get();
        raw->push(packet(greeting_payload(), 0));
        raw->push(packet(ok_payload(0), 2));
        connection_options options;
        options.user = "root";
        options.password = "";
        session.reset(new connection(options, std::move(scripted)));
    }
};

// A source that fails after `healthy` reads, to exercise the mid-stream abort.
class failing_source final : public local_data_source {
public:
    explicit failing_source(int healthy) : healthy_(healthy) {}

    crpc::result<std::size_t> read(crpc::span<std::byte> buffer) override {
        if (healthy_ <= 0) {
            return crpc::rpc_error{crpc::rpc_code::connection_reset, "source failed"};
        }
        --healthy_;
        const std::size_t count = std::min<std::size_t>(buffer.size(), 3);
        for (std::size_t i = 0; i < count; ++i) {
            buffer[i] = std::byte{'x'};
        }
        return count;
    }

private:
    int healthy_ = 0;
};

} // namespace

CRPC_TEST(load_data_streams_caller_bytes_then_terminates) {
    harness h;
    // The server asks for the file (0xfb) with sequence 1.
    h.raw->push(packet({std::byte{0xfb}}, 1));
    // After the data packets and the empty terminator: sequence 2, 3, 4, so the OK
    // is sequence 5.
    h.raw->push(packet(ok_payload(2), 5));

    // Eight bytes with a four-byte chunk size: two data packets, then the empty
    // terminator, so the server's OK carries sequence 5.
    const std::vector<std::byte> data = bytes_of("a\nb\nc\nd\n");
    slice_data_source source(crpc::span<const std::byte>(data.data(), data.size()));

    crpc::db::result_set* rows = nullptr;
    const crpc::status loaded =
        h.session->load_data("LOAD DATA LOCAL INFILE '/etc/passwd' INTO TABLE t",
                             source, &rows, 4);
    CRPC_CHECK(loaded.ok());
    if (!loaded.ok()) {
        return;
    }
    std::unique_ptr<crpc::db::result_set> guard(rows);
    CRPC_CHECK(guard != nullptr);
    if (guard != nullptr) {
        CRPC_CHECK_EQ(static_cast<std::int64_t>(2), guard->affected_rows());
    }

    // writes[0] handshake, [1] the query, [2] chunk "a\nb\n" (4 bytes), [3] "b\n"
    // is not a chunk: 4 bytes were sent first, then 4 more only if available.
    CRPC_CHECK_EQ(static_cast<int>(0x03), static_cast<int>(h.raw->command_of(1)));
    CRPC_CHECK_EQ(std::string("LOAD DATA LOCAL INFILE '/etc/passwd' INTO TABLE t"),
                  h.raw->payload_of(1).substr(1));
    // Two data packets of at most 4 bytes, then an empty terminator.
    CRPC_CHECK_EQ(std::size_t{4}, h.raw->payload_size_of(2));
    CRPC_CHECK_EQ(std::size_t{4}, h.raw->payload_size_of(3));
    CRPC_CHECK_EQ(std::size_t{0}, h.raw->payload_size_of(4));
    // The data packets continue the server's sequence.
    CRPC_CHECK_EQ(static_cast<int>(2), static_cast<int>(h.raw->sequence_of(2)));
    CRPC_CHECK_EQ(static_cast<int>(3), static_cast<int>(h.raw->sequence_of(3)));
    CRPC_CHECK_EQ(static_cast<int>(4), static_cast<int>(h.raw->sequence_of(4)));
}

CRPC_TEST(load_data_recognizes_statements_with_leading_whitespace) {
    // Regression: the leading-whitespace skip in is_load_data_local used to
    // compare against multi-character constants ('\\t' instead of '\t'), so a
    // statement beginning with a tab, CR or LF was silently not recognized as
    // LOAD DATA LOCAL INFILE and load_data refused it outright.
    harness h;
    // The server asks for the file (0xfb) at sequence 1; one 1-byte data
    // packet (sequence 2) and the empty terminator (sequence 3) follow, so the
    // OK is sequence 4.
    h.raw->push(packet({std::byte{0xfb}}, 1));
    h.raw->push(packet(ok_payload(1), 4));

    const std::vector<std::byte> data = bytes_of("x");
    slice_data_source source(crpc::span<const std::byte>(data.data(), data.size()));
    crpc::db::result_set* rows = nullptr;
    const crpc::status loaded = h.session->load_data(
        "\t\r\n LOAD DATA LOCAL INFILE 'x' INTO TABLE t", source, &rows, 16);
    CRPC_CHECK(loaded.ok());
    if (!loaded.ok()) {
        std::printf("  error: %s\n", loaded.message().c_str());
        return;
    }
    // The wire keeps the caller's statement verbatim: only the recognition
    // step skips the leading whitespace, the SQL text is sent as-is.
    CRPC_CHECK_EQ(std::string("\t\r\n LOAD DATA LOCAL INFILE 'x' INTO TABLE t"),
                  h.raw->payload_of(1).substr(1));
}

CRPC_TEST(load_data_sends_no_bytes_when_the_server_refuses) {
    harness h;
    h.raw->push(packet(err_payload(1148, "The used command is not allowed"), 1));

    const std::vector<std::byte> data = bytes_of("x");
    slice_data_source source(crpc::span<const std::byte>(data.data(), data.size()));
    crpc::db::result_set* rows = nullptr;
    const crpc::status loaded = h.session->load_data(
        "LOAD DATA LOCAL INFILE 'x' INTO TABLE t", source, &rows, 16);
    CRPC_CHECK_FALSE(loaded.ok());
    if (!loaded.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::application),
                      static_cast<int>(loaded.code()));
    }
    // Only the handshake and the query were written: no data was streamed.
    CRPC_CHECK_EQ(std::size_t{2}, h.raw->writes.size());
}

CRPC_TEST(load_data_rejects_a_non_load_statement) {
    harness h;
    const std::vector<std::byte> data = bytes_of("x");
    slice_data_source source(crpc::span<const std::byte>(data.data(), data.size()));
    crpc::db::result_set* rows = nullptr;
    const crpc::status loaded =
        h.session->load_data("SELECT 1", source, &rows, 16);
    CRPC_CHECK_FALSE(loaded.ok());
    if (!loaded.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(loaded.code()));
    }
    // Nothing reached the server.
    CRPC_CHECK_EQ(std::size_t{1}, h.raw->writes.size());
}

CRPC_TEST(load_data_accepts_lowercase_and_leading_space) {
    harness h;
    h.raw->push(packet({std::byte{0xfb}}, 1));
    // one data packet (seq 2), the empty terminator (seq 3), so the OK is seq 4.
    h.raw->push(packet(ok_payload(0), 4));

    const std::vector<std::byte> data = bytes_of("z");
    slice_data_source source(crpc::span<const std::byte>(data.data(), data.size()));
    crpc::db::result_set* rows = nullptr;
    const crpc::status loaded =
        h.session->load_data("   load data local infile 'z' into table t", source, &rows, 8);
    CRPC_CHECK(loaded.ok());
    if (loaded.ok()) {
        delete rows;
    }
}

CRPC_TEST(load_data_aborts_and_closes_when_the_source_fails) {
    harness h;
    h.raw->push(packet({std::byte{0xfb}}, 1));
    failing_source source(1); // one good chunk, then failure

    crpc::db::result_set* rows = nullptr;
    const crpc::status loaded = h.session->load_data(
        "LOAD DATA LOCAL INFILE 'x' INTO TABLE t", source, &rows, 8);
    CRPC_CHECK_FALSE(loaded.ok());
    if (!loaded.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_reset),
                      static_cast<int>(loaded.code()));
    }
    // A partial file has already been sent, so the session cannot be reused.
    CRPC_CHECK_FALSE(h.session->is_open());
    CRPC_CHECK(h.raw->closed);
}

CRPC_TEST(load_data_succeeds_when_the_server_does_not_ask) {
    harness h;
    // The server answers OK directly: LOCAL is disabled server-side, or the
    // statement needed no data. That is a success, and no data may be sent.
    h.raw->push(packet(ok_payload(0), 1));

    const std::vector<std::byte> data = bytes_of("x");
    slice_data_source source(crpc::span<const std::byte>(data.data(), data.size()));
    crpc::db::result_set* rows = nullptr;
    const crpc::status loaded = h.session->load_data(
        "LOAD DATA LOCAL INFILE 'x' INTO TABLE t", source, &rows, 16);
    CRPC_CHECK(loaded.ok());
    if (loaded.ok()) {
        delete rows;
    }
    CRPC_CHECK_EQ(std::size_t{2}, h.raw->writes.size());
}

CRPC_TEST(load_data_rejects_a_zero_chunk_size) {
    harness h;
    const std::vector<std::byte> data = bytes_of("x");
    slice_data_source source(crpc::span<const std::byte>(data.data(), data.size()));
    crpc::db::result_set* rows = nullptr;
    const crpc::status loaded = h.session->load_data(
        "LOAD DATA LOCAL INFILE 'x' INTO TABLE t", source, &rows, 0);
    CRPC_CHECK_FALSE(loaded.ok());
    if (!loaded.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(loaded.code()));
    }
}

CRPC_TEST_MAIN()