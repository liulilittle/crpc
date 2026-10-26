// CRPC Redis -- asynchronous client facade tests.
//
// The facade composes the connection pool and the command executor: every
// command acquires a pooled connection, wraps it in a `command_channel` and
// releases it again. These tests pin that behaviour against the in-process
// backend, so they need no server and are deterministic:
//
//   * connect/close/health check and the pool acquire/release contract;
//   * single commands and batches (order, count);
//   * the typed convenience methods;
//   * a Redis error reply stays a value while a transport failure fails the
//     `result`;
//   * a coroutine parks on a slow reply and the shard stays free.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/io.hpp>
#include <crpc/redis/client.hpp>
#include <crpc/redis/connection.hpp>
#include <crpc/redis/pipeline.hpp>
#include <crpc/redis/resp.hpp>
#include <crpc/runtime.hpp>

#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

crpc::redis::resp_value text(std::string value) {
    return crpc::redis::resp_value(crpc::redis::resp_bulk_string{std::move(value)});
}

crpc::redis::resp_value simple(std::string value) {
    return crpc::redis::resp_value(crpc::redis::resp_simple_string{std::move(value)});
}

crpc::redis::resp_value integer(std::int64_t value) {
    return crpc::redis::resp_value(crpc::redis::resp_integer{value});
}

// --- In-process fake Redis server -------------------------------------------

struct scripted_reply {
    std::vector<std::string> chunks;
    std::chrono::milliseconds delay{0};
};

using server_handler = std::function<scripted_reply(const crpc::redis::command&)>;

class fake_redis_server final : public crpc::io::source {
public:
    fake_redis_server(crpc::connection* conn, server_handler handler)
        : conn_(conn), handler_(std::move(handler)) {
        conn_->bind_source(this);
    }

    ~fake_redis_server() override {
        conn_->bind_source(nullptr);
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    fake_redis_server(const fake_redis_server&) = delete;
    fake_redis_server& operator=(const fake_redis_server&) = delete;

    std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
        try {
            parser_.feed(data);
            for (;;) {
                crpc::redis::resp_value value;
                const crpc::redis::resp_parser::result decoded = parser_.next(value);
                if (decoded != crpc::redis::resp_parser::result::value_ready) {
                    break;
                }
                if (!value.is_array()) {
                    continue;
                }
                crpc::redis::command cmd;
                for (const crpc::redis::resp_value& item : value.as_array()) {
                    if (item.is_bulk_string()) {
                        cmd.push_back(item.as_bulk_string());
                    } else if (item.is_simple_string()) {
                        cmd.push_back(item.as_simple_string());
                    } else {
                        cmd.clear();
                        break;
                    }
                }
                if (cmd.empty()) {
                    continue;
                }
                scripted_reply reply = handler_(cmd);
                if (reply.delay.count() > 0) {
                    workers_.emplace_back([this, reply = std::move(reply)]() {
                        try {
                            std::this_thread::sleep_for(reply.delay);
                            for (const std::string& chunk : reply.chunks) {
                                write_chunk(chunk);
                            }
                        } catch (...) {
                        }
                    });
                } else {
                    for (const std::string& chunk : reply.chunks) {
                        write_chunk(chunk);
                    }
                }
            }
        } catch (...) {
            // A test server must never throw across the io callback boundary.
        }
        return data.size();
    }

    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

private:
    class write_done final : public crpc::io::completion {
    public:
        explicit write_done(std::shared_ptr<std::string> data)
            : data_(std::move(data)) {}
        void complete(crpc::io::io_result) noexcept override { delete this; }

    private:
        std::shared_ptr<std::string> data_;
    };

    void write_chunk(const std::string& body) {
        auto data = std::make_shared<std::string>(body);
        auto* done = new write_done(data);
        conn_->sink().push(
            crpc::span<const std::byte>(
                reinterpret_cast<const std::byte*>(data->data()), data->size()),
            done);
    }

