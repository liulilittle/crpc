// CRPC Redis -- pipeline batching, command-executor contract and a real
// command_channel over an in-process CRPC transport.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/io.hpp>
#include <crpc/redis/connection.hpp>
#include <crpc/redis/pipeline.hpp>
#include <crpc/runtime.hpp>

#include "redis_test_support.hpp"
#include "test_framework.hpp"

using namespace redis_test;

namespace {

class short_executor final : public crpc::redis::command_executor {
public:
    crpc::result<crpc::redis::resp_value> execute(
        const crpc::redis::command&) override {
        return simple("OK");
    }

    crpc::result<std::vector<crpc::redis::resp_value>> execute_batch(
        const std::vector<crpc::redis::command>&) override {
        return std::vector<crpc::redis::resp_value>{simple("OK")};
    }
};

class failing_batch_executor final : public crpc::redis::command_executor {
public:
    crpc::result<crpc::redis::resp_value> execute(
        const crpc::redis::command&) override {
        return simple("OK");
    }

    crpc::result<std::vector<crpc::redis::resp_value>> execute_batch(
        const std::vector<crpc::redis::command>&) override {
        return crpc::rpc_error{crpc::rpc_code::broken_pipe, "write failed"};
    }
};

} // namespace

CRPC_TEST(pipeline_batches_commands_in_order) {
    scripted_executor executor([](const command& cmd) -> crpc::result<resp_value> {
        if (cmd[0] == "SET") {
            return simple("OK");
        }
        if (cmd[0] == "INCR") {
            return integer(7);
        }
        return text("value");
    });

    crpc::redis::pipeline pipeline(executor);
    pipeline.add({"SET", "key", "value"})
        .add("INCR", "counter")
        .add({"GET", "key"});

    crpc::result<std::vector<resp_value>> replies = pipeline.run();
    CRPC_CHECK(replies.ok());
    CRPC_CHECK_EQ(std::size_t{1}, executor.batch_calls);
    CRPC_CHECK_EQ(std::size_t{1}, executor.batches.size());
    CRPC_CHECK_EQ(std::size_t{3}, executor.batches[0].size());
    CRPC_CHECK_EQ(std::size_t{3}, replies.value().size());
    CRPC_CHECK_EQ(std::string("OK"), replies.value()[0].as_simple_string());
    CRPC_CHECK_EQ(std::int64_t{7}, replies.value()[1].as_integer());
    CRPC_CHECK_EQ(std::string("value"), replies.value()[2].as_bulk_string());
    CRPC_CHECK_EQ(std::size_t{3}, executor.executed.size());
    CRPC_CHECK_EQ(std::string("INCR"), executor.executed[1][0]);
    CRPC_CHECK_EQ(std::string("counter"), executor.executed[1][1]);
}

CRPC_TEST(pipeline_empty_does_not_touch_transport) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return simple("OK");
    });

    crpc::redis::pipeline pipeline(executor);
    crpc::result<std::vector<resp_value>> replies = pipeline.run();
    CRPC_CHECK(replies.ok());
    CRPC_CHECK(replies.value().empty());
    CRPC_CHECK_EQ(std::size_t{0}, executor.batch_calls);
    CRPC_CHECK(executor.executed.empty());
}

CRPC_TEST(pipeline_clear_drops_queued_commands) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return simple("OK");
    });

    crpc::redis::pipeline pipeline(executor);
    pipeline.add({"SET", "a", "1"});
    CRPC_CHECK_EQ(std::size_t{1}, pipeline.size());
    pipeline.clear();
    CRPC_CHECK(pipeline.empty());
    CRPC_CHECK_EQ(std::size_t{0}, pipeline.size());
}

CRPC_TEST(pipeline_run_consumes_the_queued_commands) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return simple("OK");
    });

    crpc::redis::pipeline pipeline(executor);
    pipeline.add({"SET", "a", "1"}).add({"SET", "b", "2"});
    CRPC_CHECK_EQ(std::size_t{2}, pipeline.size());

    crpc::result<std::vector<resp_value>> first = pipeline.run();
    CRPC_CHECK(first.ok());
    CRPC_CHECK_EQ(std::size_t{1}, executor.batch_calls);
    CRPC_CHECK(pipeline.empty());
    CRPC_CHECK_EQ(std::size_t{0}, pipeline.size());

    // A second run must not silently re-execute the same non-idempotent batch.
    crpc::result<std::vector<resp_value>> second = pipeline.run();
    CRPC_CHECK(second.ok());
    CRPC_CHECK(second.value().empty());
    CRPC_CHECK_EQ(std::size_t{1}, executor.batch_calls);
}

