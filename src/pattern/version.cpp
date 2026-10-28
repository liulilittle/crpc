// CRPC -- version pattern: non-template helpers.

#include <cstdio>
#include <string>

#include <crpc/pattern/version.hpp>

namespace crpc {
namespace pattern {

namespace {

// The version loop may retry exactly this failure and nothing else.
bool version_conflict_retryable(rpc_code code, bool /* idempotent */,
                                void* /* context */) noexcept {
    return code == rpc_code::failed_precondition;
}

} // namespace

rpc_error make_version_conflict(u64 expected, u64 actual) noexcept {
    char buffer[96];
    const int written = std::snprintf(
        buffer, sizeof(buffer), "version conflict: expected %llu, current %llu",
        static_cast<unsigned long long>(expected),
        static_cast<unsigned long long>(actual));
    const std::string message =
        written > 0 ? std::string(buffer) : std::string("version conflict");
    return rpc_error(rpc_code::failed_precondition, message);
}

crpc::resilience::retry_policy default_version_policy() noexcept {
    crpc::resilience::retry_policy policy;
    policy.retryable = &version_conflict_retryable;
    return policy;
}

} // namespace pattern
} // namespace crpc