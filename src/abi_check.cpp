// CRPC -- compile-time ABI layout guards.
//
// This translation unit contains no runtime code. It exists so that a change
// to the layout of a public type that crosses the ABI boundary fails the build
// instead of failing silently at a consumer. The rules it enforces are the
// ones docs/abi.md section 4 states:
//
//   * every type listed here is standard-layout, which is the precondition for
//     `offsetof` and for the "no base-class trickery" part of the policy;
//   * every record listed here has exactly the declared number of direct
//     members (a structured binding, so an append or a removal breaks the build
//     on every target, including where tail padding would hide it);
//   * adjacent members of the plain-`std::size_t` records keep their order and
//     stay one `std::size_t` apart (a relative STEP chain, not absolute
//     offsets, so a 32-bit build is checked with the same source);
//   * the total width of `http_statistics` is exactly its field count times
//     `sizeof(std::size_t)`, and the last field ends the struct (no trailing
//     padding);
//   * on x86 and x64 only, each record has the absolute `sizeof` and the key
//     `offsetof` values measured on that architecture. Other targets keep the
//     architecture-independent guards above and no absolute numbers.
//
// Types that carry standard-library members (`buffer`, `endpoint`, `rpc_error`,
// `status`) get relative-order and member-count assertions but no absolute size:
// their width is an implementation detail of the standard library configuration,
// which docs/abi.md section 9 explicitly does not guarantee. `result<T>` is
// deliberately absent: it stores a `std::variant`, is therefore not
// standard-layout, and `offsetof` on it would be ill-formed.

#include <cstddef>
#include <functional>
#include <type_traits>
#include <vector>

#include <crpc/buffer.hpp>
#include <crpc/client_facade.hpp>
#include <crpc/error.hpp>
#include <crpc/frame.hpp>
#include <crpc/frame_compression.hpp>
#include <crpc/http/server.hpp>
#include <crpc/http2/connection.hpp>
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

// An exact member count, independent of padding and of the target ABI. The
// structured binding decomposes `T` into one name per direct member, bound in
// declaration order. If a member is appended, prepended or removed the number
// of names no longer matches the number of direct members and the binding is
// ill-formed, so the definition fails to compile. The body is always compiled
// even though the function is never called.
//
// This is the only guard that sees a tail append on a 32-bit target. An
// absolute `sizeof` assertion cannot: on x86 `server::statistics` is 104 bytes
// with 4 bytes of tail padding after `push_acks`, so an appended
// `std::size_t` lands in that padding and `sizeof` stays 104. The relative
// STEP chains cannot see it either, because they only relate existing members.
// A count does not care where the bytes go.
//
// It also covers every other target (ARM64 included), where no absolute size
// is maintained.
//
// Known limits of this guard, which must stay in sync with docs/abi.md:
//
//   * the binding names are new local names, not references to the member
//     names, so the binding itself does not validate names. A rename is caught
//     by the relative chain where one exists; the only member-counted record
//     with no chain is `rpc_error`, whose five members are named one by one in
//     the five `decltype` assertions below, so a rename of any of them fails
//     this translation unit. Its constructors name only `code`, `message` and
//     `retryable`, so they do not catch a rename of `retry_after` or
//     `metadata`. (`status` has no public data member; HTTP/2 records have chains.)
//   * a reorder that keeps the count is not detected here either; the relative
//     chains cover it where they exist. `rpc_error` has no relative chain, so a
//     reorder in `rpc_error` is not detected by this unit;
//   * a member *type* change that keeps the count is not detected here. For
//     `rpc_error` the explicit member-type assertions below cover it; for the
//     counter records the absolute `sizeof`/`offsetof` anchors cover the
//     changes that alter width or offset.
//
// The mechanism relies on every direct data member being public and being
// reachable through a structured binding; a private member makes the binding
// ill-formed, which is why `status` is not covered by this macro.
#define CRPC_MEMBER_COUNT(tag, T, ...)                                       \
    [[maybe_unused]] void crpc_member_count_##tag() {                        \
        T value{};                                                           \
        [[maybe_unused]] auto& [__VA_ARGS__] = value;                        \
    }

// --- Exact member counts ----------------------------------------------------
//
// One call per cross-ABI record. These are the padding-independent half of the
// guard: they catch an appended or removed field on every target, including x86
// and the architectures for which no absolute size is maintained below.
//
// The names are only placeholders bound in declaration order; they are not
// checked against the real member names. See the limits listed on the macro.
//
// `rpc_error` and `status` are handled separately: `rpc_error` has public
// members and decomposes, while `status` wraps a single private `rpc_error` and
// cannot be decomposed at all (its shape is pinned by the sizeof/alignof pair
// further down).

CRPC_MEMBER_COUNT(frame_header, frame_header, payload_len, type, flags,
                  stream_id, ext)
