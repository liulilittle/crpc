# CRPC performance

[简体中文](performance.zh-CN.md) | **English**

Version 1.0 / 2026-10-19

This document states what has actually been measured in the repository, on what
hardware and toolchain, what is only a budget, and how the library will be
benchmarked. A target is not a result. Numbers that do not exist are marked as
absent rather than estimated.

## Contents

| # | Section |
|---|---|
| [1](#s1) | Measurement method |
| [2](#s2) | Recorded evidence |
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

**There is no library benchmark in the repository.** `bench/` is empty (no
`CMakeLists.txt`, no `bench_rpc.cpp`, `bench_http.cpp` or `bench_codec.cpp`),
`docs/benchmarks/` does not exist, and `.opencode/` contains no benchmark result
file. `CRPC_BUILD_BENCH` is therefore off, and the numbers in this section are
not CRPC throughput.

Two records do exist:

1. **Raw loopback probe** in `.opencode/audit-arch-a.md` §12.2. Hardware: AMD
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

No p99, p999, cross-shard handoff, codec or resilience-pipeline measurement
exists. Any such number in a report today would be invented.

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
| aggregate throughput | 5M RPC/s | target, explicitly not a gate (R10) |
| per-connection pair, batched | ≥ 1M RPC/s | 1.05M on the raw audit probe, not CRPC |
| p99 latency | < 100 µs | target; must be measured end to end |
| cross-shard handoff | 15–30 ns per batched message | budget; shard model not implemented |
| codec, 64-byte message | < 100 ns encode and decode | budget; not measured |
| resilience pipeline | 35–65 ns per successful call | budget; pipeline not implemented |

The only defensible statement today is: the transport layer is designed around
bounded queues and batching, and a raw Windows loopback probe shows that
batching changes throughput by roughly 50x. That motivates the design; it does
not prove the target.

## 4. Benchmark plan <a id="s4"></a>

When the benchmark targets land, they will be:

- `bench/bench_rpc.cpp` — unary calls over inproc, epoll loopback and IOCP
  loopback; request/response sizes 16 B, 64 B, 256 B, 1 KiB; 1, 8, 64, 512
  connections; batching 1 and 64; shard count = physical cores − 1; separate
  runs for reliable calls, unreliable one-way and ordered lanes.
- `bench/bench_http.cpp` — HTTP/1.1 and HTTP/2 request/response, keep-alive,
  header sizes, upload/download streaming.
- `bench/bench_codec.cpp` — crpc-binary, JSON, XML, INI, TOML and the schema
  formats at 64 B, 256 B and 1 KiB, with a warm cache and no allocation on the
  measured path.

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

Current status: the library has functional test coverage and a raw transport
probe, but no end-to-end benchmark. The benchmark plan above is the next
measurement step; until it runs, the 5M RPC/s and p99 targets remain
**unverified design targets**, and the 128-byte call-slot budget remains
**unmet** at 440 bytes on the measured toolchain.