    crpc::connection* conn_;
    server_handler handler_;
    crpc::redis::resp_parser parser_;
    std::vector<std::thread> workers_;
};

// Accepts every server side the client establishes and keeps it alive for as
// long as the client may still hold the matching pooled connection.
class server_holder final : public crpc::accept_sink {
public:
    explicit server_holder(server_handler handler) : handler_(std::move(handler)) {}

    void on_accept(std::unique_ptr<crpc::connection> conn) noexcept override {
        try {
            auto server = std::make_unique<fake_redis_server>(conn.get(), handler_);
            servers_.push_back(std::move(server));
            conns_.push_back(std::move(conn));
        } catch (...) {
        }
    }

    std::size_t accepts() const noexcept { return servers_.size(); }

private:
    server_handler handler_;
    // `conns_` is declared before `servers_` so the reverse destruction order
    // destroys each server (which unbinds from its connection) before the
    // connection it points at.
    std::vector<std::unique_ptr<crpc::connection>> conns_;
    std::vector<std::unique_ptr<fake_redis_server>> servers_;
};

struct harness {
    crpc::inproc_backend backend;
    crpc::endpoint ep{crpc::endpoint::inproc("crpc-redis-client")};
    std::unique_ptr<crpc::listener> listener;
    std::unique_ptr<server_holder> server;

    harness(server_handler handler) {
        server = std::make_unique<server_holder>(std::move(handler));
        listener = backend.listen(ep, crpc::listen_options{});
        if (listener != nullptr) {
            listener->start(server.get());
        }
    }

    crpc::redis::client::options options() const {
        crpc::redis::client::options opts;
        opts.pool.connection.ep = ep;
        opts.pool.connection.operation_timeout = std::chrono::seconds(2);
        return opts;
    }
};

// A backend that never produces a connection: every connect is refused.
class null_backend final : public crpc::backend_factory {
public:
    std::unique_ptr<crpc::connection> connect(const crpc::endpoint&,
                                              const crpc::connect_options&) override {
        return nullptr;
    }

    std::unique_ptr<crpc::listener> listen(const crpc::endpoint&,
                                           const crpc::listen_options&) override {
        return nullptr;
    }
};

