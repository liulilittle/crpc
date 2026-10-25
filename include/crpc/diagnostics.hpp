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
// Every predicate is noexcept and reads only `err.origin`. The mapping follows
// the library's own semantics (Ruling 16):
//   is_local           = local_lifecycle | local_capacity
//   is_network         = endpoint_pre_send | endpoint_sent
//   is_timeout         = timeout
//   is_circuit_breaker = endpoint_open
//   is_rate_limiter    = local_gate
//   is_fallback        = local_lifecycle (lifecycle marker, not a fallback detector)
//   is_server          = remote
// `local_gate` is rate limiting + bulkhead, `local_capacity` is connection-pool
// capacity, and `endpoint_open` is a refused connect (breaker open). `cancel`
// matches no predicate: a caller-initiated cancel is not a subsystem failure.

CRPC_API bool is_local(const rpc_error& err) noexcept;
CRPC_API bool is_network(const rpc_error& err) noexcept;
CRPC_API bool is_timeout(const rpc_error& err) noexcept;
CRPC_API bool is_circuit_breaker(const rpc_error& err) noexcept;
CRPC_API bool is_rate_limiter(const rpc_error& err) noexcept;
CRPC_API bool is_fallback(const rpc_error& err) noexcept;
CRPC_API bool is_server(const rpc_error& err) noexcept;

// --- Completion hook ---------------------------------------------------------
//
// A process-wide notification for callers that need one place to observe every
// terminal call outcome (metrics, debug logging, tracing) without wrapping each
// call site. `response<Resp>::get()` invokes `on_completion` the first time it
// observes a terminal outcome; the error is `ok` for a success and the terminal
// failure (including a cancel) otherwise.
//
// `set_completion_hook` installs a hook plus an opaque `self` cookie; passing a
// null hook clears it. Both functions are safe to call from any thread.
// `on_completion` invokes the registered hook exactly once per call and does not
// hold a library lock while the hook runs, so the hook may call
// `set_completion_hook`/`on_completion` again. The hook must not throw; it is
// declared `noexcept` for that reason.
namespace diagnostics {

using completion_hook = void (*)(const rpc_error&, void* self) noexcept;

CRPC_API void set_completion_hook(completion_hook hook, void* self) noexcept;
CRPC_API void on_completion(const rpc_error& err) noexcept;

} // namespace diagnostics

} // namespace crpc

#endif // CRPC_DIAGNOSTICS_HPP
