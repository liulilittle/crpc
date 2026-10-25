// CRPC -- error diagnostics.
//
// A failed call is more useful when the caller can answer two questions
// without parsing anything: what happened in one line, and which subsystem
// failed. `to_string` answers the first; the classification predicates answer
// the second.
//
// The predicates are a *view* over `rpc_error::origin`, not a second source of
// truth. They deliberately overlap: a `local_gate` failure is both `is_local`
// and `is_circuit_breaker`, and a `local_lifecycle` failure is both `is_local`
// and `is_fallback`. A caller asks the question it cares about; it does not
// switch on a single enum. Because the mapping is derived from the existing
// `failure_origin` values it can never disagree with the origin the call
// actually recorded.
#ifndef CRPC_DIAGNOSTICS_HPP
#define CRPC_DIAGNOSTICS_HPP

#include <string>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {

// A single human-readable line naming the code, the message, the origin and the
// attempt count. The format is stable but not part of the wire protocol; tests
// and operators match on its substrings, never on byte equality.
CRPC_API std::string to_string(const rpc_error& err);

// --- Classification predicates ----------------------------------------------
//
// Every predicate is noexcept and reads only `err.origin`. `is_local` covers
// the three local origins, `is_network` the three endpoint origins, and the
// remaining predicates name the specific subsystem. `cancel` matches no
// predicate: a caller-initiated cancel is not a subsystem failure.

CRPC_API bool is_local(const rpc_error& err) noexcept;
CRPC_API bool is_network(const rpc_error& err) noexcept;
CRPC_API bool is_timeout(const rpc_error& err) noexcept;
CRPC_API bool is_circuit_breaker(const rpc_error& err) noexcept;
CRPC_API bool is_rate_limiter(const rpc_error& err) noexcept;
CRPC_API bool is_fallback(const rpc_error& err) noexcept;
CRPC_API bool is_server(const rpc_error& err) noexcept;

} // namespace crpc

#endif // CRPC_DIAGNOSTICS_HPP
