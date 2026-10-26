// CRPC -- memcached transport end-to-end tests.
//
// The client tests use a scripted transport; these drive the *real*
// `connection_transport` over an in-process connection, so the exchange loop
// (write, read one byte, drain the rest, reassemble a value block) is executed
// for real. The server side is scripted, so the cases stay deterministic.
#include "test_framework.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/memcached/client.hpp>
#include <crpc/memcached/protocol.hpp>

namespace {

std::vector<std::byte> to_bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

class noop_completion final : public crpc::io::completion {
public:
    void complete(crpc::io::io_result) noexcept override {}
};

// Records everything the client writes, so a case can wait until the request has
// arrived and only then push the reply.
class recording_source final : public crpc::io::source {
public:
    std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            received_.insert(received_.end(), data.begin(), data.end());
        }
        cv_.notify_all();
        return data.size();
    }

    void on_eof() noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            eof_ = true;
        }
        cv_.notify_all();
    }

    void on_error(std::error_code) noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool wait_for(std::size_t count, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&] { return received_.size() >= count; });
    }

    [[nodiscard]] std::string text() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::string(reinterpret_cast<const char*>(received_.data()), received_.size());
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::byte> received_;
    bool eof_ = false;
    bool error_ = false;
};

// Hands the already-created in-process client connection to the pool.
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

} // namespace

CRPC_TEST(transport_round_trip_store_and_get) {
    auto pair = crpc::inproc_pair::make_typed();
    recording_source server;
    pair.second->bind_source(&server);
    pair_backend backend(std::move(pair.first));

    crpc::memcached::connection_transport_config config;
    config.pool.max_size = 1;
    config.pool.connection.ep = crpc::endpoint::inproc("crpc-memcached-transport");
    crpc::memcached::connection_transport transport(std::move(config), backend);

    const std::vector<std::byte> request = to_bytes("set k 0 0 5\r\nhello\r\n");
    std::optional<crpc::result<crpc::memcached::text_reply>> outcome;
    std::thread caller([&] {
        outcome = transport.exchange(crpc::span<const std::byte>(request.data(), request.size()));
    });
    CRPC_CHECK(server.wait_for(request.size(), 5000));
    CRPC_CHECK_EQ(std::string("set k 0 0 5\r\nhello\r\n"), server.text());
    noop_completion done;
    const std::vector<std::byte> stored = to_bytes("STORED\r\n");
    pair.second->sink().push(crpc::span<const std::byte>(stored.data(), stored.size()), &done);
    caller.join();
    CRPC_CHECK(outcome.has_value());
    if (!outcome.has_value()) {
        return;
    }
    const auto& reply = *outcome;

    CRPC_CHECK(reply.ok());
    if (reply.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::store_status::stored),
                      static_cast<int>(reply.value().store));
    }
}

CRPC_TEST(transport_reassembles_fragmented_value) {
    auto pair = crpc::inproc_pair::make_typed();
    recording_source server;
    pair.second->bind_source(&server);
    pair_backend backend(std::move(pair.first));

    crpc::memcached::connection_transport_config config;
    config.pool.max_size = 1;
    config.pool.connection.ep = crpc::endpoint::inproc("crpc-memcached-frag");
    crpc::memcached::connection_transport transport(std::move(config), backend);

    const std::string value(70000, 'x');
    const std::vector<std::byte> request = to_bytes("get big\r\n");
    std::optional<crpc::result<crpc::memcached::text_reply>> outcome;
    std::thread caller([&] {
        outcome = transport.exchange(crpc::span<const std::byte>(request.data(), request.size()));
    });
    CRPC_CHECK(server.wait_for(request.size(), 5000));

    // Push the reply in many small pieces, with the header split from the value
    // and the value split across pushes: the parser must reassemble it.
    noop_completion done;
    const std::string header = "VALUE big 0 " + std::to_string(value.size()) + "\r\n";
    const std::string body = value + "\r\nEND\r\n";
    const std::vector<std::byte> header_bytes = to_bytes(header);
    pair.second->sink().push(
        crpc::span<const std::byte>(header_bytes.data(), header_bytes.size()), &done);

    constexpr std::size_t piece = 4096;
    for (std::size_t offset = 0; offset < body.size(); offset += piece) {
        const std::size_t count = std::min(piece, body.size() - offset);
        const std::vector<std::byte> chunk = to_bytes(body.substr(offset, count));
        pair.second->sink().push(
            crpc::span<const std::byte>(chunk.data(), chunk.size()), &done);
    }
    caller.join();
    CRPC_CHECK(outcome.has_value());
    if (!outcome.has_value()) {
        return;
    }
    const auto& reply = *outcome;

    CRPC_CHECK(reply.ok());
    if (reply.ok()) {
        CRPC_CHECK_EQ(std::size_t{1}, reply.value().entries.size());
        CRPC_CHECK_EQ(value.size(), reply.value().entries[0].data.size());
        CRPC_CHECK(reply.value().entries[0].data == value);
    }
}

