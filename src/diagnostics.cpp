// CRPC -- readable error strings and classification predicates.
#include <crpc/diagnostics.hpp>

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
    case failure_origin::local_gate:
    case failure_origin::local_capacity:
        return true;
    default:
        return false;
    }
}

bool is_network(const rpc_error& err) noexcept {
    switch (err.origin) {
    case failure_origin::endpoint_open:
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
    return err.origin == failure_origin::local_gate;
}

bool is_rate_limiter(const rpc_error& err) noexcept {
    return err.origin == failure_origin::local_capacity;
}

bool is_fallback(const rpc_error& err) noexcept {
    return err.origin == failure_origin::local_lifecycle;
}

bool is_server(const rpc_error& err) noexcept {
    return err.origin == failure_origin::remote;
}

} // namespace crpc
