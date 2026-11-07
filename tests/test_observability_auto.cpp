// CRPC -- automatic span and metrics wiring tests.
//
// These cases prove that the low-level client, the facade client and the real
// server dispatch create spans without the caller asking for one, that the
// trace context on the wire is the client span's context, and that the
// in-process server span is its child. A second block asserts that the default
// (no tracer exporter, no metrics exporter) stays disabled.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <crpc/client_facade.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/observability/metrics.hpp>
#include <crpc/observability/tracer.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

namespace obs = crpc::obs;
using namespace std::chrono_literals;

namespace {

using namespace crpc;
using namespace crpc::rpc;

struct collecting_exporter final : obs::exporter {
    std::mutex mutex;
    std::vector<obs::span_data> spans;

    void export_spans(::crpc::span<const obs::span_data> batch) override {
        std::lock_guard<std::mutex> lock(mutex);
        for (const obs::span_data& span : batch) {
            spans.push_back(span);
        }
    }
};

struct memory_metric_exporter final : obs::metric_exporter {
    static constexpr std::size_t kCount =
        static_cast<std::size_t>(obs::metric_id::count);

    std::atomic<crpc::u64> counters[kCount]{};
    std::atomic<crpc::u64> gauges[kCount]{};
    std::atomic<crpc::u64> histogram_samples[kCount]{};

