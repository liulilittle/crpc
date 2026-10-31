# Distributed testing and benchmarks

[简体中文](distributed-testing.zh-CN.md) | **English**

Version 1.0 / 2026-10-26

This document records how the multi-node/distributed test suite and the
performance benchmarks are built, run and read. The numbers were all measured in one Debug build on a single development
machine, with the in-process backend unless stated otherwise.

## Contents

| # | Section |
|---|---|
| [1](#s1) | What is covered |
| [2](#s2) | Build |
| [3](#s3) | Run the distributed tests |
| [4](#s4) | Scenarios and what each proves |
| [5](#s5) | Run the benchmarks |
| [6](#s6) | Recorded results |
| [7](#s7) | Reading the results |
| [8](#s8) | Limitations |

## 1. What is covered <a id="s1"></a>

| File | Target | Coverage |
|---|---|---|
| `tests/test_distributed_rpc.cpp` | `crpc_test_distributed_rpc` | Multi-node discovery and route convergence, provider removal and failover, health-driven eviction/recovery, transport partition plus idempotent replay, registry partition recovery and watch replay/out-of-order de-duplication. |
| `tests/test_distributed_push.cpp` | `crpc_test_distributed_push` | Three-node two-hop application-level relay push, `PUSH_ACK`, no replay after a fully acknowledged reconnect, middle-hop partition recovery, unreliable delivery without ACK. |
| `tests/test_distributed_tcp.cpp` | `crpc_test_distributed_tcp` | The same scenarios over a real TCP transport (IOCP on Windows, epoll on Linux) with at least three nodes on `127.0.0.1`: discovery and route convergence, failover around an unreachable endpoint, health-driven eviction/recovery, idempotent replay, three-node two-hop push with `PUSH_ACK`, and middle-hop partition recovery. |
| `bench/bench_compression.cpp` | `crpc_bench_compression` | none/lz4/deflate/lz77/zip/gzip x text/incompressible binary x 4 KiB/64 KiB: ratio and encode/decode throughput. |
| `bench/bench_rpc_matrix.cpp` | `crpc_bench_rpc_matrix` | Reliable RPC latency, reliable closed-loop throughput (payload x concurrency), unreliable `ONE_WAY` issue/delivery throughput, and pipelined batch throughput (batch 1/16/64 x concurrency 1/2/4, reliable and `ONE_WAY`). |
| `bench/bench_redis.cpp` | `crpc_bench_redis` | Redis pool acquire/release, RESP command encode/reply parse, pipeline batch throughput. |

The distributed tests are deterministic: they use the in-process backend (the
original two suites) or real loopback TCP with dynamic ports and sync points
(the TCP suite), so a failure is reproducible without a live cluster. They are
correctness tests, not network-capacity measurements.

## 2. Build <a id="s2"></a>

```powershell
$env:CRPC_RTTI_DIR   = 'F:\RTTI'
$env:CRPC_COROCC_DIR = 'F:\corocc'
cmake -S . -B .opencode/tmp/out-distbench -G Ninja `
      -DCMAKE_BUILD_TYPE=Debug -DCRPC_WARNINGS_AS_ERRORS=ON `
      -DCRPC_BUILD_TESTS=ON -DCRPC_BUILD_BENCH=ON `
      -DCRPC_RTTI_DIR=F:\RTTI -DCRPC_COROCC_DIR=F:\corocc
cmake --build .opencode/tmp/out-distbench --parallel 3
```

The build is a C0 gate: `/W4 /WX` (MSVC) or `-Wall -Wextra -Wpedantic -Werror`
(GCC/Clang) with zero errors and zero warnings. Benchmarks are built only with
`CRPC_BUILD_BENCH=ON`; they are not CTest entries.

## 3. Run the distributed tests <a id="s3"></a>

```powershell
ctest --test-dir .opencode/tmp/out-distbench `
      -R crpc_test_distributed --output-on-failure
```

Recorded result:

```text
1/3 Test #11: crpc_test_distributed_push ....... Passed 0.03 sec
2/3 Test #12: crpc_test_distributed_rpc ........ Passed 0.38 sec
3/3 Test #13: crpc_test_distributed_tcp ........ Passed 4.75 sec
100% tests passed, 0 tests failed out of 3
```

Direct execution prints the per-case counters:

```text
crpc_test_distributed_rpc.exe:  5 case(s), 150 check(s), 0 failure(s)
crpc_test_distributed_push.exe: 3 case(s),  64 check(s), 0 failure(s)
crpc_test_distributed_tcp.exe:  6 case(s), 185 check(s), 0 failure(s)
```

Stability: each binary was run five consecutive times with zero failures. The
TCP binary was also run 10 additional times (and 10 per-case runs for the
crash-prone cases) with zero crashes after the pump-drain teardown described in
§8.

To run a single case, use the test framework's filter (the executable accepts
the case-name substring as its argument):

```powershell
.\.opencode\tmp\out-distbench\crpc_test_distributed_rpc.exe partition
```

## 4. Scenarios and what each proves <a id="s4"></a>

### 4.1 Discovery and route convergence

Three providers register in `cluster::registry`; the caller builds a `client`
from a `passing_only` query. Thirty calls cover all three nodes. After a fourth
provider joins, the old route never hits the new node in 30 calls; a discovery
refresh makes 40 calls cover all four. After one provider is stopped and
deregistered:

- **Red:** the stale route still contains the dead node, so 60 calls fail
  (`CRPC_CHECK(stale_failures > 0)`, `tests/test_distributed_rpc.cpp:503`).
- **Green:** after a discovery refresh the route contains only the three live
  nodes, 60 calls have **0 failures**, and the dead node is never selected
  (`tests/test_distributed_rpc.cpp:524`).

The red/green pair is the proof that discovery, not luck, changes the route.

### 4.2 Failover

Two providers register; one is stopped but the registry is not yet updated.
Twenty calls all succeed and are answered by the live node; the stopped node's
handler count is 0. The facade observes pre-send retry/breaker skip
(`tests/test_distributed_rpc.cpp:582`).

### 4.3 Partition plus idempotent replay

The server handler writes its idempotency state, then waits on a test latch; the
caller closes the connection before the response (a deterministic partition
window). The first call fails, but the server's `side_effects == 1`
(`tests/test_distributed_rpc.cpp:687-688`). After reconnecting, replaying the
same `req-1|v1` returns `v1` and `side_effects` is still 1 (`:701`). Sending a
new `req-2|v2` and replaying `req-1`/`req-2` out of order leaves exactly two
side effects and the final state `req-1=v1\nreq-2=v2\n` (`:712` onward).

This is the "at-most-once effect despite retry" property; it is not a promise
that a request is never executed twice at the transport level.

### 4.4 Registry partition recovery and watch replay

A node that returns with incarnation 1 after the authority moved to
incarnation 2 is rejected with `failed_precondition`
(`tests/test_distributed_rpc.cpp:762`). Rejoining with incarnation 2 converges
to three nodes, one instance each (`:770`). A watch that receives duplicate,
old and current indices dispatches only the two deltas; after resync the final
snapshot has three instances (`:812` onward).

### 4.5 Cross-node push

Topology: `server A --relay 1--> server B --relay 2--> server C --> subscriber`.

- Three reliable messages arrive at C in order with `seq = 1/2/3`.
- A, B and C each report `push_acks >= 3`
  (`tests/test_distributed_push.cpp:333-337`).
- After the subscriber fully acknowledges and reconnects, nothing is replayed;
  a new message is `seq = 4` (`:353`, `:364`).
- After the middle relay disconnects, messages published at A queue in B's hub.
  Reconnecting the relay with the same subscriber id replays exactly once; C
  receives `before-partition` and `during-partition` with `seq = 1/2`
  (`:434-437`).
- On the unreliable path a delivered message has `seq = 0` and A/B have
  `push_acks == 0` (`:486-487`).

The library's `push::hub` is single-shard authority; the relay itself is
application-level code in the test, not a built-in cross-node replication
feature.

### 4.6 TCP transport

`tests/test_distributed_tcp.cpp` repeats the scenarios over a real TCP backend
with at least three nodes, all in one process, each node on a distinct
`127.0.0.1` port from a per-process counter (the listener bind is retried, so no
fixed port is required). The registry authority is the same in-process catalog
as the in-process suites; what changes is that every discovered endpoint is a
real listener and every call crosses a socket.

- **Discovery and route convergence.** Three providers register; 30 calls cover
  all three through the facade's round-robin balancer. A fourth joins, the old
  route never hits it, a refreshed route covers all four. After one provider is
  stopped and deregistered, a fresh client built from the stale route fails
  (`CRPC_CHECK(stale_failures > 0)`, `tests/test_distributed_tcp.cpp`), while
  a refreshed route has **0 failures** over 60 calls. The stale client is built
  fresh from the stale table and the listener is waited on, so the failure is a
  fast `connection_refused` rather than a timeout.
- **Failover.** The route contains one live provider and one unreachable
  endpoint (an in-process endpoint that the TCP backend rejects immediately).
  Twenty calls all succeed on the live provider (`pre_send_retries >= 1`), which
  proves the facade routes around an endpoint it cannot connect to. A real
  recently-closed TCP port can take ~2s to be refused on this host, which would
  dominate the test; the in-process endpoint makes the timing deterministic.
  The real listener-teardown path is covered by the discovery case.
- **Health-driven eviction/recovery.** The callback probe drives
  `health_monitor`; a failing probe turns the registry state critical, discovery
  drops to one node, two consecutive passes restore it, discovery returns to two.
- **Idempotent replay.** The same `req-1|v1` is sent twice over TCP; the handler
  deduplicates it, so `side_effects` stays 1 and the replay returns `v1`. An
  out-of-order duplicate of `req-2` does not re-apply it either. The
  transport-partition variant stays in the in-process suite (see §8).
- **Three-node push.** Topology `A --relay 1--> B --relay 2--> C --> subscriber`
  over TCP: three messages arrive at C in order with `seq = 1/2/3`, A/B/C each
  report `push_acks >= 3`, and after a fully acknowledged reconnect nothing is
  replayed and the next message is `seq = 4`.
- **Middle-hop partition.** After the middle relay stops, the server has
  detached the bridge before the message is published (`push_active_connections
  == 0`); the message queues in B's hub and is replayed exactly once when the
  relay reconnects with the same subscriber id. C receives
  `before-partition`/`during-partition` with `seq = 1/2`.

## 5. Run the benchmarks <a id="s5"></a>

```powershell
Set-Location .opencode/tmp/out-distbench
.\crpc_bench_rpc_matrix.exe
.\crpc_bench_compression.exe
.\crpc_bench_redis.exe
```

Each benchmark accepts a substring filter as its first argument:

```powershell
.\crpc_bench_rpc_matrix.exe latency
.\crpc_bench_compression.exe text_4096
.\crpc_bench_redis.exe redis/pipeline
.\crpc_bench_rpc_matrix.exe pipeline
```

`CRPC_BENCH_RPC_CALLS` overrides the per-case RPC call count, so a slow machine
can reproduce one case with a smaller workload:

```powershell
$env:CRPC_BENCH_RPC_CALLS = '200'
.\crpc_bench_rpc_matrix.exe latency
```

The `pipeline` filter runs the pipelined batch cases. Each one runs in a child
process with a watchdog timeout, because the reliable pipelined path can deadlock
in a Debug build; a case that exceeds the timeout is killed and the harness
records the closed-loop fallback for it. The watchdog is implemented for
Windows (`CreateProcess`); on other platforms the pipelined case runs in-process
and the timeout does not apply.

Every case warms up, runs several times and reports the median; the harness also
prints the compiler, build type and architecture. Benchmarks are not part of the
CI pass/fail gate.

## 6. Recorded results <a id="s6"></a>

Environment: MSVC x64 **Debug**, AMD Ryzen 9 7945HX (16 cores / 32 threads),
single-process `inproc_backend`, no network, no sanitizer. Debug numbers are for
same-environment regression comparison only; they do not represent Release or
production capacity.

### 6.1 Reliable RPC latency (closed loop, depth 1; 5 x 2000 samples, median)

| Payload | p50 | p99 |
|---|---:|---:|
| 16 B | 64.0 µs | 177.1 µs |
| 256 B | 59.3 µs | 174.8 µs |
| 4 KiB | 67.1 µs | 194.8 µs |

### 6.2 Reliable RPC throughput (closed loop, payload x concurrency, 0 errors)

| Payload | c=1 | c=4 |
|---|---:|---:|
| 16 B | 83.1 µs/call (~12.0k/s) | 96.9 µs/call (~10.3k/s) |
| 256 B | 113.9 µs/call (~8.8k/s) | 85.8 µs/call (~11.7k/s) |
| 4 KiB | 123.9 µs/call (~8.1k/s) | 99.1 µs/call (~10.1k/s) |
| 64 KiB | 265.1 µs/call (~3.8k/s) | 272.5 µs/call (~3.7k/s) |

### 6.3 Unreliable `ONE_WAY` issue/delivery (0 errors, drained = yes)

| Payload | c=1 issued/delivered | c=4 issued/delivered |
|---|---:|---:|
| 16 B | 10000 / 10000, 0.4894 s (~20.4k/s) | 10000 / 10000, 0.5816 s (~17.2k/s) |
| 4 KiB | 4000 / 4000, 0.2004 s (~20.0k/s) | 4000 / 4000, 0.2665 s (~15.0k/s) |

### 6.4 Frame compression

Text payload ratio (wire/plain):

| Algorithm | 4 KiB | 64 KiB |
|---|---:|---:|
| none | 1.004 | 1.000 |
| lz4 | 0.026 | 0.005 |
| deflate | 0.027 | 0.004 |
| lz77 | 0.048 | 0.025 |
| zip | 0.034 | 0.005 |
| gzip | 0.031 | 0.005 |

Text throughput examples (Debug):

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

### 6.5 Redis client

| Case | Result |
|---|---:|
| connection_pool acquire/release | 792.4 ns/op, 0 idle, 0 error |
| RESP encode command (1 argument, 1 B wire) | 3275.4 ns/op |
| RESP parse bulk reply | 4085.4 ns/op |
| pipeline batch=1 | 15157.7 ns/batch, ~0.07 M cmds/s |
| pipeline batch=16 | 194894.4 ns/batch, ~0.08 M cmds/s |
| pipeline batch=64 | 763389.9 ns/batch, ~0.08 M cmds/s |

Log: `.opencode/tmp/distbench-redis.log`.

### 6.6 Pipelined batch throughput (Debug)

Reliable, median ns/call (payload x batch x concurrency; 3 rounds x 10000 calls).
`hang` means the pipelined case deadlocked and the watchdog recorded the
closed-loop fallback:

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

Log: `.opencode/tmp/bench-pipeline-watchdog2.log`. The hang is timing-dependent,
so the exact set of hanging cases varies between runs; the fallback is always
recorded explicitly.

## 7. Reading the results <a id="s7"></a>

- **The distributed tests are the correctness evidence.** The red/green pairs
  show that the behavior changes when the mechanism is removed or the stale
  state is exercised; a green run alone would not prove that.
- **The RPC/one-way numbers are framework upper bounds.** They run over the
  in-process backend, so they exclude sockets, TLS and the kernel. Use them to
  compare changes in the same environment, not to plan network capacity.
- **Compression ratios are data-dependent.** The text payload is repeated log
  lines; a real body will land somewhere else. The incompressible case is the
  important control: it proves the sender falls back to `none` instead of
  inflating the frame.
- **gzip/deflate/zip are slow in Debug.** The DEFLATE encoder is not optimized
  in this build (about 5 MiB/s). Do not read the Debug encode rate as a Release
  rate. Decode is much faster than encode here, and lz4 is the low-latency
  choice.
- **The Redis numbers isolate layers.** The pool case uses a null transport, so
  it measures pool bookkeeping, not Redis. The pipeline case uses a wire-shaped
  executor, so it includes RESP encode/parse but not a server or network.
- **The reliable pipelined path deadlocks in Debug.** The concurrent issue path
  and the in-process backend's synchronous completion path contend on the client's
  completion lock while a server handler coroutine holds it, so a high batch x
  concurrency case stops making progress. The harness runs each pipelined case in
  a child process with a watchdog timeout and records the closed-loop fallback
  when it hangs; the fallback is never presented as a pipelined number. The
  `ONE_WAY` path does not deadlock because its issue path does not wait.
- **Debug vs Release.** A number without its build type is not comparable. The
  recorded tables are Debug; re-run the same command after a Release build for a
  Release claim.

## 8. Limitations <a id="s8"></a>

1. **Single-process catalog; real TCP transport.** The original two distributed
   suites use `inproc_backend`; `tests/test_distributed_tcp.cpp` now repeats the
   same scenarios over a real TCP transport (IOCP on Windows, epoll on Linux)
   with at least three nodes, so real TCP multi-node behavior **is** covered.
   Two things are still not covered: the TCP suite runs one process with several
   TCP listeners rather than separate OS processes, and the `cluster::registry`
   authority is the same in-process catalog the other suites use, so a
   distributed catalog or a cross-host deployment is not exercised. The TCP
   suite also inherits a known IOCP backend race: closing a TCP client mid-send
   can trip a use-after-free in `handle_send_complete` (documented in
   `src/io/iocp_backend.cpp`). The suite therefore drains the backend pump with
   `backend->stop()` before teardown and keeps the transport-partition scenario
   in the in-process suite, where the backend closes connections
   synchronously.
2. **Cross-node push is application-level relay.** The library's `push::hub` is
   single-shard authority and has no built-in cross-node replication/forwarding.
   The test subscribes a relay to the upstream and publishes to the downstream;
   this is not a library feature.
3. **RPC throughput has a closed-loop baseline and a pipelined extension.** The
   completion race that once made pipelined batches plus immediate facade-handle
   destruction unreliable is fixed: `call_slot::invoke_callback()` claims the
   callback exactly once via `callback_armed.exchange`, and `release_pending()`
   synchronizes handle destruction with any in-flight callback. The fix is
   covered by `facade_handle_destroy_races_completion` in
   `tests/test_server_client.cpp`. The closed-loop cases are kept as the
   reproducible baseline, and the pipelined batch cases (§6.6) add the issue
   shape a throughput caller uses; the library source is not changed by either
   choice.
4. **The reliable pipelined path deadlocks in Debug at high batch x
   concurrency.** The concurrent issue path and the in-process backend's
   synchronous completion path deadlock (see §7); the harness detects it with a
   child-process watchdog and records the closed-loop fallback. This is a library
   finding for the parent, not a benchmark artefact; a stack trace was
   captured with those measurements.
5. **Debug numbers.** Compression encode throughput in particular is not
   representative; see §7.
5. **`none` decode is not decompression.** Its "decode throughput" is header
   parsing plus payload borrowing, used only as a baseline.
6. **Incompressible data falls back to `none`.** A ratio of 1.0 is the correct
   fail-safe behavior, not a codec failure.

## 9. See also <a id="s9"></a>

- [Performance report §2.5](performance.md#s2-5) — the same numbers in the
  performance document's evidence section.
- [Wire protocol §24](protocol.md#s24) — frame compression format and selection.
- [Redis client](redis-client.md) — API map and build/test commands for the
  Redis suites.
