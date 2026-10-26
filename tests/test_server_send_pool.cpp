// CRPC -- send-path record pool tests.
//
// `server_connection::send_payload` stages every outgoing frame (RESPONSE,
// GOAWAY and PUSH) in a pooled `send_op` record plus its reusable byte buffer.
// These cases pin the properties that make the pool both safe and effective:
//
//   * after warm-up a frame creates no record and grows no buffer, so the
//     per-frame heap allocation is gone (the record is reused);
//   * a buffer that grew past the retain limit is released when its record
//     returns, so a burst of oversized frames cannot pin a large block;
//   * the pool stays bounded by its cap under a concurrent burst.
//
// The counters come from `server::send_pool_stats()`, which is the public
// snapshot of the pool. A red build (per-frame `new`/`new[]`) makes the first
// two cases fail: `ops_allocated` and `buffers_allocated` keep growing.
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <crpc/client_facade.hpp>
#include <crpc/io.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

using namespace std::chrono_literals;

namespace {

constexpr crpc::u64 kService = 901;
constexpr crpc::u64 kEcho = 1;

struct echo_service {
    std::string echo(const std::string& value) { return value; }
};

struct fixture {
    crpc::inproc_backend backend;
    echo_service service;
    std::unique_ptr<crpc::server> srv;
    std::unique_ptr<crpc::client> cli;
    std::string name = "crpc-send-pool";

    ~fixture() {
        if (cli) {
            cli->close();
        }
        if (srv) {
            srv->stop();
        }
    }
};

bool start(fixture& f) {
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc(f.name), f.backend);
    sb.add_service(kService, "svc");
    sb.add_method<echo_service, &echo_service::echo>(kService, kEcho, "echo",
                                                     f.service);
    sb.drain_timeout(2000ms);
    f.srv = sb.build();
    if (!f.srv || !f.srv->start().ok()) {
        return false;
    }

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc(f.name), f.backend);
    cb.pool_size(4);
    cb.max_inflight_per_connection(1024);
    cb.default_deadline(5s);
    f.cli = cb.build();
    return f.cli != nullptr;
}

// Runs `count` echo calls and returns how many round-tripped successfully.
std::size_t echo_calls(crpc::client& cli, const std::string& request,
                       std::size_t count) {
    std::size_t ok = 0;
    auto stub = cli.service<std::string, std::string>(kService, kEcho);
    for (std::size_t i = 0; i < count; ++i) {
        auto started = stub.call(request);
        if (!started.ok()) {
            continue;
        }
        auto result = std::move(started).value().get();
        if (result.ok() && result.value() == request) {
            ++ok;
        }
    }
    return ok;
}

// Sets or clears an environment variable, returning the previous value (empty
// when it was unset) so the caller can restore it.
std::string set_env(const char* name, const char* value) {
    std::string previous;
#if defined(_WIN32)
    char* old = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&old, &size, name) == 0 && old != nullptr) {
        previous = old;
        std::free(old);
    }
    _putenv_s(name, value != nullptr ? value : "");
#else
    const char* old = std::getenv(name);
    if (old != nullptr) {
        previous = old;
    }
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
    return previous;
}

} // namespace

// After the pool has seen its first frame, a steady stream of calls creates no
// new record and grows no buffer: the per-frame heap allocation is gone.
CRPC_TEST(send_pool_reuses_records_without_allocation) {
    fixture f;
    CRPC_CHECK(start(f));

    CRPC_CHECK_EQ(std::size_t{200}, echo_calls(*f.cli, std::string(64, 'x'), 200));

    const crpc::server::send_pool_counters before = f.srv->send_pool_stats();
    CRPC_CHECK(before.ops_allocated >= 1);
    CRPC_CHECK_EQ(std::size_t{200}, echo_calls(*f.cli, std::string(64, 'x'), 200));
    const crpc::server::send_pool_counters after = f.srv->send_pool_stats();

    CRPC_CHECK_EQ(before.ops_allocated, after.ops_allocated);
    CRPC_CHECK_EQ(before.buffers_allocated, after.buffers_allocated);
    CRPC_CHECK_EQ(std::size_t{0}, after.ops_live);
}

// A buffer that grew past the retain limit is released when its record returns
// to the pool, so the next small frame reallocates instead of pinning the large
// block for the server's life. The retain limit is lowered through the
// environment so the case does not depend on a 1 MiB frame.
CRPC_TEST(send_pool_releases_oversized_buffer) {
    const std::string previous =
        set_env("CRPC_SEND_BUFFER_RETAIN_BYTES", "65536");
    struct restore {
        const std::string& value;
        const char* name = "CRPC_SEND_BUFFER_RETAIN_BYTES";
        ~restore() { set_env(name, value.empty() ? nullptr : value.c_str()); }
    } guard{previous};

    fixture f;
    CRPC_CHECK(start(f));

    // Warm up with a small frame so the record's buffer is small.
    CRPC_CHECK_EQ(std::size_t{50}, echo_calls(*f.cli, std::string(32, 'x'), 50));
    const crpc::server::send_pool_counters small = f.srv->send_pool_stats();

    // A frame larger than the 64 KiB retain limit grows the buffer.
    const std::string large(256 * 1024, 'y');
    CRPC_CHECK_EQ(std::size_t{1}, echo_calls(*f.cli, large, 1));
    const crpc::server::send_pool_counters grew = f.srv->send_pool_stats();
    CRPC_CHECK(grew.buffers_allocated > small.buffers_allocated);
    CRPC_CHECK_EQ(small.ops_allocated, grew.ops_allocated);

    // The large buffer was released on return, so the small frame reallocates.
    CRPC_CHECK_EQ(std::size_t{50}, echo_calls(*f.cli, std::string(32, 'x'), 50));
    const crpc::server::send_pool_counters reallocated =
        f.srv->send_pool_stats();
    CRPC_CHECK(reallocated.buffers_allocated > grew.buffers_allocated);
    CRPC_CHECK_EQ(grew.ops_allocated, reallocated.ops_allocated);
    CRPC_CHECK_EQ(std::size_t{0}, reallocated.ops_live);
}

// The pool never creates more than its cap records, even when many connections
// send at once. A small cap is set through the environment.
CRPC_TEST(send_pool_is_bounded_by_its_cap) {
    const std::string previous = set_env("CRPC_SEND_POOL_MAX_OPS", "4");
    struct restore {
        const std::string& value;
        const char* name = "CRPC_SEND_POOL_MAX_OPS";
        ~restore() { set_env(name, value.empty() ? nullptr : value.c_str()); }
    } guard{previous};

    fixture f;
    CRPC_CHECK(start(f));

    // One round-trip creates at least one record.
    CRPC_CHECK_EQ(std::size_t{1}, echo_calls(*f.cli, std::string(32, 'x'), 1));
    const crpc::server::send_pool_counters after_one = f.srv->send_pool_stats();
    CRPC_CHECK(after_one.ops_allocated >= 1);

    // A burst of concurrent calls must not push the pool past its cap. Some
    // calls may be refused (the pool is exhausted) -- that is the backpressure
    // the cap exists for; the bound is the property under test.
    constexpr int kThreads = 8;
    constexpr int kPerThread = 50;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&f] {
            (void)echo_calls(*f.cli, std::string(32, 'x'), kPerThread);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    const crpc::server::send_pool_counters burst = f.srv->send_pool_stats();
    CRPC_CHECK(burst.ops_allocated <= 4);
    CRPC_CHECK_EQ(std::size_t{0}, burst.ops_live);
}

CRPC_TEST_MAIN()