CRPC_TEST(pipeline_preserves_error_replies) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return error("ERR unknown command");
    });

    crpc::redis::pipeline pipeline(executor);
    pipeline.add({"BOGUS"});
    crpc::result<std::vector<resp_value>> replies = pipeline.run();
    CRPC_CHECK(replies.ok());
    CRPC_CHECK_EQ(std::size_t{1}, replies.value().size());
    CRPC_CHECK(replies.value()[0].is_error());
    CRPC_CHECK_EQ(std::string("ERR unknown command"),
                  replies.value()[0].as_error());
}

CRPC_TEST(pipeline_transport_error_stops_the_batch) {
    failing_batch_executor executor;
    crpc::redis::pipeline pipeline(executor);
    pipeline.add({"SET", "a", "1"});

    crpc::result<std::vector<resp_value>> replies = pipeline.run();
    CRPC_CHECK_FALSE(replies.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::broken_pipe),
                  static_cast<int>(replies.code()));
}

CRPC_TEST(pipeline_rejects_reply_count_mismatch) {
    short_executor executor;
    crpc::redis::pipeline pipeline(executor);
    pipeline.add({"SET", "a", "1"}).add({"SET", "b", "2"});

    crpc::result<std::vector<resp_value>> replies = pipeline.run();
    CRPC_CHECK_FALSE(replies.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(replies.code()));
}

CRPC_TEST(pipeline_variadic_add_builds_command) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return simple("OK");
    });

    crpc::redis::pipeline pipeline(executor);
    pipeline.add("SET", "key", "value");
    const std::vector<command>& commands = pipeline.commands();
    CRPC_CHECK_EQ(std::size_t{1}, commands.size());
    CRPC_CHECK_EQ(std::string("SET"), commands[0][0]);
    CRPC_CHECK_EQ(std::string("key"), commands[0][1]);
    CRPC_CHECK_EQ(std::string("value"), commands[0][2]);
}

CRPC_TEST(pipeline_accepts_moved_command) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return simple("OK");
    });

    command cmd{"LPUSH", "list", "a", "b"};
    crpc::redis::pipeline pipeline(executor);
    pipeline.add(std::move(cmd));
    CRPC_CHECK_EQ(std::size_t{4}, pipeline.commands()[0].size());
}

// --- command_channel over a real in-process transport -----------------------

namespace {

struct scripted_reply {
    std::vector<std::string> chunks;
    std::chrono::milliseconds delay{0};
};

using server_handler = std::function<scripted_reply(const command&)>;

class pair_backend final : public crpc::backend_factory {
public:
    explicit pair_backend(std::unique_ptr<crpc::connection> client)
        : client_(std::move(client)) {}

    std::unique_ptr<crpc::connection> connect(const crpc::endpoint&,
                                              const crpc::connect_options&) override {
        return std::move(client_);
    }