    void on_metric(obs::metric_id id, obs::metric_kind kind,
                   crpc::u64 value) noexcept override {
        const std::size_t index = static_cast<std::size_t>(id);
        if (index >= kCount) {
            return;
        }
        switch (kind) {
        case obs::metric_kind::counter:
            counters[index].store(value, std::memory_order_relaxed);
            break;
        case obs::metric_kind::gauge:
            gauges[index].store(value, std::memory_order_relaxed);
            break;
        case obs::metric_kind::histogram:
            histogram_samples[index].fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }

    [[nodiscard]] crpc::u64 counter(obs::metric_id id) const noexcept {
        return counters[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
    }
    [[nodiscard]] crpc::u64 gauge(obs::metric_id id) const noexcept {
        return gauges[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
    }
    [[nodiscard]] crpc::u64 samples(obs::metric_id id) const noexcept {
        return histogram_samples[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
    }
};

// --- Low-level in-process transport (dispatcher only) -----------------------

struct envelope_capture {
    std::mutex mutex;
    obs::trace_context context;
    bool seen = false;
};

status capture_handler(void* self, const call_envelope& envelope,
                       span<const std::byte> body, buffer& out, call_context&) {
    auto* capture = static_cast<envelope_capture*>(self);
    {
        std::lock_guard<std::mutex> lock(capture->mutex);
        capture->context = obs::trace_context{};
        if (envelope.extensions.has(trace_extension::has_trace_id)) {
            capture->context.trace.bytes = envelope.extensions.trace_id;
        }
        if (envelope.extensions.has(trace_extension::has_span_id)) {
            capture->context.span.bytes = envelope.extensions.span_id;
        }
        capture->seen = true;
    }
    (void)out.append(body);
    return status{};
}

struct noop_completion final : io::completion {
    void complete(io::io_result) noexcept override {}
};

class test_server final : public io::source {
public:
    test_server(dispatcher& d, io::sink& sink) : dispatcher_(d), sink_(sink) {}

    std::size_t on_data(span<const std::byte> data) noexcept override {
        decoder_.feed(data);
        frame value;
        while (decoder_.next(value) == frame_decoder::result::frame_ready) {
            if (value.header.type == frame_type::request ||
                value.header.type == frame_type::one_way) {
                buffer payload;
                bool has_response = false;
                (void)dispatcher_.dispatch(value, payload, has_response,
                                           std::chrono::steady_clock::now());
                if (has_response) {
                    send(value.header.stream_id, payload.readable());
                }
            }
        }
        return data.size();
    }

    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

private:
    void send(u32 stream_id, span<const std::byte> payload) {
        frame response;
        response.header.type = frame_type::response;
        response.header.stream_id = stream_id;
        response.payload = payload;
        const std::size_t size = encoded_frame_size(response);
        buffer encoded;
        if (!encoded.reserve(size)) {
            return;
        }
        std::size_t written = 0;
        if (!encode_frame(span<std::byte>(encoded.data(), size), response, written).ok()) {
            return;
        }
        noop_completion done;
        sink_.push(span<const std::byte>(encoded.data(), written), &done);
    }

    frame_decoder decoder_;
    dispatcher& dispatcher_;
    io::sink& sink_;
};

span<const std::byte> bytes_of(const std::string& text) {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

// --- Coroutine shard helper (facade calls block) -----------------------------

struct coroutine_job {
    std::function<void()> body;
    std::atomic<bool> done{false};
};

void coroutine_entry(void* context) {
    std::shared_ptr<coroutine_job> job(
        std::move(*static_cast<std::shared_ptr<coroutine_job>*>(context)));
    delete static_cast<std::shared_ptr<coroutine_job>*>(context);
    job->body();
    job->done.store(true, std::memory_order_release);
}

bool run_on_shard(crpc::runtime& shard, std::function<void()> body,
                  int timeout_ms = 5000) {
    auto job = std::make_shared<coroutine_job>();
    job->body = std::move(body);
    auto* context = new std::shared_ptr<coroutine_job>(job);
    if (!shard.spawn(&coroutine_entry, context)) {
        delete context;
        return false;
    }
    return crpc_test::wait_for(
        [job] { return job->done.load(std::memory_order_acquire); }, timeout_ms);
}

const obs::span_data* find_span(const std::vector<obs::span_data>& spans,
                                const char* name) {
    for (const obs::span_data& data : spans) {
        if (std::string_view(data.name, data.name_len) == name) {
            return &data;
        }
    }
    return nullptr;
}

struct echo_service {
    int echo(const int& value) { return value + 1; }
};

} // namespace

// --- Client span and wire context -------------------------------------------

CRPC_TEST(client_auto_span_is_injected_into_the_envelope) {
    obs::tracer& tracer = obs::tracer::instance();
    tracer.reset();
    collecting_exporter exported;
    obs::tracer_config config;
    config.sample_rate = 1.0;
    config.queue_capacity = 64;
    tracer.configure(config);
    tracer.set_exporter(&exported);
    tracer.start();

    envelope_capture capture;
    service_registry reg;
    CRPC_CHECK(reg.register_service(1, "svc").ok());
    CRPC_CHECK(reg.register_method(1, 1, "capture", &capture_handler, &capture).ok());
    CRPC_CHECK(reg.freeze().ok());
    dispatcher d(reg);

    auto pair = inproc_pair::make();
    auto client_conn = std::move(pair.first);
    auto server_conn = std::move(pair.second);
    test_server server(d, server_conn->sink());
    server_conn->bind_source(&server);

    crpc::rpc::client_options options;
    options.slot_capacity = 16;
    crpc::rpc::client c(std::move(client_conn), options);

    const std::string body = "payload";
    auto started = c.start_call<std::string>(1, 1, bytes_of(body));
    CRPC_REQUIRE(started.ok());
    auto handle = std::move(started).value();
    const auto result = handle.get();
    CRPC_CHECK(result.ok());
    CRPC_CHECK_EQ(std::string("payload"), result.value());

    tracer.shutdown();

    CRPC_CHECK(capture.seen);
    std::lock_guard<std::mutex> lock(exported.mutex);
    const obs::span_data* client_span = find_span(exported.spans, "rpc.client.call");
    CRPC_REQUIRE(client_span != nullptr);
    CRPC_CHECK(client_span->status == rpc_code::ok);
    CRPC_CHECK(client_span->trace == capture.context.trace);
    CRPC_CHECK(client_span->span == capture.context.span);
    tracer.reset();
}

CRPC_TEST(default_no_tracer_leaves_context_untouched) {
    obs::tracer& tracer = obs::tracer::instance();
    tracer.reset();
    CRPC_CHECK_FALSE(tracer.enabled());

    envelope_capture capture;
    service_registry reg;
    CRPC_CHECK(reg.register_service(1, "svc").ok());
    CRPC_CHECK(reg.register_method(1, 1, "capture", &capture_handler, &capture).ok());
    CRPC_CHECK(reg.freeze().ok());
    dispatcher d(reg);

    auto pair = inproc_pair::make();
    auto client_conn = std::move(pair.first);
    auto server_conn = std::move(pair.second);
    test_server server(d, server_conn->sink());
    server_conn->bind_source(&server);

    crpc::rpc::client_options options;
    options.slot_capacity = 8;
    crpc::rpc::client c(std::move(client_conn), options);

    const std::string body = "plain";
    auto started = c.start_call<std::string>(1, 1, bytes_of(body));
    CRPC_REQUIRE(started.ok());
    auto handle = std::move(started).value();
    CRPC_CHECK(handle.get().ok());

    CRPC_CHECK(capture.seen);
    CRPC_CHECK_FALSE(capture.context.trace.valid());
    CRPC_CHECK_FALSE(capture.context.span.valid());
}

// --- Real server dispatch span and metrics ----------------------------------

CRPC_TEST(client_and_server_auto_spans_share_one_trace) {
    obs::tracer& tracer = obs::tracer::instance();
    tracer.reset();
    collecting_exporter exported;
    obs::tracer_config config;
    config.sample_rate = 1.0;
    config.queue_capacity = 256;
    tracer.configure(config);
    tracer.set_exporter(&exported);
    tracer.start();

    obs::metrics& metrics = obs::metrics::instance();
    metrics.reset();
    memory_metric_exporter stats;
    metrics.set_exporter(&stats);

    crpc::inproc_backend backend;
    const std::string name = "span-auto-test";

    crpc::server::builder builder;
    builder.add_listener(crpc::endpoint::inproc(name), backend);
    builder.add_service(100, "svc");
    echo_service service;
    builder.add_method<echo_service, &echo_service::echo>(100, 1, "echo", service);
    auto srv = builder.build();
    CRPC_REQUIRE(static_cast<bool>(srv));
    CRPC_REQUIRE(srv->start().ok());

    crpc::runtime_options runtime_options;
    runtime_options.start_handler_pool = false;
    crpc::runtime shard(runtime_options);
    shard.start_thread();

    crpc::client::builder client_builder;
    client_builder.add_endpoint(crpc::endpoint::inproc(name), backend);
    client_builder.default_deadline(5s);
    client_builder.on_shard(shard);
    auto cli = client_builder.build();
    CRPC_REQUIRE(static_cast<bool>(cli));

    int value = -1;
    int code = -1;
    const bool ran = run_on_shard(shard, [&] {
        auto stub = cli->service<int, int>(100, 1);
        auto started = stub.call(41);
        if (!started.ok()) {
            code = static_cast<int>(started.code());
            return;
        }
        auto handle = std::move(started).value();
        auto result = handle.get();
        if (result.ok()) {
            value = result.value();
            code = 0;
        } else {
            code = static_cast<int>(result.code());
        }
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(0, code);
    CRPC_CHECK_EQ(42, value);

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();

    CRPC_CHECK(crpc_test::wait_for(
        [&exported] {
            std::lock_guard<std::mutex> lock(exported.mutex);
            return exported.spans.size() >= 2;
        },
        2000));
    tracer.shutdown();

    CRPC_CHECK(stats.counter(obs::metric_id::rpc_client_calls) >= 1);
    CRPC_CHECK_EQ(crpc::u64{0}, stats.counter(obs::metric_id::rpc_client_errors));
    CRPC_CHECK(stats.counter(obs::metric_id::rpc_server_calls) >= 1);
    CRPC_CHECK_EQ(crpc::u64{0}, stats.counter(obs::metric_id::rpc_server_errors));
    CRPC_CHECK_EQ(crpc::u64{0}, stats.gauge(obs::metric_id::rpc_client_inflight));
    CRPC_CHECK_EQ(crpc::u64{0}, stats.gauge(obs::metric_id::rpc_server_inflight));
    CRPC_CHECK(stats.samples(obs::metric_id::rpc_client_queue_depth) > 0);

    std::lock_guard<std::mutex> lock(exported.mutex);
    const obs::span_data* client_span = find_span(exported.spans, "rpc.client.call");
    const obs::span_data* server_span = find_span(exported.spans, "rpc.server.call");
    CRPC_REQUIRE(client_span != nullptr);
    CRPC_REQUIRE(server_span != nullptr);
    CRPC_CHECK(client_span->status == rpc_code::ok);
    CRPC_CHECK(server_span->status == rpc_code::ok);
    CRPC_CHECK(client_span->trace == server_span->trace);
    CRPC_CHECK(server_span->parent == client_span->span);

    tracer.reset();
    metrics.reset();
}

CRPC_TEST_MAIN()

