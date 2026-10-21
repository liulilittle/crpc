// CRPC -- compile-time ABI layout guards.
//
// This translation unit contains no runtime code. It exists so that a change
// to the layout of a public type that crosses the ABI boundary fails the build
// instead of failing silently at a consumer. The rules it enforces are the
// ones docs/abi.md section 4 states:
//
//   * every type listed here is standard-layout, which is the precondition for
//     `offsetof` and for the "no base-class trickery" part of the policy;
//   * adjacent members of the plain-`std::size_t` records keep their order and
//     stay one `std::size_t` apart (a relative STEP chain, not absolute
//     offsets, so a 32-bit build is checked with the same source);
//   * the total width of `http_statistics` is exactly its field count times
//     `sizeof(std::size_t)`, and the last field ends the struct (no trailing
//     padding).
//
// Types that carry standard-library members (`buffer`, `endpoint`, `rpc_error`,
// `status`) only get relative-order assertions: their absolute size is an
// implementation detail of the standard library, which docs/abi.md section 9
// explicitly does not guarantee. `result<T>` is deliberately absent: it stores
// a `std::variant`, is therefore not standard-layout, and `offsetof` on it
// would be ill-formed.

#include <cstddef>
#include <type_traits>

#include <crpc/buffer.hpp>
#include <crpc/client_facade.hpp>
#include <crpc/error.hpp>
#include <crpc/frame.hpp>
#include <crpc/http/server.hpp>
#include <crpc/io.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/server.hpp>

