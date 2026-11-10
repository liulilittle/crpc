// CRPC -- G19 step2: fence authority seam.
//
// Deliberately does NOT include registry.hpp: the seam must not leak the
// registry's internal types, so step3 can supply a leader-committed
// implementation without the service knowing.
//
// The shape mirrors `catalog_sink` (registry.hpp:300): a function pointer plus
// a void* context. That is zero ABI risk, crosses a DLL boundary without a
// vtable, and lets step3 replace the local authority with a leader-committed
// one with no service-layer change.
#ifndef CRPC_CLUSTER_FENCE_AUTHORITY_HPP
#define CRPC_CLUSTER_FENCE_AUTHORITY_HPP

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace cluster {

struct fence_request {
    const char* service;   // borrowed, valid for the call
    const char* node_id;   // borrowed
    u64 lease_id;          // 0 when not a lease write
    u64 principal;         // injected by the ACL layer, never a client claim
};

// Returns the next fencing token, or an error that the service passes through
// unchanged. `context` is the authority's own state pointer.
using fence_authority_fn =
    result<u64> (*)(void* context, const fence_request& request) noexcept;

struct fence_authority {
    fence_authority_fn fn = nullptr;
    void* context = nullptr;

    [[nodiscard]] result<u64> allocate(const fence_request& request) const {
        if (fn == nullptr) {
            return rpc_error(rpc_code::failed_precondition,
                             "registry_service: no fence authority configured");
        }
        return fn(context, request);
    }
};

} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_FENCE_AUTHORITY_HPP
