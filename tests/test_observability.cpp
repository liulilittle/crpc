// CRPC -- observability tests: trace context, AOP chain, logger and tracer.
#include <crpc/observability/context.hpp>
#include <crpc/observability/interceptor.hpp>
#include <crpc/observability/log.hpp>
#include <crpc/observability/tracer.hpp>

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace obs = crpc::obs;

static_assert(std::is_trivially_copyable<obs::trace_id>::value, "trace_id must be trivially copyable");
static_assert(std::is_trivially_copyable<obs::span_id>::value, "span_id must be trivially copyable");
static_assert(std::is_trivially_copyable<obs::trace_context>::value,
              "trace_context must be trivially copyable");
static_assert(std::is_trivially_copyable<obs::deadline>::value,
              "deadline must be trivially copyable");
static_assert(std::is_trivially_copyable<obs::baggage>::value,
              "baggage must be trivially copyable");
static_assert(sizeof(obs::trace_id) == 16, "trace_id is 16 bytes");
static_assert(sizeof(obs::span_id) == 8, "span_id is 8 bytes");
static_assert(sizeof(obs::log_record) <= 512, "log_record fits a 512-byte slot");

// Counting allocator used to prove that the disabled tracer path and the empty
// interceptor chain do not allocate. The executable replaces the global
// allocation functions; every test that measures takes a snapshot first.
namespace {
std::atomic<std::size_t> g_new_calls{0};
} // namespace