CRPC_MEMBER_COUNT(connect_options, ::crpc::connect_options, connect_timeout,
                  recv_buffer_bytes, send_buffer_bytes, tcp_no_delay)
CRPC_MEMBER_COUNT(listen_options, ::crpc::listen_options, backlog, reuse_port,
                  recv_buffer_bytes, send_buffer_bytes)
CRPC_MEMBER_COUNT(client_options, rpc::client_options, slot_capacity,
                  max_pending_write_bytes, clock, clock_ctx, default_timeout,
                  client_id, client_epoch)
CRPC_MEMBER_COUNT(http_statistics, http::http_statistics, active_connections,
                  accepted_connections, requests_handled, responses_written,
                  bad_requests, h2_connections, tls_connections,
                  rejected_connections, timed_out_connections,
                  sse_connections, sse_events_delivered, sse_slow_closes,
                  sse_hub_published, sse_hub_delivered, sse_hub_queued,
                  sse_hub_dropped, responses_compressed, requests_decoded,
                  downloads_served, longpoll_waiters, longpoll_subscribers,
                  longpoll_events, longpoll_timeouts, longpoll_ack_clamped,
                  longpoll_conflicts, longpoll_restarts, longpoll_overloaded,
                  longpoll_replay_window)
CRPC_MEMBER_COUNT(client_statistics, client::statistics, active_connections,
                  in_flight_calls, failed_connections, calls_started, retries,
                  breaker_rejections)
CRPC_MEMBER_COUNT(resilience_statistics, client::resilience_statistics,
                  rate_limited, bulkhead_rejected, local_gate_rejected,
                  endpoint_open, lb_unavailable, pre_send_retries,
                  probe_admitted, retries_delayed_by_hint, bulkhead_in_use)
CRPC_MEMBER_COUNT(server_statistics, server::statistics, active_connections,
                  in_flight_calls, accepted_connections, requests_handled,
                  rejected_overload, handler_failures,
                  rate_limited_connection, rate_limited_service,
                  bulkhead_rejected, breaker_open, deadline_rejected,
                  probe_admitted, push_active_connections,
                  push_queued_messages, push_queued_bytes, push_egress_bytes,
                  push_egress_high_water, push_dropped_overflow, push_acks)
CRPC_MEMBER_COUNT(rpc_error, rpc_error, code, message, retryable, retry_after,
                  metadata)
CRPC_MEMBER_COUNT(h2_error, http2::h2_error, code, scope, stream_id)
CRPC_MEMBER_COUNT(connection_options, http2::connection_options, role,
                  local_settings, initial_connection_send_window,
                  initial_connection_recv_window, max_send_queue_bytes,
                  reject_unknown_frame_types)
CRPC_MEMBER_COUNT(stream_snapshot, http2::stream_snapshot, id, state,
                  send_window, recv_window, local_end_stream,
                  remote_end_stream, queued_bytes)
CRPC_MEMBER_COUNT(window_credit, http2::window_credit, connection, stream)
CRPC_MEMBER_COUNT(frame_compression_info, frame_compression_info, algorithm,
                  wire_size, logical_size, dropped, drop_reason)
// `fallback_context` crosses the callback boundary: the library builds it and
// the consumer's `fallback_fn` reads it, so both sides must agree on the
// layout. No signature spells the type name (`set_fallback` takes the
// `fallback_fn` typedef and `fallback_execute` is header-inline), so the
// documentation scan cannot discover it: this anchor is hand-written on
// purpose, like `frame_header`.
CRPC_MEMBER_COUNT(fallback_context, resilience::fallback_context, code, stage,
                  retry_after, one_way, sent, idempotent, has_deadline,
                  deadline)

// `server::options` is the 2.0.0 MAJOR break point: it is passed by value into
// the exported `server` constructor, so the caller and the library must agree on
// its size. It is guarded here rather than only by policy: the member count
// catches an append on every target (including x86, where a trailing `std::size_t`
// can be absorbed by tail padding), and the per-architecture absolute anchors
// below pin the width and the position of the appended `clock_source` field.
CRPC_MEMBER_COUNT(server_options, server::options, shards, runtime,
                  per_connection_inflight, per_service_inflight, drain_timeout,
                  overload_retry_after, breaker, per_connection_rate,
                  per_service_rate, per_service_bulkhead, resilience_enabled,
                  send_goaway_on_stop, push_egress_max_bytes, push_subscribe,
                  clock_source)

