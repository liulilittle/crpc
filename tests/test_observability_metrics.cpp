// CRPC -- metrics interface tests.
//
// The metrics layer must be a pure no-op until an exporter is installed. This
// binary therefore replaces the global allocation functions and asserts that
// the disabled path performs no allocation at all; the same file then installs
// an in-memory exporter and asserts the sampled values.
#include "test_framework.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <ostream>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

#include <crpc/observability/metrics.hpp>

namespace {

void* allocate_aligned(std::size_t size, std::size_t alignment) {
#if defined(_MSC_VER)
    return _aligned_malloc(size, alignment);
#else
    void* memory = nullptr;
    if (posix_memalign(&memory, alignment, size) != 0) {
        return nullptr;
    }
    return memory;
#endif
}

void free_aligned(void* memory) noexcept {
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

} // namespace

namespace {

std::atomic<std::size_t> g_allocations{0};

} // namespace

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (size == 0) {
        size = 1;
    }
    void* memory = std::malloc(size);
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }
    return memory;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

void* operator new(std::size_t size, std::align_val_t alignment) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (size == 0) {
        size = 1;
    }
    void* memory = allocate_aligned(size, static_cast<std::size_t>(alignment));
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }
    return memory;
}

void operator delete(void* memory, std::align_val_t) noexcept { free_aligned(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { free_aligned(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}

namespace {

namespace obs = crpc::obs;

struct memory_exporter final : obs::metric_exporter {
    static constexpr std::size_t kCount =
        static_cast<std::size_t>(obs::metric_id::count);

    std::atomic<crpc::u64> counter[kCount]{};
    std::atomic<crpc::u64> gauge[kCount]{};
    std::atomic<crpc::u64> histogram_count[kCount]{};
    std::atomic<crpc::u64> histogram_sum[kCount]{};

    void on_metric(obs::metric_id id, obs::metric_kind kind,
                   crpc::u64 value) noexcept override {
        const std::size_t index = static_cast<std::size_t>(id);
        if (index >= kCount) {
            return;
        }
        switch (kind) {
        case obs::metric_kind::counter:
            counter[index].store(value, std::memory_order_relaxed);
            break;
        case obs::metric_kind::gauge:
            gauge[index].store(value, std::memory_order_relaxed);
            break;
        case obs::metric_kind::histogram:
            histogram_count[index].fetch_add(1, std::memory_order_relaxed);
            histogram_sum[index].fetch_add(value, std::memory_order_relaxed);
            break;
        }
    }

    [[nodiscard]] crpc::u64 count(obs::metric_id id) const noexcept {
        return counter[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
    }
    [[nodiscard]] crpc::u64 gauge_value(obs::metric_id id) const noexcept {
        return gauge[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
    }
    [[nodiscard]] crpc::u64 histogram_samples(obs::metric_id id) const noexcept {
        return histogram_count[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
    }
    [[nodiscard]] crpc::u64 histogram_total(obs::metric_id id) const noexcept {
        return histogram_sum[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
    }
};

} // namespace

CRPC_TEST(metrics_disabled_path_allocates_nothing) {
    obs::metrics& metrics = obs::metrics::instance();
    metrics.reset();
    CRPC_CHECK_FALSE(metrics.enabled());

    const obs::counter calls = metrics.counter_of(obs::metric_id::rpc_client_calls);
    const obs::gauge inflight = metrics.gauge_of(obs::metric_id::rpc_client_inflight);
    const obs::histogram depth =
        metrics.histogram_of(obs::metric_id::rpc_client_queue_depth);

    // Warm any lazy, one-time state before measuring.
    calls.add(0);
    inflight.set(0);
    depth.observe(0);

    const std::size_t before = g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i) {
        calls.add();
        inflight.set(static_cast<crpc::u64>(i));
        depth.observe(static_cast<crpc::u64>(i));
    }
    const std::size_t after = g_allocations.load(std::memory_order_relaxed);
    CRPC_CHECK_EQ(std::size_t{0}, after - before);

    // A disabled metric keeps its zero value and reports no samples.
    CRPC_CHECK_EQ(crpc::u64{0}, metrics.value(obs::metric_id::rpc_client_calls));
    CRPC_CHECK_EQ(crpc::u64{0}, metrics.samples(obs::metric_id::rpc_client_queue_depth));
}

CRPC_TEST(metrics_exporter_receives_sampled_values) {
    obs::metrics& metrics = obs::metrics::instance();
    metrics.reset();

    memory_exporter exporter;
    metrics.set_exporter(&exporter);
    CRPC_CHECK(metrics.enabled());

    const obs::counter calls = metrics.counter_of(obs::metric_id::rpc_client_calls);
    calls.add();
    calls.add(4);
    CRPC_CHECK_EQ(crpc::u64{5}, exporter.count(obs::metric_id::rpc_client_calls));
    CRPC_CHECK_EQ(crpc::u64{5}, metrics.value(obs::metric_id::rpc_client_calls));

    const obs::counter errors = metrics.counter_of(obs::metric_id::rpc_client_errors);
    errors.add(2);
    CRPC_CHECK_EQ(crpc::u64{2}, exporter.count(obs::metric_id::rpc_client_errors));

    const obs::gauge inflight =
        metrics.gauge_of(obs::metric_id::rpc_client_inflight);
    inflight.set(7);
    inflight.set(3);
    CRPC_CHECK_EQ(crpc::u64{3}, exporter.gauge_value(obs::metric_id::rpc_client_inflight));
    CRPC_CHECK_EQ(crpc::u64{3}, metrics.value(obs::metric_id::rpc_client_inflight));

    const obs::histogram latency =
        metrics.histogram_of(obs::metric_id::rpc_client_queue_depth);
    latency.observe(10);
    latency.observe(30);
    CRPC_CHECK_EQ(crpc::u64{2},
                  exporter.histogram_samples(obs::metric_id::rpc_client_queue_depth));
    CRPC_CHECK_EQ(crpc::u64{40},
                  exporter.histogram_total(obs::metric_id::rpc_client_queue_depth));
    CRPC_CHECK_EQ(crpc::u64{2},
                  metrics.samples(obs::metric_id::rpc_client_queue_depth));

    metrics.reset();
    CRPC_CHECK_FALSE(metrics.enabled());
}

CRPC_TEST(metrics_static_helpers_and_names) {
    obs::metrics& metrics = obs::metrics::instance();
    metrics.reset();
    memory_exporter exporter;
    metrics.set_exporter(&exporter);

    obs::metrics::increment(obs::metric_id::rpc_server_calls);
    obs::metrics::increment(obs::metric_id::rpc_server_calls, 2);
    obs::metrics::set_gauge(obs::metric_id::rpc_server_inflight, 4);
    obs::metrics::record(obs::metric_id::rpc_server_queue_depth, 8);

    CRPC_CHECK_EQ(crpc::u64{3}, exporter.count(obs::metric_id::rpc_server_calls));
    CRPC_CHECK_EQ(crpc::u64{4}, exporter.gauge_value(obs::metric_id::rpc_server_inflight));
    CRPC_CHECK_EQ(crpc::u64{1},
                  exporter.histogram_samples(obs::metric_id::rpc_server_queue_depth));

    CRPC_CHECK(std::string(obs::to_string(obs::metric_id::rpc_client_calls)) ==
               "rpc.client.calls");
    CRPC_CHECK(std::string(obs::to_string(obs::metric_id::rpc_server_inflight)) ==
               "rpc.server.inflight");

    metrics.reset();
}

CRPC_TEST_MAIN()

