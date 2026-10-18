// CRPC -- tracer implementation.
#include <crpc/observability/tracer.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace crpc {
namespace obs {
namespace {

std::size_t round_up_power_of_two(std::size_t value) noexcept {
    if (value < 2) {
        return 2;
    }
    std::size_t result = 2;
    while (result < value) {
        if (result > std::numeric_limits<std::size_t>::max() / 2) {
            return result;
        }
        result <<= 1;
    }
    return result;
}

std::uint64_t default_clock(void*) noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::uint64_t& rng_state() noexcept {
    static thread_local std::uint64_t state = 0;
    if (state == 0) {
        state = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        state ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&state));
        if (state == 0) {
            state = 0x9e3779b97f4a7c15ull;
        }
    }
    return state;
}

std::uint64_t next_random() noexcept {
    std::uint64_t& state = rng_state();
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

trace_id generate_trace_id() noexcept {
    trace_id id;
    const std::uint64_t high = next_random();
    const std::uint64_t low = next_random();
    std::memcpy(id.bytes.data(), &high, sizeof(high));
    std::memcpy(id.bytes.data() + sizeof(high), &low, sizeof(low));
    if (!id.valid()) {
        id.bytes.back() = std::byte{1};
    }
    return id;
}

span_id generate_span_id() noexcept {
    std::uint64_t value = next_random();
    if (value == 0) {
        value = 1;
    }
    span_id id;
    std::memcpy(id.bytes.data(), &value, sizeof(value));
    return id;
}

std::uint64_t hash_trace(const trace_id& id) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::byte byte : id.bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t rate_threshold(double rate) noexcept {
    if (!(rate > 0.0)) {
        return 0;
    }
    if (rate >= 1.0) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const double scaled =
        rate * static_cast<double>(std::numeric_limits<std::uint64_t>::max());
    if (!(scaled > 0.0)) {
        return 0;
    }
    return static_cast<std::uint64_t>(scaled);
}

bool sampled_by_threshold(const trace_id& id, std::uint64_t threshold) noexcept {
    if (threshold == 0) {
        return false;
    }
    if (threshold == std::numeric_limits<std::uint64_t>::max()) {
        return true;
    }
    return hash_trace(id) < threshold;
}

} // namespace

