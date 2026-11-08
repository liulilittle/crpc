// CRPC -- call state machine, slot and slot-pool implementation.
#include <crpc/rpc/call.hpp>

#include <new>

namespace crpc::rpc {
namespace {

// A very large response buffer is returned to the allocator when a slot is
// recycled, so a single oversized call cannot pin memory on a pooled slot for
// the life of the client. Small buffers are kept for reuse.
constexpr std::size_t response_retain_bytes = 64 * 1024;

} // namespace

// --- State machine -----------------------------------------------------------

const char* to_string(call_state state) noexcept {
    switch (state) {
    case call_state::idle:      return "idle";
    case call_state::queued:    return "queued";
    case call_state::sent:      return "sent";
    case call_state::completed: return "completed";
    case call_state::timed_out: return "timed_out";
    case call_state::canceled:  return "canceled";
    case call_state::failed:    return "failed";
    }
    return "unknown";
}

rpc_code default_terminal_code(call_state state) noexcept {
    switch (state) {
    case call_state::completed: return rpc_code::ok;
    case call_state::timed_out: return rpc_code::deadline_exceeded;
    case call_state::canceled:  return rpc_code::canceled;
    case call_state::failed:    return rpc_code::internal;
    case call_state::idle:
    case call_state::queued:
    case call_state::sent:
        break;
    }
    return rpc_code::ok;
}

call_state load_state(const call_slot& slot) noexcept {
    return static_cast<call_state>(slot.state.load(std::memory_order_acquire));
}

void set_state(call_slot& slot, call_state state) noexcept {
    slot.state.store(static_cast<u32>(state), std::memory_order_release);
}

bool try_claim_terminal(call_slot& slot, call_state terminal) noexcept {
    const u32 desired = static_cast<u32>(terminal);
    u32 current = slot.state.load(std::memory_order_acquire);
    for (;;) {
        if (is_terminal(static_cast<call_state>(current))) {
            return false;
        }
        if (slot.state.compare_exchange_weak(current, desired, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return true;
        }
    }
}

bool result_ready(const call_slot& slot) noexcept {
    return slot.result_ready.load(std::memory_order_acquire);
}

bool finish_slot(call_slot& slot, call_state terminal, rpc_code code, rpc_error error,
                 span<const std::byte> body) noexcept {
    if (!try_claim_terminal(slot, terminal)) {
        return false;
    }
    // Sole writer of the result fields from here on.
    slot.code = code;
    slot.error = std::move(error);
    slot.response.clear();
    if (!body.empty() && !slot.response.append(body)) {
        slot.code = rpc_code::resource_exhausted;
        slot.error = rpc_error(rpc_code::resource_exhausted, "response buffer allocation failed");
    }
    slot.result_ready.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(slot.wait_mutex);
    }
    slot.wait_cv.notify_all();
    return true;
}

// --- call_slot ---------------------------------------------------------------

void call_slot::reset() noexcept {
    state.store(static_cast<u32>(call_state::idle), std::memory_order_relaxed);
    stream_id = 0;
    service_id = 0;
    method_id = 0;
    deadline = std::chrono::steady_clock::time_point{};
    has_deadline = false;
    idempotent = false;
    one_way = false;
    idem = call_id{};
    written.store(false, std::memory_order_release);
    code = rpc_code::ok;
    error = rpc_error{};
    if (response.capacity() > response_retain_bytes) {
        response.release();
    } else {
        response.clear();
    }
    result_ready.store(false, std::memory_order_relaxed);
    callback_invoke = nullptr;
    callback_owner = nullptr;
    callback_armed.store(false, std::memory_order_relaxed);
    owner = nullptr;
    owner_cancel = nullptr;
    owner_detach = nullptr;
}

void call_slot::invoke_callback() noexcept {
    // Claim exactly once. An acquire exchange synchronizes with `then()`'s
    // release store, so `callback_invoke`/`callback_owner` are visible; a
    // concurrent detach or `then()` claim makes this a no-op (CF-1).
    if (!callback_armed.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const call_callback_invoker invoke = callback_invoke;
    void* const callback_context = callback_owner;
    if (invoke != nullptr) {
        invoke(callback_storage.bytes, callback_context, *this);
    }
}

// --- Slot pool ---------------------------------------------------------------

call_slot_pool::call_slot_pool(u32 capacity) : capacity_(capacity) {
    if (capacity_ == 0) {
        return;
    }
    slots_.reset(new (std::nothrow) call_slot[capacity_]);
    if (!slots_) {
        capacity_ = 0;
        return;
    }
    free_.reserve(capacity_);
    for (u32 i = 0; i < capacity_; ++i) {
        free_.push_back(&slots_[i]);
    }
}

call_slot_pool::~call_slot_pool() = default;

call_slot* call_slot_pool::try_acquire() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_.empty()) {
        return nullptr;
    }
    call_slot* slot = free_.back();
    free_.pop_back();
    slot->reset();
    in_use_.fetch_add(1, std::memory_order_acq_rel);
    return slot;
}

void call_slot_pool::release(call_slot* slot) noexcept {
    if (slot == nullptr) {
        return;
    }
    slot->reset();
    slot->generation += 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        free_.push_back(slot);
    }
    in_use_.fetch_sub(1, std::memory_order_acq_rel);
}

u32 call_slot_pool::available() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<u32>(free_.size());
}

} // namespace crpc::rpc