    std::unique_ptr<crpc::listener> listen(const crpc::endpoint&,
                                          const crpc::listen_options&) override {
        return nullptr;
    }

private:
    std::unique_ptr<crpc::connection> client_;
};

class fake_redis_server final : public crpc::io::source {
public:
    fake_redis_server(crpc::connection& conn, server_handler handler)
        : conn_(&conn), handler_(std::move(handler)) {
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
                resp_value value;
                const crpc::redis::resp_parser::result decoded = parser_.next(value);
                if (decoded != crpc::redis::resp_parser::result::value_ready) {
                    break;
                }
                if (!value.is_array()) {
                    continue;
                }
                command cmd;
                for (const resp_value& item : value.as_array()) {
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

    void write_chunk(const std::string& text) {
        auto data = std::make_shared<std::string>(text);
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

struct channel_fixture {
    std::unique_ptr<crpc::connection> server_conn;
    std::unique_ptr<pair_backend> backend;
    std::unique_ptr<fake_redis_server> server;
    std::unique_ptr<crpc::redis::connection> client;
    std::unique_ptr<crpc::redis::command_channel> channel;

    void start(server_handler handler, crpc::runtime* shard = nullptr) {
        auto pair = crpc::inproc_pair::make();
        server_conn = std::move(pair.second);
        backend = std::make_unique<pair_backend>(std::move(pair.first));
        server = std::make_unique<fake_redis_server>(*server_conn, std::move(handler));

        crpc::redis::connection_config config;
        config.ep = crpc::endpoint::inproc("redis-g-channel");
        config.operation_timeout = std::chrono::seconds(2);
        client = std::make_unique<crpc::redis::connection>(config, *backend, shard);
        channel = std::make_unique<crpc::redis::command_channel>(*client);
    }
};

server_handler basic_handler() {
    return [](const command& cmd) -> scripted_reply {
        if (cmd[0] == "PING") {
            return {{"+PONG\r\n"}, {}};
        }
        if (cmd[0] == "SET") {
            return {{"+OK\r\n"}, {}};
        }
        if (cmd[0] == "GET") {
            return {{"$5\r\nvalue\r\n"}, {}};
        }
        if (cmd[0] == "INCR") {
            return {{":1\r\n"}, {}};
        }
        if (cmd[0] == "SPLIT") {
            return {{"$5\r\nva", "lue\r\n"}, {}};
        }
        return {{"-ERR unknown command\r\n"}, {}};
    };
}

} // namespace

CRPC_TEST(command_channel_executes_and_batches_over_inproc) {
    channel_fixture fixture;
    fixture.start(basic_handler());

    crpc::result<resp_value> empty = fixture.channel->execute(command{});
    CRPC_CHECK_FALSE(empty.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(empty.code()));

    crpc::result<resp_value> ping = fixture.channel->execute({"PING"});
    CRPC_CHECK(ping.ok());
    CRPC_CHECK(ping.value().is_simple_string());
    CRPC_CHECK_EQ(std::string("PONG"), ping.value().as_simple_string());

    crpc::redis::pipeline pipeline(*fixture.channel);
    pipeline.add({"SET", "key", "value"})
        .add({"GET", "key"})
        .add({"INCR", "counter"});
    crpc::result<std::vector<resp_value>> replies = pipeline.run();
    CRPC_CHECK(replies.ok());
    CRPC_CHECK_EQ(std::size_t{3}, replies.value().size());
    CRPC_CHECK_EQ(std::string("OK"),
                  replies.value()[0].as_simple_string());
    CRPC_CHECK_EQ(std::string("value"),
                  replies.value()[1].as_bulk_string());
    CRPC_CHECK_EQ(std::int64_t{1}, replies.value()[2].as_integer());

    // A bulk payload split across two transport pushes is reassembled by the
    // parser, not by a line-oriented read.
    crpc::result<resp_value> split = fixture.channel->execute({"SPLIT"});
    CRPC_CHECK(split.ok());
    CRPC_CHECK_EQ(std::string("value"), split.value().as_bulk_string());
}

namespace {

struct slow_state {
    std::atomic<bool> started{false};
    std::atomic<bool> seen{false};
    std::atomic<bool> done{false};
    std::atomic<bool> shard_ran{false};
    int code = -1;
    std::string payload;
    crpc::redis::command_channel* channel = nullptr;
};

void slow_entry(void* ctx) {
    auto* state = static_cast<slow_state*>(ctx);
    state->started.store(true);
    crpc::result<resp_value> reply = state->channel->execute({"SLOW"});
    state->code = reply.ok() ? 0 : static_cast<int>(reply.code());
    if (reply.ok() && reply.value().is_simple_string()) {
        state->payload = reply.value().as_simple_string();
    }
    state->done.store(true);
}

void mark_true(void* ctx) noexcept {
    static_cast<std::atomic<bool>*>(ctx)->store(true);
}

bool coroutine_loop_available() {
#if defined(CRPC_HAVE_COROCC)
    return true;
#else
    return false;
#endif
}

} // namespace

CRPC_TEST(command_channel_parks_a_coroutine_until_the_reply_arrives) {
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
        channel_fixture fixture;
        fixture.start(
            [&state](const command& cmd) -> scripted_reply {
                if (cmd[0] == "SLOW") {
                    state.seen.store(true);
                    return {{"+PONG\r\n"}, std::chrono::milliseconds(300)};
                }
                return {{"+OK\r\n"}, {}};
            },
            &shard);

        state.channel = fixture.channel.get();
        CRPC_CHECK(shard.spawn(&slow_entry, &state));
        CRPC_CHECK(crpc_test::wait_for(
            [&state] { return state.seen.load(std::memory_order_acquire); }, 2000));

        // The coroutine is parked waiting for the delayed reply. The shard is
        // free to run another task, and the reply has not arrived yet.
        CRPC_CHECK(shard.post(&mark_true, &state.shard_ran));
        CRPC_CHECK(crpc_test::wait_for(
            [&state] { return state.shard_ran.load(std::memory_order_acquire); },
            2000));
        CRPC_CHECK_FALSE(state.done.load(std::memory_order_acquire));

        CRPC_CHECK(crpc_test::wait_for(
            [&state] { return state.done.load(std::memory_order_acquire); }, 3000));
        CRPC_CHECK_EQ(0, state.code);
        CRPC_CHECK_EQ(std::string("PONG"), state.payload);

        // Join the shard while the fixture and channel are still alive: a
        // coroutine left parked by a timed-out wait must never resume after
        // the objects it calls are destroyed. stop()/join() are idempotent,
        // so the calls after the block stay.
        shard.stop();
        shard.join();
    }

    shard.stop();
    shard.join();
}

CRPC_TEST_MAIN()