server_handler basic_handler() {
    // Tracks which string keys exist so SET/DEL/EXISTS/GET observe each other.
    auto keys = std::make_shared<std::set<std::string>>();
    return [keys](const crpc::redis::command& cmd) -> scripted_reply {
        if (cmd.empty()) {
            return {{"-ERR empty command\r\n"}, {}};
        }
        const std::string& name = cmd[0];
        if (name == "SET" || name == "SETEX") {
            if (cmd.size() >= 2) {
                keys->insert(cmd[1]);
            }
            return {{"+OK\r\n"}, {}};
        }
        if (name == "DEL") {
            std::int64_t removed = 0;
            for (std::size_t i = 1; i < cmd.size(); ++i) {
                removed += keys->erase(cmd[i]) != 0 ? 1 : 0;
            }
            return {{":" + std::to_string(removed) + "\r\n"}, {}};
        }
        if (name == "EXISTS") {
            std::int64_t present = 0;
            for (std::size_t i = 1; i < cmd.size(); ++i) {
                present += keys->count(cmd[i]) != 0 ? 1 : 0;
            }
            return {{":" + std::to_string(present) + "\r\n"}, {}};
        }
        if (name == "GET") {
            if (cmd.size() >= 2 && keys->count(cmd[1]) != 0) {
                return {{"$5\r\nvalue\r\n"}, {}};
            }
            return {{"$-1\r\n"}, {}};
        }
        if (name == "PING") return {{"+PONG\r\n"}, {}};
        if (name == "INCR") return {{":1\r\n"}, {}};
        if (name == "DECR") return {{":0\r\n"}, {}};
        if (name == "INCRBY") return {{":1\r\n"}, {}};
        if (name == "EXISTS") return {{":1\r\n"}, {}};
        if (name == "DEL") return {{":1\r\n"}, {}};
        if (name == "EXPIRE") return {{":1\r\n"}, {}};
        if (name == "TTL") return {{":100\r\n"}, {}};
        if (name == "APPEND") return {{":5\r\n"}, {}};
        if (name == "STRLEN") return {{":5\r\n"}, {}};
        if (name == "KEYS") return {{"*1\r\n$3\r\nfoo\r\n"}, {}};
        if (name == "MGET") return {{"*1\r\n$5\r\nvalue\r\n"}, {}};
        if (name == "MSET") return {{"+OK\r\n"}, {}};
        if (name == "HSET") return {{":1\r\n"}, {}};
        if (name == "HGET") return {{"$5\r\nvalue\r\n"}, {}};
        if (name == "HDEL") return {{":1\r\n"}, {}};
        if (name == "HEXISTS") return {{":1\r\n"}, {}};
        if (name == "HGETALL") return {{"*2\r\n$5\r\nfield\r\n$5\r\nvalue\r\n"}, {}};
        if (name == "LPUSH") return {{":1\r\n"}, {}};
        if (name == "RPUSH") return {{":1\r\n"}, {}};
        if (name == "LPOP") return {{"$5\r\nvalue\r\n"}, {}};
        if (name == "RPOP") return {{"$5\r\nvalue\r\n"}, {}};
        if (name == "LLEN") return {{":1\r\n"}, {}};
        if (name == "SADD") return {{":1\r\n"}, {}};
        if (name == "SREM") return {{":1\r\n"}, {}};
        if (name == "SMEMBERS") return {{"*1\r\n$6\r\nmember\r\n"}, {}};
        if (name == "SISMEMBER") return {{":1\r\n"}, {}};
        if (name == "ZADD") return {{":1\r\n"}, {}};
        if (name == "ZRANGE") return {{"*1\r\n$6\r\nmember\r\n"}, {}};
        if (name == "ZSCORE") return {{"$3\r\n1.5\r\n"}, {}};
        return {{"-ERR unknown command\r\n"}, {}};
    };
}

bool coroutine_loop_available() {
#if defined(CRPC_HAVE_COROCC)
    return true;
#else
    return false;
#endif
}

} // namespace

// --- connection management ---------------------------------------------------

CRPC_TEST(client_connect_establishes_pool) {
    harness h(basic_handler());
    crpc::redis::client c(h.options(), h.backend);

    crpc::status st = c.connect();
    CRPC_CHECK(st.ok());
    CRPC_CHECK(c.is_open());
    CRPC_CHECK_EQ(std::size_t{1}, c.total());
    CRPC_CHECK_EQ(std::size_t{1}, c.idle());
}

CRPC_TEST(client_close_rejects_further_work) {
    harness h(basic_handler());
    crpc::redis::client c(h.options(), h.backend);
    CRPC_CHECK(c.connect().ok());

    c.close();
    CRPC_CHECK_FALSE(c.is_open());

    crpc::result<crpc::redis::resp_value> reply = c.execute({"PING"});
    CRPC_CHECK_FALSE(reply.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down),
                  static_cast<int>(reply.code()));
}

CRPC_TEST(client_health_check_reflects_reachability) {
    harness h(basic_handler());
    crpc::redis::client alive(h.options(), h.backend);
    CRPC_CHECK(alive.health_check().ok());

    null_backend backend;
    crpc::redis::client::options opts;
    opts.pool.connection.ep = crpc::endpoint::inproc("crpc-redis-null");
    crpc::redis::client dead(opts, backend);
    CRPC_CHECK_FALSE(dead.health_check().ok());
}

CRPC_TEST(client_pool_reuses_released_connections) {
    harness h(basic_handler());
    crpc::redis::client c(h.options(), h.backend);

    CRPC_CHECK(c.execute({"PING"}).ok());
    CRPC_CHECK(c.execute({"PING"}).ok());

    CRPC_CHECK_EQ(std::size_t{1}, c.total());
    CRPC_CHECK_EQ(std::size_t{1}, c.idle());
    CRPC_CHECK_EQ(std::size_t{0}, c.in_use());
}

