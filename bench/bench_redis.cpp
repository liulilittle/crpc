// CRPC -- Redis client benchmark.
//
// Three pieces of the Redis client are measured independently because they
// scale differently:
//
//   * connection_pool acquire/release -- the per-command bookkeeping cost when
//     a caller checks a pooled connection out and back in. The transport is a
//     null connection: this isolates the pool, not a network.
//   * RESP command encode and reply parse -- the wire codec cost per command.
//   * pipeline batch throughput -- build N commands, execute them in one batch
//     and parse N replies. A wire-shaped executor (RESP encode + parse) is used
//     so the number is not just vector bookkeeping.
//
// Every case is warmed up and the harness reports the median of several runs.
// The pool case uses a no-op backend on purpose; a real server would add the
// network to every number and make the pool overhead invisible.
#include <crpc/io.hpp>
#include <crpc/redis/connection.hpp>
#include <crpc/redis/pipeline.hpp>
#include <crpc/redis/resp.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bench_harness.hpp"

namespace {

using namespace std::chrono_literals;

bool want(const char* key) {
    const char* filter = crpc_bench::filter();
    return filter == nullptr || std::string(filter).find(key) != std::string::npos;
}

// --- pool transport ----------------------------------------------------------

class null_source final : public crpc::io::source {
public:
    std::size_t on_data(crpc::span<const std::byte>) noexcept override {
        return 0;
    }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}
};

class null_sink final : public crpc::io::sink {
public:
    void push(crpc::span<const std::byte>,
              crpc::io::completion* completion) noexcept override {
        if (completion != nullptr) {
            completion->complete(crpc::io::io_result{});
        }
    }
};

class null_connection final : public crpc::connection {
public:
    crpc::io::source& source() noexcept override { return source_; }
    crpc::io::sink& sink() noexcept override { return sink_; }
    void bind_source(crpc::io::source*) noexcept override {}
    void resume_read() noexcept override {}
    void close() noexcept override {
        open_.store(false, std::memory_order_release);
    }
    [[nodiscard]] bool is_open() const noexcept override {
        return open_.load(std::memory_order_acquire);
    }
    [[nodiscard]] crpc::endpoint remote() const override {
        return crpc::endpoint::inproc("bench-redis-null");
    }

private:
    null_source source_;
    null_sink sink_;
    std::atomic<bool> open_{true};
};

class null_backend final : public crpc::backend_factory {
public:
    std::unique_ptr<crpc::connection> connect(
        const crpc::endpoint&, const crpc::connect_options&) override {
        return std::make_unique<null_connection>();
    }
    std::unique_ptr<crpc::listener> listen(
        const crpc::endpoint&, const crpc::listen_options&) override {
        return nullptr;
    }
};

void bench_pool() {
    null_backend backend;
    crpc::redis::pool_config config;
    config.connection.ep = crpc::endpoint::inproc("bench-redis-null");
    config.connection.operation_timeout = 1s;
    config.min_size = 4;
    config.max_size = 4;
    config.acquire_timeout = 1s;

    crpc::redis::connection_pool pool(config, backend);
    pool.warmup();

    std::size_t errors = 0;
    for (int i = 0; i < 2000; ++i) {
        auto acquired = pool.acquire();
        if (acquired.ok()) {
            pool.release(std::move(acquired).value());
        } else {
            ++errors;
        }
    }

    crpc_bench::measure("redis/pool_acquire_release", 200000, 7,
                        [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            auto acquired = pool.acquire();
            if (acquired.ok()) {
                pool.release(std::move(acquired).value());
            } else {
                ++errors;
            }
        }
    });
    std::printf("redis/pool errors=%zu idle=%zu in_use=%zu total=%zu\n", errors,
                pool.idle(), pool.in_use(), pool.total());
    pool.close();
}

// --- RESP codec --------------------------------------------------------------

void bench_resp() {
    crpc::redis::resp_encoder encoder(crpc::redis::resp_protocol::resp2);
    const crpc::redis::resp_command command{"SET", "bench:key",
                                             "value-0123456789"};
    const std::size_t size = encoder.encoded_size(command);
    std::vector<std::byte> out(size);
    std::size_t written = 0;
    std::size_t errors = 0;

    crpc_bench::measure("redis/resp_encode_command", 2000000, 7,
                        [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            written = 0;
            const crpc::status status = encoder.encode(
                crpc::span<std::byte>(out.data(), out.size()), command, written);
            if (!status.ok()) {
                ++errors;
            }
        }
    });

    const std::string reply_wire = "$11\r\nhello-world\r\n";
    const auto* reply_bytes = reinterpret_cast<const std::byte*>(
        reply_wire.data());
    std::size_t parsed = 0;
    crpc_bench::measure("redis/resp_parse_bulk_reply", 2000000, 7,
                        [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            crpc::redis::resp_parser parser;
            parser.feed(crpc::span<const std::byte>(reply_bytes,
                                                    reply_wire.size()));
            crpc::redis::resp_value value;
            const crpc::redis::resp_parser::result ready = parser.next(value);
            if (ready == crpc::redis::resp_parser::result::value_ready) {
                parsed += value.as_bulk_string().size();
            } else {
                ++errors;
            }
        }
    });
    std::printf("redis/resp errors=%zu parsed_bytes=%zu command_wire=%zu\n",
                errors, parsed, written);
}

