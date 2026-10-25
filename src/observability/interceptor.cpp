// CRPC -- default observability interceptor chain implementation.
#include <crpc/observability/interceptor.hpp>

#include <atomic>

namespace crpc {
namespace obs {
namespace {

std::atomic<interceptor_chain*> g_default_chain{nullptr};

} // namespace

void set_default_interceptor_chain(interceptor_chain* chain) noexcept {
    g_default_chain.store(chain, std::memory_order_release);
}

interceptor_chain* default_interceptor_chain() noexcept {
    return g_default_chain.load(std::memory_order_acquire);
}

} // namespace obs
} // namespace crpc
