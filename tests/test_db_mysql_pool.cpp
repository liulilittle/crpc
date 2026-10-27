// CRPC -- MySQL adapter over the shared connection pool, end to end.
//
// The pool and the adapter were tested separately; this suite joins them, which is
// where the interesting interactions live: validation on borrow sends a PING that the
// scripted server must answer, a released connection must come back, and closing the
// pool must not leave a session half-open.
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/db/connection.hpp>
#include <crpc/db/mysql/channel.hpp>
#include <crpc/db/mysql/connection.hpp>
#include <crpc/db/mysql/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::mysql::capability;
using crpc::db::mysql::channel_factory;
using crpc::db::mysql::connection_options;
using crpc::db::mysql::mask;
using crpc::db::pool;
using crpc::db::pool_options;

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

[[nodiscard]] std::vector<std::byte> eof_payload() {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0xfe});
    append_u16(payload, 0);
    append_u16(payload, 2);
    return payload;
}

[[nodiscard]] std::vector<std::byte> column_payload(std::string_view name, std::uint8_t type) {
    std::vector<std::byte> payload;
    crpc::db::mysql::append_lenenc_string(payload, "def");
    crpc::db::mysql::append_lenenc_string(payload, "app");
    crpc::db::mysql::append_lenenc_string(payload, "t");
    crpc::db::mysql::append_lenenc_string(payload, "t");
    crpc::db::mysql::append_lenenc_string(payload, name);
    crpc::db::mysql::append_lenenc_string(payload, name);
    crpc::db::mysql::append_lenenc_int(payload, 0x0c);
    append_u16(payload, 33);
    append_u32(payload, 255);
    payload.push_back(static_cast<std::byte>(type));
    append_u16(payload, 0);
    payload.push_back(std::byte{0});
    append_u16(payload, 0);
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
};

// One scripted session: handshake, then a text-protocol query returning one row,
// then a PING for the pool's borrow validation.
[[nodiscard]] std::unique_ptr<scripted_channel> make_session(int id) {
    auto channel = std::unique_ptr<scripted_channel>(new scripted_channel);
    channel->push(packet(greeting_payload(), 0));
    channel->push(packet(ok_payload(0), 2));
    // Query: one column, one row whose value is the session id.
    channel->push(packet({std::byte{1}}, 1));
    channel->push(packet(column_payload("id", 8), 2));
    channel->push(packet(eof_payload(), 3));
    std::vector<std::byte> row;
    crpc::db::mysql::append_lenenc_string(row, std::to_string(id));
    channel->push(packet(row, 4));
    channel->push(packet(eof_payload(), 5));
    // The pool pings before handing an idle session out, so the PING answer comes
    // between the first query's replies and the second query's.
    channel->push(packet(ok_payload(0), 1));
    channel->push(packet({std::byte{1}}, 1));
    channel->push(packet(column_payload("id", 8), 2));
    channel->push(packet(eof_payload(), 3));
    channel->push(packet(row, 4));
    channel->push(packet(eof_payload(), 5));
    return channel;
}

} // namespace

CRPC_TEST(mysql_pool_opens_queries_and_reuses) {
    int opened = 0;
    connection_options options;
    options.user = "root";
    options.password = "";
    channel_factory factory(options, [&opened] {
        ++opened;
        return crpc::result<std::unique_ptr<crpc::db::mysql::channel>>(
            std::unique_ptr<crpc::db::mysql::channel>(make_session(opened).release()));
    });

    pool_options pool_options;
    pool_options.max_size = 1;
    pool_options.max_idle = 1;
    pool_options.min_size = 0;
    pool_options.validate_on_borrow = true;
    pool connections(pool_options, factory);

    {
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
        if (!lease.ok()) {
            return;
        }
        crpc::db::result_set* rows = nullptr;
        const crpc::status ran = lease.value()->query("SELECT id FROM t", {}, &rows);
        CRPC_CHECK(ran.ok());
        std::unique_ptr<crpc::db::result_set> guard(rows);
        CRPC_CHECK(guard != nullptr);
        if (guard != nullptr) {
            CRPC_CHECK(guard->next());
            CRPC_CHECK_EQ(static_cast<std::int64_t>(1), guard->get_int64(0).value());
        }
        CRPC_CHECK(lease.value().release().ok());
    }
    CRPC_CHECK_EQ(1, opened);
    CRPC_CHECK_EQ(std::size_t{1}, connections.idle());

    {
        // The second borrow reuses the idle session; the pool pings it first, which
        // the scripted server answers.
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
        if (!lease.ok()) {
            return;
        }
        crpc::db::result_set* rows = nullptr;
        const crpc::status ran = lease.value()->query("SELECT id FROM t", {}, &rows);
        CRPC_CHECK(ran.ok());
        delete rows;
        CRPC_CHECK(lease.value().release().ok());
    }
    // Still one connection: the pool reused it rather than opening another.
    CRPC_CHECK_EQ(1, opened);
}

CRPC_TEST(mysql_pool_close_closes_the_session) {
    int opened = 0;
    connection_options options;
    options.user = "root";
    options.password = "";
    channel_factory factory(options, [&opened] {
        ++opened;
        return crpc::result<std::unique_ptr<crpc::db::mysql::channel>>(
            std::unique_ptr<crpc::db::mysql::channel>(make_session(opened).release()));
    });

    pool_options pool_options;
    pool_options.max_size = 1;
    pool_options.max_idle = 1;
    pool_options.validate_on_borrow = false;
    pool connections(pool_options, factory);

    {
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
        if (!lease.ok()) {
            return;
        }
        CRPC_CHECK(lease.value().release().ok());
    }
    CRPC_CHECK_EQ(std::size_t{1}, connections.total());

    connections.close();
    CRPC_CHECK_EQ(std::size_t{0}, connections.total());
    auto rejected = connections.acquire();
    CRPC_CHECK_FALSE(rejected.ok());
    if (!rejected.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down),
                      static_cast<int>(rejected.error().code));
    }
}

CRPC_TEST(mysql_pool_reports_a_failed_handshake) {
    connection_options options;
    options.user = "root";
    options.password = "";
    channel_factory factory(options, [] {
        auto channel = std::unique_ptr<scripted_channel>(new scripted_channel);
        // An ERR packet where the greeting belongs: the server refused the session.
        std::vector<std::byte> err;
        err.push_back(std::byte{0xff});
        append_u16(err, 1045);
        err.push_back(std::byte{'#'});
        append_text(err, "28000");
        append_text(err, "Access denied");
        channel->push(packet(err, 0));
        return crpc::result<std::unique_ptr<crpc::db::mysql::channel>>(
            std::unique_ptr<crpc::db::mysql::channel>(channel.release()));
    });

    pool_options pool_options;
    pool_options.max_size = 1;
    pool_options.validate_on_borrow = false;
    pool connections(pool_options, factory);

    auto lease = connections.acquire();
    CRPC_CHECK_FALSE(lease.ok());
    // The slot was released, so a later attempt can still try.
    CRPC_CHECK_EQ(std::size_t{0}, connections.total());
}

CRPC_TEST_MAIN()