// CRPC -- readable error strings, classification predicates and the completion
// hook.
#include <crpc/diagnostics.hpp>

#include <mutex>
#include <string>

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

// One process-wide hook. The mutex publishes the hook/cookie pair as a unit and
// is only ever held for the copy: `on_completion` releases it before invoking
// the hook so a hook may re-enter `set_completion_hook` or `on_completion`.
std::mutex g_hook_mutex;
completion_hook g_hook = nullptr;
void* g_hook_self = nullptr;

} // namespace

void set_completion_hook(completion_hook hook, void* self) noexcept {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    g_hook = hook;
    g_hook_self = self;
}

void on_completion(const rpc_error& err) noexcept {
    completion_hook hook = nullptr;
    void* self = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_hook_mutex);
        hook = g_hook;
        self = g_hook_self;
    }
    if (hook != nullptr) {
        hook(err, self);
    }
}

} // namespace diagnostics

} // namespace crpc