CRPC_TEST(client_pool_exhaustion_fails_with_resource_exhausted) {
    harness h(basic_handler());
    crpc::redis::client::options opts = h.options();
    opts.pool.max_size = 1;
    opts.pool.acquire_timeout = std::chrono::milliseconds(200);
    crpc::redis::client c(opts, h.backend);

    crpc::result<std::shared_ptr<crpc::redis::connection>> held = c.acquire();
    CRPC_CHECK(held.ok());
    CRPC_CHECK_EQ(std::size_t{1}, c.in_use());

    crpc::result<crpc::redis::resp_value> reply = c.execute({"PING"});
    CRPC_CHECK_FALSE(reply.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                  static_cast<int>(reply.code()));

    c.release(std::move(held).value());
    CRPC_CHECK_EQ(std::size_t{0}, c.in_use());
}

// --- command execution -------------------------------------------------------

CRPC_TEST(client_execute_single_command) {
    harness h(basic_handler());
    crpc::redis::client c(h.options(), h.backend);

    crpc::result<crpc::redis::resp_value> reply = c.execute({"PING"});
    CRPC_CHECK(reply.ok());
    CRPC_CHECK(reply.value().is_simple_string());
    CRPC_CHECK_EQ(std::string("PONG"), reply.value().as_simple_string());
}

CRPC_TEST(client_execute_accepts_rvalue_command) {
    harness h(basic_handler());
    crpc::redis::client c(h.options(), h.backend);

    crpc::redis::command cmd{"PING"};
    crpc::result<crpc::redis::resp_value> reply = c.execute(std::move(cmd));
    CRPC_CHECK(reply.ok());
    CRPC_CHECK_EQ(std::string("PONG"), reply.value().as_simple_string());
}

CRPC_TEST(client_execute_batch_returns_replies_in_order) {
    harness h(basic_handler());
    crpc::redis::client c(h.options(), h.backend);

    std::vector<crpc::redis::command> cmds{
        {"SET", "key", "value"}, {"GET", "key"}, {"INCR", "n"}};
    crpc::result<std::vector<crpc::redis::resp_value>> replies = c.execute_batch(cmds);
    CRPC_CHECK(replies.ok());
    CRPC_CHECK_EQ(std::size_t{3}, replies.value().size());
    CRPC_CHECK_EQ(std::string("OK"), replies.value()[0].as_simple_string());
    CRPC_CHECK_EQ(std::string("value"), replies.value()[1].as_bulk_string());
    CRPC_CHECK_EQ(std::int64_t{1}, replies.value()[2].as_integer());
}

CRPC_TEST(client_execute_empty_batch_skips_transport) {
    harness h(basic_handler());
    crpc::redis::client c(h.options(), h.backend);

    crpc::result<std::vector<crpc::redis::resp_value>> replies =
        c.execute_batch(std::vector<crpc::redis::command>{});
    CRPC_CHECK(replies.ok());
    CRPC_CHECK(replies.value().empty());
    CRPC_CHECK_EQ(std::size_t{0}, c.total());
}

CRPC_TEST(client_error_reply_is_a_value) {
    harness h(basic_handler());
    crpc::redis::client c(h.options(), h.backend);

    crpc::result<crpc::redis::resp_value> reply = c.execute({"BOGUS"});
    CRPC_CHECK(reply.ok());
    CRPC_CHECK(reply.value().is_error());
    CRPC_CHECK_EQ(std::string("ERR unknown command"), reply.value().as_error());
}

