# CRPC engineering principles

[简体中文](principles.zh-CN.md) | **English**

Version 1.0 / 2026-10-19

This document states the rules the implementation is held to, what each rule
forbids, how the code enforces it, and how it is verified. A rule whose
enforcement is missing is marked as such rather than described as done.

## Contents

| # | Section |
|---|---|
| [1](#s1) | Three bottom lines |
| [2](#s2) | Distributed laws |
| [3](#s3) | AP before C |
| [4](#s4) | No-stall |
| [5](#s5) | Causality, not wall clocks |
| [6](#s6) | Exactly-once |
| [7](#s7) | Control theory |
| [8](#s8) | Explicit over implicit |
| [9](#s9) | Enforcement and verification |

## 1. Three bottom lines <a id="s1"></a>

These are non-negotiable and override every performance or schedule goal.

| Line | What it forbids | How the code enforces it | How it is verified |
|---|---|---|---|
| **No deadlock** | I/O, a user callback, coroutine suspension, `sem_post`, a log sink or a blocking allocation while a lock is held; cross-shard writes; more than one reader/writer per connection; waiting on a lane from itself | Documented orders: `lease_manager` mutex before `registry` mutex; registry sinks run after the writer mutex; connection callbacks are serialized; `call_slot` state is a CAS; breakers are atomic; `io` has one reader and one writer | close-race, reentrant-push and four-way state-race tests; the design's lock-order assertion and TSan gate are **specified, not implemented** in CI |
| **No crash** | Unvalidated external input; UB (unaligned access, overflow, dangling, data race); an exception crossing a coroutine/ABI boundary; `assert` as a runtime check; unbounded recursion | `byte_reader`/`byte_writer` bounds checks; fail-closed frame, HTTP, multipart and envelope decoders; `result<T>`/`status` instead of exceptions; `new (std::nothrow)` in `buffer`; codec depth 32; generation checks | malformed-frame/envelope tests; HTTP and HTTP/2 never-crash fuzz cases; UBSan/ASan scripts; standalone fuzz targets are **specified, not implemented** |
| **No leak** | An acquire without exactly one release; delivery after close; a late completion touching a recycled object; `shared_ptr` cycles; unbounded pools | fixed `call_slot_pool` with generation bump on recycle; RAII permits in bulkhead/lease; idempotent `close`/`cancel`/`reset`; `file_part` deletes its temporary file; inproc/epoll/IOCP complete in-flight writes exactly once when closing | slot-recycle and no-leak tests; close-race tests; ASan/LSan runs; a `drain()` assertion that all counts return to zero is **specified, not implemented** |

The three lines are enforced by different mechanisms, not by a single check.
A path that cannot be proven acyclic must use try-lock with a timeout and an
explicit recovery path; no such path is currently marked in the code, so the
rule is a review obligation.

## 2. Distributed laws <a id="s2"></a>

| # | Law | What it forbids | Enforcement in CRPC | Verification |
|---|---|---|---|---|
| 1 | No real-time global state | Depending on arrival order; a terminal state that depends on which message came first | monotonic registry `index`; generation-checked call slots; watch snapshot + delta; terminal CAS | reorder injection; four-way terminal race; watch two-order tests |
| 2 | Half-open is normal | Assuming an operation either completed or did not | idempotency key; `timeout` after write with a non-idempotent call reports `unknown_side_effect` | `client_timeout_is_terminal_deadline_exceeded`, `client_idempotent_timeout_has_no_unknown_side_effect` |
| 3 | Peer writes diverge | Two writers mutating one state without authority | registry accepts only the owner `node_id` or admin token; incarnation and fence checks; single-writer client | `registry_rejects_non_owner_write`, `registry_rejects_stale_incarnation`, `registry_index_and_fence_are_monotonic` |
| 4 | External effects are irrevocable | Rolling back a completed side effect; claiming “not executed” after a timeout | no rollback path; retry only with idempotency; `unknown_side_effect` is explicit | retry tests; timeout semantic test |
| 5 | Consensus is the last resort | Refusing writes because a majority is unreachable | registry is a single authority; AP is preserved; Raft is explicitly absent | `registry_concurrent_snapshots`, authority tests |
| 6 | Synchronous resonance | Unbounded retries or synchronized retry storms | retry budget (default 10% of traffic), max 3 attempts, jitter, exponential cap | `retry_budget_exhaustion_stops_retries`, `retry_jitter_within_bounds` |
| 7 | Monotonic anchor | Dedup or ordering based on a random UUID or a clock | `call_id` = `(client_id, epoch, seq)`; registry `index`; `lane_seq`; push `seq`/`topic_seq` | `call_id` encode/decode tests; registry and push sequence tests |
| 8 | Rule pile-up | Handling an unknown state by guessing | fail-closed decoders; explicit state machines for calls, streams, h2 and watch | malformed-frame/envelope tests; illegal-transition tests |
| 9 | Asymmetric safety | Trusting the untrusted direction | every external byte is validated; unknown type/codec/flag/version is rejected; HTTP smuggling defenses | frame/HTTP/HTTP2 fail-closed tests |
| 10 | Events precede data | Applying a delta before the snapshot or without a gap check | watch state index `S`: `E <= S` dropped, `E == S+1` applied, `E > S+1` resync | `watch_gap_triggers_resync`, `watch_drops_duplicate_and_old_index` |
| 11 | NULL must not pass through | Deleting by absence; a stale registration reviving a tombstone | tombstones; a tombstone rejects every incarnation that is not strictly greater | `registry_tombstone_blocks_old_incarnation`, `watch` tombstone tests |
| 12 | Causality | Ordering cross-node events by wall clock | monotonic clocks for all control decisions; trace parent from the send/receive pair; registry index | `tracer_parent_child_relation`; flow clock tests; see §5 for the deadline defect |
| 13 | End-to-end argument | Treating a middle-layer ACK as correctness | idempotency key travels to the endpoint; TCP framing is transport, not a business guarantee | key carried in the envelope; server dedup is **specified, not implemented** |
| 14 | Observation interferes | Blocking the forwarding path on logging or metrics | log records go to a per-thread ring; tracer uses a bounded queue; hooks are frozen and empty-chain is cheap | log/tracer allocation and overflow tests; AOP is not yet on the call path |

## 3. AP before C <a id="s3"></a>

**Forbids:** refusing a write because a quorum is unreachable; making the
data plane depend on Raft; losing the last readable snapshot when the authority
disappears.

**Enforcement:** the registry accepts writes only through its single authority
path, but that path is local and does not wait for a majority. Readers load the
last immutable snapshot (`fail-static`) while writes fail closed. Conflict
resolution is by incarnation and fence, not by arrival order. No Raft/PreVote
code is on the data path; `cluster::election` is a separate primitive. A
tombstone cannot be revived by an older incarnation.

**Verification:** `registry_concurrent_snapshots`, the non-owner/stale-incarnation
tests, and the lease expiry tests. A partitioned multi-node deployment cannot be
tested yet because the network registry service is **specified, not
implemented**.

## 4. No-stall <a id="s4"></a>

**Forbids:** a connection or stream that silently stops delivering; waiting
forever on a zero window, an exhausted budget or an empty pool; dropping bytes
without a count; relying on TCP FIN as the only close signal.

**Enforcement:** the in-process backend defers a push whose tail does not fit
and completes it when the drain frees room; it never fails or drops for
backpressure. The epoll write loop self-posts when its byte budget is exhausted
(R4). IOCP re-posts the remainder of a short write and treats cancellation as a
completion. The client rejects a frame that would exceed
`max_pending_write_bytes` with `resource_exhausted` instead of blocking. Push
queues have byte/message bounds and every overflow is counted; a slow subscriber
is disconnected with an observable event. HTTP/2 `send_data` parks bytes and
exposes `parked_streams()` so the owner can arm a zero-window probe.

**Gaps:** the periodic zero-window probe, keepalive/stall detector and a global
“connection made no progress” timer are **specified, not implemented**. The h2
layer documents that its owner must arm the probe; no owner does yet. The
no-stall rule is therefore enforced at the queue and write-path level, not yet
end to end.

**Verification:** `inproc_bounded_queue_defers_completion`,
`epoll_one_mib_echo_small_queue_bounded_wakes`,
`iocp_large_transfer_small_queue`, `no_stall_writer_queue_rejects_bounded`,
`hub_publisher_never_blocks`, `hub_slow_subscriber_disconnected`.

## 5. Causality, not wall clocks <a id="s5"></a>

**Forbids:** ordering cross-node events by `system_clock`; deriving a fence
from `time.Now()`; using a wall-clock RTT; treating a timestamp as a
happens-before edge.

**Enforcement:** registry fences derive from the monotonic index; leases,
health, breakers, rate limiters and flow control take an injected monotonic
clock; `flow::rtt_tracker` rejects backwards time; trace parent/child comes from
the send/receive pair, not timestamps; `obs::deadline` is a relative duration.

**Known defect:** `rpc::client` writes an absolute `steady_clock` value into
`deadline_ns`, while the design requires a relative `remaining_ns`. This is a
causality violation for cross-process calls and is recorded in
[the protocol document](protocol.md#s9). Until it is fixed, multi-process
deadline propagation must not be claimed.

**Verification:** `deadline_is_relative` (observability), `tracker_rejects_backwards_time`,
`tracer_parent_child_relation`, and the registry index/fence tests. A
clock-rollback test for the whole call path is **specified, not implemented**.

## 6. Exactly-once <a id="s6"></a>

Exactly-once is defined as **at-least-once delivery plus a monotonic dedup
anchor at the endpoint**. A middle-layer ACK is an optimization, never the
correctness boundary.

**Forbids:** random UUIDs as idempotency keys; a dedup table evicted by wall
clock; retrying a non-idempotent call without a key; claiming “not executed”
after a timeout.

**Enforcement today:** the client assigns the R6 triple
`(client_id[16], client_epoch, seq)`, reuses it on retry, and writes it into the
call envelope. `call_context` tracks `idempotent` and `has_idempotency_key`.
The dispatcher decodes the key into the context. A written non-idempotent call
that times out is marked `unknown_side_effect`. `retry` refuses
`unknown_side_effect` unless the call is idempotent.

**Gap:** the server-side dedup table (fresh/in-progress/done, per-client ring,
FIFO eviction) is **specified, not implemented**. Exactly-once is therefore not
yet a property of a CRPC deployment; only the key transport and the timeout
semantics exist.

**Verification:** `client_idempotent_timeout_has_no_unknown_side_effect`,
`retry_unknown_side_effect_requires_idempotency`, and the `call_id`
encode/decode tests. A replay test that executes a handler once is **specified,
not implemented** because there is no dedup table to test.

## 7. Control theory <a id="s7"></a>

The flow-control design follows five rules from feedback control, not from a
textbook model.

1. **Do not seek “to know”; seek “to approach”.** The RTT baseline moves in
   small, gated steps: downward immediately, upward geometrically and bounded.
   Enforced by `flow::rtt_estimator`; verified by
   `estimator_downward_is_immediate`, `estimator_upward_is_bounded_and_capped_at_observation`.
2. **Overshoot buys information; it cannot be eliminated.** Probing is
   mandatory, but its cost is bounded by a budget. Enforced by the pacer's
   bounded burst and the congestion window's self-clocked growth; verified by
   `pacer_take_is_bounded_by_credit`, `window_slow_start_grows_per_ack`.
3. **Lag is the original sin.** Every sample describes a past path. The tracker
   records the observation delay and rejects stale samples instead of
   pretending to compensate. Enforced by `rtt_tracker`; verified by
   `tracker_rejects_stale_token`.
4. **No optimal step exists.** Choose “good enough” defaults (12.2% upward,
   2 ms burst, 70% window after congestion) and accept the trade-off. Enforced
   by `flow::config`; verified by the clamp tests.
5. **Do not invent a mechanism that “detects the path is clear”.** The design
   removed such a mechanism because it always detected clearance and oscillated.
   The estimator has no clearance predictor; path changes are inferred only from
   sustained directional evidence (`estimator_sustained_shift_is_detected_as_path_change`).

Two corollaries are implemented: when everything has reset, restore the
historical high-water mark rather than ramping from cold
(`restore_high_water`, `window_high_water_survives_a_decrease`), and measure
width and fluctuation in phases rather than trying to learn both at once.
Fairness is not promised; the only guarantee is that no subscriber is silently
starved, which is why overflow is always counted and slow subscribers are
reported.

## 8. Explicit over implicit <a id="s8"></a>

**Forbids:** host-endian wire fields; `long`/`unsigned long` in a public
signature; struct `memcpy`; bit-fields; unaligned `reinterpret_cast`; hidden
ownership; exceptions across a boundary; a default that silently guesses.

**Enforcement:** fixed-width `u8`…`u64`, explicit little-endian load/store,
bounds-checked readers/writers, `span` for borrowed views, move-only handles,
`noexcept` on callbacks, `status`/`result<T>` for failure, and `static_assert`
on the platform invariants (8-bit byte, exact integer widths, IEEE-754). The ABI
policy forbids standard containers and non-trivial types in exported signatures.
A missing codec specialization is a compile error, not an empty payload.

**Verification:** `test_endian.cpp`, `test_wire.cpp`, `test_frame.cpp`, UBSan
alignment checks, and the ABI layout assertions. The rule “no behaviour that
matters lives only in an inline function” is stated in the ABI policy and
reviewed by inspection; no automated ABI checker is checked in.

## 9. Enforcement and verification <a id="s9"></a>

A principle is real only when a failing test or a build gate can catch its
violation. The current gates are:

- **Build:** zero warnings (`/W4 /WX`, `-Wall -Wextra -Wpedantic -Werror`),
  `static_assert` coverage for the wire and ABI assumptions.
- **Unit/functional:** the 16 suites listed in
  [the architecture document](architecture.md#s20).
- **Sanitizers:** ASan/LSan and UBSan on Linux/Clang; TSan intended for the
  shard model, which does not exist yet. MSVC has no TSan.
- **Fault injection:** inproc drop/delay/reorder/write-error/read-error, close
  races, four-way terminal races.
- **Missing gates:** standalone fuzz targets, a lock-order assertion, a
  whole-call clock-rollback test, a dedup replay test, a drain/leak assertion,
  and the multi-node partition test. Each missing gate maps to a feature marked
  **specified, not implemented** elsewhere in this documentation.

The review rule is simple: a claim that cannot name its test, its sanitizer
result, or its recorded measurement is a design intent, not a guarantee.
