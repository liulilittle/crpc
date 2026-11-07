# 分布式测试与基准

[English](distributed-testing.md) | **简体中文**

版本 1.0 / 2026-10-26

本文记录多节点/分布式测试套件与性能基准的构建、运行与解读方法。除特别说明外，所有数字均为同一台开发机上的单次 Debug 构建、
使用进程内后端测得。

## 目录

| # | 章节 |
|---|---|
| [1](#s1) | 覆盖范围 |
| [2](#s2) | 构建 |
| [3](#s3) | 运行分布式测试 |
| [4](#s4) | 场景与各自证明什么 |
| [5](#s5) | 运行基准 |
| [6](#s6) | 已记录结果 |
| [7](#s7) | 如何解读结果 |
| [8](#s8) | 限制 |
| [9](#s9) | 另见 |

## 1. 覆盖范围 <a id="s1"></a>

| 文件 | 目标 | 覆盖 |
|---|---|---|
| `tests/test_distributed_rpc.cpp` | `crpc_test_distributed_rpc` | 多节点 discovery 与路由收敛、provider 下线与故障转移、health 驱动驱逐/恢复、传输分区 + 幂等重放、registry 分区恢复与 watch 重放/乱序去重。 |
| `tests/test_distributed_push.cpp` | `crpc_test_distributed_push` | 三节点两跳应用级 relay push、`PUSH_ACK`、完全 ACK 后重连不重放、中间跳分区恢复、unreliable 无 ACK 投递。 |
| `tests/test_distributed_tcp.cpp` | `crpc_test_distributed_tcp` | 同样场景的真实 TCP 传输（Windows IOCP、Linux epoll）覆盖，至少 3 个节点、均位于 `127.0.0.1`：discovery 与路由收敛、绕开不可达端点的故障转移、health 驱动驱逐/恢复、幂等重放、三节点两跳 push 与 `PUSH_ACK`、中间跳分区恢复。 |
| `bench/bench_compression.cpp` | `crpc_bench_compression` | none/lz4/deflate/lz77/zip/gzip × 文本/不可压缩二进制 × 4 KiB/64 KiB：压缩率与编解码吞吐。 |
| `bench/bench_rpc_matrix.cpp` | `crpc_bench_rpc_matrix` | 可靠 RPC 延迟、可靠闭环吞吐（payload × 并发）、不可靠 `ONE_WAY` 发射/投递吞吐，以及流水线批次吞吐（batch 1/16/64 × 并发 1/2/4，可靠与 `ONE_WAY`）。 |
| `bench/bench_redis.cpp` | `crpc_bench_redis` | Redis 连接池 acquire/release、RESP 命令编码/回复解析、pipeline 批量吞吐。 |

分布式测试是确定性的：原有两个套件使用进程内后端，TCP 套件使用真实环回 TCP
（动态端口 + 同步点），因此失败无需真实集群即可复现。它们是正确性测试，不是
网络容量测量。

## 2. 构建 <a id="s2"></a>

```powershell
$env:CRPC_RTTI_DIR   = 'F:\RTTI'
$env:CRPC_COROCC_DIR = 'F:\corocc'
cmake -S . -B .opencode/tmp/out-distbench -G Ninja `
      -DCMAKE_BUILD_TYPE=Debug -DCRPC_WARNINGS_AS_ERRORS=ON `
      -DCRPC_BUILD_TESTS=ON -DCRPC_BUILD_BENCH=ON `
      -DCRPC_RTTI_DIR=F:\RTTI -DCRPC_COROCC_DIR=F:\corocc
cmake --build .opencode/tmp/out-distbench --parallel 3
```

构建是 C0 硬门：`/W4 /WX`（MSVC）或 `-Wall -Wextra -Wpedantic -Werror`
（GCC/Clang），零错误零警告。基准只在 `CRPC_BUILD_BENCH=ON` 时构建，且不注册为
CTest 条目。

## 3. 运行分布式测试 <a id="s3"></a>

```powershell
ctest --test-dir .opencode/tmp/out-distbench `
      -R crpc_test_distributed --output-on-failure
```

已记录结果：

```text
1/3 Test #11: crpc_test_distributed_push ....... Passed 0.03 sec
2/3 Test #12: crpc_test_distributed_rpc ........ Passed 0.38 sec
3/3 Test #13: crpc_test_distributed_tcp ........ Passed 4.75 sec
100% tests passed, 0 tests failed out of 3
```

直接运行会打印逐用例计数：

```text
crpc_test_distributed_rpc.exe:  5 case(s), 150 check(s), 0 failure(s)
crpc_test_distributed_push.exe: 3 case(s),  64 check(s), 0 failure(s)
crpc_test_distributed_tcp.exe:  6 case(s), 185 check(s), 0 failure(s)
```

稳定性：三个二进制各连续运行 5 次，失败次数均为 0。TCP 二进制另外再跑 10 次
（并对易触发崩溃的用例各跑 10 次），在 §8 所述 pump 排空收尾之后零崩溃。

只运行单个用例时，把用例名子串作为可执行文件参数：

```powershell
.\.opencode\tmp\out-distbench\crpc_test_distributed_rpc.exe partition
```

## 4. 场景与各自证明什么 <a id="s4"></a>

### 4.1 discovery 与路由收敛

三个 provider 注册到 `cluster::registry`；调用方用 `passing_only` 查询构建
`client`。30 次调用覆盖全部 3 个节点。第 4 个节点加入后，旧路由 30 次调用从未
命中新节点；刷新 discovery 后 40 次调用覆盖全部 4 个节点。停掉并注销一个 provider
后：

- **红：** 旧路由表仍包含已下线节点，60 次调用出现失败
  （`CRPC_CHECK(stale_failures > 0)`，`tests/test_distributed_rpc.cpp:503`）。
- **绿：** 刷新 discovery 后路由表只剩 3 个存活节点，60 次调用 **0 失败**，且
  不再命中下线节点（`tests/test_distributed_rpc.cpp:524`）。

红→绿证明改变路由的是 discovery，而不是运气。

### 4.2 故障转移

两个 provider 注册；停掉一个但暂不更新 registry。20 次调用全部成功且均由存活节点
应答；被停节点的 handler 调用数为 0。Facade 观测到 pre-send 重试/breaker 跳过
（`tests/test_distributed_rpc.cpp:582`）。

### 4.3 分区 + 幂等重放

服务端 handler 先写入幂等状态，再在测试门闩处等待；调用方在响应前关闭连接
（确定性分区窗口）。第一次调用失败，但服务端 `side_effects == 1`
（`tests/test_distributed_rpc.cpp:687-688`）。恢复连接后重放同一 `req-1|v1`：
返回 `v1`，`side_effects` 仍为 1（`:701`）。再发送 `req-2|v2` 并乱序重放
`req-1`/`req-2`：副作用总数恰为 2，最终状态为 `req-1=v1\nreq-2=v2\n`
（`:712` 及后续断言）。

这是「重试下副作用至多一次」的性质；它不是「传输层绝不重复执行请求」的承诺。

### 4.4 registry 分区恢复与 watch 重放

节点在 authority 更新到 incarnation 2 后以 incarnation 1 回归：写入被拒绝，
返回 `failed_precondition`（`tests/test_distributed_rpc.cpp:762`）。以
incarnation 2 重新加入后，查询收敛到 3 个节点、每节点一个实例（`:770`）。
Watch 收到重复 index、旧 index、正常 index 时只派发 2 个 delta；resync 后最终
快照为 3 个实例（`:812` 起）。

### 4.5 跨节点 push

拓扑：`server A --relay 1--> server B --relay 2--> server C --> subscriber`。

- 3 条 reliable 消息按序到达 C，`seq = 1/2/3`。
- A、B、C 三跳的 `push_acks` 均 `>= 3`
  （`tests/test_distributed_push.cpp:333-337`）。
- Subscriber 完全 ACK 后重连：无重放；新消息 `seq = 4`（`:353`、`:364`）。
- 中间跳 relay 断开后，A 发布的消息在 B hub 排队；relay 以同一 subscriber id
  重连后只重放一次，C 收到 `before-partition`、`during-partition`，`seq = 1/2`
  （`:434-437`）。
- Unreliable 路径：消息送达时 `seq = 0`，A/B `push_acks == 0`（`:486-487`）。

库的 `push::hub` 是单 shard 权威；测试里的 relay 是应用级代码，不是库内置的跨节点
复制/转发功能。

### 4.6 TCP 传输

`tests/test_distributed_tcp.cpp` 在真实 TCP 后端上、用至少 3 个节点重复同样的
场景，全部位于同一进程，每个节点使用来自进程内计数器的不同 `127.0.0.1` 端口
（监听绑定会重试，因此不需要固定端口）。registry authority 与进程内套件相同；
变化的是每个被发现的端点都是真实监听、每次调用都经过 socket。

- **discovery 与路由收敛。** 三个 provider 注册；30 次调用经 facade 的
  round-robin 覆盖全部三个。第四个加入后，旧路由从不命中它，刷新后的路由覆盖
  全部四个。停掉并注销一个 provider 后，用陈旧路由新建 client 会出现失败
  （`CRPC_CHECK(stale_failures > 0)`，`tests/test_distributed_tcp.cpp`），
  而刷新后的路由 60 次调用 **0 失败**。陈旧 client 用陈旧表新建并等待监听关闭，
  因此失败是快速的 `connection_refused`，而不是超时。
- **故障转移。** 路由包含一个存活 provider 与一个不可达端点（TCP 后端会立即拒绝
  的进程内端点）。20 次调用全部在存活 provider 上成功（`pre_send_retries >= 1`），
  证明 facade 会绕开无法连接的端点。本机上一个刚关闭的真实 TCP 端口可能要 ~2s
  才被拒绝，会主导测试；进程内端点使时序确定。真实监听关闭路径由 discovery 用例
  覆盖。
- **health 驱动驱逐/恢复。** 回调 probe 驱动 `health_monitor`；probe 失败会把
  registry 状态变为 critical，discovery 只剩一个节点，连续两次通过恢复后，
  discovery 回到两个。
- **幂等重放。** 同一 `req-1|v1` 经 TCP 发送两次；handler 去重，`side_effects`
  保持 1，重放返回 `v1`。乱序重放 `req-2` 也不会重复应用。传输分区变体仍保留在
  进程内套件（见 §8）。
- **三节点 push。** TCP 上的 `A --relay 1--> B --relay 2--> C --> subscriber`：
  三条消息按序到达 C，`seq = 1/2/3`，A/B/C 各自 `push_acks >= 3`，完全 ACK 后
  重连不重放，下一条为 `seq = 4`。
- **中间跳分区。** 中间 relay 停止后，server 在发布消息之前已 detach 桥接
  （`push_active_connections == 0`）；消息在 B 的 hub 排队，relay 以同一
  subscriber id 重连后恰好重放一次。C 收到 `before-partition`/`during-partition`，
  `seq = 1/2`。

## 5. 运行基准 <a id="s5"></a>

```powershell
Set-Location .opencode/tmp/out-distbench
.\crpc_bench_rpc_matrix.exe
.\crpc_bench_compression.exe
.\crpc_bench_redis.exe
```

每个基准接受第一个参数作为子串过滤器：

```powershell
.\crpc_bench_rpc_matrix.exe latency
.\crpc_bench_compression.exe text_4096
.\crpc_bench_redis.exe redis/pipeline
.\crpc_bench_rpc_matrix.exe pipeline
```

`CRPC_BENCH_RPC_CALLS` 覆盖 RPC 每用例调用数，便于慢机缩小工作量复现：

```powershell
$env:CRPC_BENCH_RPC_CALLS = '200'
.\crpc_bench_rpc_matrix.exe latency
```

`pipeline` 过滤器运行流水线批次用例。每个用例在带 watchdog 超时的子进程中运行，以便把
挂起报告为被终止的用例而不是让整轮阻塞；超时的用例会被杀掉，harness 为其记录闭环回退。
watchdog 在 Windows（`CreateProcess`）上实现；其他平台上流水线用例在进程内运行，超时
不适用。促使 Debug 构建引入该 watchdog 的死锁已在 `59936b6` 中修复（见 §7）；watchdog
保留为挂起探测器，`tests/test_pipeline_deadlock.cpp` 守护同一形状。

每个用例先预热，多次运行取中位数；harness 同时打印编译器、构建类型与架构。基准不
参与 CI 的通过/失败门禁。

## 6. 已记录结果 <a id="s6"></a>

环境：MSVC x64 **Debug**、AMD Ryzen 9 7945HX（16 核 / 32 线程）、单进程
`inproc_backend`、无网络、无消毒器。Debug 数字只用于同环境回归对比，不代表
Release 或生产容量。

### 6.1 可靠 RPC 延迟（闭环，depth 1；5 × 2000 样本取中位）

| Payload | p50 | p99 |
|---|---:|---:|
| 16 B | 64.0 µs | 177.1 µs |
| 256 B | 59.3 µs | 174.8 µs |
| 4 KiB | 67.1 µs | 194.8 µs |

### 6.2 可靠 RPC 吞吐（闭环，payload × 并发，0 errors）

| Payload | c=1 | c=4 |
|---|---:|---:|
| 16 B | 83.1 µs/call（约 12.0k/s） | 96.9 µs/call（约 10.3k/s） |
| 256 B | 113.9 µs/call（约 8.8k/s） | 85.8 µs/call（约 11.7k/s） |
| 4 KiB | 123.9 µs/call（约 8.1k/s） | 99.1 µs/call（约 10.1k/s） |
| 64 KiB | 265.1 µs/call（约 3.8k/s） | 272.5 µs/call（约 3.7k/s） |

### 6.3 不可靠 `ONE_WAY` 发射/投递（0 errors，drained = yes）

| Payload | c=1 issued/delivered | c=4 issued/delivered |
|---|---:|---:|
| 16 B | 10000 / 10000，0.4894 s（约 20.4k/s） | 10000 / 10000，0.5816 s（约 17.2k/s） |
| 4 KiB | 4000 / 4000，0.2004 s（约 20.0k/s） | 4000 / 4000，0.2665 s（约 15.0k/s） |

### 6.4 帧压缩

文本 payload 压缩率（wire/plain）：

| 算法 | 4 KiB | 64 KiB |
|---|---:|---:|
| none | 1.004 | 1.000 |
| lz4 | 0.026 | 0.005 |
| deflate | 0.027 | 0.004 |
| lz77 | 0.048 | 0.025 |
| zip | 0.034 | 0.005 |
| gzip | 0.031 | 0.005 |

文本吞吐示例（Debug）：

| 用例 | 编码 | 解码 |
|---|---:|---:|
| text 4 KiB lz4 | 307.9 MiB/s | 316.3 MiB/s |
| text 64 KiB lz4 | 343.0 MiB/s | 474.3 MiB/s |
| text 4 KiB deflate | 5.57 MiB/s | 90.75 MiB/s |
| text 4 KiB gzip | 5.51 MiB/s | 66.95 MiB/s |

不可压缩二进制：所有算法按设计回退到 `none`，ratio 为 1.0。例如 lz4 尝试编码
4 KiB 时为 84.0 MiB/s，最终 `chosen=none`。该轮共记录 120 个 case
（`.opencode/tmp/distbench-compression.log`）。

### 6.5 Redis 客户端

| 用例 | 结果 |
|---|---:|
| connection_pool acquire/release | 792.4 ns/op，0 idle，0 error |
| RESP encode command（1 参数，1 B wire） | 3275.4 ns/op |
| RESP parse bulk reply | 4085.4 ns/op |
| pipeline batch=1 | 15157.7 ns/batch，约 0.07 M cmds/s |
| pipeline batch=16 | 194894.4 ns/batch，约 0.08 M cmds/s |
| pipeline batch=64 | 763389.9 ns/batch，约 0.08 M cmds/s |

日志：`.opencode/tmp/distbench-redis.log`。

### 6.6 流水线批次吞吐（Debug，2026-10-26 历史运行）

可靠，中位数 ns/call（payload × batch × 并发；3 轮 × 10000 调用）。该轮记录早于死锁修复；
`hang` 表示流水线用例死锁，watchdog 记录了闭环回退：

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

日志：`.opencode/tmp/bench-pipeline-watchdog2.log`。挂起是时序相关的，因此挂起的用例集合在
多次运行间会变化；回退总是被显式记录。该死锁已在 `59936b6` 中移除（见 §7）；该表保留为
修复前的记录。

## 7. 如何解读结果 <a id="s7"></a>

- **分布式测试是正确性证据。** 红→绿成对出现，说明移除机制或触发陈旧状态时行为
  确实改变；只有绿色通过不能证明这一点。
- **RPC/one-way 数字是框架上界。** 它们跑在进程内后端，不含 socket、TLS 与内核。
  用于同环境比较改动，不用于规划网络容量。
- **压缩率依赖数据。** 文本 payload 是重复日志行；真实 body 会落在别处。不可压缩
  用例是重要对照：它证明发送方回退到 `none`，而不是放大帧。
- **Debug 下 gzip/deflate/zip 很慢。** 该构建的 DEFLATE 编码器未优化（约
  5 MiB/s）。不要把 Debug 编码速率当作 Release 速率。这里解码明显快于编码，lz4
  是低延迟选择。
- **Redis 数字是分层隔离的。** 连接池用例使用空传输，测的是池记账而非 Redis；
  pipeline 用例使用线缆形状的 executor，包含 RESP 编解码但不含服务器与网络。
- **可靠流水线路径曾在 Debug 下死锁，该缺陷已修复。** 并发发射路径与进程内后端的同步
  完成路径以相反顺序获取 `completion_mutex_` 与 `map_mutex_`（ABBA），导致高 batch ×
  并发的用例可能停止推进。提交 `59936b6` 统一了加锁顺序（`completion_mutex_` 外层、
  `map_mutex_` 内层），挂起已消失：`tests/test_pipeline_deadlock.cpp` 用无进展 watchdog
  守护高 batch 形状（挂起时以退出码 42 结束），本树上 2 cases / 6 checks / 0 failures
  通过。harness 仍为每个流水线用例运行带 watchdog 的子进程，但它是挂起探测器而不是已知
  局限；闭环回退绝不被当作流水线数字。`ONE_WAY` 路径从不在完成上等待，因此不受影响。
- **Debug 与 Release。** 不带构建类型的数字不可比较。记录表是 Debug；Release 结论
  必须在 Release 构建下重跑同一命令。

## 8. 限制 <a id="s8"></a>

1. **单进程目录；真实 TCP 传输。** 原有两个分布式套件使用 `inproc_backend`；
   `tests/test_distributed_tcp.cpp` 现在在真实 TCP 传输（Windows IOCP、Linux
   epoll）上用至少 3 个节点重复同样场景，因此真实 TCP 多节点行为**已**覆盖。
   仍未覆盖的有两点：TCP 套件以单进程多 TCP 监听运行，而非多个独立 OS 进程；
   `cluster::registry` authority 与其他套件相同，都是进程内目录，因此不覆盖
   分布式目录或跨主机部署。TCP 套件还继承了 IOCP 后端的一个已知竞态：在发送中途
   关闭 TCP client 可能触发 `handle_send_complete` 的 use-after-free（记录于
   `src/io/iocp_backend.cpp`）。因此套件在收尾前用 `backend->stop()` 排空
   backend pump，并把传输分区场景保留在进程内套件（其后端同步关闭连接）。
2. **跨节点 push 是应用级 relay。** 库的 `push::hub` 是单 shard 权威，没有内置
   跨节点复制/转发。测试用 relay 订阅上游并向发布下游；这不是库功能。
3. **RPC 吞吐有闭环基线与流水线扩展。** 曾经导致「流水线批处理 + facade handle 立即
   析构」不可靠的 completion 竞争已修复：`call_slot::invoke_callback()` 通过
   `callback_armed.exchange` 恰好认领一次回调，`release_pending()` 使 handle 析构
   与任何在途回调同步。该修复由 `tests/test_server_client.cpp` 中的
   `facade_handle_destroy_races_completion` 覆盖。闭环用例保留为可复现基线，流水线
   批次用例（§6.6）补充吞吐调用方使用的发射形状；两种选择都不改变库源码。
4. **可靠流水线死锁是历史记录，不是当前局限。** Debug 构建在高 batch × 并发下曾死锁：
   并发发射路径与进程内后端的同步完成路径以相反顺序获取 `completion_mutex_` 与
   `map_mutex_`（ABBA）。提交 `59936b6` 统一加锁顺序，移除了该死锁。
   `tests/test_pipeline_deadlock.cpp` 用无进展 watchdog 守护该形状，本树上 2 cases /
   6 checks / 0 failures 通过。§6.6 的基准表与日志中的 `hang` 单元格保留为修复前记录；
   该次运行的调用栈随原始测量一并留存。
5. **Debug 数字。** 尤其压缩编码吞吐不具代表性，见 §7。
5. **`none` 解码不是解压。** 它的「解码吞吐」是 header 解析 + payload 借用，仅作
   基线。
6. **不可压缩数据回退 `none`。** ratio 为 1.0 是正确的 fail-safe 行为，不是编解码
   失败。

## 9. 另见 <a id="s9"></a>

- [性能报告 §2.5](performance.zh-CN.md#s2-5) —— 同一批数字在性能文档证据章节中的
  版本。
- [线上协议 §24](protocol.zh-CN.md#s24) —— 帧压缩格式与算法选择。
- [Redis 客户端](redis-client.zh-CN.md) —— API 地图与 Redis 套件的构建/测试命令。