CRPC_TEST(client_transport_error_fails_the_result) {
    null_backend backend;
    crpc::redis::client::options opts;
    opts.pool.connection.ep = crpc::endpoint::inproc("crpc-redis-null2");
    opts.pool.connection.operation_timeout = std::chrono::seconds(2);
    crpc::redis::client c(opts, backend);

    crpc::result<crpc::redis::resp_value> reply = c.execute({"PING"});
    CRPC_CHECK_FALSE(reply.ok());
    // The endpoint is unreachable, so the pool reports a retryable transport
    // failure (the exact code depends on whether the reconnect backoff window
    // has elapsed, hence the retryable check rather than a fixed code).
    CRPC_CHECK(crpc::is_retryable(reply.code()));
}

// --- typed convenience -------------------------------------------------------

CRPC_TEST(client_typed_get_set) {
    harness h(basic_handler());
    crpc::redis::client c(h.options(), h.backend);

    crpc::result<crpc::redis::resp_value> set = c.set("key", "value");
    CRPC_CHECK(set.ok());
    crpc::result<crpc::redis::resp_value> get = c.get("key");
    CRPC_CHECK(get.ok());
    CRPC_CHECK_EQ(std::string("value"), get.value().as_bulk_string());
}

CRPC_TEST(client_typed_exists_del) {
    harness h(basic_handler());
    crpc::redis::client c(h.options(), h.backend);

    CRPC_CHECK(c.set("k", "v").ok());
    crpc::result<crpc::redis::resp_value> before = c.exists({"k"});
    CRPC_CHECK(before.ok());
    CRPC_CHECK_EQ(std::int64_t{1}, before.value().as_integer());

    crpc::result<crpc::redis::resp_value> del = c.del({"k"});
    CRPC_CHECK(del.ok());
    CRPC_CHECK_EQ(std::int64_t{1}, del.value().as_integer());

    crpc::result<crpc::redis::resp_value> after = c.exists({"k"});
    CRPC_CHECK(after.ok());
    CRPC_CHECK_EQ(std::int64_t{0}, after.value().as_integer());
}

CRPC_TEST(client_typed_expire_ttl) {
    harness h(basic_handler());
    crpc::redis::client c(h.options(), h.backend);

    CRPC_CHECK(c.set("k", "v").ok());
    crpc::result<crpc::redis::resp_value> exp = c.expire("k", std::chrono::seconds(100));
    CRPC_CHECK(exp.ok());
    CRPC_CHECK_EQ(std::int64_t{1}, exp.value().as_integer());

    crpc::result<crpc::redis::resp_value> ttl = c.ttl("k");
    CRPC_CHECK(ttl.ok());
    CRPC_CHECK_EQ(std::int64_t{100}, ttl.value().as_integer());
}

CRPC_TEST(client_typed_incr_decr_incrby) {
    harness h(basic_handler());
    crpc::redis::client c(h.options(), h.backend);

    crpc::result<crpc::redis::resp_value> i = c.incr("n");
    CRPC_CHECK(i.ok());
    CRPC_CHECK_EQ(std::int64_t{1}, i.value().as_integer());

    crpc::result<crpc::redis::resp_value> d = c.decr("n");
    CRPC_CHECK(d.ok());
    CRPC_CHECK_EQ(std::int64_t{0}, d.value().as_integer());

    crpc::result<crpc::redis::resp_value> b = c.incrby("n", 5);
    CRPC_CHECK(b.ok());
    CRPC_CHECK_EQ(std::int64_t{1}, b.value().as_integer());
}