// --- Per-architecture absolute layout anchors -------------------------------
//
// The relative chains above and the member counts are architecture-independent,
// but they cannot state the absolute width of a record. An absolute width is
// what a consumer compiled against an older release needs: it detects a change
// that a same-release rebuild would silently absorb.
//
// These numbers were measured, not derived: MSVC 19.44 compiled the same
// headers once for x64 and once for x86, in both Release and Debug
// (`_ITERATOR_DEBUG_LEVEL=2`), and the resulting `sizeof`/`offsetof` values were
// copied from that run. Every type below reported the same width in both
// configurations; `rpc_error` and `status` did not, and are therefore excluded
// (see the note after the block). GCC 13 and Clang 18 on x86-64 report the same
// numbers as MSVC x64.
//
// They are pinned per architecture because the values differ: `std::size_t` is
// 8 bytes on x64 and 4 on x86, and the `std::uint64_t` resilience block of
// `server::statistics` forces 8-byte alignment on both, which leaves 4 bytes of
// tail padding on x86 and none on x64. Only x86 and x64 are maintained; see the
// `#else` branch.

#if defined(_M_IX86) || defined(__i386__)
#define CRPC_ABI_ARCH_X86 1
#elif defined(_M_X64) || defined(__x86_64__)
#define CRPC_ABI_ARCH_X64 1
#endif

#if defined(CRPC_ABI_ARCH_X86)

