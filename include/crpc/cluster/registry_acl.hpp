// CRPC -- G19 step2: static ACL for the registry service.
//
// Trust boundary (read this before using the type)
// ------------------------------------------------
// The registry service's ACL is a **bearer-token authorisation** layer, not an
// authentication layer. A holder of a valid token can act as the principal that
// token was issued to. There is no transport security: the RPC layer carries no
// peer identity (`call_envelope`, `call_context` and `frame` have no identity
// field) and there is no TLS. A token observed on the wire can be replayed until
// it expires. Deployments that need authenticated peers must terminate TLS in
// front of the service or run it on a trusted network.
//
// `acl_token_hash` is a deterministic FNV-1a of the client's token, using the
// same function the registry already uses for principal_hash (registry.cpp:133).
// Not std::hash: the value is compared across processes and must not depend on a
// build-specific seed.
//
// This header adds new types only. It does not include registry.hpp: the ACL is
// a service-layer concept and must not drag the registry's internal layout into
// the wire contract (docs/abi.md section 1: new symbols are MINOR).
#ifndef CRPC_CLUSTER_REGISTRY_ACL_HPP
#define CRPC_CLUSTER_REGISTRY_ACL_HPP

#include <string>
#include <vector>

#include <crpc/config.hpp>

namespace crpc {
namespace cluster {

// Deterministic FNV-1a of the client's token. Same function the registry uses
// for principal_hash (registry.cpp:133).
using acl_token_hash = u64;

// One static ACL entry. The service matches the presented token hash against
// this table and derives the principal from the matched rule; the client never
// names its own principal.
struct acl_rule {
    acl_token_hash token_hash = 0;   // hash of the bearer token the client presents
    std::string    principal_id;      // the identity this token acts as
    std::string    node_id;           // empty = any node; else the key's node must match
    std::vector<std::string> read_prefixes;   // empty = no read (fail closed)
    std::vector<std::string> write_prefixes;  // empty = no write (fail closed)
    bool admin = false;               // may act on behalf of any node_id
};

} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_REGISTRY_ACL_HPP