namespace crpc {
namespace {

// The distance between two adjacent members of the same scalar type. Used for
// the `std::size_t` counters, where any reordering, insertion or removal
// changes at least one STEP.
#define CRPC_STEP(T, a, b)                                                   \
    static_assert(offsetof(T, b) - offsetof(T, a) == sizeof(std::size_t),    \
                  #T " " #a "->" #b)

// A weaker, type-agnostic ordering invariant: `b` starts at or after the end of
// `a`. Used where the member types differ or are standard-library types whose
// width is not ours to pin.
#define CRPC_AFTER(T, a, b)                                                  \
    static_assert(offsetof(T, b) >= offsetof(T, a) + sizeof(((T*)0)->a),     \
                  #T " " #a " must precede " #b)

// The distance between two adjacent members of the same type, using that
// member's own width as the step. This is the `CRPC_STEP` rule for a run whose
// element type is not `std::size_t` (for example the `std::uint64_t` resilience
// counters in `server::statistics`).
#define CRPC_STEP_SAME(T, a, b)                                              \
    static_assert(offsetof(T, b) - offsetof(T, a) == sizeof(((T*)0)->a),     \
                  #T " " #a "->" #b)

// The expected offset of `b` when it directly follows `a` and must honour its
// own alignment. This is what a record that mixes `std::size_t` and
// `std::uint64_t` needs: on x64 both are 8 bytes and the step is 8, while on
// x86 `std::size_t` is 4 bytes, so the following `std::uint64_t` may start 4 or
// 8 bytes later depending on where the `std::size_t` run ended. The assertion
// compares offsets (a relative relation) rather than pinning absolute numbers,
// so the same source checks both targets. `alignof` takes a type-id, hence the
// `decltype`.
#define CRPC_STEP_MIXED(T, a, b)                                             \
    static_assert(offsetof(T, b) ==                                          \
                      (offsetof(T, a) + sizeof(((T*)0)->a) +                 \
                       alignof(decltype(((T*)0)->b)) - 1) /                  \
                              alignof(decltype(((T*)0)->b)) *                \
                          alignof(decltype(((T*)0)->b)),                     \
                  #T " " #a "->" #b)

// --- Standard-layout preconditions ------------------------------------------
//
// `offsetof` is only well-defined for standard-layout types, so this block is
// the precondition for every assertion below and is worth keeping even where a
// type has no further assertions.

static_assert(std::is_standard_layout<frame_header>::value,
              "frame_header must be standard-layout");
static_assert(std::is_standard_layout<::crpc::connect_options>::value,
              "::crpc::connect_options must be standard-layout");
static_assert(std::is_standard_layout<::crpc::listen_options>::value,
              "::crpc::listen_options must be standard-layout");
static_assert(std::is_standard_layout<rpc::client_options>::value,
              "rpc::client_options must be standard-layout");
static_assert(std::is_standard_layout<http::http_statistics>::value,
              "http::http_statistics must be standard-layout");
static_assert(std::is_standard_layout<client::statistics>::value,
              "client::statistics must be standard-layout");
static_assert(std::is_standard_layout<client::resilience_statistics>::value,
              "client::resilience_statistics must be standard-layout");
static_assert(std::is_standard_layout<server::statistics>::value,
              "server::statistics must be standard-layout");
// `buffer` is intentionally *not* asserted standard-layout. It owns a
// `std::unique_ptr`, and in C++17 a class is only standard-layout when every
// non-static data member is standard-layout too. MSVC and GCC happen to accept
// `std::unique_ptr` here; libc++ does not, so the assertion would be a
// toolchain-dependent build break. What matters for the ABI is that `buffer` is
// an owning, opaque type that never crosses the boundary by value, which the
// negative assertion below states directly.
static_assert(!std::is_trivially_copyable<buffer>::value,
              "buffer must stay an owning, non-trivially-copyable type");

// --- Fixed-width records ----------------------------------------------------
//
// `frame_header` is the 16-byte wire header. Its members are distinct types, so
// the chain is ordering plus width, not a uniform STEP.

static_assert(offsetof(frame_header, payload_len) == 0,
              "frame_header payload_len must be first");
CRPC_AFTER(frame_header, payload_len, type);
CRPC_AFTER(frame_header, type, flags);
CRPC_AFTER(frame_header, flags, stream_id);
CRPC_AFTER(frame_header, stream_id, ext);
static_assert(sizeof(frame_header) == 16,
              "frame_header is a 16-byte wire structure");

// --- Option records ---------------------------------------------------------
//
// These carry `std::chrono` members and a `bool`; the chain keeps the field
// order stable without pinning a layout the standard library could widen.

CRPC_AFTER(::crpc::connect_options, connect_timeout, recv_buffer_bytes);
CRPC_AFTER(::crpc::connect_options, recv_buffer_bytes, send_buffer_bytes);
CRPC_AFTER(::crpc::connect_options, send_buffer_bytes, tcp_no_delay);

CRPC_AFTER(::crpc::listen_options, backlog, reuse_port);
CRPC_AFTER(::crpc::listen_options, reuse_port, recv_buffer_bytes);
CRPC_AFTER(::crpc::listen_options, recv_buffer_bytes, send_buffer_bytes);

CRPC_AFTER(rpc::client_options, slot_capacity, max_pending_write_bytes);
CRPC_AFTER(rpc::client_options, max_pending_write_bytes, clock);
CRPC_AFTER(rpc::client_options, clock, clock_ctx);
CRPC_AFTER(rpc::client_options, clock_ctx, default_timeout);
CRPC_AFTER(rpc::client_options, default_timeout, client_id);
CRPC_AFTER(rpc::client_options, client_id, client_epoch);

// --- http_statistics: the 28-counter STEP chain -----------------------------
//
// Every member is a plain `std::size_t`, so a reordering, an insertion or a
// removal breaks the chain at exactly one link. This is the relative form: no
// absolute offset is asserted, so the same source checks x86 and x64.

CRPC_STEP(http::http_statistics, active_connections, accepted_connections);
CRPC_STEP(http::http_statistics, accepted_connections, requests_handled);
CRPC_STEP(http::http_statistics, requests_handled, responses_written);
CRPC_STEP(http::http_statistics, responses_written, bad_requests);
CRPC_STEP(http::http_statistics, bad_requests, h2_connections);
CRPC_STEP(http::http_statistics, h2_connections, tls_connections);
CRPC_STEP(http::http_statistics, tls_connections, rejected_connections);
CRPC_STEP(http::http_statistics, rejected_connections, timed_out_connections);
CRPC_STEP(http::http_statistics, timed_out_connections, sse_connections);
CRPC_STEP(http::http_statistics, sse_connections, sse_events_delivered);
CRPC_STEP(http::http_statistics, sse_events_delivered, sse_slow_closes);
CRPC_STEP(http::http_statistics, sse_slow_closes, sse_hub_published);
CRPC_STEP(http::http_statistics, sse_hub_published, sse_hub_delivered);
CRPC_STEP(http::http_statistics, sse_hub_delivered, sse_hub_queued);
CRPC_STEP(http::http_statistics, sse_hub_queued, sse_hub_dropped);
CRPC_STEP(http::http_statistics, sse_hub_dropped, responses_compressed);
CRPC_STEP(http::http_statistics, responses_compressed, requests_decoded);
CRPC_STEP(http::http_statistics, requests_decoded, downloads_served);
CRPC_STEP(http::http_statistics, downloads_served, longpoll_waiters);
CRPC_STEP(http::http_statistics, longpoll_waiters, longpoll_subscribers);
CRPC_STEP(http::http_statistics, longpoll_subscribers, longpoll_events);
CRPC_STEP(http::http_statistics, longpoll_events, longpoll_timeouts);
CRPC_STEP(http::http_statistics, longpoll_timeouts, longpoll_ack_clamped);
CRPC_STEP(http::http_statistics, longpoll_ack_clamped, longpoll_conflicts);
CRPC_STEP(http::http_statistics, longpoll_conflicts, longpoll_restarts);
CRPC_STEP(http::http_statistics, longpoll_restarts, longpoll_overloaded);
CRPC_STEP(http::http_statistics, longpoll_overloaded, longpoll_replay_window);

// The record is 28 counters and nothing else. `sizeof(std::size_t)` rather than
// a literal 224 keeps the assertion meaningful on 32-bit builds.
static_assert(sizeof(http::http_statistics) == 28 * sizeof(std::size_t),
              "http_statistics must be exactly 28 std::size_t counters");

// The last counter ends the struct: no trailing padding was introduced by a
// reorder or by a new member after it.
static_assert(offsetof(http::http_statistics, longpoll_replay_window) +
                      sizeof(std::size_t) ==
                  sizeof(http::http_statistics),
              "longpoll_replay_window must be the last field");

// --- client and server statistics -------------------------------------------
//
// Both records are plain `std::size_t` runs; the STEP chain keeps their order
// and width stable, and the total-width assertion catches an added field.

CRPC_STEP(client::statistics, active_connections, in_flight_calls);
CRPC_STEP(client::statistics, in_flight_calls, failed_connections);
CRPC_STEP(client::statistics, failed_connections, calls_started);
CRPC_STEP(client::statistics, calls_started, retries);
CRPC_STEP(client::statistics, retries, breaker_rejections);
static_assert(sizeof(client::statistics) == 6 * sizeof(std::size_t),
              "client::statistics must be exactly 6 std::size_t counters");

CRPC_STEP(client::resilience_statistics, rate_limited, bulkhead_rejected);
CRPC_STEP(client::resilience_statistics, bulkhead_rejected,
          local_gate_rejected);
CRPC_STEP(client::resilience_statistics, local_gate_rejected, endpoint_open);
CRPC_STEP(client::resilience_statistics, endpoint_open, lb_unavailable);
CRPC_STEP(client::resilience_statistics, lb_unavailable, pre_send_retries);
CRPC_STEP(client::resilience_statistics, pre_send_retries, probe_admitted);
CRPC_STEP(client::resilience_statistics, probe_admitted,
          retries_delayed_by_hint);
CRPC_STEP(client::resilience_statistics, retries_delayed_by_hint,
          bulkhead_in_use);
static_assert(sizeof(client::resilience_statistics) == 9 * sizeof(std::size_t),
              "client::resilience_statistics must be exactly 9 counters");

// `server::statistics` mixes widths: six `std::size_t` counters, six
// `std::uint64_t` resilience counters, then seven `std::size_t` push counters.
// A uniform STEP would be wrong on 32-bit targets, where the `std::size_t` runs
// are 4 bytes wide but the `std::uint64_t` block is 8-byte aligned. The chain
// therefore uses the matching macro at each boundary, and no total-width
// assertion is made because the width legitimately differs between targets.
CRPC_STEP(server::statistics, active_connections, in_flight_calls);
CRPC_STEP(server::statistics, in_flight_calls, accepted_connections);
CRPC_STEP(server::statistics, accepted_connections, requests_handled);
CRPC_STEP(server::statistics, requests_handled, rejected_overload);
CRPC_STEP(server::statistics, rejected_overload, handler_failures);
CRPC_STEP_MIXED(server::statistics, handler_failures, rate_limited_connection);
CRPC_STEP_SAME(server::statistics, rate_limited_connection,
               rate_limited_service);
CRPC_STEP_SAME(server::statistics, rate_limited_service, bulkhead_rejected);
CRPC_STEP_SAME(server::statistics, bulkhead_rejected, breaker_open);
CRPC_STEP_SAME(server::statistics, breaker_open, deadline_rejected);
CRPC_STEP_SAME(server::statistics, deadline_rejected, probe_admitted);
CRPC_STEP_MIXED(server::statistics, probe_admitted, push_active_connections);
CRPC_STEP(server::statistics, push_active_connections, push_queued_messages);
CRPC_STEP(server::statistics, push_queued_messages, push_queued_bytes);
CRPC_STEP(server::statistics, push_queued_bytes, push_egress_bytes);
CRPC_STEP(server::statistics, push_egress_bytes, push_egress_high_water);
CRPC_STEP(server::statistics, push_egress_high_water, push_dropped_overflow);
CRPC_STEP(server::statistics, push_dropped_overflow, push_acks);

// --- Standard-library-bearing records ---------------------------------------
//
// `rpc_error` and `status` own `std::string` / `std::vector` members. Those
// standard-library types are not standard-layout, so neither record is, and
// `offsetof` on them would be ill-formed. They get the invariants that are
// still meaningful without `offsetof`: `status` is exactly one `rpc_error` and
// nothing else. Their absolute size belongs to the standard library and is
// deliberately not pinned (docs/abi.md section 9).

static_assert(sizeof(status) == sizeof(rpc_error),
              "status must add no storage to rpc_error");
static_assert(alignof(status) == alignof(rpc_error),
              "status must keep the alignment of rpc_error");

// `buffer` and `endpoint` expose no public members. Their alignment must at
// least hold a pointer, which is the floor any owning container needs.
static_assert(alignof(buffer) >= alignof(void*),
              "buffer must be pointer-aligned");
static_assert(alignof(endpoint) >= alignof(void*),
              "endpoint must be pointer-aligned");

#undef CRPC_STEP
#undef CRPC_STEP_SAME
#undef CRPC_STEP_MIXED
#undef CRPC_AFTER

// A named symbol keeps MSVC from emitting LNK4221 ("no public symbols found")
// for this translation unit. It is internal detail, not part of the ABI, and is
// declared in no header. `[[maybe_unused]]` is required because the unit has no
// code that reads it: GCC and Clang otherwise reject it under -Werror.
[[maybe_unused]] int abi_check_translation_unit = 0;

} // namespace
} // namespace crpc
