# CRPC performance

[简体中文](performance.zh-CN.md) | **English**

Version 2.0 / 2026-10-24

This document states what has actually been measured in the repository, on what
hardware and toolchain, what is only a budget, and how the library will be
benchmarked. A target is not a result. Numbers that do not exist are marked as
absent rather than estimated.

## Contents

| # | Section |
|---|---|
| [1](#s1) | Measurement method |
| [2](#s2) | Recorded evidence |
| [2.5](#s2-5) | Compression, RPC matrix and Redis benchmarks (2026-10-26) |
| [3](#s3) | Budgets and targets |
| [4](#s4) | Benchmark plan |
| [5](#s5) | Anti-patterns the design rejects |
| [6](#s6) | Reporting and acceptance |

## 1. Measurement method <a id="s1"></a>

A performance claim is valid only with all of the following:

- what was measured (RPC unary, HTTP request, codec operation, raw socket);
- message size, connection count, thread/shard count and batching;
- the exact toolchain and optimisation level (compiler version, `-O2`/`-O3`,
  MSVC `/O2`, sanitizer off);
- CPU model, core count, OS and loopback or network path;
- the metric: throughput from an **independent request counter**, plus
  p50/p99/p999 latency, CPU utilisation and syscalls per message;
- a warmup and a run count, with the median reported;
- the benchmark source and the command line.

The design's measurement protocol is a 10 s warmup, five 30 s runs, and the
median of the five. Results must not be produced by dividing a batch size by an
RTT or by converting “64 frames per RTT” into RPC/s (design R10). Those
calculations describe a kernel path, not an application call rate.

## 2. Recorded evidence <a id="s2"></a>

`bench/bench_rpc.cpp` now exists (target `crpc_bench_rpc`, gated by
`CRPC_BUILD_BENCH`) and measures the real client facade and dispatcher over the
in-process backend and, on Windows, the IOCP TCP loopback (§2.3). The
microbenchmarks in `bench/bench_core.cpp` cover the wire/codec layers. The raw
loopback probe and the object-layout probe below predate the benchmark target
and are kept as history.

1. **Raw loopback probe**, recorded in this section. Hardware: AMD
   Ryzen 9 7945HX, 16 cores / 32 threads. Toolchain: MSVC 2022, `/O2`, Windows
   loopback. Workload: 16-byte frame, blocking socket echo, 1,000,000 RPCs, one
   connection, one thread pair, no coroutine, codec or multiplexing.

   ```
   per_send=1   rpc=1000000 time=61.905s -> 0.02 M RPC/s
   per_send=64  rpc=1000000 time=0.949s  -> 1.05 M RPC/s
   ```

   The 50x difference is the syscall/wakeup cost of one send+recv per message
   versus batching 64. This is an upper bound on the kernel path, not a CRPC
   measurement.

2. **Object-layout probe**, compiled for this document with MSVC 2022 x64,
   `cl /nologo /std:c++17 /EHsc /I include`:

   | Type | `sizeof` |
   |---|---|
   | `rpc::call_slot` | 440 bytes |
   | `rpc::call<int>` | 24 bytes |
   | `rpc::client` | 568 bytes |
   | `push::hub` | 8 bytes (pimpl pointer) |
   | `cluster::registry` | 336 bytes |
   | `cluster::lease_manager` | 184 bytes |

   Layout, not speed. It shows that the design's 128-byte call-slot budget is
   not met by the current slot (`std::mutex`, `condition_variable`, buffer,
   `rpc_error` and callback storage dominate).

### 2.3 Measured end-to-end RPC (2026-10-19)

Source: `bench/bench_rpc.cpp`; command:
`crpc_bench_rpc` (Windows MSVC Release) and the same target built under WSL
(GCC Release).
Workload: 64-byte `std::string` echo through a registered `bench.echo` method,
encoded/decoded with `call_codec<std::string>` (crpc-binary path), one client
thread over the in-process backend (and IOCP TCP loopback on Windows). Machine:
AMD Ryzen 9 7945HX, 32 logical cores. Compilers: MSVC 19.44 x64 Release;
GCC 13.3 Release under WSL2 (kernel 6.18). Median of three runs.

| Case | depth | MSVC inproc | GCC/WSL inproc | MSVC IOCP loopback |
|---|---:|---|---|---|
| sync latency p50 | 1 | 2.80 µs | 60.4 µs | 86.2 µs |
| sync latency p90 | 1 | 4.20 µs | 95.8 µs | 123.8 µs |
| sync latency p99 | 1 | **22.6 µs** | 151.8 µs | 170.2 µs |
| sync latency p999 | 1 | 49.0 µs | 286 µs | 343.7 µs |
| sync latency max | 1 | 269 µs | 4.30 ms | 4.35 ms |
| throughput | 1 | 0.157 M RPC/s | 0.0085 M RPC/s | 0.013 M RPC/s |
| throughput | 16 | 0.241 M RPC/s | 0.034 M RPC/s | 0.024 M RPC/s |
| throughput | 64 | 0.275 M RPC/s | 0.076 M RPC/s | not run |
| multi-shard 1/2/4 | 16 | 0.217 / 0.196 / 0.193 M | 0.0275 / 0.0274 / 0.0275 M | n/a |

Tail: after the wake-path fix the ≈100 ms stalls are gone. On MSVC the in-process
maximum over 50k samples is now 168-624 µs (median 269 µs) and the WSL maximum is
2.6-14.5 ms (median 4.3 ms). The MSVC IOCP maximum is noisy: 0.54-1.36 ms over
three idle 20k-sample runs, and 1.2-27 ms across eight runs under load, while
p99 (159-169 µs) and p999 (237-295 µs) stay flat. A max-only outlier with a flat
p99/p999 is scheduler/OS noise, not a distribution shift, so it is reported as
such rather than as a tail regression.

Bottleneck evidence (measured, not inferred). Per-call costs on MSVC Release:
`make_shared<call_state<std::string>>` 52 ns, the per-connection
`std::recursive_mutex` 8 ns, string codec encode plus decode 17 ns. A phase split
at depth 64 over 200k calls gives issue 2612 ns + wait 934 ns; only 0.6 % of calls
are already complete at issue, so the time is the round trip itself, not the
bookkeeping. Multi-client threads against multi-shard: 1/2/4/8 threads →
0.227/0.382/0.526/0.773 M RPC/s (1→8 is 3.4x, saturating near 0.8 M). The measured
ceiling of the current design is therefore about 1 M RPC/s per process; the
remaining 5-14x to the 5 M target needs the round trip removed (batching or
client-shard affinity), not allocation or lock tuning. The target numbers are not
met (see §3).

Two plausible optimisations were tested and rejected, so nobody repeats them:

| Experiment | Change | Result | Verdict |
|---|---|---|---|
| Coroutine stack pooling | `runtime_options::handler_stack_bytes = 0` (reuses corocc's default 256 KiB pool instead of mapping 64 KiB per spawn) | depth 1/16/64 = 0.11/0.18/~0.20 M RPC/s, identical to the default within noise | Not the bottleneck; reverted |
| Deadline timer removal | `default_deadline(0 ms)` in the benchmark (no per-call arm/cancel) | depth 64 went 0.19 → 0.23 M RPC/s | Not the bottleneck; reverted |

Both were reverted; the measured ceiling is unchanged. The per-call cost is the
synchronous round trip (issue 2612 ns + wait 934 ns at depth 64), so allocation
and timer work are not where the 5 M target is lost.

No p99, p999, cross-shard handoff, codec or resilience-pipeline measurement
existed before this document; the p99/p999 figures above are now measured, the
cross-shard handoff and resilience-pipeline budgets remain unmeasured.

### 2.4 CRC32C and push-hub statistics (2026-10-24) <a id="s2-4"></a>

Source: `bench/bench_core.cpp` (`crpc_bench_core crc32c`). Toolchain: MSVC 19.51
x64 Release, `NDEBUG`, sanitizer off. Machine: AMD Ryzen 9 7945HX, 16 cores /
32 threads. Median of seven runs after one discarded warm-up pass.

`frame.cpp` computed CRC32C one byte at a time through a 256-entry table. It now
uses slicing-by-8 (eight interleaved tables) as the portable path and, on x64
MSVC when CPUID reports SSE4.2, the hardware `crc32` instructions; the choice is
made once at run time, so a CPU without SSE4.2 still takes the portable path.

| Case | byte loop (before) | slicing-by-8 | `crc32c()` (hardware) |
|---|---:|---:|---:|
| 256 B | 504.2 ns | 125.6 ns | **58.5 ns** |
| 16 KiB | 32 490.3 ns | 8 264.7 ns | **3 683.6 ns** |

The measured speedup over the byte loop is 8.6x (256 B) and 8.8x (16 KiB). The
`crc32c()` column is faster than the local slicing-by-8 reference measured on the
same input, which confirms the hardware path is selected on this CPU. Correctness
is covered by `tests/test_frame.cpp`: the known CRC32C vectors plus a cross-check
against an independent per-byte reference over every length 0..2048 and several
sizes up to 70 000 bytes.

The push hub's `stats()` no longer scans every subscriber and subscription. The
server reads it on every PUSH_ACK (`refresh_push_stats_locked`), so the old
O(subscribers x subscriptions) scan ran per acknowledgement. Four counters are
maintained incrementally at each queue/subscription mutation and `stats()` is
O(1); `tests/test_push.cpp` walks every mutation path (subscribe, publish,
overflow, retransmit, front replay, ack, take, detach, release, unsubscribe,
disconnect) and compares the aggregates against the values a full scan would
produce.

Those two hot-path copies are now gone. The frame decoder gained
`feed_external()`/`drain_external()`: `feed_external()` borrows the caller's
recv chunk and decodes complete frames straight out of it, so in borrowing mode
a returned payload points into that chunk with no per-chunk copy, and
`drain_external()` copies back only the trailing bytes that do not complete a
frame. The TLS writer's `push()` now borrows the caller's span and materialises
the unwritten remainder into `write_buffer_` only when the write parks, so the
common synchronous write performs no copy and the caller may reuse the span as
soon as `push()` returns. `feed()` keeps its copy-in contract (the caller's span
need not outlive the call) and `push()` keeps its reuse-after-return contract;
`feed_external()` is the zero-copy form for a caller that consumes every decoded
frame before the chunk goes away. The `feed_external()` contract is specified in
[protocol §15](protocol.md#s15); the paths are exercised by `tests/test_frame.cpp`
(`frame_decoder_external_*`) and `tests/test_tls_write_borrow.cpp`.

| Case | `feed()` | `feed_external()` | speedup |
|---|---:|---:|---:|
| 1 KiB frame | 56.0 ns/frame | **16.4 ns/frame** | 3.4x |
| 64-frame batch | 44.9 ns/frame | **10.5 ns/frame** | 4.3x |
| payload copies | 1.00 | **0.00** | — |

The cases are the bench records `frame/feed_copy_1KiB_per_frame` vs
`frame/feed_external_1KiB_per_frame`, the `*_batch64_per_frame` pair, and
`frame/feed_*_payload_copies`. The copies row is a deterministic pointer check,
not a timing: a `feed()` payload points into the decoder while a
`feed_external()` payload is the chunk's own bytes.
`frame_decoder_external_matches_feed` checks that the same frame stream is
produced at every split point, including a split inside a frame.

### 2.5 Compression, RPC matrix and Redis benchmarks (2026-10-26) <a id="s2-5"></a>

Sources: `bench/bench_compression.cpp`, `bench/bench_rpc_matrix.cpp` and
`bench/bench_redis.cpp` (targets `crpc_bench_compression`,
`crpc_bench_rpc_matrix`, `crpc_bench_redis`). Toolchain: MSVC x64 **Debug**
(`/W4 /WX`), AMD Ryzen 9 7945HX (16 cores / 32 threads), single-process
`inproc_backend`, no network, no sanitizer. Every case warms up, runs several
times and reports the median. Debug numbers are for same-environment regression
comparison only; they are not Release or production capacity. Run method,
scenarios and the full scenario evidence are in
[distributed-testing.md](distributed-testing.md).

**Frame compression (text payload, wire/plain).** The benchmark uses the real
frame path (header + 4-byte logical-length prefix + codec stream), not a bare
codec call.

| Algorithm | 4 KiB | 64 KiB |
|---|---:|---:|
| none | 1.004 | 1.000 |
| lz4 | 0.026 | 0.005 |
| deflate | 0.027 | 0.004 |
| lz77 | 0.048 | 0.025 |
| zip | 0.034 | 0.005 |
| gzip | 0.031 | 0.005 |

**Frame compression throughput (text, Debug).**

| Case | Encode | Decode |
|---|---:|---:|
| text 4 KiB lz4 | 307.9 MiB/s | 316.3 MiB/s |
| text 64 KiB lz4 | 343.0 MiB/s | 474.3 MiB/s |
| text 4 KiB deflate | 5.57 MiB/s | 90.75 MiB/s |
| text 4 KiB gzip | 5.51 MiB/s | 66.95 MiB/s |

Incompressible binary: every algorithm falls back to `none` by design, so the
ratio is 1.0. For example, lz4 attempts to encode 4 KiB at 84.0 MiB/s and the
final `chosen` is `none`. The run recorded 120 cases
(`.opencode/tmp/distbench-compression.log`).

**Reliable RPC latency (closed loop, depth 1; 5 x 2000 samples, median).**

| Payload | p50 | p99 |
|---|---:|---:|
| 16 B | 64.0 µs | 177.1 µs |
| 256 B | 59.3 µs | 174.8 µs |
| 4 KiB | 67.1 µs | 194.8 µs |

**Reliable RPC throughput (closed loop, payload x concurrency, 0 errors).**

| Payload | c=1 | c=4 |
|---|---:|---:|
| 16 B | 83.1 µs/call (~12.0k/s) | 96.9 µs/call (~10.3k/s) |
| 256 B | 113.9 µs/call (~8.8k/s) | 85.8 µs/call (~11.7k/s) |
| 4 KiB | 123.9 µs/call (~8.1k/s) | 99.1 µs/call (~10.1k/s) |
| 64 KiB | 265.1 µs/call (~3.8k/s) | 272.5 µs/call (~3.7k/s) |

**Unreliable `ONE_WAY` issue/delivery (0 errors, drained = yes).**

| Payload | c=1 issued/delivered | c=4 issued/delivered |
|---|---:|---:|
| 16 B | 10000 / 10000, 0.4894 s (~20.4k/s) | 10000 / 10000, 0.5816 s (~17.2k/s) |
| 4 KiB | 4000 / 4000, 0.2004 s (~20.0k/s) | 4000 / 4000, 0.2665 s (~15.0k/s) |

**Pipelined batch throughput (Debug, 2026-10-26).** The closed-loop cases above
issue one call and wait for it; the pipelined cases issue a batch of 1/16/64
calls before waiting for any of them, which is the issue shape a throughput
caller uses. Every case warms up, runs three timed iterations and reports the
median, p99 and spread. `batch=1` is the same issue shape as the closed loop and
is the harness sanity check.

Reliable, median ns/call (payload x batch x concurrency; `hang` means the
pipelined case deadlocked and the watchdog recorded the closed-loop fallback,
see [distributed-testing.md §7](distributed-testing.md#s7)):

| Payload | batch | c=1 | c=2 | c=4 |
|---|---:|---:|---:|---:|
| 16 B | 1 | 88.1 µs | 75.0 µs | 80.4 µs |
| 16 B | 16 | 54.9 µs | 51.9 µs | hang (~0.009 M/s) |
| 16 B | 64 | 46.7 µs | 82.1 µs | hang (~0.010 M/s) |
| 256 B | 1 | 74.6 µs | 82.1 µs | 71.5 µs |
| 256 B | 16 | 45.2 µs | 72.7 µs | hang (~0.009 M/s) |
| 256 B | 64 | hang (~0.008 M/s) | hang (~0.009 M/s) | hang (~0.009 M/s) |

Unreliable `ONE_WAY`, median delivered M calls/s (16 B; all drained, 0 errors):

| batch | c=1 | c=2 | c=4 |
|---:|---:|---:|---:|
| 1 | 0.029 | 0.025 | 0.024 |
| 16 | 0.028 | 0.019 | 0.020 |
| 64 | 0.025 | 0.018 | 0.019 |

The reliable table above is a historical run from 2026-10-26, taken before the
pipelined deadlock was removed. A `hang` cell is the closed-loop fallback the
watchdog recorded when the completion path and the handle-destruction path took
`completion_mutex_` and `map_mutex_` in opposite orders (an ABBA). The deadlock
was a library defect, not a benchmark artefact: one lock order now applies
everywhere, and `tests/test_pipeline_deadlock.cpp` guards the high-batch
shapes with a watchdog (exit 42 on a hang). The suite is 2 cases / 6 checks /
0 failures on this tree, so the reliable path no longer hangs. A stack trace
from that run was captured and the limitation was recorded in
[distributed-testing.md §8](distributed-testing.md#s8). The `ONE_WAY` path
never waited on completion, so its pipelined numbers are unaffected.

**Redis client (isolated layers).** The pool case uses a null transport; the
pipeline case uses a wire-shaped executor (RESP encode + parse), so neither
includes a real server or a network.

| Case | Result |
|---|---:|
| connection_pool acquire/release | 792.4 ns/op, 0 idle, 0 error |
| RESP encode command (1 argument, 1 B wire) | 3275.4 ns/op |
| RESP parse bulk reply | 4085.4 ns/op |
| pipeline batch=1 | 15157.7 ns/batch, ~0.07 M cmds/s |
| pipeline batch=16 | 194894.4 ns/batch, ~0.08 M cmds/s |
| pipeline batch=64 | 763389.9 ns/batch, ~0.08 M cmds/s |

Log: `.opencode/tmp/distbench-redis.log`.

The distributed correctness suite is not a benchmark. It is recorded here only
so the numbers and their environment can be found together: two executables,
5 + 3 cases, 150 + 64 checks, 0 failures, each run five times with zero
failures. See [distributed-testing.md §3](distributed-testing.md#s3).

## 3. Budgets and targets <a id="s3"></a>

Memory budgets are from design §17.1; status is assessed against the code.

| Item | Budget | Status |
|---|---|---|
| idle connection | ≤ 2 KiB | design target; not measured |
| active connection | ≤ 160 KiB | design target; epoll ring is 64 KiB, IOCP send queue 256 KiB, read buffer 64 KiB |
| call slot | 128 B | **not met**: 440 B measured on MSVC x64 |
| shard fixed | ≤ 8 MiB | not applicable: no shard/coroutine pool yet |
| 100k in-flight calls | ≤ 64 MiB | design target based on 128 B slots; the current slot size alone is ~44 MB for 100k |
| 10k idle connections | ≤ 20 MiB | design target; not measured |
| subscription table | ≤ 5 MiB | design target; hub bounds are per-subscription, not a global table |
| per-lane queue | ≤ 16 KiB | specified, not implemented |
| send ring per connection | ≤ 256 KiB | epoll default 64 KiB; IOCP default 256 KiB |

Performance targets:

| Metric | Target | Status |
|---|---|---|
| aggregate throughput | 5M RPC/s | **not met**: 0.275M RPC/s best single-client measured (inproc, MSVC, depth 64); 0.773M RPC/s with eight client threads over four shards (§2.3); 0.024M over IOCP loopback |
| per-connection pair, batched | ≥ 1M RPC/s | **not met**: 0.275M RPC/s inproc measured (§2.3) |
| p99 latency | < 100 µs | **partly met**: 22.6 µs inproc (MSVC); 170.2 µs IOCP loopback; 151.8 µs WSL inproc |
| cross-shard handoff | 15–30 ns per batched message | budget; not measured |
| codec, 64-byte message | < 100 ns encode and decode | **not demonstrated**: `docs/benchmarks/2026-10-19-core-x64-msvc-release.txt` records `codec/binary_encode_small` 218.8 ns and `codec/binary_decode_small` 232.9 ns. The benchmark payload is a small `bench_request` (string/vector fields), not exactly 64 bytes, so the numbers are not directly comparable to the budget; they still show the target is not met. |
| resilience pipeline | 35–65 ns per successful call | budget; not measured |

The codec budget deserves a direct warning: the recorded microbenchmark above is
a real measurement, but it is 2.2-2.3x over the `< 100 ns` target and uses a
different payload than "64-byte message". It should not be read as a passing
result.

The only defensible statement today is: the transport layer is designed around
bounded queues and batching, and a raw Windows loopback probe shows that
batching changes throughput by roughly 50x. That motivates the design; it does
not prove the target.

## 4. Benchmark plan <a id="s4"></a>

The RPC benchmark now lands as `bench/bench_rpc.cpp` (§2.3). Its current cases
are 64 B sync latency, pipeline depth 1/16/64 and 1/2/4 shards over inproc, plus
IOCP loopback on Windows. The wider matrix is being filled in:

- `bench/bench_rpc_matrix.cpp` — implemented: 16 B/256 B/4 KiB/64 KiB payloads,
  1/2/4 caller threads, reliable closed-loop latency/throughput, unreliable
  `ONE_WAY` issue/delivery, and pipelined batch throughput (batch 1/16/64 x
  concurrency 1/2/4) with a child-process watchdog that falls back to the closed
  loop when the pipelined path deadlocks (§2.5).
- `bench/bench_compression.cpp` — implemented: none/lz4/deflate/lz77/zip/gzip
  over text and incompressible binary at 4 KiB/64 KiB (§2.5).
- `bench/bench_redis.cpp` — implemented: pool acquire/release, RESP encode/parse
  and pipeline batch throughput (§2.5).
- `bench/bench_rpc.cpp` — still to extend to 1 KiB, 1/8/64/512 connections,
  epoll loopback, and separate reliable/unreliable/ordered-lane runs.
Two planned benchmarks are **not implemented**, and no file of that
name exists in `bench/`; they are listed so the gap is visible rather
than implied to be covered:

- HTTP/1.1 and HTTP/2 request/response, keep-alive, header sizes,
  upload/download streaming.
- Codec throughput for crpc-binary, JSON, XML, INI, TOML and the schema
  formats at 64 B, 256 B and 1 KiB with a warm cache.

The codec numbers quoted above come from `bench/bench_core.cpp`
(`codec/binary_encode_small` and the `crc32c` cases), not from a
dedicated codec benchmark.

The run commands, substring filters and scenario interpretation for the three
implemented benchmarks are in
[distributed-testing.md §5](distributed-testing.md#s5).

Every run reports the fields in §1. The counter is incremented by the client
after a response is decoded, never by the server after it writes bytes. CPU
utilisation is sampled separately; syscalls per message are counted with the
platform tracer when available. In-process results are reported as an upper
bound for the framework, not as a network result. A benchmark that changes the
assertions or skips the conformance path is invalid (P1).

The benchmark matrix is a release artefact, not a merge gate for correctness.
The design's release gate requires performance to be measured and reported
honestly; it does not require 5M RPC/s on every host (O13).

## 5. Anti-patterns the design rejects <a id="s5"></a>

| Anti-pattern | Why it is rejected | What CRPC does instead |
|---|---|---|
| Unbounded send/receive queues | memory grows with a slow peer; latency becomes unbounded | bounded rings/queues; `resource_exhausted`, parking or an explicit overflow policy |
| Fixed huge buffers (bufferbloat) | queues grow, RTT rises, tail latency explodes | 64 KiB read/write defaults; grow on demand; 256 KiB/turn write budget; pacer burst 2 ms |
| Retry as congestion recovery | a retry storm turns overload into collapse | retry budget (10%), 3 attempts, jitter, exponential cap; breaker-open skips retries |
| Loss as the only congestion signal | loss appears after queues are already full | RTT baseline and queue delay drive the window; loss is not the sole signal |
| Wall-clock RTT | a clock jump looks like a path change | injected monotonic clocks; stale/backwards samples rejected |
| Timer-driven window growth | sends more when the path is silent | window grows only on ACKs (self-clocked) |
| “Path is clear” detector | it always detects clearance and oscillates | removed; only sustained directional evidence moves the baseline |
| Silent millisecond fallback | hides a broken timer and breaks p99 | epoll fails closed if `timerfd` is unavailable |
| One syscall and one wakeup per message | measured 0.02M vs 1.05M RPC/s in the audit probe | batching, send rings, completion batches |
| Deriving RPC/s from frames per RTT | measures an imaginary pipeline | independent counter, end-to-end latency |

## 6. Reporting and acceptance <a id="s6"></a>

A performance report is accepted only when it includes the benchmark source,
the command line, the log/artifact path, the hardware and toolchain, and all
fields in §1. It must state which backends were measured and which code paths
were disabled (sanitizers, tracing, logging). If a target is missed, the report
states the measured value and the gap; it does not replace a missing measurement
with a projection.

Current status: the library has functional test coverage, an HTTP server facade,
an in-process and IOCP end-to-end RPC benchmark (§2.3), a wire/codec
microbenchmark, and the compression/RPC-matrix/Redis benchmarks in §2.5. The 5M
RPC/s and IOCP p99 targets are **not met** (measured
0.275M RPC/s inproc single-client, 0.773M RPC/s multi-thread, and 170 µs p99
over IOCP); the in-process p99 target is met
at 22.6 µs on MSVC. The 128-byte call-slot budget remains **unmet** at 440 bytes
on the measured toolchain. The measured figures, the rejected experiments and
the remaining gaps are recorded in §2 and §3; the shipped microbenchmark log is
`docs/benchmarks/2026-10-19-core-x64-msvc-release.txt`.