// --- pipeline ----------------------------------------------------------------

// A wire-shaped executor: every command is encoded with the RESP encoder and
// every reply is parsed with the RESP parser. It is the same codec work
// command_channel does, without a transport.
class wire_executor final : public crpc::redis::command_executor {
public:
    wire_executor()
        : reply_(crpc::span<const std::byte>(
              reinterpret_cast<const std::byte*>(kReply), kReplySize)) {}

    crpc::result<crpc::redis::resp_value> execute(
        const crpc::redis::command& cmd) override {
        std::vector<crpc::redis::resp_value> replies;
        const crpc::result<std::vector<crpc::redis::resp_value>> batch =
            execute_batch_impl(&cmd, 1, replies);
        if (!batch) {
            return batch.error();
        }
        return std::move(replies.front());
    }

    crpc::result<std::vector<crpc::redis::resp_value>> execute_batch(
        const std::vector<crpc::redis::command>& commands) override {
        std::vector<crpc::redis::resp_value> replies;
        return execute_batch_impl(commands.data(), commands.size(), replies);
    }

private:
    static constexpr const char* kReply = "$2\r\nOK\r\n";
    static constexpr std::size_t kReplySize = 8;

    crpc::result<std::vector<crpc::redis::resp_value>> execute_batch_impl(
        const crpc::redis::command* commands, std::size_t count,
        std::vector<crpc::redis::resp_value>& replies) {
        replies.clear();
        replies.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t need = encoder_.encoded_size(commands[i]);
            if (scratch_.size() < need) {
                scratch_.resize(need);
            }
            std::size_t written = 0;
            const crpc::status encoded = encoder_.encode(
                crpc::span<std::byte>(scratch_.data(), scratch_.size()),
                commands[i], written);
            if (!encoded.ok()) {
                return encoded.error();
            }

            parser_.reset();
            parser_.feed(reply_);
            crpc::redis::resp_value value;
            const crpc::redis::resp_parser::result ready = parser_.next(value);
            if (ready != crpc::redis::resp_parser::result::value_ready) {
                return crpc::rpc_error(parser_.error(),
                                       "RESP reply parse failed");
            }
            replies.push_back(std::move(value));
        }
        return replies;
    }

    crpc::redis::resp_encoder encoder_{crpc::redis::resp_protocol::resp2};
    crpc::redis::resp_parser parser_;
    std::vector<std::byte> scratch_;
    crpc::span<const std::byte> reply_;
};

void bench_pipeline(std::size_t batch, std::size_t iterations) {
    wire_executor executor;
    crpc::redis::pipeline pipeline(executor);

    auto fill = [&pipeline, batch] {
        pipeline.clear();
        for (std::size_t i = 0; i < batch; ++i) {
            pipeline.add("SET", "bench:key", "value-0123456789");
        }
    };

    fill();
    auto warm = pipeline.run();
    if (!warm.ok()) {
        std::printf("redis/pipeline_%zu: warm-up failed\n", batch);
        return;
    }

    std::size_t errors = 0;
    char name[96];
    std::snprintf(name, sizeof(name), "redis/pipeline_batch_%zu", batch);
    crpc_bench::measure(name, iterations, 5, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            fill();
            auto replies = pipeline.run();
            if (!replies.ok() || replies.value().size() != batch) {
                ++errors;
            }
        }
    });

    if (crpc_bench::enabled(name)) {
        const double ns = crpc_bench::results().back().value;
        const double commands_per_second =
            ns > 0.0 ? static_cast<double>(batch) * 1e9 / ns : 0.0;
        char rate[96];
        std::snprintf(rate, sizeof(rate), "redis/pipeline_batch_%zu_commands",
                      batch);
        crpc_bench::record(rate, "M cmds/s", commands_per_second / 1e6);
    }
    std::printf("redis/pipeline batch=%zu iterations=%zu errors=%zu\n", batch,
                iterations, errors);
    std::fflush(stdout);
}

void run_all_cases() {
    if (want("pool")) {
        bench_pool();
    }
    if (want("resp")) {
        bench_resp();
    }
    if (want("pipeline")) {
        bench_pipeline(1, 100000);
        bench_pipeline(16, 20000);
        bench_pipeline(64, 5000);
    }
}

} // namespace

CRPC_BENCH_MAIN()