CRPC_TEST(transport_reports_eof_mid_reply) {
    auto pair = crpc::inproc_pair::make_typed();
    recording_source server;
    pair.second->bind_source(&server);
    pair_backend backend(std::move(pair.first));

    crpc::memcached::connection_transport_config config;
    config.pool.max_size = 1;
    config.pool.connection.ep = crpc::endpoint::inproc("crpc-memcached-eof");
    crpc::memcached::connection_transport transport(std::move(config), backend);

    const std::vector<std::byte> request = to_bytes("get k\r\n");
    std::optional<crpc::result<crpc::memcached::text_reply>> outcome;
    std::thread caller([&] {
        outcome = transport.exchange(crpc::span<const std::byte>(request.data(), request.size()));
    });
    CRPC_CHECK(server.wait_for(request.size(), 5000));

    // A partial reply, then the peer closes: the caller must get an error, not a
    // truncated value that looks complete.
    noop_completion done;
    const std::vector<std::byte> partial = to_bytes("VALUE k 0 5\r\nhe");
    pair.second->sink().push(crpc::span<const std::byte>(partial.data(), partial.size()), &done);
    pair.second->close();
    caller.join();
    CRPC_CHECK(outcome.has_value());
    if (!outcome.has_value()) {
        return;
    }
    const auto& reply = *outcome;

    CRPC_CHECK_FALSE(reply.ok());
    if (!reply.ok()) {
        const int code = static_cast<int>(reply.error().code);
        // The link may report eof/reset/broken-pipe directly, or the transport's
        // lazy reconnect may report connection_refused afterwards. All of them are
        // transport failures, which is what the caller must see.
        CRPC_CHECK(code == static_cast<int>(crpc::rpc_code::eof) ||
                   code == static_cast<int>(crpc::rpc_code::connection_reset) ||
                   code == static_cast<int>(crpc::rpc_code::broken_pipe) ||
                   code == static_cast<int>(crpc::rpc_code::connection_refused));
    }
}

CRPC_TEST(transport_rejects_unsolicited_bytes) {
    auto pair = crpc::inproc_pair::make_typed();
    recording_source server;
    pair.second->bind_source(&server);
    pair_backend backend(std::move(pair.first));

    crpc::memcached::connection_transport_config config;
    config.pool.max_size = 1;
    config.pool.connection.ep = crpc::endpoint::inproc("crpc-memcached-extra");
    crpc::memcached::connection_transport transport(std::move(config), backend);

    const std::vector<std::byte> request = to_bytes("get k\r\n");
    std::optional<crpc::result<crpc::memcached::text_reply>> outcome;
    std::thread caller([&] {
        outcome = transport.exchange(crpc::span<const std::byte>(request.data(), request.size()));
    });
    CRPC_CHECK(server.wait_for(request.size(), 5000));

    // A complete reply plus a second reply nobody asked for: the stream and the
    // parser disagree, which must be an error rather than silently swallowed.
    noop_completion done;
    const std::vector<std::byte> two = to_bytes("END\r\nSTORED\r\n");
    pair.second->sink().push(crpc::span<const std::byte>(two.data(), two.size()), &done);
    caller.join();
    CRPC_CHECK(outcome.has_value());
    if (!outcome.has_value()) {
        return;
    }
    const auto& reply = *outcome;

    CRPC_CHECK_FALSE(reply.ok());
    if (!reply.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                      static_cast<int>(reply.error().code));
    }
}

CRPC_TEST_MAIN()