CRPC_TEST(client_typed_strings_and_collections) {
    harness h(basic_handler());
    crpc::redis::client c(h.options(), h.backend);

    CRPC_CHECK(c.setex("k", std::chrono::seconds(10), "v").ok());
    CRPC_CHECK_EQ(std::string("value"), c.get("k").value().as_bulk_string());
    CRPC_CHECK(c.append("k", "x").ok());
    CRPC_CHECK_EQ(std::int64_t{5}, c.strlen("k").value().as_integer());
    CRPC_CHECK(c.keys("*").ok());
    CRPC_CHECK_EQ(std::string("value"), c.mget({"k"}).value().as_array()[0].as_bulk_string());
    CRPC_CHECK(c.mset({{"a", "1"}, {"b", "2"}}).ok());

    CRPC_CHECK(c.hset("h", "f", "v").ok());
    CRPC_CHECK_EQ(std::string("value"), c.hget("h", "f").value().as_bulk_string());
    CRPC_CHECK(c.hexists("h", "f").ok());
    CRPC_CHECK(c.hdel("h", {"f"}).ok());
    CRPC_CHECK(c.hgetall("h").ok());

    CRPC_CHECK(c.lpush("l", {"a"}).ok());
    CRPC_CHECK(c.rpush("l", {"b"}).ok());
    CRPC_CHECK(c.lpop("l").ok());
    CRPC_CHECK(c.rpop("l").ok());
    CRPC_CHECK_EQ(std::int64_t{1}, c.llen("l").value().as_integer());

    CRPC_CHECK(c.sadd("s", {"m"}).ok());
    CRPC_CHECK(c.sismember("s", "m").ok());
    CRPC_CHECK(c.srem("s", {"m"}).ok());
    CRPC_CHECK(c.smembers("s").ok());

    CRPC_CHECK(c.zadd("z", 1.5, "m").ok());
    CRPC_CHECK(c.zrange("z", 0, -1).ok());
    CRPC_CHECK_EQ(std::string("1.5"), c.zscore("z", "m").value().as_bulk_string());
}

// --- cooperative suspension --------------------------------------------------

namespace {

struct slow_state {
    std::atomic<bool> seen{false};
    std::atomic<bool> shard_ran{false};
    std::atomic<bool> done{false};
    int code = -1;
    std::string payload;
    crpc::redis::client* client = nullptr;
};

void slow_entry(void* ctx) {
    auto* state = static_cast<slow_state*>(ctx);
    crpc::result<crpc::redis::resp_value> reply = state->client->execute({"SLOW"});
    state->code = reply.ok() ? 0 : static_cast<int>(reply.code());
    if (reply.ok() && reply.value().is_simple_string()) {
        state->payload = reply.value().as_simple_string();
    }
    state->done.store(true);
}

void mark_true(void* ctx) noexcept {
    static_cast<std::atomic<bool>*>(ctx)->store(true);
}

} // namespace

CRPC_TEST(client_parks_a_coroutine_until_the_reply_arrives) {
    if (!coroutine_loop_available()) {
        crpc_test::skip("requires CRPC_HAVE_COROCC");
    }

    crpc::runtime_options options;
    options.start_handler_pool = false;
    crpc::runtime shard(options);
    shard.start_thread();
    CRPC_CHECK(shard.loop_mode());

    slow_state state;
    {
        harness h([&state](const crpc::redis::command& cmd) -> scripted_reply {
            if (!cmd.empty() && cmd[0] == "SLOW") {
                state.seen.store(true);
                return {{"+PONG\r\n"}, std::chrono::milliseconds(300)};
            }
            return {{"+OK\r\n"}, {}};
        });
        crpc::redis::client c(h.options(), h.backend, &shard);
        state.client = &c;

        CRPC_CHECK(shard.spawn(&slow_entry, &state));
        CRPC_CHECK(crpc_test::wait_for(
            [&state] { return state.seen.load(std::memory_order_acquire); }, 2000));

        // The coroutine is parked on the delayed reply. The shard is free to run
        // another task, and the reply has not arrived yet.
        CRPC_CHECK(shard.post(&mark_true, &state.shard_ran));
        CRPC_CHECK(crpc_test::wait_for(
            [&state] { return state.shard_ran.load(std::memory_order_acquire); }, 2000));
        CRPC_CHECK_FALSE(state.done.load(std::memory_order_acquire));

        CRPC_CHECK(crpc_test::wait_for(
            [&state] { return state.done.load(std::memory_order_acquire); }, 3000));
        CRPC_CHECK_EQ(0, state.code);
        CRPC_CHECK_EQ(std::string("PONG"), state.payload);
    }

    shard.stop();
    shard.join();
}

CRPC_TEST_MAIN()
