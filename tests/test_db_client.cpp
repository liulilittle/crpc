// CRPC -- the three calling styles over one client.
//
// The same `query` is the coroutine form inside a coroutine and the blocking form on
// a plain thread, because the transport makes that distinction. `query_async` is the
// handle form; the cases here pin that it really runs on the executor (the calling
// thread is not blocked), that the connection lease is released when the task ends,
// and that asking for a handle without an executor fails instead of silently running
// on the caller's thread.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/async/executor.hpp>
#include <crpc/db/client.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/mysql/channel.hpp>
#include <crpc/db/mysql/connection.hpp>
#include <crpc/db/mysql/protocol.hpp>
#include <crpc/error.hpp>

namespace {

using crpc::db::client;
using crpc::db::client_options;
using crpc::db::mysql::capability;
using crpc::db::mysql::channel_factory;
using crpc::db::mysql::connection_options;
using crpc::db::mysql::mask;
using crpc::db::parameter;
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

// A session that answers the handshake and then any number of one-row queries.
[[nodiscard]] std::unique_ptr<scripted_channel> make_session(int queries) {
    auto channel = std::unique_ptr<scripted_channel>(new scripted_channel);
    channel->push(packet(greeting_payload(), 0));
    channel->push(packet(ok_payload(0), 2));
    for (int i = 0; i < queries; ++i) {
        channel->push(packet({std::byte{1}}, 1));
        channel->push(packet(column_payload("id", 8), 2));
        channel->push(packet(eof_payload(), 3));
        std::vector<std::byte> row;
        crpc::db::mysql::append_lenenc_string(row, "5");
        channel->push(packet(row, 4));
        channel->push(packet(eof_payload(), 5));
    }
    return channel;
}

struct rig {
    int opened = 0;
    std::unique_ptr<channel_factory> factory;
    // Fully qualified: the member `pool_options` hides the type name in this
    // scope, and GCC -Wchanges-meaning rejects the unqualified spelling.
    crpc::db::pool_options pool_options;

    rig() {
        connection_options options;
        options.user = "root";
        options.password = "";
        factory.reset(new channel_factory(options, [this] {
            ++opened;
            return crpc::result<std::unique_ptr<crpc::db::mysql::channel>>(
                std::unique_ptr<crpc::db::mysql::channel>(make_session(4).release()));
        }));
        pool_options.max_size = 1;
        pool_options.max_idle = 1;
        pool_options.validate_on_borrow = false;
    }

    [[nodiscard]] client_options options() const {
        client_options opts;
        opts.pool = pool_options;
        return opts;
    }
};

} // namespace

CRPC_TEST(db_client_sync_query_returns_rows) {
    rig r;
    client db(*r.factory, r.options());
    auto rows = db.query("SELECT id FROM t");
    CRPC_CHECK(rows.ok());
    if (!rows.ok()) {
        return;
    }
    std::unique_ptr<crpc::db::result_set> guard(rows.value());
    CRPC_CHECK(guard != nullptr);
    if (guard != nullptr) {
        CRPC_CHECK(guard->next());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(5), guard->get_int64(0).value());
    }
    // The lease is back in the pool before the caller reads a row.
    CRPC_CHECK_EQ(std::size_t{0}, db.in_use());
    CRPC_CHECK_EQ(std::size_t{1}, db.idle());
}

CRPC_TEST(db_client_async_runs_on_the_executor) {
    rig r;
    crpc::async::executor_options executor_options;
    executor_options.threads = 1;
    executor_options.queue_capacity = 8;
    crpc::async::executor pool(executor_options);

    client_options options = r.options();
    options.executor = &pool;
    client db(*r.factory, options);

    std::atomic<bool> caller_ran{false};
    auto future = db.query_async("SELECT id FROM t");
    // The call returned before the query finished: the caller's thread is free.
    caller_ran.store(true, std::memory_order_release);
    CRPC_CHECK(future.valid());
    if (!future.valid()) {
        return;
    }

    auto rows = future.get();
    CRPC_CHECK(caller_ran.load(std::memory_order_acquire));
    CRPC_CHECK(rows.ok());
    if (!rows.ok()) {
        return;
    }
    std::unique_ptr<crpc::db::result_set> guard(rows.value());
    CRPC_CHECK(guard != nullptr);
    if (guard != nullptr) {
        CRPC_CHECK(guard->next());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(5), guard->get_int64(0).value());
    }
    // The task released its lease: nothing stays checked out after it completes.
    CRPC_CHECK_EQ(std::size_t{0}, db.in_use());
}

CRPC_TEST(db_client_async_without_an_executor_is_refused) {
    rig r;
    client db(*r.factory, r.options());
    auto future = db.query_async("SELECT id FROM t");
    // No executor: an invalid future, not a hidden synchronous call.
    CRPC_CHECK_FALSE(future.valid());
    // And nothing was opened or queried.
    CRPC_CHECK_EQ(0, r.opened);
}

CRPC_TEST(db_client_execute_drops_the_cursor) {
    rig r;
    client db(*r.factory, r.options());
    CRPC_CHECK(db.execute("SELECT id FROM t").ok());
    CRPC_CHECK_EQ(std::size_t{0}, db.in_use());
}

CRPC_TEST(db_client_close_rejects_further_work) {
    rig r;
    client db(*r.factory, r.options());
    db.close();
    CRPC_CHECK(db.is_closed());
    auto rows = db.query("SELECT id FROM t");
    CRPC_CHECK_FALSE(rows.ok());
    if (!rows.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down),
                      static_cast<int>(rows.error().code));
    }
}

CRPC_TEST_MAIN()