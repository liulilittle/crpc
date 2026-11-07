// CRPC -- metrics implementation.
#include <crpc/observability/metrics.hpp>

#include <array>
#include <limits>

namespace crpc {
namespace obs {
namespace {

constexpr std::size_t kMetricCount = static_cast<std::size_t>(metric_id::count);

[[nodiscard]] bool valid(metric_id id) noexcept {
    return static_cast<std::size_t>(id) < kMetricCount;
}

void deliver(std::atomic<metric_exporter*>& exporter, metric_id id,
             metric_kind kind, u64 value) noexcept {
    metric_exporter* sink = exporter.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink->on_metric(id, kind, value);
    }
}

} // namespace

const char* to_string(metric_id id) noexcept {
    static const char* const names[kMetricCount] = {
        "rpc.client.calls",
        "rpc.client.errors",
        "rpc.client.inflight",
        "rpc.client.queue_depth",
        "rpc.server.calls",
        "rpc.server.errors",
        "rpc.server.inflight",
        "rpc.server.queue_depth",
    };
    const std::size_t index = static_cast<std::size_t>(id);
    return index < kMetricCount ? names[index] : "unknown";
}

// --- Handles -----------------------------------------------------------------

void counter::add(u64 delta) const noexcept {
    metrics::increment(id_, delta);
}

void gauge::set(u64 value) const noexcept {
    metrics::set_gauge(id_, value);
}

void gauge::add(std::int64_t delta) const noexcept {
    metrics::add_gauge(id_, delta);
}

void histogram::observe(u64 value) const noexcept {
    metrics::record(id_, value);
}

// --- Singleton ---------------------------------------------------------------

metrics::metrics() noexcept {
    for (std::size_t i = 0; i < metric_count; ++i) {
        values_[i].store(0, std::memory_order_relaxed);
        samples_[i].store(0, std::memory_order_relaxed);
    }
}

metrics& metrics::instance() noexcept {
    static metrics value;
    return value;
}

void metrics::set_exporter(metric_exporter* exporter) noexcept {
    exporter_.store(exporter, std::memory_order_release);
}

void metrics::reset() noexcept {
    exporter_.store(nullptr, std::memory_order_release);
    for (std::size_t i = 0; i < metric_count; ++i) {
        values_[i].store(0, std::memory_order_relaxed);
        samples_[i].store(0, std::memory_order_relaxed);
    }
}

u64 metrics::value(metric_id id) const noexcept {
    return valid(id) ? values_[static_cast<std::size_t>(id)].load(
                           std::memory_order_relaxed)
                     : 0;
}

u64 metrics::samples(metric_id id) const noexcept {
    return valid(id) ? samples_[static_cast<std::size_t>(id)].load(
                           std::memory_order_relaxed)
                     : 0;
}

void metrics::increment(metric_id id, u64 delta) noexcept {
    metrics& self = instance();
    if (!self.enabled() || !valid(id)) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(id);
    const u64 total = self.values_[index].fetch_add(delta, std::memory_order_relaxed) + delta;
    deliver(self.exporter_, id, metric_kind::counter, total);
}

void metrics::set_gauge(metric_id id, u64 value) noexcept {
    metrics& self = instance();
    if (!self.enabled() || !valid(id)) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(id);
    self.values_[index].store(value, std::memory_order_relaxed);
    deliver(self.exporter_, id, metric_kind::gauge, value);
}

void metrics::add_gauge(metric_id id, std::int64_t delta) noexcept {
    metrics& self = instance();
    if (!self.enabled() || !valid(id)) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(id);
    u64 current = self.values_[index].load(std::memory_order_relaxed);
    for (;;) {
        u64 next = 0;
        if (delta >= 0) {
            next = current + static_cast<u64>(delta);
        } else {
            const u64 magnitude = static_cast<u64>(-(delta + 1)) + 1;
            next = current > magnitude ? current - magnitude : 0;
        }
        if (self.values_[index].compare_exchange_weak(current, next,
                                                      std::memory_order_relaxed,
                                                      std::memory_order_relaxed)) {
            deliver(self.exporter_, id, metric_kind::gauge, next);
            return;
        }
    }
}

void metrics::record(metric_id id, u64 value) noexcept {
    metrics& self = instance();
    if (!self.enabled() || !valid(id)) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(id);
    self.samples_[index].fetch_add(1, std::memory_order_relaxed);
    self.values_[index].store(value, std::memory_order_relaxed);
    deliver(self.exporter_, id, metric_kind::histogram, value);
}

} // namespace obs
} // namespace crpc