// 32-bit: measured with MSVC 19.44, x86, Release and Debug identical.
#define CRPC_ABS_SIZE(T, n) static_assert(sizeof(T) == n, #T " absolute size")
#define CRPC_ABS_OFF(T, m, n)                                                \
    static_assert(offsetof(T, m) == n, #T "::" #m " absolute offset")

CRPC_ABS_SIZE(frame_header, 16);
CRPC_ABS_OFF(frame_header, payload_len, 0);
CRPC_ABS_OFF(frame_header, type, 4);
CRPC_ABS_OFF(frame_header, flags, 5);
CRPC_ABS_OFF(frame_header, stream_id, 8);
CRPC_ABS_OFF(frame_header, ext, 12);

CRPC_ABS_SIZE(::crpc::connect_options, 24);
CRPC_ABS_OFF(::crpc::connect_options, connect_timeout, 0);
CRPC_ABS_OFF(::crpc::connect_options, recv_buffer_bytes, 8);
CRPC_ABS_OFF(::crpc::connect_options, send_buffer_bytes, 12);
CRPC_ABS_OFF(::crpc::connect_options, tcp_no_delay, 16);

CRPC_ABS_SIZE(::crpc::listen_options, 16);
CRPC_ABS_OFF(::crpc::listen_options, backlog, 0);
CRPC_ABS_OFF(::crpc::listen_options, reuse_port, 4);
CRPC_ABS_OFF(::crpc::listen_options, recv_buffer_bytes, 8);
CRPC_ABS_OFF(::crpc::listen_options, send_buffer_bytes, 12);

CRPC_ABS_SIZE(rpc::client_options, 48);
CRPC_ABS_OFF(rpc::client_options, slot_capacity, 0);
CRPC_ABS_OFF(rpc::client_options, max_pending_write_bytes, 4);
CRPC_ABS_OFF(rpc::client_options, clock, 8);
CRPC_ABS_OFF(rpc::client_options, clock_ctx, 12);
CRPC_ABS_OFF(rpc::client_options, default_timeout, 16);
CRPC_ABS_OFF(rpc::client_options, client_id, 24);
CRPC_ABS_OFF(rpc::client_options, client_epoch, 40);

CRPC_ABS_SIZE(http::http_statistics, 112);
CRPC_ABS_OFF(http::http_statistics, active_connections, 0);
CRPC_ABS_OFF(http::http_statistics, longpoll_replay_window, 108);

CRPC_ABS_SIZE(client::statistics, 24);
CRPC_ABS_OFF(client::statistics, active_connections, 0);
CRPC_ABS_OFF(client::statistics, breaker_rejections, 20);

CRPC_ABS_SIZE(client::resilience_statistics, 36);
CRPC_ABS_OFF(client::resilience_statistics, rate_limited, 0);
CRPC_ABS_OFF(client::resilience_statistics, bulkhead_in_use, 32);

CRPC_ABS_SIZE(resilience::fallback_statistics, 28);
CRPC_ABS_OFF(resilience::fallback_statistics, ineligible, 0);
CRPC_ABS_OFF(resilience::fallback_statistics, applied, 24);

// 104, not 100: `push_acks` ends at 100 and the record is 8-byte aligned, so
// the last 4 bytes are tail padding. This padding is exactly why an absolute
// size alone cannot catch an appended field on x86; the member count above
// does. The anchor still has to exist, because a *reordering* that moves a
// `std::uint64_t` into the tail would change the width.
CRPC_ABS_SIZE(server::statistics, 104);
CRPC_ABS_OFF(server::statistics, active_connections, 0);
CRPC_ABS_OFF(server::statistics, handler_failures, 20);
CRPC_ABS_OFF(server::statistics, rate_limited_connection, 24);
CRPC_ABS_OFF(server::statistics, probe_admitted, 64);
CRPC_ABS_OFF(server::statistics, push_active_connections, 72);
CRPC_ABS_OFF(server::statistics, push_acks, 96);

CRPC_ABS_SIZE(http2::h2_error, 12);
CRPC_ABS_OFF(http2::h2_error, code, 0);
CRPC_ABS_OFF(http2::h2_error, scope, 4);
CRPC_ABS_OFF(http2::h2_error, stream_id, 8);

CRPC_ABS_SIZE(http2::connection_options, 44);
CRPC_ABS_OFF(http2::connection_options, role, 0);
CRPC_ABS_OFF(http2::connection_options, local_settings, 4);
CRPC_ABS_OFF(http2::connection_options, initial_connection_send_window, 28);
CRPC_ABS_OFF(http2::connection_options, initial_connection_recv_window, 32);
CRPC_ABS_OFF(http2::connection_options, max_send_queue_bytes, 36);
CRPC_ABS_OFF(http2::connection_options, reject_unknown_frame_types, 40);

CRPC_ABS_SIZE(http2::stream_snapshot, 32);
CRPC_ABS_OFF(http2::stream_snapshot, id, 0);
CRPC_ABS_OFF(http2::stream_snapshot, state, 4);
CRPC_ABS_OFF(http2::stream_snapshot, send_window, 8);
CRPC_ABS_OFF(http2::stream_snapshot, recv_window, 16);
CRPC_ABS_OFF(http2::stream_snapshot, local_end_stream, 24);
CRPC_ABS_OFF(http2::stream_snapshot, remote_end_stream, 25);
CRPC_ABS_OFF(http2::stream_snapshot, queued_bytes, 28);

CRPC_ABS_SIZE(http2::window_credit, 8);
CRPC_ABS_OFF(http2::window_credit, connection, 0);
CRPC_ABS_OFF(http2::window_credit, stream, 4);

CRPC_ABS_SIZE(frame_compression_info, 16);
CRPC_ABS_OFF(frame_compression_info, algorithm, 0);
CRPC_ABS_OFF(frame_compression_info, wire_size, 4);
CRPC_ABS_OFF(frame_compression_info, logical_size, 8);
CRPC_ABS_OFF(frame_compression_info, dropped, 12);
CRPC_ABS_OFF(frame_compression_info, drop_reason, 14);

// `fallback_context` has the same width and offsets on both targets: its only
// sub-word member is the 2-byte `std::optional<stage_kind>` and every other
// field is 8 bytes, so no 4-byte member can reorder it. The numbers are
// repeated here anyway because `CRPC_ABS_*` is defined inside this block.
CRPC_ABS_SIZE(resilience::fallback_context, 32);
CRPC_ABS_OFF(resilience::fallback_context, code, 0);
CRPC_ABS_OFF(resilience::fallback_context, stage, 2);
CRPC_ABS_OFF(resilience::fallback_context, retry_after, 8);
CRPC_ABS_OFF(resilience::fallback_context, one_way, 16);
CRPC_ABS_OFF(resilience::fallback_context, sent, 17);
CRPC_ABS_OFF(resilience::fallback_context, idempotent, 18);
CRPC_ABS_OFF(resilience::fallback_context, has_deadline, 19);
CRPC_ABS_OFF(resilience::fallback_context, deadline, 24);

// `server::options` is 264 bytes on x86: the pre-2.0.0 layout was 256 and the
// appended `clock_source` (a function pointer plus a context pointer) adds 8.
// The last pre-existing field `push_subscribe` stays at 232 and `clock_source`
// lands at 252, so every existing member keeps its offset.
CRPC_ABS_SIZE(server::options, 264);
CRPC_ABS_OFF(server::options, send_goaway_on_stop, 225);
CRPC_ABS_OFF(server::options, push_subscribe, 232);
CRPC_ABS_OFF(server::options, clock_source, 252);

#elif defined(CRPC_ABI_ARCH_X64)

// 64-bit: measured with MSVC 19.44, x64, Release and Debug identical.
#define CRPC_ABS_SIZE(T, n) static_assert(sizeof(T) == n, #T " absolute size")
#define CRPC_ABS_OFF(T, m, n)                                                \
    static_assert(offsetof(T, m) == n, #T "::" #m " absolute offset")

CRPC_ABS_SIZE(frame_header, 16);
CRPC_ABS_OFF(frame_header, payload_len, 0);
CRPC_ABS_OFF(frame_header, type, 4);
CRPC_ABS_OFF(frame_header, flags, 5);
CRPC_ABS_OFF(frame_header, stream_id, 8);
CRPC_ABS_OFF(frame_header, ext, 12);

CRPC_ABS_SIZE(::crpc::connect_options, 32);
CRPC_ABS_OFF(::crpc::connect_options, connect_timeout, 0);
CRPC_ABS_OFF(::crpc::connect_options, recv_buffer_bytes, 8);
CRPC_ABS_OFF(::crpc::connect_options, send_buffer_bytes, 16);
CRPC_ABS_OFF(::crpc::connect_options, tcp_no_delay, 24);

CRPC_ABS_SIZE(::crpc::listen_options, 32);
CRPC_ABS_OFF(::crpc::listen_options, backlog, 0);
CRPC_ABS_OFF(::crpc::listen_options, reuse_port, 8);
CRPC_ABS_OFF(::crpc::listen_options, recv_buffer_bytes, 16);
CRPC_ABS_OFF(::crpc::listen_options, send_buffer_bytes, 24);

CRPC_ABS_SIZE(rpc::client_options, 64);
CRPC_ABS_OFF(rpc::client_options, slot_capacity, 0);
CRPC_ABS_OFF(rpc::client_options, max_pending_write_bytes, 8);
CRPC_ABS_OFF(rpc::client_options, clock, 16);
CRPC_ABS_OFF(rpc::client_options, clock_ctx, 24);
CRPC_ABS_OFF(rpc::client_options, default_timeout, 32);
CRPC_ABS_OFF(rpc::client_options, client_id, 40);
CRPC_ABS_OFF(rpc::client_options, client_epoch, 56);

CRPC_ABS_SIZE(http::http_statistics, 224);
CRPC_ABS_OFF(http::http_statistics, active_connections, 0);
CRPC_ABS_OFF(http::http_statistics, longpoll_replay_window, 216);

CRPC_ABS_SIZE(client::statistics, 48);
CRPC_ABS_OFF(client::statistics, active_connections, 0);
CRPC_ABS_OFF(client::statistics, breaker_rejections, 40);

CRPC_ABS_SIZE(client::resilience_statistics, 72);
CRPC_ABS_OFF(client::resilience_statistics, rate_limited, 0);
CRPC_ABS_OFF(client::resilience_statistics, bulkhead_in_use, 64);

CRPC_ABS_SIZE(resilience::fallback_statistics, 56);
CRPC_ABS_OFF(resilience::fallback_statistics, ineligible, 0);
CRPC_ABS_OFF(resilience::fallback_statistics, applied, 48);

// 152 with no tail padding: `push_acks` ends exactly at the end of the record.
// An appended field therefore changes the width on x64 even without the member
// count; on x86 it does not. That asymmetry is why both halves exist.
CRPC_ABS_SIZE(server::statistics, 152);
CRPC_ABS_OFF(server::statistics, active_connections, 0);
CRPC_ABS_OFF(server::statistics, handler_failures, 40);
CRPC_ABS_OFF(server::statistics, rate_limited_connection, 48);
CRPC_ABS_OFF(server::statistics, probe_admitted, 88);
CRPC_ABS_OFF(server::statistics, push_active_connections, 96);
CRPC_ABS_OFF(server::statistics, push_acks, 144);

CRPC_ABS_SIZE(http2::h2_error, 12);
CRPC_ABS_OFF(http2::h2_error, code, 0);
CRPC_ABS_OFF(http2::h2_error, scope, 4);
CRPC_ABS_OFF(http2::h2_error, stream_id, 8);

CRPC_ABS_SIZE(http2::connection_options, 56);
CRPC_ABS_OFF(http2::connection_options, role, 0);
CRPC_ABS_OFF(http2::connection_options, local_settings, 4);
CRPC_ABS_OFF(http2::connection_options, initial_connection_send_window, 28);
CRPC_ABS_OFF(http2::connection_options, initial_connection_recv_window, 32);
CRPC_ABS_OFF(http2::connection_options, max_send_queue_bytes, 40);
CRPC_ABS_OFF(http2::connection_options, reject_unknown_frame_types, 48);

CRPC_ABS_SIZE(http2::stream_snapshot, 40);
CRPC_ABS_OFF(http2::stream_snapshot, id, 0);
CRPC_ABS_OFF(http2::stream_snapshot, state, 4);
CRPC_ABS_OFF(http2::stream_snapshot, send_window, 8);
CRPC_ABS_OFF(http2::stream_snapshot, recv_window, 16);
CRPC_ABS_OFF(http2::stream_snapshot, local_end_stream, 24);
CRPC_ABS_OFF(http2::stream_snapshot, remote_end_stream, 25);
CRPC_ABS_OFF(http2::stream_snapshot, queued_bytes, 32);

CRPC_ABS_SIZE(http2::window_credit, 8);
CRPC_ABS_OFF(http2::window_credit, connection, 0);
CRPC_ABS_OFF(http2::window_credit, stream, 4);

CRPC_ABS_SIZE(frame_compression_info, 16);
CRPC_ABS_OFF(frame_compression_info, algorithm, 0);
CRPC_ABS_OFF(frame_compression_info, wire_size, 4);
CRPC_ABS_OFF(frame_compression_info, logical_size, 8);
CRPC_ABS_OFF(frame_compression_info, dropped, 12);
CRPC_ABS_OFF(frame_compression_info, drop_reason, 14);

// Same numbers as the x86 block above: `fallback_context` has no 4-byte
// member, so the two targets agree. The anchor is repeated because
// `CRPC_ABS_*` is defined inside each architecture block.
CRPC_ABS_SIZE(resilience::fallback_context, 32);
CRPC_ABS_OFF(resilience::fallback_context, code, 0);
CRPC_ABS_OFF(resilience::fallback_context, stage, 2);
CRPC_ABS_OFF(resilience::fallback_context, retry_after, 8);
CRPC_ABS_OFF(resilience::fallback_context, one_way, 16);
CRPC_ABS_OFF(resilience::fallback_context, sent, 17);
CRPC_ABS_OFF(resilience::fallback_context, idempotent, 18);
CRPC_ABS_OFF(resilience::fallback_context, has_deadline, 19);
CRPC_ABS_OFF(resilience::fallback_context, deadline, 24);

// `server::options` is 320 bytes on x64: the pre-2.0.0 layout was 304 and the
// appended `clock_source` (a function pointer plus a context pointer) adds 16.
// The last pre-existing field `push_subscribe` stays at 264 and `clock_source`
// lands at 304, so every existing member keeps its offset.
CRPC_ABS_SIZE(server::options, 320);
CRPC_ABS_OFF(server::options, send_goaway_on_stop, 249);
CRPC_ABS_OFF(server::options, push_subscribe, 264);
CRPC_ABS_OFF(server::options, clock_source, 304);

#else

// Any other target (ARM64, RISC-V, ...) keeps the architecture-independent
// guards only: standard-layout, the relative chains and the exact member
// counts. Absolute sizes are maintained for x86 and x64 only, so no absolute
// anchor is asserted here and this translation unit must keep compiling on
// those targets.
//
// The member counts still catch an appended or removed field; the relative
// chains catch a reorder or a mid-record insertion where they exist. The only
// member-counted record without a relative chain is `rpc_error`: a reorder
// inside it is not caught here, but a rename is caught by the five `decltype`
// assertions above. What is *not* guarded outside x86/x64 is a change that
// preserves the member count and the relative order but alters the absolute
// width, which in practice means a change to the standard library's own types.

#endif

// `server::builder` is exported (`class CRPC_API builder`) and its out-of-line
// member functions read the private `options_` member of a consumer-allocated
// object, so its layout is ABI-relevant even though it is not a plain aggregate
// record. `CRPC_MEMBER_COUNT` and `offsetof` cannot be applied to it because
// its data members are private. Its width is also not a fixed number: it is
// `sizeof(server::options)` plus its three `std::vector` members, rounded to
// the builder's alignment, and `std::vector` widens with the standard
// library's debug configuration (MSVC x64: 392 bytes in Release, 416 in Debug;
// x86: 304 / 312). The structural relation below catches an added, removed or
// re-typed builder member on every target and configuration without pinning a
// standard-library width, and the alignment assertion catches a member that
// would shift the vector offsets without changing the total width. A reorder
// of the three same-width vectors is still covered only by policy, like the
// other unguarded cases in docs/abi.md.
static_assert(
    sizeof(server::builder) ==
        (sizeof(server::options) +
         sizeof(std::vector<server::listener_config>) +
         sizeof(std::vector<std::function<status(rpc::service_registry&)>>) +
         sizeof(std::vector<server_interceptor*>) +
         alignof(server::builder) - 1) /
            alignof(server::builder) *
            alignof(server::builder),
    "server::builder must be options plus its three vector members");
static_assert(alignof(server::builder) == alignof(server::options),
              "server::builder must keep the options alignment");

#undef CRPC_ABS_SIZE
#undef CRPC_ABS_OFF
#undef CRPC_ABI_ARCH_X86
#undef CRPC_ABI_ARCH_X64

// `rpc_error` and `status` deliberately carry no absolute size. Both own
// `std::string` and `std::vector` members, whose width is a property of the
// standard library configuration, not of this library: measured with MSVC
// 19.44 the pair is 80 bytes on x64 Release and 96 in Debug
// (`_ITERATOR_DEBUG_LEVEL=2`), 56 on x86 Release and 64 in Debug, and 112 with
// libstdc++ in `_GLIBCXX_DEBUG` mode. Pinning any one of those numbers would
// break the other required configurations, so these two records are guarded by
// their member count, their standard-layout precondition and the
// `sizeof(status) == sizeof(rpc_error)` relation instead.
//
// Because their size is not pinned, a member *type* change that keeps the
// member count would otherwise be invisible: changing `retry_after` from
// `std::chrono::milliseconds` to `std::chrono::microseconds`, for example,
// leaves the count, the `sizeof(status) == sizeof(rpc_error)` relation and
// (on x86) the absolute width all unchanged. `rpc_error` has public members,
// so the declared types can be asserted directly. This is independent of the
// standard library configuration and of the build type.

static_assert(std::is_same<decltype(rpc_error::code), rpc_code>::value,
              "rpc_error::code must stay rpc_code");
static_assert(std::is_same<decltype(rpc_error::message), std::string>::value,
              "rpc_error::message must stay std::string");
static_assert(std::is_same<decltype(rpc_error::retryable), bool>::value,
              "rpc_error::retryable must stay bool");
static_assert(
    std::is_same<decltype(rpc_error::retry_after),
                 std::chrono::milliseconds>::value,
    "rpc_error::retry_after must stay std::chrono::milliseconds");
static_assert(std::is_same<decltype(rpc_error::metadata), metadata_list>::value,
              "rpc_error::metadata must stay metadata_list");

// `status` has no public data member, so its member types cannot be named from
// here. Its single member is a `rpc_error`, and the `sizeof`/`alignof` pair
// above pins that relation.

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
static_assert(std::is_standard_layout<resilience::fallback_statistics>::value,
              "resilience::fallback_statistics must be standard-layout");
static_assert(std::is_standard_layout<server::statistics>::value,
              "server::statistics must be standard-layout");
// The HTTP/2 value types below cross the `CRPC_API connection` boundary by value
// (returned or accepted), so they are subject to the same rules as the records
// above even though they live in a protocol layer rather than the facade.
static_assert(std::is_standard_layout<http2::h2_error>::value,
              "http2::h2_error must be standard-layout");
static_assert(std::is_standard_layout<http2::connection_options>::value,
              "http2::connection_options must be standard-layout");
static_assert(std::is_standard_layout<http2::stream_snapshot>::value,
              "http2::stream_snapshot must be standard-layout");
static_assert(std::is_standard_layout<http2::window_credit>::value,
              "http2::window_credit must be standard-layout");
static_assert(std::is_standard_layout<frame_compression_info>::value,
              "frame_compression_info must be standard-layout");
// The library builds a `fallback_context` and the consumer's callback reads it,
// so it is a cross-boundary record even though no signature spells its name.
static_assert(std::is_standard_layout<resilience::fallback_context>::value,
              "resilience::fallback_context must be standard-layout");
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

// --- HTTP/2 and compression value types -------------------------------------
//
// `class CRPC_API connection` returns `h2_error`, `stream_snapshot` and
// `window_credit` by value and takes `connection_options` by value, so these
// four records cross the exported ABI in the same sense as the option records
// above. They were previously unguarded.
//
// The chains use `CRPC_AFTER` where the member types differ, and
// `CRPC_STEP_SAME` where two adjacent members share a type.

CRPC_AFTER(http2::h2_error, code, scope);
CRPC_AFTER(http2::h2_error, scope, stream_id);

CRPC_AFTER(http2::connection_options, role, local_settings);
CRPC_AFTER(http2::connection_options, local_settings,
           initial_connection_send_window);
CRPC_AFTER(http2::connection_options, initial_connection_send_window,
           initial_connection_recv_window);
CRPC_AFTER(http2::connection_options, initial_connection_recv_window,
           max_send_queue_bytes);
CRPC_AFTER(http2::connection_options, max_send_queue_bytes,
           reject_unknown_frame_types);

CRPC_AFTER(http2::stream_snapshot, id, state);
CRPC_AFTER(http2::stream_snapshot, state, send_window);
CRPC_AFTER(http2::stream_snapshot, send_window, recv_window);
CRPC_AFTER(http2::stream_snapshot, recv_window, local_end_stream);
CRPC_AFTER(http2::stream_snapshot, local_end_stream, remote_end_stream);
CRPC_AFTER(http2::stream_snapshot, remote_end_stream, queued_bytes);

CRPC_STEP_SAME(http2::window_credit, connection, stream);

CRPC_AFTER(frame_compression_info, algorithm, wire_size);
CRPC_AFTER(frame_compression_info, wire_size, logical_size);
CRPC_AFTER(frame_compression_info, logical_size, dropped);
CRPC_AFTER(frame_compression_info, dropped, drop_reason);

// `fallback_context` mixes a 2-byte optional, a duration and three adjacent
// bools, so the absolute anchors above pin the layout on x86/x64 while this
// relative chain catches a reorder on every target (including the ones with no
// absolute anchor). Without it, a swap of `sent`/`idempotent` would be invisible
// outside x86/x64.
CRPC_AFTER(resilience::fallback_context, code, stage);
CRPC_AFTER(resilience::fallback_context, stage, retry_after);
CRPC_AFTER(resilience::fallback_context, retry_after, one_way);
CRPC_AFTER(resilience::fallback_context, one_way, sent);
CRPC_AFTER(resilience::fallback_context, sent, idempotent);
CRPC_AFTER(resilience::fallback_context, idempotent, has_deadline);
CRPC_AFTER(resilience::fallback_context, has_deadline, deadline);

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

// The fallback snapshot is seven plain `std::size_t` counters. Three guards
// cover three different changes: ABS_SIZE above catches an append (seven
// size_t values have no tail padding), the STEP chain catches an insertion, a
// removal or a reorder, and the `decltype` assertions below catch a width or
// type change. The last one is not redundant: CRPC_STEP hardcodes
// `sizeof(std::size_t)`, so narrowing `applied` to `std::uint32_t` keeps the
// step, the absolute offset and the total width unchanged on x64.
CRPC_STEP(resilience::fallback_statistics, ineligible, skipped_deadline);
CRPC_STEP(resilience::fallback_statistics, skipped_deadline, attempted);
CRPC_STEP(resilience::fallback_statistics, attempted, failed);
CRPC_STEP(resilience::fallback_statistics, failed, canceled);
CRPC_STEP(resilience::fallback_statistics, canceled, overrun);
CRPC_STEP(resilience::fallback_statistics, overrun, applied);

static_assert(
    std::is_same<decltype(resilience::fallback_statistics::ineligible),
                 std::size_t>::value,
    "fallback_statistics::ineligible must be std::size_t");
static_assert(
    std::is_same<decltype(resilience::fallback_statistics::skipped_deadline),
                 std::size_t>::value,
    "fallback_statistics::skipped_deadline must be std::size_t");
static_assert(
    std::is_same<decltype(resilience::fallback_statistics::attempted),
                 std::size_t>::value,
    "fallback_statistics::attempted must be std::size_t");
static_assert(
    std::is_same<decltype(resilience::fallback_statistics::failed),
                 std::size_t>::value,
    "fallback_statistics::failed must be std::size_t");
static_assert(
    std::is_same<decltype(resilience::fallback_statistics::canceled),
                 std::size_t>::value,
    "fallback_statistics::canceled must be std::size_t");
static_assert(
    std::is_same<decltype(resilience::fallback_statistics::overrun),
                 std::size_t>::value,
    "fallback_statistics::overrun must be std::size_t");
static_assert(
    std::is_same<decltype(resilience::fallback_statistics::applied),
                 std::size_t>::value,
    "fallback_statistics::applied must be std::size_t");

// `server::statistics` mixes widths: six `std::size_t` counters, six
// `std::uint64_t` resilience counters, then seven `std::size_t` push counters.
// A uniform STEP would be wrong on 32-bit targets, where the `std::size_t` runs
// are 4 bytes wide but the `std::uint64_t` block is 8-byte aligned. The chain
// therefore uses the matching macro at each boundary. The total width differs
// between targets (104 on x86, 152 on x64), so it is asserted in the
// per-architecture block above rather than here.
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
// `rpc_error`, `status` and `endpoint` ARE standard-layout on the toolchains
// audited here (MSVC 19.44, GCC 13, Clang 18 all report is_standard_layout ==
// true). `buffer` is standard-layout on MSVC and GCC but NOT on Clang, so it is
// the one exception and no blanket claim covers all four.
//
// Because they are standard-layout, `offsetof` on their members is not
// ill-formed: `offsetof(rpc_error, message)` compiles and yields 8 under
// /W4 with all three compilers. It is nevertheless not *portable*: the offset
// depends on how the standard library lays out its own types, which is the same
// configuration dependency that makes their absolute size unpinnable. No
// `offsetof` assertion is therefore made on them, and no `is_standard_layout`
// assertion is added either (it would fire on `buffer` under Clang for a reason
// that is not a CRPC layout change).
//
// What is asserted is only what does not name an offset or an absolute size:
// `status` is exactly one `rpc_error` and nothing else, and `buffer` /
// `endpoint` must be at least pointer-aligned. Their absolute size belongs to
// the standard library and is deliberately not pinned (docs/abi.md section 9).

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
#undef CRPC_MEMBER_COUNT

// A named symbol keeps MSVC from emitting LNK4221 ("no public symbols found")
// for this translation unit. It is internal detail, not part of the ABI, and is
// declared in no header. `[[maybe_unused]]` is required because the unit has no
// code that reads it: GCC and Clang otherwise reject it under -Werror.
[[maybe_unused]] int abi_check_translation_unit = 0;

} // namespace
} // namespace crpc