void* operator new(std::size_t size) {
    g_new_calls.fetch_add(1, std::memory_order_relaxed);
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        return pointer;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

std::size_t count_newlines(const std::string& text) {
    std::size_t count = 0;
    for (const char c : text) {
        if (c == '\n') {
            ++count;
        }
    }
    return count;
}

void remove_file(const std::string& path) { (void)std::remove(path.c_str()); }

obs::logger_config make_log_config(const std::string& path, std::size_t capacity) {
    obs::logger_config config;
    config.file_path = path;
    config.ring_capacity = capacity;
    config.max_file_size = 0;
    config.rotate_interval = std::chrono::milliseconds(0);
    config.overflow = obs::overflow_policy::drop_and_count;
    config.enable_file = true;
    return config;
}

struct hook_recorder : obs::interceptor {
    std::vector<int>* order = nullptr;
    int id = 0;
    bool result = true;
    bool should_throw = false;
    int mutate_priority = -1;
    int seen_priority = -1;
    bool mutate_deadline = false;
    bool mutate_metadata = false;

    bool on_hook(obs::hook_point, obs::call_context& context) override {
        if (order != nullptr) {
            order->push_back(id);
        }
        seen_priority = static_cast<int>(context.priority);
        if (mutate_priority >= 0) {
            context.priority = static_cast<crpc::u8>(mutate_priority);
        }
        if (mutate_deadline) {
            context.deadline = obs::deadline::after(std::chrono::milliseconds(5));
        }
        if (mutate_metadata) {
            context.metadata.set("mutated", "yes");
        }
        if (should_throw) {
            throw std::runtime_error("hook failure");
        }
        if (!result) {
            context.set_error(crpc::rpc_code::canceled, "short circuit");
        }
        return result;
    }
};

void mask_password(void*, const char* key, std::size_t key_length, char* value,
                   std::size_t, std::size_t* value_length) noexcept {
    const std::string_view name(key, key_length);
    if (name.find("password") != std::string_view::npos) {
        const char mask[] = "***";
        std::memcpy(value, mask, sizeof(mask) - 1);
        *value_length = sizeof(mask) - 1;
    }
}

struct collecting_exporter : obs::exporter {
    std::mutex mutex;
    std::vector<obs::span_data> spans;

    void export_spans(::crpc::span<const obs::span_data> batch) override {
        std::lock_guard<std::mutex> lock(mutex);
        for (const obs::span_data& span : batch) {
            spans.push_back(span);
        }
    }
};

struct blocking_exporter : obs::exporter {
    std::atomic<bool> release{false};
    std::atomic<int> calls{0};

    void export_spans(::crpc::span<const obs::span_data>) override {
        calls.fetch_add(1, std::memory_order_relaxed);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
};

std::atomic<int> g_clock_calls{0};

std::uint64_t counting_clock(void*) noexcept {
    g_clock_calls.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

bool drop_all_tail(void*, const obs::span_data&) noexcept { return false; }

struct compressor_state {
    std::atomic<int> calls{0};
};

std::size_t marker_compressor(void* user, const char*, std::size_t, char* output,
                              std::size_t capacity) noexcept {
    static_cast<compressor_state*>(user)->calls.fetch_add(1, std::memory_order_relaxed);
    if (capacity < 1) {
        return 0;
    }
    output[0] = 'Z';
    return 1;
}

} // namespace

// --- Trace context -----------------------------------------------------------

CRPC_TEST(traceparent_round_trip) {
    obs::trace_context context;
    for (std::size_t i = 0; i < context.trace.bytes.size(); ++i) {
        context.trace.bytes[i] = static_cast<std::byte>(i + 1);
    }
    for (std::size_t i = 0; i < context.span.bytes.size(); ++i) {
        context.span.bytes[i] = static_cast<std::byte>(0x10 + i);
    }
    context.flags = 0x01;

    char text[55];
    const std::size_t length = obs::format_traceparent(context, text, sizeof(text));
    CRPC_CHECK_EQ(length, std::size_t{55});

    const obs::traceparent_result parsed =
        obs::parse_traceparent(std::string_view(text, length));
    CRPC_CHECK(parsed.ok());
    CRPC_CHECK(parsed.context.trace == context.trace);
    CRPC_CHECK(parsed.context.span == context.span);
    CRPC_CHECK_EQ(static_cast<int>(parsed.context.flags), 1);
    CRPC_CHECK(parsed.context.sampled());
}

CRPC_TEST(traceparent_rejects_invalid_forms) {
    obs::trace_context context;
    for (std::size_t i = 0; i < context.trace.bytes.size(); ++i) {
        context.trace.bytes[i] = static_cast<std::byte>(i + 1);
    }
    for (std::size_t i = 0; i < context.span.bytes.size(); ++i) {
        context.span.bytes[i] = static_cast<std::byte>(0x20 + i);
    }
    context.flags = 0x01;

    char text[55];
    const std::size_t length = obs::format_traceparent(context, text, sizeof(text));
    CRPC_CHECK_EQ(length, std::size_t{55});
    const std::string valid(text, length);

    CRPC_CHECK_EQ(static_cast<int>(obs::parse_traceparent(valid.substr(0, 54)).status),
                  static_cast<int>(obs::traceparent_status::invalid_length));

    std::string bad_hex = valid;
    bad_hex[3] = 'g';
    CRPC_CHECK_EQ(static_cast<int>(obs::parse_traceparent(bad_hex).status),
                  static_cast<int>(obs::traceparent_status::invalid_hex));

    std::string bad_version = valid;
    bad_version[0] = 'f';
    bad_version[1] = 'f';
    CRPC_CHECK_EQ(static_cast<int>(obs::parse_traceparent(bad_version).status),
                  static_cast<int>(obs::traceparent_status::invalid_version));

    std::string zero_trace = valid;
    for (std::size_t i = 3; i < 35; ++i) {
        zero_trace[i] = '0';
    }
    CRPC_CHECK_EQ(static_cast<int>(obs::parse_traceparent(zero_trace).status),
                  static_cast<int>(obs::traceparent_status::invalid_trace_id));

    std::string zero_span = valid;
    for (std::size_t i = 36; i < 52; ++i) {
        zero_span[i] = '0';
    }
    CRPC_CHECK_EQ(static_cast<int>(obs::parse_traceparent(zero_span).status),
                  static_cast<int>(obs::traceparent_status::invalid_span_id));

    std::string uppercase = valid;
    uppercase[3] = 'A';
    CRPC_CHECK_EQ(static_cast<int>(obs::parse_traceparent(uppercase).status),
                  static_cast<int>(obs::traceparent_status::invalid_hex));
}

CRPC_TEST(baggage_is_ordered_and_bounded) {
    obs::baggage values;
    for (std::size_t i = 0; i < obs::baggage::max_entries; ++i) {
        char key[16];
        std::snprintf(key, sizeof(key), "k%zu", i);
        char value[16];
        std::snprintf(value, sizeof(value), "v%zu", i);
        CRPC_CHECK(values.set(key, value));
    }
    CRPC_CHECK_EQ(values.size(), obs::baggage::max_entries);
    CRPC_CHECK_EQ(values.at(0).key, std::string_view("k0"));
    CRPC_CHECK_EQ(values.at(0).value, std::string_view("v0"));
    CRPC_CHECK_EQ(values.at(31).key, std::string_view("k31"));

    // Replacing an existing key preserves its position in the ordered list.
    CRPC_CHECK(values.set("k5", "updated"));
    CRPC_CHECK_EQ(values.size(), obs::baggage::max_entries);
    CRPC_CHECK_EQ(values.at(5).key, std::string_view("k5"));
    CRPC_CHECK_EQ(values.at(5).value, std::string_view("updated"));
    CRPC_CHECK_EQ(values.at(6).key, std::string_view("k6"));

    CRPC_CHECK_FALSE(values.set("overflow", "x"));
    CRPC_CHECK(values.truncated());
    CRPC_CHECK(values.dropped_entries() >= 1);

    const obs::baggage copy = values;
    CRPC_CHECK_EQ(copy.size(), values.size());
    CRPC_CHECK_EQ(copy.at(5).value, std::string_view("updated"));

    obs::baggage bytes;
    const std::string chunk(3000, 'a');
    CRPC_CHECK(bytes.set("a", chunk));
    CRPC_CHECK(bytes.set("b", chunk));
    CRPC_CHECK_FALSE(bytes.set("c", chunk));
    CRPC_CHECK(bytes.truncated());
    CRPC_CHECK(bytes.bytes_used() <= obs::baggage::max_bytes);

    obs::baggage huge;
    const std::string oversized(5000, 'x');
    CRPC_CHECK_FALSE(huge.set("big", oversized));
    CRPC_CHECK(huge.truncated());
}

CRPC_TEST(deadline_is_relative) {
    obs::deadline value = obs::deadline::after(std::chrono::milliseconds(100));
    CRPC_CHECK(value.has_deadline());
    CRPC_CHECK_FALSE(value.expired());
    value.consume(std::chrono::milliseconds(40));
    CRPC_CHECK_EQ(value.remaining.count(), 60000000);
    value.consume(std::chrono::seconds(1));
    CRPC_CHECK(value.expired());
    CRPC_CHECK_EQ(value.remaining.count(), 0);

    obs::deadline infinite = obs::deadline::infinite_deadline();
    infinite.consume(std::chrono::seconds(1));
    CRPC_CHECK(infinite.infinite);
    CRPC_CHECK_FALSE(infinite.expired());
}

// --- Interceptor chain -------------------------------------------------------

CRPC_TEST(interceptor_empty_chain_is_transparent) {
    obs::interceptor_chain chain;
    obs::call_context context;
    CRPC_CHECK(chain.run(obs::hook_point::before_call, context));
    CRPC_CHECK_EQ(chain.count(obs::hook_point::before_call), std::size_t{0});
    CRPC_CHECK(context.ok());
}

CRPC_TEST(interceptor_order_and_context_mutation) {
    obs::interceptor_chain chain;
    std::vector<int> order;

    hook_recorder setter;
    setter.order = &order;
    setter.id = 1;
    setter.mutate_priority = 42;
    setter.mutate_deadline = true;
    setter.mutate_metadata = true;

    hook_recorder observer;
    observer.order = &order;
    observer.id = 2;

    CRPC_CHECK(chain.add(obs::hook_point::before_call, &setter));
    CRPC_CHECK(chain.add(obs::hook_point::before_call, &observer));

    obs::call_context context;
    CRPC_CHECK(chain.run(obs::hook_point::before_call, context));
    CRPC_CHECK_EQ(order.size(), std::size_t{2});
    CRPC_CHECK_EQ(order[0], 1);
    CRPC_CHECK_EQ(order[1], 2);
    CRPC_CHECK_EQ(static_cast<int>(context.priority), 42);
    CRPC_CHECK_EQ(observer.seen_priority, 42);
    CRPC_CHECK(context.deadline.has_deadline());
    CRPC_CHECK_EQ(context.deadline.remaining.count(), 5000000);
    const std::string* metadata = context.metadata.find("mutated");
    CRPC_CHECK(metadata != nullptr);
    if (metadata != nullptr) {
        CRPC_CHECK_EQ(*metadata, std::string("yes"));
    }
}

CRPC_TEST(interceptor_short_circuit_and_exception) {
    obs::interceptor_chain chain;
    std::vector<int> order;

    hook_recorder stopper;
    stopper.order = &order;
    stopper.id = 1;
    stopper.result = false;

    hook_recorder never;
    never.order = &order;
    never.id = 2;

    CRPC_CHECK(chain.add(obs::hook_point::before_call, &stopper));
    CRPC_CHECK(chain.add(obs::hook_point::before_call, &never));

    obs::call_context context;
    CRPC_CHECK_FALSE(chain.run(obs::hook_point::before_call, context));
    CRPC_CHECK(context.result == crpc::rpc_code::canceled);
    CRPC_CHECK_EQ(order.size(), std::size_t{1});

    obs::interceptor_chain throwing;
    hook_recorder thrower;
    thrower.should_throw = true;
    CRPC_CHECK(throwing.add(obs::hook_point::after_call, &thrower));
    obs::call_context error_context;
    CRPC_CHECK_FALSE(throwing.run(obs::hook_point::after_call, error_context));
    CRPC_CHECK(error_context.result == crpc::rpc_code::internal);
}

CRPC_TEST(interceptor_empty_chain_does_not_allocate) {
    obs::interceptor_chain chain;
    obs::call_context context;
    const std::size_t before = g_new_calls.load(std::memory_order_relaxed);

    bool continued = true;
    for (int i = 0; i < 10000; ++i) {
        continued = chain.run(obs::hook_point::before_call, context) && continued;
    }
    CRPC_CHECK(continued);
    CRPC_CHECK_EQ(g_new_calls.load(std::memory_order_relaxed), before);
}

CRPC_TEST(interceptor_chain_freezes) {
    obs::interceptor_chain chain;
    hook_recorder hook;
    CRPC_CHECK(chain.add(obs::hook_point::on_connect, &hook));
    chain.freeze();
    CRPC_CHECK(chain.frozen());
    CRPC_CHECK_FALSE(chain.add(obs::hook_point::on_connect, &hook));
}

// --- Logger ------------------------------------------------------------------

CRPC_TEST(logger_formats_without_allocation) {
    char storage[64];
    obs::log_buffer out(storage, sizeof(storage));
    obs::format_to(out, "x={} y={} z={} {{}}", 42, "abc", true);
    CRPC_CHECK_EQ(std::string(out.data(), out.size()), std::string("x=42 y=abc z=true {}"));
    CRPC_CHECK_FALSE(out.truncated());

    char small[8];
    obs::log_buffer truncated(small, sizeof(small));
    obs::format_to(truncated, "abcdefghij");
    CRPC_CHECK_EQ(truncated.size(), std::size_t{8});
    CRPC_CHECK(truncated.truncated());
}

CRPC_TEST(logger_submit_does_not_allocate_after_registration) {
    obs::logger& log = obs::logger::instance();
    log.reset();

    const std::string path = "obs_noalloc.log";
    remove_file(path);
    log.configure(make_log_config(path, 8192));

    const obs::log_record record = obs::make_record(obs::log_level::info, "steady state");
    log.submit(record);

    const std::size_t before = g_new_calls.load(std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i) {
        log.submit(record);
    }
    CRPC_CHECK_EQ(g_new_calls.load(std::memory_order_relaxed), before);
    log.reset();
}

CRPC_TEST(logger_ring_overflow_drops_and_counts) {
    obs::logger& log = obs::logger::instance();
    log.reset();

    const std::string path = "obs_overflow.log";
    remove_file(path);
    log.configure(make_log_config(path, 4));

    for (int i = 0; i < 10; ++i) {
        const obs::log_record record = obs::make_record(obs::log_level::info, "overflow");
        log.submit(record);
    }

    // A four-slot SPSC ring holds four records; the remaining six are dropped
    // and counted.
    CRPC_CHECK_EQ(log.dropped_total(), std::uint64_t{6});
    CRPC_CHECK_EQ(log.written_records(), std::uint64_t{0});
    log.reset();
}

CRPC_TEST(logger_level_filtering_and_shutdown_flush) {
    obs::logger& log = obs::logger::instance();
    log.reset();

    const std::string path = "obs_levels.log";
    remove_file(path);
    log.configure(make_log_config(path, 256));
    log.set_level(obs::log_level::warn);
    log.start();

    CRPC_LOG_INFO("hidden {}", 1);
    CRPC_LOG_WARN("visible {}", 2);
    CRPC_LOG_ERROR("visible error");
    log.shutdown();

    const std::string text = read_file(path);
    CRPC_CHECK(text.find("hidden") == std::string::npos);
    CRPC_CHECK(text.find("visible 2") != std::string::npos);
    CRPC_CHECK(text.find("visible error") != std::string::npos);
    CRPC_CHECK_EQ(count_newlines(text), std::size_t{2});
    CRPC_CHECK_EQ(log.written_records(), std::uint64_t{2});
    log.reset();
}

CRPC_TEST(logger_rotates_by_size) {
    obs::logger& log = obs::logger::instance();
    log.reset();

    const std::string path = "obs_rotation.log";
    remove_file(path);
    remove_file(path + ".1");
    obs::logger_config config = make_log_config(path, 1024);
    config.max_file_size = 150;
    log.configure(config);
    log.start();

    for (int i = 0; i < 20; ++i) {
        CRPC_LOG_INFO("rotation record {}", i);
    }
    log.shutdown();

    std::ifstream rotated(path + ".1", std::ios::binary);
    CRPC_CHECK(rotated.good());
    CRPC_CHECK(!read_file(path).empty());
    log.reset();
}

CRPC_TEST(logger_shutdown_flushes_without_start) {
    obs::logger& log = obs::logger::instance();
    log.reset();

    const std::string path = "obs_no_start.log";
    remove_file(path);
    log.configure(make_log_config(path, 256));

    CRPC_LOG_INFO("buffered {}", 7);
    log.shutdown();

    const std::string text = read_file(path);
    CRPC_CHECK(text.find("buffered 7") != std::string::npos);
    CRPC_CHECK_EQ(count_newlines(text), std::size_t{1});
    log.reset();
}

CRPC_TEST(logger_concurrent_threads_lose_nothing) {
    obs::logger& log = obs::logger::instance();
    log.reset();

    const std::string path = "obs_concurrent.log";
    remove_file(path);
    log.configure(make_log_config(path, 2048));
    log.set_level(obs::log_level::trace);
    log.start();

    constexpr int thread_count = 8;
    constexpr int per_thread = 500;
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (int thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([thread]() {
            for (int index = 0; index < per_thread; ++index) {
                CRPC_LOG_INFO("thread {} record {}", thread, index);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    log.shutdown();

    const std::string text = read_file(path);
    CRPC_CHECK_EQ(count_newlines(text),
                  static_cast<std::size_t>(thread_count * per_thread));
    CRPC_CHECK_EQ(log.dropped_total(), std::uint64_t{0});
    log.reset();
}

CRPC_TEST(logger_structured_fields_and_redaction) {
    obs::logger& log = obs::logger::instance();
    log.reset();

    const std::string path = "obs_redaction.log";
    remove_file(path);
    log.configure(make_log_config(path, 256));
    log.start();
    log.set_redactor(&mask_password, nullptr);

    {
        obs::log_event event(obs::log_level::info, "login");
        event.field("user", "bob").field("password", "secret");
    }
    log.shutdown();

    const std::string text = read_file(path);
    CRPC_CHECK(text.find("password=***") != std::string::npos);
    CRPC_CHECK(text.find("secret") == std::string::npos);
    CRPC_CHECK(text.find("user=bob") != std::string::npos);
    log.reset();
}

CRPC_TEST(logger_compressor_hook_is_used) {
    obs::logger& log = obs::logger::instance();
    log.reset();

    const std::string path = "obs_compressed.log";
    remove_file(path);
    log.configure(make_log_config(path, 256));
    log.start();

    compressor_state state;
    log.set_compressor(&marker_compressor, &state);

    CRPC_LOG_INFO("compress me");
    log.shutdown();

    CRPC_CHECK_EQ(read_file(path), std::string("Z"));
    CRPC_CHECK(state.calls.load(std::memory_order_relaxed) > 0);
    log.reset();
}

// --- Tracer ------------------------------------------------------------------

CRPC_TEST(tracer_parent_child_relation) {
    obs::tracer& tracer = obs::tracer::instance();
    tracer.reset();

    collecting_exporter exported;
    obs::tracer_config config;
    config.sample_rate = 1.0;
    config.queue_capacity = 64;
    tracer.configure(config);
    tracer.set_exporter(&exported);
    tracer.start();

    {
        obs::span root = tracer.start_span("root");
        CRPC_CHECK(root.active());
        CRPC_CHECK(root.sampled());

        obs::span child = root.start_child("child");
        CRPC_CHECK(child.active());
        child.set_attribute("key", "value");
        child.set_status(crpc::rpc_code::ok);
        child.finish();
        root.finish();
    }
    tracer.shutdown();

    std::lock_guard<std::mutex> lock(exported.mutex);
    CRPC_CHECK_EQ(exported.spans.size(), std::size_t{2});

    const obs::span_data* root_data = nullptr;
    const obs::span_data* child_data = nullptr;
    for (const obs::span_data& data : exported.spans) {
        const std::string_view name(data.name, data.name_len);
        if (name == "root") {
            root_data = &data;
        } else if (name == "child") {
            child_data = &data;
        }
    }
    CRPC_CHECK(root_data != nullptr);
    CRPC_CHECK(child_data != nullptr);
    if (root_data != nullptr && child_data != nullptr) {
        CRPC_CHECK(child_data->trace == root_data->trace);
        CRPC_CHECK(child_data->parent == root_data->span);
        CRPC_CHECK_EQ(static_cast<int>(child_data->attribute_count), 1);
    }
    tracer.reset();
}

CRPC_TEST(tracer_sampling_is_deterministic) {
    obs::trace_id id;
    for (std::size_t i = 0; i < id.bytes.size(); ++i) {
        id.bytes[i] = static_cast<std::byte>(i + 1);
    }

    const bool first = obs::tracer::sample_trace(id, 0.5);
    for (int i = 0; i < 16; ++i) {
        CRPC_CHECK_EQ(obs::tracer::sample_trace(id, 0.5), first);
    }
    CRPC_CHECK_FALSE(obs::tracer::sample_trace(id, 0.0));
    CRPC_CHECK(obs::tracer::sample_trace(id, 1.0));
}

CRPC_TEST(tracer_export_queue_overflow_is_counted) {
    obs::tracer& tracer = obs::tracer::instance();
    tracer.reset();

    blocking_exporter exporter;
    obs::tracer_config config;
    config.sample_rate = 1.0;
    config.queue_capacity = 4;
    tracer.configure(config);
    tracer.set_exporter(&exporter);
    tracer.start();

    {
        obs::span blocker = tracer.start_span("blocker");
        blocker.finish();
    }

    for (int attempt = 0; attempt < 2000 && exporter.calls.load() == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CRPC_CHECK(exporter.calls.load() > 0);

    for (int i = 0; i < 64; ++i) {
        obs::span span = tracer.start_span("overflow");
        span.finish();
    }
    CRPC_CHECK(tracer.dropped_spans() > 0);

    exporter.release.store(true, std::memory_order_release);
    tracer.shutdown();
    tracer.reset();
}

CRPC_TEST(tracer_disabled_path_does_not_allocate) {
    obs::tracer& tracer = obs::tracer::instance();
    tracer.reset();

    obs::tracer_config config;
    tracer.configure(config);

    const std::size_t before = g_new_calls.load(std::memory_order_relaxed);
    bool all_inactive = true;
    for (int i = 0; i < 10000; ++i) {
        obs::span disabled = tracer.start_span("disabled");
        all_inactive = all_inactive && !disabled.active();
    }
    CRPC_CHECK(all_inactive);
    CRPC_CHECK_EQ(g_new_calls.load(std::memory_order_relaxed), before);
    tracer.reset();
}

CRPC_TEST(tracer_disabled_path_reads_no_clock) {
    obs::tracer& tracer = obs::tracer::instance();
    tracer.reset();

    g_clock_calls.store(0, std::memory_order_relaxed);
    obs::tracer_config config;
    config.clock = &counting_clock;
    tracer.configure(config);

    CRPC_CHECK_FALSE(tracer.enabled());
    obs::span disabled = tracer.start_span("disabled");
    CRPC_CHECK_FALSE(disabled.active());
    CRPC_CHECK_EQ(g_clock_calls.load(std::memory_order_relaxed), 0);
    CRPC_CHECK_EQ(tracer.dropped_spans(), std::uint64_t{0});
    tracer.reset();
}

CRPC_TEST(tracer_tail_sampling_hook) {
    obs::tracer& tracer = obs::tracer::instance();
    tracer.reset();

    collecting_exporter exported;
    obs::tracer_config config;
    config.sample_rate = 1.0;
    config.queue_capacity = 64;
    tracer.configure(config);
    tracer.set_exporter(&exported);
    tracer.set_tail_sampler(&drop_all_tail, nullptr);
    tracer.start();

    {
        obs::span span = tracer.start_span("dropped");
        span.finish();
    }
    tracer.shutdown();

    CRPC_CHECK_EQ(exported.spans.size(), std::size_t{0});
    CRPC_CHECK_EQ(tracer.tail_dropped_spans(), std::uint64_t{1});
    tracer.reset();
}

CRPC_TEST(trace_context_propagates_across_hop) {
    obs::tracer& tracer = obs::tracer::instance();
    tracer.reset();

    collecting_exporter exported;
    obs::tracer_config config;
    config.sample_rate = 1.0;
    config.queue_capacity = 64;
    tracer.configure(config);
    tracer.set_exporter(&exported);
    tracer.start();

    {
        obs::span sender = tracer.start_span("sender");
        const obs::trace_context sender_context = sender.context();

        char header[55];
        const std::size_t length =
            obs::format_traceparent(sender_context, header, sizeof(header));
        CRPC_CHECK_EQ(length, std::size_t{55});

        const obs::traceparent_result received =
            obs::parse_traceparent(std::string_view(header, length));
        CRPC_CHECK(received.ok());

        obs::span receiver = tracer.start_span("receiver", received.context);
        CRPC_CHECK(receiver.sampled());
        CRPC_CHECK(receiver.context().trace == sender_context.trace);
        CRPC_CHECK(receiver.context().parent == sender_context.span);

        receiver.finish();
        sender.finish();
    }
    tracer.shutdown();

    std::lock_guard<std::mutex> lock(exported.mutex);
    CRPC_CHECK_EQ(exported.spans.size(), std::size_t{2});
    tracer.reset();
}

CRPC_TEST_MAIN()
