#include <crpc/ipc/channel.hpp>

#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

namespace crpc {
namespace ipc {
namespace {

// The mapping is one flat region; these are the offsets into it. Kept as functions
// rather than one struct so the payload areas can be computed without constructing
// anything over the whole region.
[[nodiscard]] ring_control* outbound_control(std::byte* base) noexcept {
    return reinterpret_cast<ring_control*>(base + sizeof(channel_header));
}

[[nodiscard]] ring_control* inbound_control(std::byte* base) noexcept {
    return reinterpret_cast<ring_control*>(base + sizeof(channel_header) +
                                           sizeof(ring_control));
}

[[nodiscard]] std::byte* outbound_payload(std::byte* base) noexcept {
    return base + channel_fixed_size;
}

[[nodiscard]] std::byte* inbound_payload(std::byte* base, std::size_t capacity) noexcept {
    return base + channel_fixed_size + capacity;
}

} // namespace

channel::~channel() = default;

channel::channel(channel&& other) noexcept
    : mapping_(std::move(other.mapping_)),
      outbound_(std::move(other.outbound_)),
      inbound_(std::move(other.inbound_)),
      capacity_(other.capacity_),
      creator_(other.creator_),
      name_(std::move(other.name_)) {
    other.capacity_ = 0;
    other.name_.clear();
    // The views carry raw pointers into the old mapping; after the move those
    // addresses are only valid if the mapping itself did not move its pages, which it
    // does not -- the mapping object moved, not the region. Re-binding is still done
    // so the code does not depend on that reasoning.
    bind_views();
}

channel& channel::operator=(channel&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    mapping_ = std::move(other.mapping_);
    outbound_ = std::move(other.outbound_);
    inbound_ = std::move(other.inbound_);
    capacity_ = other.capacity_;
    creator_ = other.creator_;
    name_ = std::move(other.name_);
    other.capacity_ = 0;
    other.name_.clear();
    bind_views();
    return *this;
}

void channel::bind_views() {
    if (!mapping_.valid() || capacity_ == 0) {
        outbound_ = ring_view{};
        inbound_ = ring_view{};
        return;
    }
    std::byte* base = mapping_.bytes().data();
    if (creator_) {
        outbound_ = ring_view::attach_producer(outbound_control(base),
                                               span<std::byte>(outbound_payload(base),
                                                               capacity_));
        inbound_ = ring_view::attach_consumer(
            inbound_control(base),
            span<std::byte>(inbound_payload(base, capacity_), capacity_));
    } else {
        outbound_ = ring_view::attach_producer(
            inbound_control(base),
            span<std::byte>(inbound_payload(base, capacity_), capacity_));
        inbound_ = ring_view::attach_consumer(outbound_control(base),
                                              span<std::byte>(outbound_payload(base),
                                                              capacity_));
    }
}

result<channel> channel::create(std::string_view name, std::size_t capacity) {
    if (capacity == 0) {
        return rpc_error{rpc_code::invalid_argument,
                         "a channel ring must have room for at least one byte"};
    }
    // Create the mapping at the full size; `open` with `create` refuses an existing
    // name, which is what keeps two creators from racing into one region.
    auto mapping = fs::shared_memory::open(name, channel_mapping_size(capacity),
                                           fs::shared_memory_mode::create);
    if (!mapping) {
        return mapping.error();
    }

    channel out;
    out.mapping_ = std::move(mapping.value());
    out.capacity_ = capacity;
    out.name_.assign(name.data(), name.size());

    std::byte* base = out.mapping_.bytes().data();
    channel_header* header = reinterpret_cast<channel_header*>(base);
    header->magic = channel_magic;
    header->capacity = capacity;
    // Publish the readiness flag last: every field it guards must already be in
    // memory, or a joiner that attaches now would read a half-written channel.
    header->ready.store(0, std::memory_order_relaxed);

    ring_view::initialise(outbound_control(base), capacity);
    ring_view::initialise(inbound_control(base), capacity);

    header->ready.store(1, std::memory_order_release);
    out.creator_ = true;
    out.bind_views();
    return out;
}

result<channel> channel::join(std::string_view name) {
    auto mapping = fs::shared_memory::open(name, 0, fs::shared_memory_mode::open_existing);
    if (!mapping) {
        return mapping.error();
    }
    std::byte* base = mapping.value().bytes().data();
    if (mapping.value().size() < channel_fixed_size) {
        return rpc_error{rpc_code::failed_precondition,
                         "the mapping is too small to be a channel"};
    }
    const channel_header* header = reinterpret_cast<const channel_header*>(base);
    if (header->magic != channel_magic) {
        return rpc_error{rpc_code::failed_precondition,
                         "the mapping was not created by ipc::channel"};
    }
    // Acquire pairs with the creator's release: seeing ready == 1 means the two ring
    // controls are visible too.
    if (header->ready.load(std::memory_order_acquire) != 1) {
        return rpc_error{rpc_code::unavailable,
                         "the channel exists but is still being initialised"};
    }
    const std::size_t capacity = static_cast<std::size_t>(header->capacity);
    if (capacity == 0 || mapping.value().size() < channel_mapping_size(capacity)) {
        return rpc_error{rpc_code::failed_precondition,
                         "the channel's capacity does not fit its mapping"};
    }

    channel out;
    out.mapping_ = std::move(mapping.value());
    out.capacity_ = capacity;
    out.creator_ = false;
    out.name_.assign(name.data(), name.size());
    // The joiner is the mirror image: it produces into the ring the creator consumes
    // from, and consumes from the ring the creator produces into. bind_views is the
    // single place that knows this, so a move cannot silently flip it.
    out.bind_views();
    return out;
}

bool channel::try_send(span<const std::byte> data) noexcept {
    const bool queued = outbound_.try_write(data);
    if (queued && shard_ != nullptr) {
        // Wake a reader parked on this runtime. Cheap and idempotent, and it must
        // not be skipped: a reader that parked before this write would otherwise
        // sit until its timeout even though the bytes are already visible.
        shard_->notify();
    }
    return queued;
}

std::size_t channel::try_receive(span<std::byte> out) noexcept {
    return inbound_.try_read(out);
}

result<std::size_t> channel::receive(span<std::byte> out,
                                     std::chrono::nanoseconds timeout) {
    for (;;) {
        const std::size_t got = inbound_.try_read(out);
        if (got != 0) {
            return got;
        }
        if (timeout <= std::chrono::nanoseconds::zero()) {
            return std::size_t{0}; // no wait requested: an empty read is the answer
        }
        // On a shard thread a park is the only acceptable wait: blocking here would
        // stop every other task on the shard, and a peer that is itself waiting on
        // this shard would then deadlock. park() reports failed_precondition when
        // this is not a coroutine of the runtime, which is the signal to fall back.
        if (shard_ != nullptr && shard_->loop_mode() && shard_->is_owner_thread()) {
            const status parked = shard_->park();
            if (parked.ok()) {
                continue;
            }
            if (parked.code() != rpc_code::failed_precondition) {
                return parked.error();
            }
            // Not inside a coroutine of this runtime: fall through to a bounded wait
            // rather than surfacing a spurious error.
        }
        // Plain-thread fallback. There is no cross-process signal to wait on, so
        // this is a bounded poll: it is the honest behaviour for a transport whose
        // peer cannot notify us, and the timeout is the caller's contract.
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
            const std::size_t retried = inbound_.try_read(out);
            if (retried != 0) {
                return retried;
            }
        }
        return rpc_error{rpc_code::deadline_exceeded,
                         "no data arrived before the timeout"};
    }
}

status channel::send(span<const std::byte> data, std::chrono::nanoseconds timeout) {
    for (;;) {
        if (outbound_.try_write(data)) {
            if (shard_ != nullptr) {
                shard_->notify();
            }
            return {};
        }
        if (timeout <= std::chrono::nanoseconds::zero()) {
            return status{rpc_code::resource_exhausted,
                          "the channel ring is full"};
        }
        if (shard_ != nullptr && shard_->loop_mode() && shard_->is_owner_thread()) {
            const status parked = shard_->park();
            if (parked.ok()) {
                continue;
            }
            if (parked.code() != rpc_code::failed_precondition) {
                return parked;
            }
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
            if (outbound_.try_write(data)) {
                return {};
            }
        }
        return status{rpc_code::deadline_exceeded};
    }
}

std::size_t channel::pending() const noexcept { return inbound_.size(); }

std::size_t channel::send_capacity() const noexcept { return outbound_.free_space(); }

} // namespace ipc
} // namespace crpc
