#include <crpc/resilience/bulkhead.hpp>

namespace crpc::resilience {

bulkhead::bulkhead(const bulkhead_config& config) noexcept
    : capacity_(config.capacity == 0 ? 1 : config.capacity),
      retry_after_(config.retry_after.count() < 0 ? std::chrono::milliseconds(0)
                                                  : config.retry_after) {}

bulkhead_permit bulkhead::try_enter() noexcept {
    u32 current = in_use_.load(std::memory_order_acquire);
    for (;;) {
        if (current >= capacity_) {
            bulkhead_permit rejected;
            rejected.code_ = rpc_code::resource_exhausted;
            rejected.retry_after_ = retry_after_;
            return rejected;
        }
        if (in_use_.compare_exchange_weak(current, current + 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            bulkhead_permit permit;
            permit.owner_ = this;
            permit.held_ = true;
            permit.code_ = rpc_code::ok;
            return permit;
        }
    }
}

void bulkhead::leave() noexcept {
    u32 current = in_use_.load(std::memory_order_acquire);
    while (current > 0) {
        if (in_use_.compare_exchange_weak(current, current - 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            return;
        }
    }
}

bulkhead_permit::bulkhead_permit(bulkhead_permit&& other) noexcept
    : owner_(other.owner_),
      held_(other.held_),
      code_(other.code_),
      retry_after_(other.retry_after_) {
    other.owner_ = nullptr;
    other.held_ = false;
}

bulkhead_permit& bulkhead_permit::operator=(bulkhead_permit&& other) noexcept {
    if (this != &other) {
        leave();
        owner_ = other.owner_;
        held_ = other.held_;
        code_ = other.code_;
        retry_after_ = other.retry_after_;
        other.owner_ = nullptr;
        other.held_ = false;
    }
    return *this;
}

bulkhead_permit::~bulkhead_permit() {
    leave();
}

void bulkhead_permit::leave() noexcept {
    if (owner_ != nullptr && held_) {
        held_ = false;
        owner_->leave();
    }
    owner_ = nullptr;
}

} // namespace crpc::resilience
