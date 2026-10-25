// CRPC -- readable error strings, classification predicates and the completion
// hook.
#include <crpc/diagnostics.hpp>

#include <atomic>
#include <string>

#include <crpc/observability/interceptor.hpp>

namespace crpc {
namespace {

const char* to_origin_string(failure_origin origin) noexcept {
    switch (origin) {
    case failure_origin::local_lifecycle: return "local_lifecycle";
    case failure_origin::local_gate:       return "local_gate";
    case failure_origin::local_capacity:   return "local_capacity";
    case failure_origin::endpoint_open:    return "endpoint_open";
    case failure_origin::endpoint_pre_send: return "endpoint_pre_send";
    case failure_origin::endpoint_sent:    return "endpoint_sent";
    case failure_origin::remote:           return "remote";
    case failure_origin::timeout:          return "timeout";
    case failure_origin::cancel:           return "cancel";
    }
    // Only reachable for a value outside the enum; treat it as unknown rather
    // than returning nullptr into a std::string append.
    return "unknown";
}

} // namespace

std::string to_string(const rpc_error& err) {
    std::string out = crpc::to_string(err.code);
    out += ": ";
    out += err.message;
    out += " (origin=";
    out += to_origin_string(err.origin);
    out += ", attempts=";
    out += std::to_string(err.attempts);
    out += ")";
    return out;
}

bool is_local(const rpc_error& err) noexcept {
    switch (err.origin) {
    case failure_origin::local_lifecycle:
    case failure_origin::local_capacity:
        return true;
    default:
        return false;
    }
}

bool is_network(const rpc_error& err) noexcept {
    switch (err.origin) {
    case failure_origin::endpoint_pre_send:
    case failure_origin::endpoint_sent:
        return true;
    default:
        return false;
    }
}

bool is_timeout(const rpc_error& err) noexcept {
    return err.origin == failure_origin::timeout;
}

bool is_circuit_breaker(const rpc_error& err) noexcept {
    return err.origin == failure_origin::endpoint_open;
}

bool is_rate_limiter(const rpc_error& err) noexcept {
    return err.origin == failure_origin::local_gate;
}

bool is_fallback(const rpc_error& err) noexcept {
    // Ruling 16: a fallback that answers the call is a success and carries no
    // error, and there is no dedicated fallback origin, so this can only key on
    // `local_lifecycle`. It marks lifecycle-short-circuited errors, it is not a
    // true fallback detector (callers must read the fallback statistics for
    // that).
    return err.origin == failure_origin::local_lifecycle;
}

bool is_server(const rpc_error& err) noexcept {
    return err.origin == failure_origin::remote;
}

namespace diagnostics {
namespace {

// One process-wide hook, published as an atomic pair. The cookie is stored
// first and the hook last, so a reader that observes the hook (acquire) also
// observes the matching cookie (release). `on_completion` reads both without a
// lock, so a hook may re-enter `set_completion_hook` or `on_completion` without
// deadlocking, and a terminal `get()` with no observer installed takes no lock.
std::atomic<completion_hook> g_hook{nullptr};
std::atomic<void*> g_hook_self{nullptr};

} // namespace

void set_completion_hook(completion_hook hook, void* self) noexcept {
    g_hook_self.store(self, std::memory_order_release);
    g_hook.store(hook, std::memory_order_release);
}

bool has_completion_observer() noexcept {
    if (g_hook.load(std::memory_order_acquire) != nullptr) {
        return true;
    }
    return obs::default_interceptor_chain() != nullptr;
}

void on_completion(const rpc_error& err) noexcept {
    const completion_hook hook = g_hook.load(std::memory_order_acquire);
    if (hook != nullptr) {
        hook(err, g_hook_self.load(std::memory_order_acquire));
        return;
    }
    // Default hook: trigger the installed observability interceptor chain, if
    // any. With no chain installed the notification is a no-op.
    obs::interceptor_chain* const chain = obs::default_interceptor_chain();
    if (chain == nullptr) {
        return;
    }
    obs::call_context context;
    context.set_error(err.code, err.message);
    (void)chain->run(obs::hook_point::after_call, context);
}

} // namespace diagnostics

} // namespace crpc