namespace detail {

// Bounded multi-producer/single-consumer queue (Vyukov ring). Producers never
// block: a full queue makes `try_push` return false.
class span_queue {
public:
    explicit span_queue(std::size_t capacity) noexcept
        : capacity_(round_up_power_of_two(capacity)),
          mask_(capacity_ - 1),
          cells_(new (std::nothrow) cell[capacity_]) {
        if (cells_) {
            for (std::size_t i = 0; i < capacity_; ++i) {
                cells_[i].sequence.store(i, std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] bool valid() const noexcept { return cells_ != nullptr; }

    bool try_push(const span_data& data) noexcept {
        if (!cells_) {
            return false;
        }
        std::size_t position = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell& slot = cells_[position & mask_];
            const std::size_t sequence = slot.sequence.load(std::memory_order_acquire);
            const std::ptrdiff_t difference = static_cast<std::ptrdiff_t>(sequence) -
                                              static_cast<std::ptrdiff_t>(position);
            if (difference == 0) {
                if (enqueue_pos_.compare_exchange_weak(position, position + 1,
                                                       std::memory_order_relaxed)) {
                    slot.value = data;
                    slot.sequence.store(position + 1, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    bool try_pop(span_data& out) noexcept {
        if (!cells_) {
            return false;
        }
        std::size_t position = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell& slot = cells_[position & mask_];
            const std::size_t sequence = slot.sequence.load(std::memory_order_acquire);
            const std::ptrdiff_t difference =
                static_cast<std::ptrdiff_t>(sequence) -
                static_cast<std::ptrdiff_t>(position + 1);
            if (difference == 0) {
                if (dequeue_pos_.compare_exchange_weak(position, position + 1,
                                                       std::memory_order_relaxed)) {
                    out = slot.value;
                    slot.sequence.store(position + capacity_, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

private:
    struct cell {
        std::atomic<std::size_t> sequence;
        span_data value;
    };

    std::size_t capacity_;
    std::size_t mask_;
    std::unique_ptr<cell[]> cells_;
    std::atomic<std::size_t> enqueue_pos_{0};
    std::atomic<std::size_t> dequeue_pos_{0};
};

} // namespace detail

// --- Span --------------------------------------------------------------------

span::span() noexcept : tracer_(nullptr), finished_(true), sampled_(false) {}

span::span(span&& other) noexcept
    : tracer_(other.tracer_), finished_(other.finished_), sampled_(other.sampled_) {
    if (tracer_ != nullptr) {
        data_ = other.data_;
    }
    other.tracer_ = nullptr;
    other.finished_ = true;
    other.sampled_ = false;
}

span& span::operator=(span&& other) noexcept {
    if (this != &other) {
        finish();
        tracer_ = other.tracer_;
        finished_ = other.finished_;
        sampled_ = other.sampled_;
        if (tracer_ != nullptr) {
            data_ = other.data_;
        }
        other.tracer_ = nullptr;
        other.finished_ = true;
        other.sampled_ = false;
    }
    return *this;
}

span::~span() { finish(); }

void span::finish() noexcept {
    if (tracer_ == nullptr || finished_) {
        return;
    }
    finished_ = true;
    if (sampled_) {
        data_.end_ns = tracer_->current_time();
        tracer_->enqueue(data_);
    }
}

trace_context span::context() const noexcept {
    if (tracer_ == nullptr) {
        return trace_context{};
    }
    trace_context result;
    result.trace = data_.trace;
    result.span = data_.span;
    result.parent = data_.parent;
    result.flags = data_.flags;
    return result;
}

void span::set_attribute(std::string_view key, std::string_view value) noexcept {
    if (tracer_ == nullptr || finished_) {
        return;
    }
    if (data_.attribute_count >= span_data::max_attributes) {
        if (data_.dropped_attributes != 255) {
            ++data_.dropped_attributes;
        }
        return;
    }

    span_attribute& attribute = data_.attributes[data_.attribute_count];
    const std::size_t key_count =
        key.size() < span_attribute::max_key ? key.size() : span_attribute::max_key;
    const std::size_t value_count =
        value.size() < span_attribute::max_value ? value.size() : span_attribute::max_value;

    if (key_count != 0) {
        std::memcpy(attribute.key, key.data(), key_count);
    }
    if (value_count != 0) {
        std::memcpy(attribute.value, value.data(), value_count);
    }
    attribute.key_len = static_cast<u8>(key_count);
    attribute.value_len = static_cast<u8>(value_count);
    attribute.truncated = (key_count != key.size() || value_count != value.size()) ? 1 : 0;
    ++data_.attribute_count;
}

void span::set_status(rpc_code code) noexcept {
    if (tracer_ != nullptr && !finished_) {
        data_.status = code;
    }
}

span span::start_child(std::string_view name) noexcept {
    if (tracer_ == nullptr) {
        return span{};
    }
    return tracer_->start_span(name, context());
}

// --- Tracer ------------------------------------------------------------------

tracer::tracer() noexcept {
    sample_threshold_.store(rate_threshold(0.01), std::memory_order_relaxed);
}

tracer::~tracer() { shutdown(); }

void tracer::configure(const tracer_config& config) noexcept {
    queue_capacity_ = config.queue_capacity < 2 ? 2 : config.queue_capacity;
    sample_threshold_.store(rate_threshold(config.sample_rate), std::memory_order_relaxed);
    clock_ = config.clock;
    clock_user_ = config.clock_user;
}

void tracer::set_exporter(exporter* sink) noexcept {
    exporter_ = sink;
    enabled_.store(sink != nullptr, std::memory_order_release);
    if (sink != nullptr) {
        // Registering an exporter is the point at which export becomes useful;
        // starting here keeps the common "configure, set_exporter, log" order
        // working without a separate call. `start()` remains idempotent.
        start();
    }
}

void tracer::set_tail_sampler(tail_sampler_fn function, void* user) noexcept {
    tail_sampler_ = function;
    tail_sampler_user_ = user;
}

void tracer::start() noexcept {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    try {
        queue_ = std::unique_ptr<detail::span_queue>(new detail::span_queue(queue_capacity_));
        if (!queue_ || !queue_->valid()) {
            queue_.reset();
            return;
        }
        running_.store(true, std::memory_order_release);
        worker_ = std::thread(&tracer::export_loop, this);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        queue_.reset();
    }
}

void tracer::shutdown() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    wait_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    queue_.reset();
}

void tracer::reset() noexcept {
    shutdown();
    exporter_ = nullptr;
    enabled_.store(false, std::memory_order_release);
    dropped_.store(0, std::memory_order_relaxed);
    exported_.store(0, std::memory_order_relaxed);
    tail_dropped_.store(0, std::memory_order_relaxed);
    sample_threshold_.store(rate_threshold(0.01), std::memory_order_relaxed);
    queue_capacity_ = 4096;
    clock_ = nullptr;
    clock_user_ = nullptr;
    tail_sampler_ = nullptr;
    tail_sampler_user_ = nullptr;
}

bool tracer::sample_trace(trace_id trace, double rate) noexcept {
    return sampled_by_threshold(trace, rate_threshold(rate));
}

std::uint64_t tracer::current_time() const noexcept {
    if (clock_ != nullptr) {
        return clock_(clock_user_);
    }
    return default_clock(nullptr);
}

span tracer::start_span(std::string_view name, const trace_context& parent) noexcept {
    span result;
    if (!enabled_.load(std::memory_order_acquire)) {
        return result;
    }

    trace_id trace;
    span_id parent_span;
    bool sampled = false;

    if (parent.trace.valid()) {
        trace = parent.trace;
        parent_span = parent.span;
        sampled = parent.sampled();
    } else {
        trace = generate_trace_id();
        sampled = sampled_by_threshold(
            trace, sample_threshold_.load(std::memory_order_relaxed));
    }

    result.tracer_ = this;
    result.data_.trace = trace;
    result.data_.span = generate_span_id();
    result.data_.parent = parent_span;
    result.data_.flags = sampled ? trace_context::sampled_flag : 0;
    result.data_.status = rpc_code::ok;
    result.data_.attribute_count = 0;
    result.data_.dropped_attributes = 0;
    result.data_.start_ns = sampled ? current_time() : 0;
    result.data_.end_ns = 0;

    const std::size_t name_count =
        name.size() < span_data::max_name ? name.size() : span_data::max_name;
    if (name_count != 0) {
        std::memcpy(result.data_.name, name.data(), name_count);
    }
    result.data_.name_len = static_cast<u8>(name_count);

    result.finished_ = false;
    result.sampled_ = sampled;
    return result;
}

void tracer::enqueue(const span_data& data) noexcept {
    if (!running_.load(std::memory_order_acquire) || queue_ == nullptr) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!queue_->try_push(data)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::size_t tracer::drain(std::size_t maximum) noexcept {
    if (queue_ == nullptr) {
        return 0;
    }
    if (maximum > batch_.size()) {
        maximum = batch_.size();
    }
    std::size_t count = 0;
    while (count < maximum && queue_->try_pop(batch_[count])) {
        ++count;
    }
    return count;
}

void tracer::export_batch(std::size_t count) noexcept {
    if (count == 0) {
        return;
    }

    std::size_t kept = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (tail_sampler_ != nullptr && !tail_sampler_(tail_sampler_user_, batch_[i])) {
            tail_dropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (kept != i) {
            batch_[kept] = batch_[i];
        }
        ++kept;
    }

    if (kept == 0) {
        return;
    }
    if (exporter_ == nullptr) {
        dropped_.fetch_add(kept, std::memory_order_relaxed);
        return;
    }

    try {
        exporter_->export_spans(::crpc::span<const span_data>(batch_.data(), kept));
    } catch (...) {
        // An exporter must not take the process down from the worker thread.
    }
    exported_.fetch_add(kept, std::memory_order_relaxed);
}

void tracer::export_loop() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        const std::size_t count = drain(batch_.size());
        if (count == 0) {
            std::unique_lock<std::mutex> lock(wait_mutex_);
            wait_cv_.wait_for(lock, std::chrono::milliseconds(1));
            continue;
        }
        export_batch(count);
    }

    for (;;) {
        const std::size_t count = drain(batch_.size());
        if (count == 0) {
            break;
        }
        export_batch(count);
    }
}

} // namespace obs
} // namespace crpc
