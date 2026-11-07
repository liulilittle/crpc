# CRPC 性能

[English](performance.md) | **简体中文**

版本 2.1 / 2026-11-07

本文说明仓库中真正测过什么、在什么硬件与工具链上测的、哪些只是预算，以及库将如何基准测试。目标是目标，
不是结果。不存在的数字会明确标为缺失，而不是估算。

## 目录

| # | 章节 |
|---|---|
| [1](#s1) | 测量方法 |
| [2](#s2) | 已记录证据 |
| [2.5](#s2-5) | 压缩、RPC 矩阵与 Redis 基准（2026-10-26，历史） |
| [2.6](#s2-6) | 重测基线（2026-11-07） |
| [3](#s3) | 预算与目标 |
| [4](#s4) | 基准计划 |
| [5](#s5) | 设计拒绝的反模式 |
| [6](#s6) | 报告与验收 |

## 1. 测量方法 <a id="s1"></a>

性能声明只有在同时具备以下信息时才有效：

- 测的是什么（RPC unary、HTTP 请求、编解码操作、裸 socket）；
- 消息大小、连接数、线程/分片数与批处理方式；
- 精确工具链与优化级别（编译器版本、`-O2`/`-O3`、MSVC `/O2`、关闭消毒器）；
- CPU 型号、核数、操作系统，以及 loopback 或真实网络路径；
- 指标：来自**独立请求计数器**的吞吐，加上 p50/p99/p999 延迟、CPU
  利用率与每消息系统调用数；
- 预热与运行次数，并报告中位数；
- 基准源码与命令行。

设计的测量协议是预热 10 秒、5 次 30 秒运行、取 5 次中位数。结果不得通过“批大小除以
RTT”或把“每 RTT 64 帧”换算成 RPC/s 得出（设计 R10）。这些计算描述的是内核路径，
不是应用调用速率。

## 2. 已记录证据 <a id="s2"></a>

`bench/bench_rpc.cpp` 现已存在（目标 `crpc_bench_rpc`，由 `CRPC_BUILD_BENCH`
门控），在进程内后端上测量真实 client 门面与 dispatcher，并在 Windows 上测量
IOCP TCP loopback（§2.3）。`bench/bench_core.cpp` 覆盖线格式/编解码微基准。
下面的裸 loopback 探针与对象布局探针早于该基准，作为历史保留。

**历史章节。** §2.3（2026-10-19）、§2.4（2026-10-24）与 §2.5（2026-10-26）
保留用于对比。它们记录于流水线死锁修复（59936b6）之前、阶段 5 工作树改动之前；
当前基线是 §2.6（2026-11-07）。

1. **裸 loopback 探针**，记录于本节。硬件：AMD Ryzen 9 7945HX，16 核 / 32 线程。工具链：MSVC
   2022、`/O2`、Windows loopback。负载：16 字节帧、阻塞 socket 回显、
   1,000,000 次 RPC、一条连接、一个线程对，不含协程、编解码或多路复用。

   ```
   per_send=1   rpc=1000000 time=61.905s -> 0.02 M RPC/s
   per_send=64  rpc=1000000 time=0.949s  -> 1.05 M RPC/s
   ```

   50 倍差距来自“每消息一次 send+recv”与“每 64 条批量”的系统调用/唤醒成本差。
这是内核路径上界，不是 CRPC 测量。

2. **对象布局探针**，为本文使用 MSVC 2022 x64 编译，
   `cl /nologo /std:c++17 /EHsc /I include`：

   | 类型 | `sizeof` |
   |---|---|
   | `rpc::call_slot` | 440 字节 |
   | `rpc::call<int>` | 24 字节 |
   | `rpc::client` | 568 字节 |
   | `push::hub` | 8 字节（pimpl 指针） |
   | `cluster::registry` | 336 字节 |
   | `cluster::lease_manager` | 184 字节 |

   这是布局，不是速度。它说明设计中的 128
字节调用槽预算未被当前实现满足（`std::mutex`、`condition_variable`、
缓冲区、`rpc_error` 与回调存储占主导）。

### 2.3 端到端 RPC 实测（2026-10-19，历史）

源码：`bench/bench_rpc.cpp`；命令：`crpc_bench_rpc`（Windows MSVC Release）与
在 WSL 下构建的同一目标（GCC Release）。
负载：64 字节 `std::string` 回显，经注册的 `bench.echo` 方法，`call_codec<std::string>`
（crpc-binary 路径）编解码，单客户端线程，进程内后端（Windows 上另有 IOCP TCP loopback）。
机器：AMD Ryzen 9 7945HX，32 逻辑核。编译器：MSVC 19.44 x64 Release；WSL2 内
GCC 13.3 Release（内核 6.18）。三次运行取中位。

| 用例 | depth | MSVC inproc | GCC/WSL inproc | MSVC IOCP loopback |
|---|---:|---|---|---|
| 同步时延 p50 | 1 | 2.80 µs | 60.4 µs | 86.2 µs |
| 同步时延 p90 | 1 | 4.20 µs | 95.8 µs | 123.8 µs |
| 同步时延 p99 | 1 | **22.6 µs** | 151.8 µs | 170.2 µs |
| 同步时延 p999 | 1 | 49.0 µs | 286 µs | 343.7 µs |
| 同步时延 max | 1 | 269 µs | 4.30 ms | 4.35 ms |
| 吞吐 | 1 | 0.157 M RPC/s | 0.0085 M RPC/s | 0.013 M RPC/s |
| 吞吐 | 16 | 0.241 M RPC/s | 0.034 M RPC/s | 0.024 M RPC/s |
| 吞吐 | 64 | 0.275 M RPC/s | 0.076 M RPC/s | 未运行 |
| 多分片 1/2/4 | 16 | 0.217 / 0.196 / 0.193 M | 0.0275 / 0.0274 / 0.0275 M | 不适用 |

尾延迟：唤醒路径修复后 ≈100 ms 停顿消失。MSVC 进程内 5 万样本最大值为 168–624 µs
（中位 269 µs），WSL 最大 2.6–14.5 ms（中位 4.3 ms）。MSVC IOCP 最大值波动较大：
空闲机三次 2 万样本为 0.54–1.36 ms，负载下八次为 1.2–27 ms，而 p99（159–169 µs）与
p999（237–295 µs）保持平稳。仅最大值离群而 p99/p999 平稳，属调度/OS 噪声而非分布
整体右移，故如实记为噪声，不记为尾延迟回归。

瓶颈证据（实测，非推断）。MSVC Release 单点成本：`make_shared<call_state<std::string>>` 52 ns、
每连接 `std::recursive_mutex` 8 ns、string 编解码合计 17 ns。深度 64、20 万次调用的阶段拆分：
issue 2612 ns + wait 934 ns；仅 0.6% 的调用在发起时已完成，说明时间花在往返本身而非簿记。
多客户端线程对多分片：1/2/4/8 线程 → 0.227/0.382/0.526/0.773 M RPC/s（1→8 为 3.4×，约 0.8 M 饱和）。
当前设计的实测天花板约 1 M RPC/s 每进程；距 5 M 目标还差 5–14×，需要消除往返（批量合并或
客户端分片亲和），而不是调分配与锁。目标未达成（见 §3）。

两个看似合理的优化已实测否定，记录在此以免重复：

| 实验 | 改动 | 结果 | 结论 |
|---|---|---|---|
| 协程栈池化 | `runtime_options::handler_stack_bytes = 0`（复用 corocc 默认 256 KiB 栈池，而非每次 spawn 映射 64 KiB） | 深度 1/16/64 = 0.11/0.18/~0.20 M RPC/s，与默认值在噪声内一致 | 非瓶颈；已回滚 |
| 移除 deadline 定时器 | 基准中 `default_deadline(0 ms)`（无每调用 arm/cancel） | 深度 64 由 0.19 → 0.23 M RPC/s | 非瓶颈；已回滚 |

两者均已回滚，实测天花板不变。每调用成本是同步往返（深度 64 时 issue 2612 ns + wait 934 ns），
因此 5 M 目标的差距不在分配与定时器。

在本文之前不存在 p99/p999、跨分片交接、编解码或韧性流水线实测；上文 p99/p999 现已实测，跨分片交接与韧性流水线预算仍未测量。

### 2.4 CRC32C 与推送 hub 统计（2026-10-24，历史） <a id="s2-4"></a>

源码：`bench/bench_core.cpp`（`crpc_bench_core crc32c`）。工具链：MSVC 19.51
x64 Release、`NDEBUG`、关闭消毒器。机器：AMD Ryzen 9 7945HX，16 核 /
32 线程。丢弃一次预热后取七次运行中位数。

`frame.cpp` 原先通过 256 项表逐字节计算 CRC32C。现在以 slicing-by-8（八张
交错表）作为可移植路径，并在 x64 MSVC 且 CPUID 报告 SSE4.2 时使用硬件
`crc32` 指令；选择在运行时只做一次，因此没有 SSE4.2 的 CPU 仍走可移植路径。

| 用例 | 字节循环（前） | slicing-by-8 | `crc32c()`（硬件） |
|---|---:|---:|---:|
| 256 B | 504.2 ns | 125.6 ns | **58.5 ns** |
| 16 KiB | 32 490.3 ns | 8 264.7 ns | **3 683.6 ns** |

相对字节循环的实测加速为 8.6×（256 B）与 8.8×（16 KiB）。`crc32c()` 列比
同一输入上实测的本地 slicing-by-8 参考更快，确认本 CPU 上选择了硬件路径。
正确性由 `tests/test_frame.cpp` 覆盖：已知 CRC32C 向量，加上对 0..2048 每个
长度以及若干至 70 000 字节大小、对照独立逐字节参考的交叉校验。

推送 hub 的 `stats()` 不再扫描每个订阅者与订阅。服务端在每次 PUSH_ACK 时
读取它（`refresh_push_stats_locked`），因此旧的 O(订阅者 × 订阅) 扫描会在每次
确认时执行。现在在每次队列/订阅变更处增量维护四个计数器，`stats()` 为
O(1)；`tests/test_push.cpp` 走遍每条变更路径（subscribe、publish、overflow、
retransmit、front replay、ack、take、detach、release、unsubscribe、
disconnect），并把聚合值与全量扫描会得到的结果对比。

上述两处热路径拷贝现已移除。帧解码器新增 `feed_external()`/`drain_external()`：
`feed_external()` 借用调用方的接收块并直接从中解码完整帧，因此在借用模式下返回的
负载指向该块本身，省去了每块一次的拷贝；`drain_external()` 只把未完成一帧的尾部
字节拷贝回内部缓冲区。TLS 写侧的 `push()` 现在借用调用方的 span，仅在写入停留
（park）时把未写完的剩余部分物化到 `write_buffer_`，因此常见的同步写入不产生拷贝，
调用方在 `push()` 返回后即可复用该 span。`feed()` 的复制契约（调用方 span 不必
活过本次调用）与 `push()` 的返回后可复用契约保持不变；`feed_external()` 是面向
「在块失效前消费完每一帧」调用方的零拷贝形式。`feed_external()` 契约见
[protocol §15](protocol.zh-CN.md#s15)；两条路径分别由 `tests/test_frame.cpp`
（`frame_decoder_external_*`）与 `tests/test_tls_write_borrow.cpp` 覆盖。

| 用例 | `feed()` | `feed_external()` | 加速 |
|---|---:|---:|---:|
| 1 KiB 帧 | 56.0 ns/帧 | **16.4 ns/帧** | 3.4× |
| 64 帧批 | 44.9 ns/帧 | **10.5 ns/帧** | 4.3× |
| 负载拷贝 | 1.00 | **0.00** | — |

对应用例为基准记录 `frame/feed_copy_1KiB_per_frame` 与
`frame/feed_external_1KiB_per_frame`、`*_batch64_per_frame` 一对，以及
`frame/feed_*_payload_copies`。「负载拷贝」行是确定性的指针校验而非计时：
`feed()` 的负载指向解码器内部，`feed_external()` 的负载就是块自身字节。
`frame_decoder_external_matches_feed` 在每一种切分点（包括在一帧中间切分）都
验证产生相同的帧流。

### 2.5 压缩、RPC 矩阵与 Redis 基准（2026-10-26，历史） <a id="s2-5"></a>

源码：`bench/bench_compression.cpp`、`bench/bench_rpc_matrix.cpp` 与
`bench/bench_redis.cpp`（目标 `crpc_bench_compression`、
`crpc_bench_rpc_matrix`、`crpc_bench_redis`）。工具链：MSVC x64 **Debug**
（`/W4 /WX`）、AMD Ryzen 9 7945HX（16 核 / 32 线程）、单进程
`inproc_backend`、无网络、无消毒器。每个用例先预热，多次运行取中位数。Debug
数字只用于同环境回归对比，不代表 Release 或生产容量。运行方法、场景与完整场景证据见
[distributed-testing.zh-CN.md](distributed-testing.zh-CN.md)。

**帧压缩（文本 payload，wire/plain）。** 基准走真实帧路径（header + 4 字节逻辑
长度前缀 + 编解码流），不是裸编解码调用。

| 算法 | 4 KiB | 64 KiB |
|---|---:|---:|
| none | 1.004 | 1.000 |
| lz4 | 0.026 | 0.005 |
| deflate | 0.027 | 0.004 |
| lz77 | 0.048 | 0.025 |
| zip | 0.034 | 0.005 |
| gzip | 0.031 | 0.005 |

**帧压缩吞吐（文本，Debug）。**

| 用例 | 编码 | 解码 |
|---|---:|---:|
| text 4 KiB lz4 | 307.9 MiB/s | 316.3 MiB/s |
| text 64 KiB lz4 | 343.0 MiB/s | 474.3 MiB/s |
| text 4 KiB deflate | 5.57 MiB/s | 90.75 MiB/s |
| text 4 KiB gzip | 5.51 MiB/s | 66.95 MiB/s |

不可压缩二进制：所有算法按设计回退到 `none`，ratio 为 1.0。例如 lz4 尝试编码
4 KiB 时为 84.0 MiB/s，最终 `chosen=none`。该轮共记录 120 个 case
（`.opencode/tmp/distbench-compression.log`）。

**可靠 RPC 延迟（闭环，depth 1；5 × 2000 样本取中位）。**

| Payload | p50 | p99 |
|---|---:|---:|
| 16 B | 64.0 µs | 177.1 µs |
| 256 B | 59.3 µs | 174.8 µs |
| 4 KiB | 67.1 µs | 194.8 µs |

**可靠 RPC 吞吐（闭环，payload × 并发，0 errors）。**

| Payload | c=1 | c=4 |
|---|---:|---:|
| 16 B | 83.1 µs/call（约 12.0k/s） | 96.9 µs/call（约 10.3k/s） |
| 256 B | 113.9 µs/call（约 8.8k/s） | 85.8 µs/call（约 11.7k/s） |
| 4 KiB | 123.9 µs/call（约 8.1k/s） | 99.1 µs/call（约 10.1k/s） |
| 64 KiB | 265.1 µs/call（约 3.8k/s） | 272.5 µs/call（约 3.7k/s） |

**不可靠 `ONE_WAY` 发射/投递（0 errors，drained = yes）。**

| Payload | c=1 issued/delivered | c=4 issued/delivered |
|---|---:|---:|
| 16 B | 10000 / 10000，0.4894 s（约 20.4k/s） | 10000 / 10000，0.5816 s（约 17.2k/s） |
| 4 KiB | 4000 / 4000，0.2004 s（约 20.0k/s） | 4000 / 4000，0.2665 s（约 15.0k/s） |

**流水线批次吞吐（Debug，2026-10-26）。** 上面的闭环用例发一个调用就等它；流水线用例先发出
1/16/64 个批次的调用再等待其中任意一个，这才是吞吐型调用方使用的形状。每个用例先预热，
运行三次计时迭代，报告中位数、p99 与 spread。`batch=1` 与闭环形状相同，是用例自身的一致性
检查。

可靠，中位数 ns/call（payload × batch × 并发；`hang` 表示流水线用例死锁，watchdog 记录了
闭环回退，见 [distributed-testing.zh-CN.md §7](distributed-testing.zh-CN.md#s7)）：

| Payload | batch | c=1 | c=2 | c=4 |
|---|---:|---:|---:|---:|
| 16 B | 1 | 88.1 µs | 75.0 µs | 80.4 µs |
| 16 B | 16 | 54.9 µs | 51.9 µs | hang（约 0.009 M/s） |
| 16 B | 64 | 46.7 µs | 82.1 µs | hang（约 0.010 M/s） |
| 256 B | 1 | 74.6 µs | 82.1 µs | 71.5 µs |
| 256 B | 16 | 45.2 µs | 72.7 µs | hang（约 0.009 M/s） |
| 256 B | 64 | hang（约 0.008 M/s） | hang（约 0.009 M/s） | hang（约 0.009 M/s） |

不可靠 `ONE_WAY`，中位数投递 M calls/s（16 B；全部 drained，0 errors）：

| batch | c=1 | c=2 | c=4 |
|---:|---:|---:|---:|
| 1 | 0.029 | 0.025 | 0.024 |
| 16 | 0.028 | 0.019 | 0.020 |
| 64 | 0.025 | 0.018 | 0.019 |

上表是 2026-10-26 的实测数据，记录于可靠流水线死锁修复之前。`hang` 单元格是完成
路径与句柄销毁路径以相反顺序获取 `completion_mutex_` 与 `map_mutex_`（ABBA）时，
watchdog 记录的闭环回退。该死锁是库缺陷，不是基准假象：现在所有路径统一
加锁顺序，`tests/test_pipeline_deadlock.cpp` 用 watchdog 守护高 batch 形状（挂起时以
退出码 42 结束）。本树上该套件为 2 cases / 6 checks / 0 failures，可靠路径不再挂起。
该次运行的调用栈与当时的局限记录在
[distributed-testing.zh-CN.md §8](distributed-testing.zh-CN.md#s8)。`ONE_WAY` 路径从不
等待完成，因此其流水线数字不受影响。

**Redis 客户端（分层隔离）。** 连接池用例使用空传输；pipeline 用例使用线缆形状的
executor（RESP 编码 + 解析），两者都不含真实服务器或网络。

| 用例 | 结果 |
|---|---:|
| connection_pool acquire/release | 792.4 ns/op，0 idle，0 error |
| RESP encode command（1 参数，1 B wire） | 3275.4 ns/op |
| RESP parse bulk reply | 4085.4 ns/op |
| pipeline batch=1 | 15157.7 ns/batch，约 0.07 M cmds/s |
| pipeline batch=16 | 194894.4 ns/batch，约 0.08 M cmds/s |
| pipeline batch=64 | 763389.9 ns/batch，约 0.08 M cmds/s |

日志：`.opencode/tmp/distbench-redis.log`。

分布式正确性套件不是基准，这里只记录其环境与规模，便于与数字一起查找：两个可执行
文件、5 + 3 个用例、150 + 64 个断言、0 失败，各连续运行 5 次零失败。见
[distributed-testing.zh-CN.md §3](distributed-testing.zh-CN.md#s3)。

### 2.6 重测基线（2026-11-07） <a id="s2-6"></a>

以下数字全部来自当前树（HEAD `5a4abaf` + 未提交的阶段 5 模块）的全新构建根，
不是从历史表格抄来的。构建命令：
`.\scripts\build.ps1 -Config Release -BuildRoot F:\crpc\.build-bench5 -Bench`。
工具链：MSVC 19.51 x64 Release（`/O2 /Ob2 /GL /DNDEBUG /MD`、`/W4 /WX`）；
机器：AMD Ryzen 9 7945HX，16 核 / 32 线程，Windows loopback；关闭消毒器。
每个目标跑 3 个独立进程（每个进程内部再取 7/5 次中位），表中取 3 个进程的中位数。
原始输出在 `.opencode/tmp/bench5/raw/`。

**干扰声明。** 测量窗口内本机还有其它工作负载：
`Win32_Processor.LoadPercentage` 在 54%～90% 之间。因此数字整体偏保守。
最明显的异常是第三次 IOCP 运行（p99 3117.8 µs、depth 16 p999 14.4 ms）；
前两次运行与低负载的韧性流水线运行作为稳定性参考保留。

`bench/bench_core.cpp` 的结果：

| 用例 | 3 次运行中位数 |
|---|---:|
| `codec/binary_encode_small`（`bench_request`，非 64 B） | 411.2 ns |
| `codec/binary_decode_small`（`bench_request`，非 64 B） | 290.7 ns |
| `crc32c/optimized_256B` | 58.7 ns |
| `crc32c/optimized_16KiB` | 3617.7 ns |
| `frame/feed_copy_1KiB_per_frame` | 76.4 ns/帧 |
| `frame/feed_external_1KiB_per_frame` | 17.5 ns/帧 |

`bench/bench_rpc.cpp`（64 B `std::string` 回显，inproc 与 IOCP loopback）：

| 用例 | 3 次运行中位数 |
|---|---:|
| inproc p50 / p90 / p99 / p999 | 3.30 / 6.40 / 35.70 / 79.60 µs |
| inproc max | 444.7 µs |
| 吞吐 depth 1 / 16 / 64 | 0.093 / 0.165 / 0.165 M RPC/s |
| 多分片 1 / 2 / 4（depth 16） | 0.15 / 0.15 / 0.15 M RPC/s |
| 8 客户端线程 × 8 分片 | 0.577 M RPC/s |
| IOCP p50 / p99 / p999，depth 1 | 125.20 / 218.40 / 319.10 µs |
| IOCP 吞吐 depth 1 / 16 | 0.008 / 0.022 M RPC/s |
| IOCP p99 depth 16 | 209.30 µs |

编译期布局探针（`probe_sizes.cpp`，同样编译选项；`sizeof` 由编译器从公开头文件计算）：

| 类型 | `sizeof` |
|---|---:|
| `rpc::call_slot` | **464 B** |
| `rpc::call<int>` | 24 B |
| `rpc::client` | 1024 B |
| `push::hub` | 8 B（pimpl 指针） |
| `cluster::registry` | 336 B |
| `cluster::lease_manager` | 184 B |

因此 128 B 调用槽预算在该工具链上 **超标 3.63×**；历史 440 B 数字已过期。
`rpc::client` 为 1024 B，而非历史的 568 B；`docs/abi.md` 的消费者探针与本构建一致。

恰好 64 字节的 codec。历史 `codec/binary_*_small` 用例使用 `bench_request`
（string + vector），不是 64 字节测量。探针 `probe_codec64.cpp` 测量
`call_codec<std::string>`（输出缓冲保留容量、解码目标复用）：**encode 4.34 ns、
decode 4.88 ns**（3 个进程中位）。该路径满足 `< 100 ns` 预算。

韧性流水线。`probe_resilience.cpp` 按 facade 成功路径顺序运行
（`rate.try_acquire -> bulkhead.try_enter -> breaker.allow ->
load_balancer.select`，随后 `breaker.success()` 与 permit 释放）：
**每次成功调用 211.49 ns** 中位（低负载最好一轮 157.85 ns）。其中熔断器单独
145.91 ns、限流 43.52 ns、舱壁 5.64 ns、选择 14.09 ns。35–65 ns 预算超标
3.25×–6.04×（最好一轮 2.43×）。

跨分片交接。公开 API 只有 `runtime::post`：每次调用一次 inbox 入队加一次
doorbell `wake()`（`src/runtime/runtime.cpp:177`）。没有批量 post API，
也没有专用基准目标。探针测得**每条消息入队 80.87 ns**、端到端 80.88 ns；
一次 post 携带合成的 64 条消息时为 1818.75 ns/消息，主要受唤醒/调度延迟支配，
不是真实批量 API。15–30 ns/批量消息预算超标 2.70×–5.39×。

连接内存。`probe_conn_memory.cpp` 运行真实 IOCP server 并保持裸 loopback
socket 空闲，采样 `PrivateUsage`/`WorkingSetSize`：

| 场景 | 3 次有效运行中位数 |
|---|---:|
| 1000 条 idle 连接，每连接 committed | 70 300 B |
| 10000 条 idle 连接，private 总增量 | 697 163 776 B（664.9 MiB） |
| 10000 条 idle 连接，每连接 committed | 69 716 B（68.1 KiB） |
| 128 条活跃连接（每条 1 个 4 KiB 在途调用），每对 private | 2 063 552 B（1.97 MiB） |

idle 预算（2 KiB）超标 34.0×，10k idle 预算（20 MiB）超标 33.2×。
原因在树中可直接看到：IOCP 后端在每次 accept 时分配 64 KiB 接收缓冲
（`include/crpc/io/iocp_backend.hpp:78`、`src/io/iocp_backend.cpp:1398`）。
active 数字是**同进程 client+server 两端合计**，不能单独归因 server 端；
它只是上界，仍是 160 KiB 预算的 12.9×。server-only 的 active 数字**未验证**（W5）。

G35 池容量调整后的槽池内存（`probe_g35_memory`，MSVC 19.51 x64 Release
`/O2 /MD`，AMD Ryzen 9 7945HX，Windows；源码与原始输出在
`.opencode/tmp/g35-exec/probe_g35_memory.cpp` 与
`.opencode/tmp/g35-exec/e4-memory-run2.txt`；PrivateUsage 口径，非 RSS；单次
运行，各场景在同一进程内采样，后续场景会复用此前释放的堆）：

| 场景 | 结果 |
|---|---:|
| `sizeof(call_slot)` | 464 B（G35 未改变布局） |
| 仅池空闲，容量 1024（新默认） | 479,232 B（468.0 KiB，468.0 B/槽）——容量 65536 时为 30,998,528 B（30.9 MB） |
| idle inproc 连接，零调用 | 相对基线 +12,288 B（12.0 KiB）；池复用了上面释放的堆 |
| 仅池参照，容量 16384 | 7,622,656 B（7.3 MiB，465.2 B/槽） |
| 仅池参照，容量 131072 | 61,997,056 B（59.1 MiB，473.0 B/槽） |
| 满载，1024 在途（池 1024） | 进程 PrivateUsage 合计 1,449,984 B；每调用增量 604.0 B |
| 满载，10 000 在途（池 16384） | 进程 PrivateUsage 合计 9,752,576 B；每调用增量 836.8 B |
| 满载，100 000 在途（池 131072） | 进程 PrivateUsage 合计 74,014,720 B；每调用增量 728.1 B |
| 全局缓冲字节上限，5 lane × 16 KiB | 4 条 lane 恰好缓冲满 65,536 B，第 5 条被拒（`admission::rejected`），无驱逐 |

同一探针还端到端演练了连接级上限：五条 lane 各缓冲一帧 16 KiB，前四条恰好
填满 64 KiB 总量，第五条被拒。所有满载场景全部调用正确完成（返回值校验和
与发出序列一致），因此测量状态是「每个调用已完成、每个槽仍被持有」。

每槽字节并非跨容量的常数（1024→468.0、16384→465.2、131072→473.0）：槽本体
恒为 464 B，差异来自 free-list 向量（8 B/槽，`reserve(capacity)`）、堆块头与
64 KiB 提交粒度，小容量点被固定池结构摊薄。三点均落在定案采纳的 472 B/槽
工程口径带内（465-473）。

## 3. 预算与目标 <a id="s3"></a>

内存预算来自设计 §17.1；状态按代码实际情况评估。

| 项 | 预算 | 状态 |
|---|---|---|
| 空闲连接 | ≤ 2 KiB | **未达成**：IOCP 下每条 idle 连接 committed 69 716 B（3 次中位，§2.6），超预算 34.0× |
| 活跃连接 | ≤ 160 KiB | **server-only 未验证**：同进程 client+server 一对连接、各 1 个 4 KiB 在途调用为 2 063 552 B（1.97 MiB，§2.6），作为上界超预算 12.9×；server 端份额无法拆分 |
| 调用槽 | 128 B | **未达成**：编译期探针实测 **464 B**（MSVC x64，§2.6），超预算 3.63×；历史 440 B 已过期。默认槽池容量已从 65536 降为 1024（G35）：每连接空闲池内存从 ~30.9 MB 降至实测 479,232 B（468.0 KiB，`probe_g35_memory`），降幅 64.7× |
| 每分片固定开销 | ≤ 8 MiB | 未测量；分片运行时已存在（`src/runtime/runtime.cpp`），但没有每分片固定内存实测 |
| 10 万在途调用 | ≤ 64 MiB | **当前槽尺寸下未达成**：仅槽数组为 464 B × 100k = 44.3 MiB；端到端实测（G35，`probe_g35_memory`，Release x64）：131072 槽池钉住 59.1 MiB，持有 100,000 个在途调用及记账、句柄的进程 PrivateUsage 达 74.0 MB；回到预算内需 128 B 槽（协程停车） |
| 1 万空闲连接 | ≤ 20 MiB | **未达成**：10 000 条 idle IOCP 连接 private 增量 697 163 776 B（664.9 MiB，§2.6），超预算 33.2× |
| 订阅表 | ≤ 5 MiB | 设计目标；hub 只按订阅设边界，没有全局表 |
| 每 lane 队列 | ≤ 16 KiB | **已由准入上限达成（G35）**：每 lane 上限默认 16 KiB，另加连接级总量 64 KiB（4 条满载 lane——64 KiB 是**跨 lane 的全局上限**，不是第二个每 lane 数字）；任一上限将超时，新到调用被拒绝并回 `rpc_code::overload`（`admission::rejected`），已入队元素不被驱逐，被拒调用不消耗序号位置。上限口径为编码后帧字节之和（客户端 `predicted_frame_size`，服务端 `encoded_frame_size`）。可用 `CRPC_LANE_MAX_BUFFERED_BYTES`（每 lane）与 `CRPC_LANE_TOTAL_BUFFERED_BYTES`（连接总量）覆盖；非法值回落默认，0 合法（缓冲上限为 0 字节） |
| 每连接发送环 | ≤ 256 KiB | epoll 默认 64 KiB；IOCP 默认 256 KiB |

性能目标：

| 指标 | 目标 | 状态 |
|---|---|---|
| 聚合吞吐 | 5M RPC/s | **未达成**：单客户端 inproc 0.165M RPC/s（depth 64，3 次中位，§2.6）；8 客户端线程 × 8 分片 0.577M；IOCP loopback depth 16 为 0.022M。差距 30.3× / 8.7× / 227×。历史 2026-10-19 数字：inproc 单客户端 0.275M、多线程 0.773M、IOCP 0.024M。 |
| 单连接对、批处理 | ≥ 1M RPC/s | **未达成**：inproc 实测 0.165M RPC/s（§2.6），差 6.1× |
| p99 延迟 | < 100 µs | **部分达成**：inproc 35.70 µs（MSVC，3 次中位）；IOCP loopback depth 1 218.40 µs；IOCP depth 16 209.30 µs（§2.6）。进程内达标；IOCP 超 2.09×–2.18× |
| 跨分片交接 | 每条批量消息 15–30 ns | **未达成 / 无批量 API**：`runtime::post` 入队 80.87 ns/消息、端到端 80.88 ns（3 次中位，§2.6），超预算 2.70×–5.39×。没有批量 post API，设计中的 MPSC+doorbell 批量交接未实现，也没有专用基准目标。 |
| 编解码，64 字节消息 | 编码与解码均 < 100 ns | **字符串路径已达标**：`call_codec<std::string>` 恰好 64 字节时 encode 4.34 ns、decode 4.88 ns（§2.6）。历史警告仍适用于 `codec/binary_*_small`：它测的是 `bench_request`（411.2 ns 编 / 290.7 ns 解），不是 64 字节负载。 |
| 韧性流水线 | 每次成功调用 35–65 ns | **未达成**：经过 rate -> bulkhead -> breaker -> load-balancer 加 teardown 的成功调用 211.49 ns（3 次中位）；低负载最好一轮 157.85 ns（§2.6）。差距 3.25×–6.04×（最好 2.43×）。其中熔断器单独 145.91 ns。 |

编解码预算现在有两个不同答案。恰好 64 字节的字符串路径达标（encode 4.34 ns /
decode 4.88 ns，§2.6）；反射式 `bench_request` 微基准仍为 411.2 ns / 290.7 ns，
即 `< 100 ns` 的 4.1× / 2.9×。两者负载不同，不能混为一谈：RPC 字符串路径所用
codec 达标，反射式小对象路径未证明。

传输层围绕有界队列与批处理设计，Windows loopback 裸探针仍显示批处理把吞吐改变
约 50 倍。这解释了设计动机，不证明 5M 目标。本轮同步往返的实测天花板是单客户端
0.165M RPC/s、八客户端线程 0.577M（§2.6），所以剩余差距在同步往返本身，而不在
分配或锁调优。

## 4. 基准计划 <a id="s4"></a>

基准现在落地为 `bench/bench_rpc.cpp`（§2.3）。当前用例为 64 B 同步时延、管线深度
1/16/64 与 1/2/4 分片（inproc），Windows 上另有 IOCP loopback。更宽的矩阵包括：

- `bench/bench_rpc_matrix.cpp` —— 已实现：16 B/256 B/4 KiB/64 KiB payload、
  1/2/4 调用线程、可靠闭环延迟/吞吐、不可靠 `ONE_WAY` 发射/投递，以及流水线批次吞吐
  （batch 1/16/64 × 并发 1/2/4，可靠与 `ONE_WAY`），并带子进程 watchdog：当流水线路径
  死锁时回退到闭环（§2.5）。
- `bench/bench_compression.cpp` —— 已实现：none/lz4/deflate/lz77/zip/gzip
  在文本与不可压缩二进制、4 KiB/64 KiB 下的测量（§2.5）。
- `bench/bench_redis.cpp` —— 已实现：连接池 acquire/release、RESP 编码/解析与
  pipeline 批量吞吐（§2.5）。
- `bench/bench_rpc.cpp` —— 仍待扩展到 1 KiB、1/8/64/512 连接、
  epoll loopback，以及可靠/不可靠/有序 lane 分开运行。
以下两项基准**尚未实现**，`bench/` 下没有同名文件；列出它们是为了让缺口可见，
而不是暗示已经覆盖：

- HTTP/1.1 与 HTTP/2 请求/响应、keep-alive、头部尺寸、上传/下载流式传输。
- crpc-binary、JSON、XML、INI、TOML 及 schema 格式在 64 B、256 B、1 KiB 下的吐吐量
  （热缓存、测量路径零分配）。

上面引用的编解码数字来自 `bench/bench_core.cpp`（`codec/binary_encode_small` 与
`crc32c` 用例），不是来自独立的编解码基准。

三个已实现基准的运行命令、子串过滤与场景解读见
[distributed-testing.zh-CN.md §5](distributed-testing.zh-CN.md#s5)。

每次运行报告 §1 的全部字段。计数器由客户端在解码响应后递增，绝不由服务端写完字节后递增。CPU
利用率单独采样；每消息系统调用数在平台追踪器可用时统计。进程内结果作为框架上界报告，不作为网络结果。
改变断言或跳过一致性路径的基准无效（P1）。

基准矩阵是发布产物，不是正确性的合并门禁。设计发布门禁要求性能被诚实测量与报告；它不要求每台机器都达到
5M RPC/s（O13）。

## 5. 设计拒绝的反模式 <a id="s5"></a>

| 反模式 | 为什么拒绝 | CRPC 的做法 |
|---|---|---|
| 无界发送/接收队列 | 内存随慢对端增长；延迟无界 | 有界环/队列；`resource_exhausted`、停车或显式溢出策略 |
| 固定超大缓冲（bufferbloat） | 队列增长、RTT 上升、尾延迟爆炸 | 默认 64 KiB 读写；按需增长；每轮 256 KiB 写预算；pacer 突发 2 ms |
| 把重试当拥塞恢复 | 重试风暴把过载变成崩溃 | 重试预算（10%）、3 次尝试、抖动、指数上限；熔断打开时跳过重试 |
| 只把丢包当拥塞信号 | 丢包出现时队列已满 | RTT 基线与排队延迟驱动窗口；丢包不是唯一信号 |
| 墙钟 RTT | 时钟跳变看起来像路径变化 | 注入单调时钟；拒绝过期/倒退样本 |
| 定时器驱动窗口增长 | 路径安静时反而多发 | 窗口只在 ACK 时增长（自时钟） |
| “路径已畅通”检测器 | 它总是检测到畅通并振荡 | 已移除；只有持续方向性证据才移动基线 |
| 静默毫秒回退 | 掩盖损坏的定时器并破坏 p99 | epoll 在 `timerfd` 不可用时 fail-closed |
| 每消息一次系统调用与唤醒 | 审计探针实测 0.02M 对 1.05M RPC/s | 批处理、发送环、完成批 |
| 用每 RTT 帧数推导 RPC/s | 测量的是想象中的流水线 | 独立计数器、端到端延迟 |

## 6. 报告与验收 <a id="s6"></a>

性能报告只有在包含基准源码、命令行、日志/产物路径、硬件与工具链，以及 §1 全部字段时才被接受。
它必须说明测量了哪些后端、哪些代码路径被关闭（消毒器、追踪、日志）。目标未达成时，报告写出实测值与差距；
不得用推算替代缺失的测量。

当前状态：库有功能测试覆盖、HTTP 服务端门面、进程内与 IOCP 端到端 RPC 基准（§2.3）、
线格式/编解码微基准，以及 §2.5 的压缩/RPC 矩阵/Redis 基准。2026-11-07 重测（§2.6）
取代旧基线：5M RPC/s 与 IOCP p99 目标**未达成**（inproc 单客户端 0.165M RPC/s、
八客户端线程 0.577M、IOCP depth 1 p99 218.4 µs）；进程内 p99 目标在 MSVC 上以
35.70 µs **达成**。128 字节调用槽预算在已测工具链上仍是**未达成的 464 字节**。
64 字节字符串 codec 达标（4.34/4.88 ns）；韧性流水线（211.49 ns）与跨分片交接
（80.87 ns/消息）未达标；空闲连接内存为预算的 34×。实测数值、被否决的实验与剩余
差距记录于 §2 与 §3；随仓库发布的微基准日志为
`docs/benchmarks/2026-10-19-core-x64-msvc-release.txt`（历史），2026-11-07 原始日志在
`.opencode/tmp/bench5/raw/`。
