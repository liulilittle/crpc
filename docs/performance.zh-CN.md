# CRPC 性能

[English](performance.md) | **简体中文**

版本 2.0 / 2026-10-24

本文说明仓库中真正测过什么、在什么硬件与工具链上测的、哪些只是预算，以及库将如何基准测试。目标是目标，
不是结果。不存在的数字会明确标为缺失，而不是估算。

## 目录

| # | 章节 |
|---|---|
| [1](#s1) | 测量方法 |
| [2](#s2) | 已记录证据 |
| [2.5](#s2-5) | 压缩、RPC 矩阵与 Redis 基准（2026-10-26） |
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

### 2.3 端到端 RPC 实测（2026-10-19）

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

### 2.4 CRC32C 与推送 hub 统计（2026-10-24） <a id="s2-4"></a>

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

### 2.5 压缩、RPC 矩阵与 Redis 基准（2026-10-26） <a id="s2-5"></a>

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

在本 Debug 构建中，可靠流水线路径在高 batch × 并发下不可用：并发发射路径与进程内后端的同步
完成路径死锁，watchdog 杀掉用例并记录闭环回退，而不是流水线数字。`ONE_WAY` 路径不会死锁
（其发射路径不等待），因此其流水线数字是完整的。该死锁是库层发现，不是基准假象；调用栈随该次测量一并留存，并记为
[distributed-testing.zh-CN.md §8](distributed-testing.zh-CN.md#s8) 的局限。挂起是时序相关的，
因此挂起的用例集合在多次运行间会变化；回退总是被显式记录。

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

## 3. 预算与目标 <a id="s3"></a>

内存预算来自设计 §17.1；状态按代码实际情况评估。

| 项 | 预算 | 状态 |
|---|---|---|
| 空闲连接 | ≤ 2 KiB | 设计目标；未测量 |
| 活跃连接 | ≤ 160 KiB | 设计目标；epoll 发送环 64 KiB、IOCP 发送队列 256 KiB、读缓冲 64 KiB |
| 调用槽 | 128 B | **未达成**：MSVC x64 实测 440 B |
| 每分片固定开销 | ≤ 8 MiB | 不适用：尚无分片/协程池 |
| 10 万在途调用 | ≤ 64 MiB | 基于 128 B 槽的设计目标；当前槽大小本身对 10 万即约 44 MB |
| 1 万空闲连接 | ≤ 20 MiB | 设计目标；未测量 |
| 订阅表 | ≤ 5 MiB | 设计目标；hub 只按订阅设边界，没有全局表 |
| 每 lane 队列 | ≤ 16 KiB | 已设计，未实现 |
| 每连接发送环 | ≤ 256 KiB | epoll 默认 64 KiB；IOCP 默认 256 KiB |

性能目标：

| 指标 | 目标 | 状态 |
|---|---|---|
| 聚合吞吐 | 5M RPC/s | **未达成**：单客户端最佳实测 0.275M RPC/s（inproc，MSVC，depth 64）；八客户端线程四线程分片 0.773M RPC/s（§2.3）；IOCP loopback 0.024M |
| 单连接对、批处理 | ≥ 1M RPC/s | **未达成**：inproc 实测 0.275M RPC/s（§2.3） |
| p99 延迟 | < 100 µs | **部分达成**：inproc 22.6 µs（MSVC）；IOCP loopback 170.2 µs；WSL inproc 151.8 µs |
| 跨分片交接 | 每条批量消息 15–30 ns | 预算；未测量 |
| 编解码，64 字节消息 | 编码与解码均 < 100 ns | **未证明**：`docs/benchmarks/2026-10-19-core-x64-msvc-release.txt` 记录 `codec/binary_encode_small` 218.8 ns 与 `codec/binary_decode_small` 232.9 ns。基准负载是小型 `bench_request`（string/vector 字段），并非恰好 64 字节，因此这些数字不能与预算直接比较；但它们仍说明目标未达成。 |
| 韧性流水线 | 每次成功调用 35–65 ns | 预算；未测量 |

编解码预算需要直接警示：上文记录的微基准是真实测量，但它超出 `< 100 ns` 目标
2.2–2.3×，且使用了与「64 字节消息」不同的负载。不应把它读作通过的结果。

今天唯一可辩护的陈述是：传输层围绕有界队列与批处理设计，Windows loopback
裸探针显示批处理把吞吐改变约 50 倍。这解释了设计动机，不证明目标。

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
- `bench/bench_http.cpp` —— HTTP/1.1 与 HTTP/2 请求/响应、
  keep-alive、头部大小、上传/下载流。
- `bench/bench_codec.cpp` —— crpc-binary、JSON、XML、INI、
  TOML 与模式格式，在 64 B、256 B、1 KiB 下，热缓存且测量路径无分配。

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
线格式/编解码微基准，以及 §2.5 的压缩/RPC 矩阵/Redis 基准。5M RPC/s 与 IOCP p99 目标**未达成**（实测 inproc 单客户端 0.275M RPC/s、
多线程 0.773M RPC/s、IOCP p99 170.2 µs）；进程内 p99 目标在 MSVC 上以 22.6 µs **达成**。128 字节调用槽预算在
已测工具链上仍是**未达成的 440 字节**。实测数值、被否决的实验与剩余差距记录于 §2 与 §3；
随仓库发布的微基准日志为 `docs/benchmarks/2026-10-19-core-x64-msvc-release.txt`。
