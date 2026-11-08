# CRPC 设计（中文版）
[English](design.md) | **简体中文**

版本 1.0 / 2026-10-19

本文档是设计结论的汇总记录：它整合了此前两份独立的设计评审，正文中以 **A** 与 **B** 指代；这两份评审都不随仓库发布，因此"A §3.5"这类引用只是说明该决定的来源，不是链接。本文档中带 § 编号的章节是设计的权威陈述；[架构](architecture.zh-CN.md)记录代码今天实现了什么。若本文档与代码不一致，以代码及其测试为准。R1、R5、R6 这类编号指代该评审记录中的编号规则；带有该编号的章节即该规则的完整陈述处。

下文的代码片段描述的是内部机制，而非公开 API：`class epoll_connection` 是 `src/` 中真实存在的内部类；`class epoll_listener` 与 `class shard` 是本文档为描述内部结构而使用的示意名，在头文件中并不存在。因此只出现在本文档中的限定名属于设计草图，不是缺失的公开符号。本文档声称为**公开**的每个名字，都由 `tests/test_docs_lint.cpp` 对着头文件校验。
## 目录
| # | 章节 |
|---|---|
| [0](#s0) | 定位与不变量 |
| [1](#s1) | 术语与权威源矩阵 |
| [2](#s2) | 分层与目录结构 |
| [3](#s3) | 传输后端契约 |
| [4](#s4) | 线程与协程模型 |
| [5](#s5) | epoll ET-only 设计 |
| [6](#s6) | IOCP 设计 |
| [7](#s7) | Wire 协议 |
| [8](#s8) | 消息语义 |
| [9](#s9) | 错误模型 |
| [10](#s10) | 序列化 |
| [11](#s11) | 服务 API 与 stub |
| [12](#s12) | HTTP/1.1、HTTP/2、HTTPS |
| [13](#s13) | MVC 与 RESTful |
| [14](#s14) | 压缩 |
| [15](#s15) | 容错与治理 |
| [16](#s16) | 可观测性 |
| [17](#s17) | 内存与性能预算 |
| [18](#s18) | 并发正确性 |
| [19](#s19) | 测试策略 |
| [20](#s20) | 构建与 CI |
| [21](#s21) | v1 范围、v1.1、里程碑 |
| [22](#s22) | 未决问题与风险 |
## 0. 定位与不变量 <a id="s0"></a>
CRPC 是一个 C++17 RPC 框架，用同一条连接、同一种帧格式、同一套分片（shard）/协程模型承载两类一等能力：

1. **调用（call）**：请求/响应，支持 unary 与流式；协程内呈现同步写法，协程外通过 `call<t>` 句柄异步使用。
2. **推送（push）**：服务端到客户端，通过 `push_hub`、订阅（subscription）、SSE 与长轮询交付。

框架不是 HTTP 封装。TCP 上直接承载长度前缀的多路复用帧流，默认编解码为 `crpc-binary`。HTTP/1.1、HTTP/2、HTTPS 是同一路由表、`call_context`、容错流水线与 handler 池之上的附加适配层。
### 0.1 三条不可协商的底线
| 底线 | 强制机制 | 验证手段 | 违反后的失败模式 |
|---|---|---|---|
| **绝不死锁** | 文档化锁序（§18.1）；持锁期间禁止 I/O、用户回调、协程挂起、`sem_post`、日志 sink、阻塞分配；跨分片只走 MPSC 加门铃；每连接单读者单写者；协程内只经 `coro_rt_sem_wait`/`coro_rt_cond_wait` 等待；lane 自依赖等待直接返回 `protocol_error`，不挂起。 | debug 构建的锁序断言（线程本地持锁栈，逆序即中止）；Linux TSan；多分片交叉调用 10 分钟压力测试，随机取消与关闭。 | 某个分片永久停摆，其上所有连接超时。 |
| **绝不崩溃** | 校验全部外部输入（帧长、字段长、元素数、枚举值、UTF-8、header、路径、body）；禁止 UB（wire 逐字节访问、无符号运算加范围检查、generation 校验句柄、共享状态用原子）；异常不得跨越协程或 ABI 边界；`new` 使用 `nothrow` 或捕获为 `resource_exhausted`；递归有上限。 | ASan/UBSan；帧、HTTP、JSON/XML/TOML/INI、multipart、路由的 fuzz；畸形输入故障注入；`_GLIBCXX_ASSERTIONS`。 | 进程中止，所有连接断开，级联故障。 |
| **绝不泄露** | 每个获取/释放、订阅/退订、注册/注销、定时器 arm/cancel、wait/post、open/close 一一配对；销毁幂等；关闭后不再交付，在途 completion 恰好一次并以 canceled 完成（R9）；迟到完成按 generation 丢弃；禁止 `shared_ptr` 环；槽、流、lane、连接均有上限与回收点；call 终结时移除定时器。 | ASan/LSan 零报告；`drain()` 断言 `pending_calls == 0 && open_connections == 0 && armed_timers == 0 && temp_files == 0`；百万调用长稳 RSS 平稳；1000 次 start/stop。 | 内存或 fd 缓慢增长；数小时后 OOM 或槽耗尽。 |
### 0.2 原则 P1-P13
| # | 原则 | 强制机制 | 验证手段 |
|---|---|---|---|
| P1 | 正确性优先于性能 | 优化不得改变可观测语义；批量、gather、池化路径跑同一套 conformance。 | Debug 与 Release 同一断言；fake/inproc/TCP-loopback 三后端。 |
| P2 | 显式优于隐式 | wire 显式小端；固定宽度整数；禁止结构体 `memcpy`、位域、未对齐 `reinterpret_cast`；RAII 所有权；每个 API 标注 `noexcept`、移动、拷贝。 | §7.7 的 `static_assert`；UBSan 对齐检查；审查清单。 |
| P3 | 可验证 | 每条保证对应一个具名测试（§19.2）；每个数字可用工具链与环境复现。 | CI 要求测试存在且通过。 |
| P4 | 收敛 | 状态转移只用 CAS；单调 `index`/`seq`；FIFO 去重；墓碑；终态与到达顺序无关。 | 乱序、重复、延迟注入；watch 两序测试。 |
| P5 | 单向权威 | 每个状态变量只有一个写者（§1.2）；非权威写入被拒绝。 | `test_single_writer`；TSan。 |
| P6 | 端到端正确 | 幂等在端点完成；传输层 ACK 只是优化。 | 重试三次，handler 只执行一次。 |
| P7 | 失败可见 | 每次丢弃都有 `rpc_code`、指标、带 trace id 的日志；禁止空 catch。 | 丢弃计数测试；静态扫描。 |
| P8 | 可移植 | 不假设宿主序、64 位、`long` 宽度、`char` 有符号；编译器扩展必须有 fallback。 | MSVC x86+x64；GCC/Clang；大端字节流单测。 |
| P9 | 资源确定 | 池与队列有界；关闭幂等；generation 校验。 | LSan；活跃计数断言；长稳。 |
| P10 | 简单 | 只保留正确所需的最小接口；新增抽象必须证明必要性。 | 设计评审；`io` 只有两个虚函数。 |
| P11 | 未知输入 fail-closed | 未知帧类型、版本、能力位、枚举、wire type、Content-Type 一律拒绝。 | fuzz 加手工非法帧。 |
| P12 | 禁止墙钟排序 | deadline 用相对值；顺序只用 `stream_id`、`lane_seq`、注册中心 `index`；熔断用单调时钟。 | 时钟回拨测试；静态检查排序路径中的 `system_clock::now()`。 |
| P13 | 平台分期 | 阶段一：x86/x64 的 Windows/Linux；阶段二：ARM64、大端、Android、macOS。协议从第一天就与平台无关。 | 阶段一编译矩阵；阶段二复用同一套测试。 |
## 1. 术语与权威源矩阵 <a id="s1"></a>
### 1.1 术语
| 术语 | 定义 |
|---|---|
| **连接（connection）** | 具体传输对象，拥有一个 socket 或一对进程内队列。提供 `source`、`sink`、`close()`、`is_open()`、`remote()`、`resume_read()`。 |
| **通道（channel）** | 逻辑上独立流控的字节流。v1 每连接一个通道；HTTP/2 每个 h2 连接一个通道。`channel` 不是公开类型；旧的 channel 抽象已删除（A §3.5）。 |
| **分片（shard）** | 一个 OS 线程，加一个 corocc LOOP runtime、一个 epoll/IOCP、一个 MPSC 收件箱、一个定时轮。它是亲和性与单线程所有权的单位（R2）。 |
| **有序通道（lane）** | 由 `lane_id` 标识的逻辑串行域，作用域为 `(client_id, target_service_id)`。携带 `lane_seq`，可选串行执行（A §20）。 |
| **调用槽（call_slot）** | 预分配、带 generation 的单次在途调用记录：状态、引用计数、信号量、结果、stream id、响应缓冲、`then` 存储。 |
| **流（stream）** | 连接内的一次 RPC 或流式实例，由连接局部 `stream_id` 标识（31 位，永不复用）。 |
| **订阅（subscription）** | 把 `identity` 与 topic 绑定到有界推送队列、版本、过期时间与可选 lane 的记录。 |
| **实例（instance）** | 已注册的服务端点：`(service, instance_id, endpoint, weight, health, incarnation, index)`。 |
| **节点（node）** | 承载分片、客户端与可选注册中心权威的进程身份（`node_id`，UUIDv4）。 |
| **观察索引（watch index）** | 注册中心的单调修订号。快照与 watch 事件都携带它，是注册中心状态唯一的排序锚（A §28.9）。 |
### 1.2 单写者权威源矩阵
| 状态 | 唯一写者 | 读者 | 禁止 |
|---|---|---|---|
| 连接状态（`open/closing/closed`） | 所属分片线程 | 其他分片原子读 | 跨分片写 |
| `call_slot.state` | 首个 CAS 成功者（分片、定时器、取消者） | 任意线程只读 | 直接赋值 |
| `stream_id` 分配 | 每连接的分片单写者，单调递增 | 无 | 多写者、复用 |
| `lane_seq` | lane 所属分片在 MPSC 入队时 `fetch_add` | 服务端校验 | 两处分配 |
| 注册中心目录 | 权威分片 | 所有分片读不可变快照 | 多节点直写 |
| 熔断状态 | 每个 key 的 CAS 成功者 | 所有调用方 | 非 CAS 修改 |
| 健康状态 | 权威分片上的健康检查器 | 客户端读快照 | 客户端本地改写 |
| 配置快照 | reload 线程 `atomic_store` | 分片 `atomic_load` | 原地修改 |
| 幂等去重表 | 持有 handler 的分片 | 无 | 多分片共写 |
| 订阅表 | 持有 identity 映射的分片 | 发布路径经 MPSC | 跨分片直接修改 |
## 2. 分层与目录结构 <a id="s2"></a>
### 2.1 限界上下文
| 上下文 | 职责 | 允许依赖 |
|---|---|---|
| `core` | `span`、`buffer`、`rpc_code`、`rpc_error`、`rpc_result`、endpoint、配置原语 | 无 |
| `io` | `source`、`sink`、`completion`、`io_result`、`connection`、`backend_factory` | `core` |
| `transport` | epoll、IOCP、inproc、listener、定时器、MPSC | `core`、`io` |
| `wire` | 帧编解码、varint、握手、流控、帧压缩 | `core` |
| `codec` | `codec_registry`、crpc-binary、RTTI 格式适配 | `core`、`wire`（仅帧元数据） |
| `rpc` | `call<t>`、`call_context`、client、server、方法注册、stub、push hub | `core`、`io`、`wire`、`codec` |
| `http` | HTTP/1.1、HTTP/2、HPACK、TLS、路由、绑定、multipart、SSE、problem+json | `core`、`codec`、`rpc`（仅 context） |
| `cluster` | 节点、peer 池、注册中心、发现、负载均衡 | `core`、`rpc` |
| `resilience` | 流水线、熔断、限流、隔离舱、重试、健康 | `core`、`rpc` |
| `observe` | AOP 链、trace、日志、指标 | `core`、`rpc` |

规则：`core` 不依赖任何上下文；`io` 只依赖 `core`；不得包含未列出的上下文头文件；`transport` 不包含 `rpc`；`http` 不包含 `cluster`；`observe` 只通过 `core` 中定义的接口接收数据。
### 2.2 目录布局
```
include/crpc/
  core/types.hpp            span, endpoint, duration, deadline, ids
  core/error.hpp            rpc_code, rpc_error, rpc_result
  core/buffer.hpp           buffer, small_vector
  core/config.hpp           config loading and validation
  io.hpp                    io::io_result, completion, source, sink
  backend.hpp               connection, listener, backend_factory, options
  wire/frame.hpp            frame_header, frame_type, flags, codec
  wire/varint.hpp           put_varint, get_varint, zigzag
  wire/handshake.hpp        client_hello, server_hello, settings TLV
  wire/flow.hpp             window accounting
  rpc/call.hpp              call<t>, call_slot, when_all
  rpc/call_context.hpp      call_context, call_options, metadata
  rpc/client.hpp            client, connection pool, retry driver
  rpc/server.hpp            server, shard, handler pool
  rpc/method.hpp            method registry, method_traits, invoke
  rpc/push.hpp              push_hub, push_channel, subscription
  http/http1.hpp            HTTP/1.1 parser and serializer
  http/http2.hpp            h2 frame layer, stream state machine
  http/hpack.hpp            static/dynamic table, Huffman
  http/tls.hpp              OpenSSL context, SNI, ALPN, session cache
  http/router.hpp           route table, matcher
  http/binding.hpp          DTO binding from route/query/header/form/body
  http/multipart.hpp        streaming multipart parser
  http/sse.hpp              event stream writer
  codec/registry.hpp        codec enable/canonical mapping
  codec/binary.hpp          crpc-binary encode/decode
  codec/rtti_bridge.hpp     JSON/XML/INI/TOML/protobuf/FlatBuffers adapters
  cluster/node.hpp          node identity and lifecycle
  cluster/registry.hpp      catalog, KV, watch, ACL
  cluster/balance.hpp       instance selector, SWRR
  resilience/pipeline.hpp   fixed six-stage pipeline
  observe/trace.hpp         trace_context, traceparent
  observe/log.hpp           SPSC logger
  observe/metrics.hpp       counters and histograms
src/
  transport/epoll_backend.cpp
  transport/iocp_backend.cpp
  transport/inproc_backend.cpp
  transport/timer_wheel.cpp
  transport/mpsc.cpp
  wire/frame.cpp
  wire/handshake.cpp
  rpc/server.cpp
  rpc/client.cpp
  rpc/push.cpp
  http/http1.cpp
  http/http2.cpp
  http/hpack.cpp
  http/tls.cpp
  http/router.cpp
  http/binding.cpp
  http/multipart.cpp
  codec/binary.cpp
  codec/rtti_bridge.cpp
  cluster/registry.cpp
  cluster/discovery.cpp
  resilience/pipeline.cpp
  observe/log.cpp
tests/
  unit/ integration/ conformance/ fuzz/ golden/ long_run/
scripts/
  build.ps1 build.sh test.ps1 test.sh ci.ps1 ci.sh
bench/
  bench_rpc.cpp bench_http.cpp bench_codec.cpp
```
## 3. 传输后端契约 <a id="s3"></a>
### 3.1 精确接口（R1）
```cpp
// include/crpc/io.hpp
#pragma once
#include <cstddef>
#include <system_error>
#include "crpc/core/types.hpp"

namespace crpc {
template <class t> class span;
using byte_span = span<const std::byte>;
class buffer;
struct service_id { std::uint32_t value = 0; };
template <class t> inline constexpr service_id service_id_of{0};
}

namespace crpc::io {
struct io_result {
    std::error_code ec;
    std::size_t bytes = 0;
    [[nodiscard]] bool ok() const noexcept { return !ec; }
};
class completion {
public:
    completion() = default;
    virtual ~completion() = default;
    completion(const completion&) = delete;
    completion& operator=(const completion&) = delete;
    virtual void complete(io_result r) noexcept = 0;   // exactly once, any thread, non-blocking, no throw
};
class source {
public:
    virtual ~source() = default;
    // Backend -> framework. Returns consumed bytes. 0 = pause reading.
    // The framework copies or parses every consumed byte before returning.
    virtual std::size_t on_data(span<const std::byte> data) noexcept = 0;
};
class sink {
public:
    virtual ~sink() = default;
    // Framework -> backend. The backend writes all bytes, then calls complete() exactly once.
    // The span stays valid until complete() returns.
    virtual void push(span<const std::byte> data, completion* c) noexcept = 0;
};
} // namespace crpc::io
```
`source::on_data` 返回的是信用（credit），不是布尔值：`n == data.size()` 表示全部消费；`0 < n < data.size()` 表示消费前缀并暂停；`0` 表示不消费并暂停。这就是全部背压（backpressure）机制，source 方向没有 completion 对象（R1）。
### 3.2 连接与工厂
```cpp
// include/crpc/backend.hpp
namespace crpc {
struct endpoint {
    std::string host;                 // "127.0.0.1", "::1", or a unix path when scheme == uds
    std::uint16_t port = 0;
    enum class scheme : std::uint8_t { tcp, uds, inproc } kind = scheme::tcp;
};
struct connect_options {
    std::chrono::nanoseconds connect_timeout{std::chrono::seconds(3)};
    std::size_t recv_buffer_bytes = 64 * 1024;
    std::size_t send_buffer_bytes = 64 * 1024;
    bool tcp_no_delay = true;
};
struct listen_options {
    std::size_t backlog = 1024;
    bool reuse_port = true;
    std::size_t recv_buffer_bytes = 64 * 1024;
    std::size_t send_buffer_bytes = 64 * 1024;
};
class accept_sink {
public:
    virtual ~accept_sink() = default;
    virtual void on_accept(std::unique_ptr<class connection> c) noexcept = 0;
};
class connection {
public:
    virtual ~connection() = default;
    virtual io::source& source() noexcept = 0;
    virtual io::sink& sink() noexcept = 0;
    virtual void resume_read() noexcept = 0;   // required by R1 pause semantics; not part of source
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    [[nodiscard]] virtual endpoint remote() const = 0;
};
class listener {
public:
    virtual ~listener() = default;
    virtual void start(accept_sink* sink) = 0;
    virtual void stop() noexcept = 0;
};
class backend_factory {
public:
    virtual ~backend_factory() = default;
    virtual std::unique_ptr<connection> connect(const endpoint& ep, const connect_options& opt) = 0;
    virtual std::unique_ptr<listener> listen(const endpoint& ep, const listen_options& opt) = 0;
};
} // namespace crpc
```
建立、关闭、取消与选项全部落在 `connection`/`backend_factory`，绝不进入 `source`/`sink`。核心 RPC 层只接受 `backend_factory&`，不存在 `if (is_tcp)`。
### 3.3 契约不变量
| # | 不变量 | 验证 |
|---|---|---|
| I1 | 每次 `completion::complete` 恰好执行一次，覆盖成功、失败、取消。 | fake 后端计数；1000 次随机关闭竞态。 |
| I2 | completion 可在任意线程执行。 | 线程 id 断言允许提交线程或其他线程。 |
| I3 | completion 不得阻塞、不得抛异常。 | `noexcept`；debug 持锁断言。 |
| I4 | `close()` 后不再交付 source、不再接受新的 sink push；在途 sink completion 仍恰好一次并以 `operation_canceled` 完成（R9）。 | 关闭竞态测试。 |
| I5 | 每方向至多一个在途 sink `push`。 | 后端断言 `in_flight_sink <= 1`。 |
| I6 | 部分写由后端补完，框架绝不重提尾部。 | 短写 socket 测试。 |
| I7 | source span 在 `on_data` 返回前有效；sink span 在 `complete` 返回前有效。 | ASan 加毒化缓冲。 |
| I8 | 任一时刻每连接只有一个读者和一个写者。 | debug 原子 owner 断言。 |
| I9 | `on_data` 返回值不超过 `data.size()`。 | debug 断言；conformance。 |
| I10 | `on_data` 返回 0 或短计数后，后端保留未消费尾部，只在 `resume_read()` 后重新交付。 | 1 字节信用测试。 |
### 3.4 部分读与恢复路径
`on_data` 消费前缀时，后端必须把剩余字节保存在稳定缓冲中，不得覆盖。框架返回 0 期间，后端不得用同一尾部空转调用。`resume_read()` 是唯一恢复信号，幂等，只允许分片线程调用。
```cpp
void epoll_connection::resume_read() noexcept {
    if (!read_paused_) return;
    read_paused_ = false;
    post_local([this] { read_loop(); });   // self-post: ET gives no new edge for a retained tail
}
```
IOCP 下，`resume_read()` 仅在保留尾部为空时重新投递 `WSARecv`；否则先交付尾部，再投递。

错误传播：`source` 没有错误返回。读错误会关闭连接，框架观察到关闭/EOF 后按映射的 `rpc_code` 终结所有在途调用。`sink` 错误通过 `completion::complete({ec, bytes})` 交付，`bytes` 为出错前已写字节数，同时把连接置为 closing。任何错误都不得以静默丢弃字节的方式表达。
### 3.5 背压层级
| 层级 | 机制 | 超限行为 |
|---|---|---|
| 内核接收 | `on_data` 返回 0，后端停读 | TCP 窗口关闭 |
| 连接发送环 | 有界环（默认 256 KiB） | 空间不足前不调用 `sink::push`；调用方协程挂起 |
| 每连接在途调用 | 默认 1024 | 立即 `rpc_code::overload` |
| 每分片在途调用 | 默认 65536 | 立即 `overload` |
| 全局在途调用 | 默认 1,000,000 | 立即 `overload` |
| MPSC 收件箱 | 默认 65536 项 | 立即 `overload` |
| lane 队列 | 默认 1024 项 | 按 `lane_full_policy` 返回 `overload` 或挂起 |

默认策略是快速失败而非排队。排队必须显式开启，且等待时间计入 deadline。
### 3.6 inproc 后端等价性
```cpp
namespace crpc {
struct inproc_faults {
    std::uint32_t connect_reject_pct = 0;
    std::chrono::nanoseconds connect_delay{0};
    std::uint32_t drop_pct = 0;
    std::uint32_t reset_pct = 0;
    bool half_open = false;
    std::uint32_t reorder_pct = 0;
    std::uint32_t duplicate_pct = 0;
    std::uint32_t seed = 0;
};
struct inproc_options {
    std::size_t max_queued_bytes = 64 * 1024;
    bool serialize = true;
    inproc_faults faults;
};
class inproc_pair {
public:
    static std::pair<std::unique_ptr<connection>, std::unique_ptr<connection>>
    make(const inproc_options& opt);
};
} // namespace crpc
```
要求：每方向有界队列；交付投递到目标分片，绝不在调用方栈上递归；默认 `serialize = true`，使半包、粘包、EOF 与错误传播与 TCP 一致；所有故障注入可由 `seed` 复现；`drop_pct` 只作用于 `UNRELIABLE` 帧，绝不影响可靠调用。
## 4. 线程与协程模型 <a id="s4"></a>
### 4.1 分片布局
```
n_shards = clamp(physical_cores - 1, 1, 32)
per shard: 1 OS thread
         + 1 coro_rt_t created in LOOP mode (R2)
         + 1 epoll set or IOCP
         + 1 MPSC inbox
         + 1 timer wheel
         + 1 doorbell (eventfd / PostQueuedCompletionStatus)
         + 64 handler coroutines with 64 KiB stacks
threads = n_shards + 1 acceptor (Windows) + blocking pool (default 0)
```
实现状态：分片数按 **opt-in** 应用。默认仍为 `shards = 1`；`server::builder::shards_auto()`（以及 `shards_auto(physical_cores)`）经 `rpc::default_shard_count()` 设置分片数。多分片 server 不能 `publish`（返回 `unsupported_configuration`），也不能提供 push-control 订阅（返回 `unavailable`），因此自动分片与 PUSH 互斥。per-shard 的 IOCP/epoll 循环所有权与 `SO_REUSEPORT` 仍为**目标、未实现**：每个网络后端仍拥有自己的循环/泵线程。

协程在 M:N 意义上多于线程，但一个分片的 runtime 只由一个线程 pump。`coro_rt_pump` 拒绝第二个并发 pump（`pump_active`，`[READ:third_party/corocc/src\scheduler\rt_driver.c:458]`），LOOP runtime 只有一个 worker（`[READ:third_party/corocc/src\scheduler\rt.c:804]`）。handler 协程是长期存活的工作者，不是每请求一个：这避开 256 KiB 默认栈（`[READ:third_party/corocc/include\corocc\coro.h:38]`）与 256 项默认尺寸栈缓存（`[READ:third_party/corocc/src\scheduler\rt.h:167]`）。
### 4.2 runtime 构造
```cpp
struct shard_driver_ops {
    static std::uint64_t now(void* ctx) noexcept {
        return static_cast<shard*>(ctx)->monotonic_ns();
    }
    static void wake(void* ctx) noexcept {
        static_cast<shard*>(ctx)->wake_loop();
    }
    static int timer_arm(void* ctx, void* req, std::uint64_t deadline,
                         coro_driver_timer_fn fn, void* arg) noexcept {
        return static_cast<shard*>(ctx)->timers().arm(req, deadline, fn, arg) ? 0 : -1;
    }
    static int timer_cancel(void* ctx, void* req) noexcept {
        return static_cast<shard*>(ctx)->timers().cancel(req) ? 0 : -1;
    }
};
void shard::start() {
    coro_rt_driver_ops ops{};
    ops.struct_size = sizeof(ops);
    ops.flags = CORO_DRIVER_FLAG_LOOP;
    ops.now = &shard_driver_ops::now;
    ops.wake = &shard_driver_ops::wake;
    ops.timer_arm = &shard_driver_ops::timer_arm;
    ops.timer_cancel = &shard_driver_ops::timer_cancel;
    coro_rt_driver_register("crpc-shard", &ops, this, &driver_);
    coro_rt_driver_set_default(driver_);
    rt_ = coro_rt_create(1);
    for (std::uint32_t i = 0; i < handler_workers; ++i) {
        coro_attr_t attr = coro_attr_default();
        attr.stack_size = 64 * 1024;
        coro_rt_spawn(rt_, &attr, &shard::handler_main, this);
    }
}
```
API 证据：`coro_rt_create` `[READ:third_party/corocc/include\corocc\coro_rt.h:299]`；LOOP 标志 `[READ:third_party/corocc/include\corocc\coro_rt_driver.h:46]`；驱动注册 `[READ:third_party/corocc/include\corocc\coro_rt_driver.h:146]`；默认驱动 `[READ:third_party/corocc/include\corocc\coro_rt_driver.h:185]`；pump `[READ:third_party/corocc/include\corocc\coro_rt_driver.h:258]`；`coro_attr_default` `[READ:third_party/corocc/include\corocc\coro.h:149]`；`coro_rt_spawn` `[READ:third_party/corocc/include\corocc\coro_rt.h:339]`；信号量 `[READ:third_party/corocc/include\corocc\coro_rt.h:631]`、`:639`、`:654`；销毁时拒绝存活协程 `[READ:third_party/corocc/include\corocc\coro_rt.h:304]`。
### 4.3 主循环
```cpp
void shard::run() {
    while (!stopping_) {
        int n = poll_io(io_events_, 256, next_timeout_ns());
        for (int i = 0; i < n; ++i) dispatch_io(io_events_[i]);
        timers_.run_expired(monotonic_ns());
        drain_inbox();
        coro_rt_pump(rt_, 256);
        if (stopping_ && coro_rt_pending(rt_) == 0) break;
    }
}
```
`next_timeout_ns()` 取以下最早者：定时轮 deadline、keepalive 扫描、注册中心租约扫描、1 秒。Linux 上精确 deadline 由注册在同一 epoll 集合中的 timerfd 承载；timerfd 已 armed 时 `epoll_wait` 超时为无限。Windows 上精确 deadline 由高精度可等待定时器承载，到期向 IOCP 投递完成（R3）。
### 4.4 handler 池
reader 协程只负责读取、分帧与投递完整消息，绝不内联调用用户 handler。完整请求进入分片就绪队列，空闲 handler 协程取出并调用用户函数。handler 挂起只占用一个 handler 协程，不占用 reader，也不占用线程。
```cpp
void shard::handler_main(void* self) {
    auto& s = *static_cast<shard*>(self);
    for (;;) {
        handler_task task{};
        if (!s.ready_.try_pop(task)) {
            coro_rt_sem_wait(s.rt_, &s.ready_sem_);
            continue;
        }
        s.invoke(task);                       // user handler; may suspend
        s.finish(task);
    }
}
```
handler 并发上限为 `handler_workers`（每分片默认 64，可配）。长时间不挂起的 CPU 密集 handler 会占满一个 worker；这是已知边界，v1.1 的 `handler_executor` 是卸载出口。
### 4.5 跨线程唤醒路径
唯一跨线程路径：生产者推入目标分片的 MPSC 收件箱，置 `wake_pending`，再敲一次门铃。目标分片在自己的循环中取件并 pump 自己的 runtime。任何线程都不得对另一个分片的 runtime 调用 `coro_rt_spawn` 或 `coro_rt_pump`。
```cpp
bool shard::post(task t) noexcept {
    if (!inbox_.try_push(std::move(t))) return false;
    bool expected = false;
    if (wake_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        wake_loop();                          // eventfd write or PostQueuedCompletionStatus
    }
    return true;
}
```
`coro_rt_sem_post` 可在任意线程调用，并通过驱动 wake 回调发出活动信号（`[READ:third_party/corocc/src\scheduler\rt_sync.c:469]`、`[READ:third_party/corocc/src\scheduler\rt.c:208]`）。分片 A 上完成一个属于分片 B 的调用时，不得触碰 B 的 runtime，必须投递到 B。
### 4.6 关停顺序
1. `listener.stop()`：不再 accept。
2. 广播 `GOAWAY`；停止接受新调用（`shutting_down`）。
3. 在 `grace_period`（默认 5 秒）内 drain 在途调用。
4. 取消剩余调用并发送 `RST_STREAM`；在途 completion 恰好一次以 `operation_canceled` 完成。
5. 关闭所有连接：取消 I/O，等待两个在途操作，再关闭句柄。
6. 持续 pump 直到 `coro_rt_pending(rt) == 0`，设硬超时；超时则取消 scope 再 pump。
7. `coro_rt_destroy(rt)`；返回 `CORO_EBUSY` 表示仍有协程存活，重复第 6 步。
8. runtime 销毁后再注销驱动；驱动仍被 attach 时注销返回 `CORO_EBUSY`（`[READ:third_party/corocc/include\corocc\coro_rt_driver.h:151]`）。
9. 释放 epoll/IOCP、timerfd、eventfd 与 MPSC 存储。
### 4.7 定时精度
```cpp
class timer_wheel {
public:
    using callback = void (*)(void* arg) noexcept;
    bool arm(void* key, std::uint64_t deadline_ns, callback fn, void* arg) noexcept;
    bool cancel(void* key) noexcept;
    [[nodiscard]] std::uint64_t next_deadline_ns() const noexcept;   // UINT64_MAX if empty
    void run_expired(std::uint64_t now_ns) noexcept;
};
```
Linux：`timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)`，`timerfd_settime` 使用 `TFD_TIMER_ABSTIME`，加入 epoll 集合。可用时允许 `epoll_pwait2` 替代。Windows：`CreateWaitableTimerEx(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)`，`SetWaitableTimerEx` 使用 100 ns 单位的绝对 `LARGE_INTEGER`，`RegisterWaitForSingleObject` 的回调向分片 IOCP 投递完成。不得照抄 runtime 示例的 0.5 ms 轮询：LOOP 模式不暴露最近 deadline，因此 CRPC 必须自建定时轮（A §8.1，D16）。
## 5. epoll ET-only 设计 <a id="s5"></a>
### 5.1 注册
```cpp
epoll_event ev{};
ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
ev.data.u64 = make_token(slot_index, generation);
epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
// listener
epoll_event lev{};
lev.events = EPOLLIN | EPOLLET;
lev.data.u64 = k_listener_token;
epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &lev);
```
token 为 `u64`：`token = static_cast<std::uint64_t>(slot_index) * k_slot_stride + generation`。禁止把 32 位索引左移 32 位，这在 32 位目标上是 UB（A §4.1）。`EPOLLERR` 与 `EPOLLHUP` 无需注册，内核总会报告。
### 5.2 事件分发顺序
每个事件：校验 token 与 generation；先处理 `EPOLLERR`/`EPOLLHUP`；标记 `EPOLLRDHUP`；处理 `EPOLLIN`；处理 `EPOLLOUT`；若半关闭且读写完，优雅关闭。顺序固定，避免 `IN` 处理清空发送环后 `OUT` 边沿被浪费（E4）。
```cpp
for (int i = 0; i < n; ++i) {
    const std::uint64_t tok = events[i].data.u64;
    connection* c = lookup_alive(tok);
    if (c == nullptr) continue;
    const std::uint32_t e = events[i].events;
    if (e & (EPOLLERR | EPOLLHUP)) { c->fail(std::errc::connection_aborted); continue; }
    if (e & EPOLLRDHUP) c->peer_half_closed_ = true;
    if (e & EPOLLIN) c->read_loop();
    if (e & EPOLLOUT) c->write_loop();
    if (c->peer_half_closed_ && c->read_done() && c->write_done()) c->close_graceful();
}
```
### 5.3 读循环（R1 信用）
```cpp
void epoll_connection::read_loop() noexcept {
    if (read_paused_) return;
    for (;;) {
        const ssize_t k = ::recv(fd_, rx_, rx_cap_, 0);
        if (k > 0) {
            const std::size_t used = source().on_data({rx_, static_cast<std::size_t>(k)});
            if (used < static_cast<std::size_t>(k)) {
                const std::size_t tail = static_cast<std::size_t>(k) - used;
                std::memmove(rx_, rx_ + used, tail);
                rx_tail_ = tail;
                read_paused_ = true;
                return;                        // wait for resume_read()
            }
            continue;                          // consumed all; ET permits another recv
        }
        if (k == 0) { peer_eof_ = true; return; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        fail(errno);
        return;
    }
}
```
`resume_read()` 必须先把 `rx_tail_` 交给 `source().on_data`；只有尾部全部消费后才可再次 `recv`。带着保留尾部返回 `epoll_wait` 之所以安全，正是因为 `resume_read()` 会 self-post；ET 不会再次报告这些字节。
### 5.4 写循环（R4）
```cpp
void epoll_connection::write_loop() noexcept {
    std::size_t budget = k_write_budget_bytes;      // 256 KiB per turn
    while (!send_ring_.empty()) {
        auto slice = send_ring_.front_slice();
        if (budget == 0) {
            want_write_ = true;
            post_local([this] { write_loop(); });   // R4: self-post, do not rely on EPOLLOUT
            return;
        }
        const std::size_t n = std::min(budget, slice.size());
        const ssize_t k = ::send(fd_, slice.data(), n, MSG_NOSIGNAL);
        if (k > 0) {
            send_ring_.consume(static_cast<std::size_t>(k));
            budget -= static_cast<std::size_t>(k);
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            want_write_ = true;
            return;                                  // resume only on the next EPOLLOUT edge
        }
        if (k < 0 && errno == EINTR) continue;
        fail(errno);
        return;
    }
    want_write_ = false;
    complete_ready_outputs();                        // one completion per batch
}
```
`sink::push` 先入发送环，若 `want_write_` 为 false 则乐观调用一次 `write_loop()`。这封死 E3：发送环为空时被消耗的边沿不会让后续数据永久滞留。
### 5.5 错误与半关闭处理
| 事件 | 含义 | 动作 |
|---|---|---|
| `EPOLLERR` | socket 错误（RST 等） | 读取 `SO_ERROR`，`fail()`，不再读写 |
| `EPOLLHUP` | 双向挂断 | `fail()`；所有在途 completion 以 `connection_aborted` 完成 |
| `EPOLLRDHUP` | 对端关闭写方向 | 置 `peer_half_closed_`；读到 `recv == 0`；已入队写仍要发完 |
| `EPOLLIN | EPOLLRDHUP` | 数据加 FIN | 先读；读循环结束后再判定 EOF |
| `recv == 0` | EOF | 不再交付 source；发完剩余写；关闭 |
### 5.6 accept
```cpp
void epoll_listener::on_acceptable() noexcept {
    for (;;) {
        const int fd = ::accept4(lfd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) { assign_to_shard(fd); continue; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == ECONNABORTED) continue;
        if (errno == EMFILE || errno == ENFILE) { pause_accept_backoff(); return; }
        log_error(errno);
        return;
    }
}
```
Linux 每个分片一个 listener 并启用 `SO_REUSEPORT`（同时设置 `SO_REUSEADDR`），内核按连接哈希分流，无需跨线程交接。分片数变化需要重建 listener；`SO_REUSEPORT` 的滚动重启可平滑过渡。
### 5.7 ET 丢事件清单
| # | 场景 | 防护 |
|---|---|---|
| E1 | 只读一次就返回，仍有数据 | 循环到 `EAGAIN`；R1 信用下由 `resume_read` 交付尾部 |
| E2 | 恢复后不重新读 | `resume_read` self-post 读循环 |
| E3 | 发送环为空时边沿被消耗 | 入队后乐观调用 `write_loop` |
| E4 | IN 处理清空发送环，OUT 边沿丢失 | 固定分发顺序；OUT 总被执行 |
| E5 | 只 accept 一个连接 | accept 循环到 `EAGAIN` |
| E6 | fd 复用命中迟到事件 | token 带 generation；关闭前 `EPOLL_CTL_DEL` |
| E7 | 未注册 `EPOLLRDHUP` | 必须注册 |
| E8 | 处理函数提前返回，跳过后续事件 | 事件分发 `noexcept`；逐事件 try/catch |
| E9 | `dup`/`fork` 共享 fd | 禁止 `dup` 连接 fd；`CLOEXEC` |
| E10 | eventfd 边沿丢失 | 读 eventfd 到 `EAGAIN` |
| E11 | 未 `EPOLL_CTL_DEL` 就关闭 | 先 DEL 再 close；generation 兜底 |
| E12 | 阻塞 `send` 饿死分片 | 全部 fd 非阻塞；每轮字节预算 |
## 6. IOCP 设计 <a id="s6"></a>
### 6.1 端口与并发度
```cpp
HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
// NumberOfConcurrentThreads = 1: one logical consumer per shard.
// The value limits threads that dequeue packets, not in-flight overlapped operations.
```
每分片一个 IOCP、一个线程。监听 socket 与连接 socket 都通过 `CreateIoCompletionPort(sock, iocp, key, 0)` 绑定。完成键沿用 epoll 的 `(slot_index, generation)` token 方案。
### 6.2 每连接操作
```cpp
struct io_op {
    OVERLAPPED ov{};                 // must be first
    enum class kind : std::uint8_t { read, write, accept, connect, disconnect } k;
    connection* conn;
    std::size_t want;
    std::size_t got;
    std::error_code ec;
    io::completion* c;
    std::byte* buf;
};
```
每连接至多一个读操作、一个写操作。读完成 `got == 0` 为 EOF；否则交给 `source::on_data`。若 `on_data` 消费少于 `got`，保留尾部，并在 `resume_read()` 之前不投递下一次 `WSARecv`。写完成 `sent < want` 时续投剩余；全部写完时 `complete({ec, total})` 恰好一次。
```cpp
OVERLAPPED_ENTRY entries[64];
ULONG n = 0;
if (GetQueuedCompletionStatusEx(iocp, entries, 64, &n, timeout_ms, FALSE)) {
    for (ULONG i = 0; i < n; ++i) dispatch(entries[i]);
}
```
批量大小 64；每轮完成预算为 1024 个完成或 200 微秒，先到者为准，避免定时器与收件箱被饿死。
### 6.3 AcceptEx
```cpp
BOOL ok = AcceptEx(lfd, accept_sock, addr_buf, 0,
                   sizeof(sockaddr_storage) + 16, sizeof(sockaddr_storage) + 16,
                   &got, &op->ov);
```
预投递 64 到 256 个 accept 操作。完成时：`GetAcceptExSockaddrs`，`setsockopt(SO_UPDATE_ACCEPT_CONTEXT)`，把新 socket 绑定到目标分片的 IOCP，随后立即补投下一个 `AcceptEx`。socket 必须以 `WSA_FLAG_OVERLAPPED` 创建。Windows 没有 `SO_REUSEPORT`：v1 使用单 acceptor，接受后通过一次 `PostQueuedCompletionStatus` 交给目标分片；acceptor 不做任何每连接 I/O。
### 6.4 优雅关闭
```cpp
shutdown(sock, SD_SEND);                                  // FIN, still readable
// wait for peer FIN or timeout
DisconnectEx(sock, &op->ov, TF_REUSE_SOCKET, 0);          // overlapped
// completion, then closesocket / reuse
```
关闭顺序固定：`CancelIoEx(sock, nullptr)`；等待两个在途操作各自恰好完成一次；`shutdown`/`DisconnectEx`；`closesocket`；释放槽并递增 generation。存在在途操作时绝不 `closesocket`，否则 `OVERLAPPED` 可能已被释放。
### 6.5 CancelIoEx 竞态
| 竞态 | 结果 | 处理 |
|---|---|---|
| 操作已完成 | `CancelIoEx` 返回 `ERROR_NOT_FOUND` | 走正常完成路径，不重复回调 |
| 取消成功 | 完成返回 `ERROR_OPERATION_ABORTED` | 映射 `operation_canceled`，恰好一次 |
| 取消后数据到达 | 不会再有完成 | 连接进入 closing，停止读取 |
| `closesocket` 与完成并发 | `ERROR_OPERATION_ABORTED` 或 `ERROR_INVALID_HANDLE` | 操作持有连接引用；引用归零后才关闭 |
| 重复 `CancelIoEx` | `ERROR_NOT_FOUND` | 幂等 |

每个 `OVERLAPPED` 只有一个 owner，由完成回调释放。取消只是请求，绝不假设取消后不会再有完成。
### 6.6 错误映射
| Win32 / Winsock | 含义 | 映射 |
|---|---|---|
| `ERROR_OPERATION_ABORTED` (995) | 本地取消 | `std::errc::operation_canceled` |
| `ERROR_CONNECTION_ABORTED` (1236) | 连接中止 | `std::std::errc::connection_aborted` |
| `ERROR_HANDLE_EOF` (38) | 对端关闭 | EOF |
| `WSAECONNRESET` (10054) | RST | `std::errc::connection_reset` |
| `WSAECONNREFUSED` (10061) | 拒绝 | `std::errc::connection_refused` |
| `WSAENETUNREACH` / `WSAEHOSTUNREACH` | 不可达 | `std::errc::host_unreachable` |
| `WSAENOBUFS` | 内核缓冲不足 | 有界退避或 `no_buffer_space` |
| `ERROR_IO_PENDING` | 不是错误 | 继续等待 |
| `ERROR_NOT_FOUND` | 取消目标已完成 | 忽略 |
### 6.7 ET 与 IOCP 语义对齐
| 语义 | epoll-ET | IOCP |
|---|---|---|
| 可读 | `EPOLLIN` 边沿，读到 `EAGAIN` | `WSARecv` 完成 |
| 可写 | `EPOLLOUT` 边沿，写到 `EAGAIN` | `WSASend` 完成 |
| 部分写 | 短 `send`，继续 | 短完成，续投剩余 |
| EOF | `recv == 0` 或 `EPOLLRDHUP` | `got == 0` 或 `ERROR_HANDLE_EOF` |
| RST | `EPOLLERR` 加 `SO_ERROR` | `WSAECONNRESET` |
| 取消 | `close` 加 generation 丢弃 | `CancelIoEx` 加 aborted 完成 |
| 批量 | `epoll_wait` 数组 | `GetQueuedCompletionStatusEx` 数组 |
| 背压 | 停止 `recv` | 不投递下一次 `WSASend`/`WSARecv` |

RPC 层只看到 `source`、`sink`、`connection`，无法分辨底层后端。
## 7. Wire 协议 <a id="s7"></a>
### 7.1 帧头（16 字节，显式小端）
| 偏移 | 宽度 | 字段 | 类型 | 规则 |
|---|---|---|---|---|
| 0 | 4 | `payload_len` | u32 | 小端；`<= max_frame`（默认 16 MiB，硬上限 64 MiB） |
| 4 | 1 | `type` | u8 | 表 7.3；未知即 fail-closed |
| 5 | 1 | `flags` | u8 | 表 7.4 |
| 6 | 2 | `reserved` | u16 | 小端；必须为 0 |
| 8 | 4 | `stream_id` | u32 | 小端；bit31 必须为 0；0 表示连接级 |
| 12 | 4 | `ext` | u32 | 小端；`flags.CRC32C` 时为 CRC32C，否则为 0 |

字节序由 A §30.1 固定，它是 wire 的唯一权威。所有多字节字段逐字节读写；禁止结构体 `memcpy`、位域、未对齐转换。
### 7.2 有序扩展头（16 字节，仅 `flags.ORDERED = 1`）
| 偏移（帧头之后） | 宽度 | 字段 | 类型 |
|---|---|---|---|
| 0 | 8 | `lane_id` | u64 小端 |
| 8 | 8 | `lane_seq` | u64 小端 |

`payload_len` 只计扩展头之后的 payload。`flags.ORDERED` 置位而 payload 小于 16 字节时，以 `protocol_error` 拒绝。`flags.ORDERED` 清零时不存在扩展字节；任何按存在处理的实现都不合规。
### 7.3 帧类型
| 值 | 名称 | stream 作用域 | payload |
|---|---|---|---|
| 0x00 | `DATA` | 非零 | 流式分片或推送体分片 |
| 0x01 | `REQUEST` | 非零 | 调用信封加请求体 |
| 0x02 | `RESPONSE` | 非零 | 状态字节加响应体或错误信封 |
| 0x03 | `ONE_WAY` | 非零 | 调用信封加请求体；必须置 `UNRELIABLE` |
| 0x04 | `RST_STREAM` | 非零 | `rpc_code` u32 小端 |
| 0x05 | `SETTINGS` | 零 | 设置 TLV |
| 0x06 | `SETTINGS_ACK` | 零 | 空 |
| 0x07 | `PING` | 零 | 8 字节不透明数据 |
| 0x08 | `PONG` | 零 | 8 字节不透明数据 |
| 0x09 | `GOAWAY` | 零 | `last_stream_id` u32 加 `rpc_code` u32 |
| 0x0A | `WINDOW_UPDATE` | 零或非零 | `increment` u32 |
| 0x0B | `PUSH` | 非零 | 推送信封加消息体 |
| 0x0C | `PUSH_ACK` | 非零 | `push_seq` u64 |

数值由本文档首次固定，是对 A §9.2、A §23 与 B §4.2.1 并集的整合；修改即 wire 破坏。
### 7.4 flags
| 位 | 名称 | 含义 |
|---|---|---|
| 0 | `END_STREAM` | 本流最后一帧 |
| 1 | `COMPRESSED` | payload 已压缩；算法在 bit5-7 |
| 2 | `CRC32C` | `ext` 携带 payload 的 CRC32C |
| 3 | `ORDERED` | 帧头后跟 16 字节 lane 扩展 |
| 4 | `UNRELIABLE` | 尽力交付，不重传 |
| 5-7 | `ALGO` | 0 无，1 lz4，2 zstd，3 gzip，4-7 保留 |

`COMPRESSED = 0` 时 `ALGO` 必须为 0，否则 `protocol_error`；`ALGO != 0` 而 `COMPRESSED = 0` 同样拒绝。此处以 A §30.1 加 §31.1 解决 A/B 冲突，B §4.2.1 的位分配被取代。
### 7.5 握手
客户端 hello（12 字节前缀加 TLV）：

| 偏移 | 宽度 | 字段 |
|---|---|---|
| 0 | 4 | magic `"CRPC"` |
| 4 | 2 | `version` u16 小端，v1 = 1 |
| 6 | 2 | `flags` u16 小端，必须为 0 |
| 8 | 2 | `tlv_len` u16 小端 |
| 10 | 2 | `reserved` u16 小端，必须为 0 |
| 12 | `tlv_len` | TLV |

服务端 hello 使用同样的 12 字节前缀，但偏移 8 为 `status` u8（0 成功，1 版本不匹配，2 设置非法，3 未授权），偏移 9 保留，`tlv_len` 在偏移 10。

| TLV 类型 | 值 | 编码 |
|---|---|---|
| 0x01 | `max_frame` | u32 小端 |
| 0x02 | `initial_window` | u32 小端 |
| 0x03 | `codec` | u8：0 binary，1 json，2 xml，3 ini，4 toml，5 protobuf，6 flatbuffers |
| 0x04 | `compress_bitmap` | u8：bit0 lz4，bit1 zstd，bit2 gzip |
| 0x05 | `compress_min` | u32 小端 |
| 0x06 | `keepalive_ms` | u32 小端 |
| 0x07 | `max_streams` | u32 小端 |
| 0x08 | `identity` | u8 长度加字节，`<= 128` |
| 0x09 | `resume_token` | u64 小端 |
| 0x0A | `hmac` | 32 字节 |

握手上限 4 KiB，超时 3 秒。未知 TLV 类型、未知 codec、未知能力位、非零 reserved、版本不匹配一律 fail-closed：尽可能回一个非零 status 的服务端 hello，然后关闭（R9）。
### 7.6 payload 信封
调用信封（`REQUEST`、`ONE_WAY`，以及 `RESPONSE` 的成功路径）：
```
service_id      u32 LE
method_id       u32 LE
call_flags      u8
remaining_ns    varint (0 = no deadline)
[trace_id]      16 bytes            if call_flags bit0
[span_id]       8 bytes             if call_flags bit0
[parent_span]   8 bytes             if call_flags bit1
[idem_len]      u8 + bytes          if call_flags bit2, <= 128
[config_ver]    u64 LE              if call_flags bit3
[route_ver]     u64 LE              if call_flags bit3
meta_len        u16 LE + meta TLV
body            remaining bytes
```
`call_flags`：bit0 `has_trace`，bit1 `has_parent`，bit2 `has_idem`，bit3 `has_versions`，bit4 预留给 HLC，bit5-7 必须为 0。

`RESPONSE` payload 以 `status` u8 开头：0 成功，1 错误。成功携带响应体；错误携带 §9.3 的错误信封。

`PUSH` payload：`topic_len` u16、topic 字节、`key_len` u16、key 字节、`push_seq` u64、`idem_len` u8、幂等键字节、消息体。`PUSH_ACK` payload：`push_seq` u64。

meta TLV：重复的 `key_len` u8、key 字节、`value_len` u16、value 字节。key 为 ASCII，value 为不透明字节。meta 总量不超过 1 KiB。
### 7.7 静态断言与编码原语
```cpp
static_assert(CHAR_BIT == 8);
static_assert(sizeof(std::uint8_t) == 1 && sizeof(std::uint16_t) == 2);
static_assert(sizeof(std::uint32_t) == 4 && sizeof(std::uint64_t) == 8);
static_assert(std::numeric_limits<float>::is_iec559);
static_assert(std::numeric_limits<double>::is_iec559);
static_assert(sizeof(void*) == 4 || sizeof(void*) == 8);
namespace crpc::endian {
inline void put_le16(std::byte* p, std::uint16_t v) noexcept;
inline void put_le32(std::byte* p, std::uint32_t v) noexcept;
inline void put_le64(std::byte* p, std::uint64_t v) noexcept;
inline std::uint16_t get_le16(const std::byte* p) noexcept;
inline std::uint32_t get_le32(const std::byte* p) noexcept;
inline std::uint64_t get_le64(const std::byte* p) noexcept;
constexpr bool host_is_little() noexcept;
template <class t> constexpr t to_le(t v) noexcept;
template <class t> constexpr t from_le(t v) noexcept;
}
```
varint 为小端 base-128：低 7 位先出，bit7 为继续位。`u64` 最多 10 字节，第 10 字节只能是 `0x00` 或 `0x01`。zigzag 编码为 `(n << 1) ^ (n >> 63)`，解码为 `(u >> 1) ^ -(u & 1)`，全程无符号运算。浮点先 `bit_cast` 到 `u32`/`u64`，再按小端写。
### 7.8 长度与溢出检查
| 检查点 | 规则 |
|---|---|
| 帧长 | `payload_len <= max_frame`；分配前拒绝 |
| 累加 | 检查 `len <= avail - off`，绝不先算 `off + len` |
| u64 转 `size_t` | `if (v > SIZE_MAX) reject`；x86 必须显式 |
| 元素计数 | `count <= remaining / min_element_size`，防止乘法溢出 |
| 字符串 | `len <= max_string_bytes`（1 MiB） |
| 分配 | 每次 `resize`/`reserve` 前检查上限；失败为 `resource_exhausted` |
| 嵌套 | 编解码深度 32；HTTP 头数量 100；路由段 32 |
### 7.9 golden bytes
`REQUEST`，`payload_len = 16`，`stream_id = 1`，无 flags：
```
10 00 00 00 01 00 00 00 01 00 00 00 00 00 00 00
```
有序变体，`lane_id = 7`，`lane_seq = 9`：
```
10 00 00 00 01 08 00 00 01 00 00 00 00 00 00 00
07 00 00 00 00 00 00 00 09 00 00 00 00 00 00 00
```
`PING`，payload 为 `01 02 03 04 05 06 07 08`：
```
08 00 00 00 07 00 00 00 00 00 00 00 00 00 00 00
01 02 03 04 05 06 07 08
```
这些字节串提交为 `tests/golden/*.bin`；CI 在每个平台断言逐字节相同，并断言解码结果一致。
### 7.10 fail-closed 规则
| 条件 | 动作 |
|---|---|
| 未知 `type` | `GOAWAY(protocol_error)`，关闭 |
| `reserved != 0` | `GOAWAY(protocol_error)`，关闭 |
| `stream_id` bit31 置位 | `GOAWAY(protocol_error)`，关闭 |
| `REQUEST`/`RESPONSE`/`DATA`/`ONE_WAY`/`PUSH` 的 `stream_id == 0` | `GOAWAY(protocol_error)`，关闭 |
| `SETTINGS`/`PING`/`PONG`/`GOAWAY` 的 `stream_id != 0` | `GOAWAY(protocol_error)`，关闭 |
| `ORDERED` 置位但 payload `< 16` | `GOAWAY(protocol_error)`，关闭 |
| `COMPRESSED = 0` 但 `ALGO != 0` | `GOAWAY(protocol_error)`，关闭 |
| `ONE_WAY` 未置 `UNRELIABLE` | `GOAWAY(protocol_error)`，关闭 |
| 重复 `stream_id` | `RST_STREAM(protocol_error)` |
| 未知 `stream_id` 的数据 | 丢弃并计数，不关闭 |
| CRC 不匹配 | `RST_STREAM(protocol_error)` 并计数 |
| `payload_len > max_frame` | `GOAWAY(protocol_error)`，不分配 |
## 8. 消息语义 <a id="s8"></a>
### 8.1 三种消息形态
| 形态 | 标识 | 是否有调用槽 | 完成信号 |
|---|---|---|---|
| 调用（unary） | `stream_id` | 是 | `RESPONSE` 帧 |
| 调用（流式） | `stream_id` | 是 | 带 `END_STREAM` 的 `RESPONSE` |
| 推送 | `(topic, key, push_seq)` | 否（用订阅） | 可靠模式下 `PUSH_ACK` |

调用槽在发送第一个字节前创建，终态后才释放。推送没有调用槽，只有订阅项与重放缓冲。
### 8.2 投递语义 × 顺序语义
| | 可靠 | 不可靠 |
|---|---|---|
| **普通** | 默认 RPC：请求/响应、超时、取消、带幂等键重试、明确失败 | 指标、日志、心跳：`ONE_WAY` 加 `UNRELIABLE`；队列满即丢并计数；无句柄 |
| **有序（lane）** | 命令流：入队/发送/到达严格有序，服务端串行执行，原位重试且 `lane_seq` 与幂等键不变 | 顺序发送但可能丢；出现空洞只上报计数；不重传 |

默认是普通加可靠。有序与不可靠必须显式开启。v1 不单独提供可靠单向消息：可靠单向用响应体为空的 `REQUEST` 表达（A §19.1）。`ONE_WAY` 永远是不可靠语义。
### 8.3 ACK、重传与去重
TCP 已提供连接内的有序可靠字节交付。CRPC 不为调用重新实现逐帧 ACK、累积窗口或 RTO。跨连接丢失与 handler 失败由 RPC 层处理：

| 场景 | 机制 |
|---|---|
| 调用成功 | `RESPONSE` 帧即应用层确认 |
| 调用重试 | 新 attempt、新 `stream_id`、同一幂等键；服务端去重表返回缓存结果 |
| 可靠推送 | `PUSH` 加 `PUSH_ACK`；每订阅保留 256 条重放缓冲；客户端按 `push_seq` 去重 |
| 不可靠消息 | 无 ACK、无重试，丢弃并计数 `crpc_unreliable_dropped_total{reason}` |

幂等键（A §28.3）：
```
idempotency_key = { client_id (16 bytes), client_epoch (u64), seq (u64) }
```
`client_id` 按进程持久化；`client_epoch` 每次启动递增；`seq` 按 `(client_id, target_service)` 递增。天然业务键可以替代三元组。禁止用随机 UUID 作为幂等键。该三元组就是单调去重锚：同一 epoch 内 `seq` 永不回退，服务端因此可以 FIFO 淘汰并按序号识别重放，无需比较墙钟。

服务端去重表：每客户端一个环，默认 256 项，键为 `(client_id, client_epoch, seq)`；只允许 FIFO 淘汰，禁止整体清空。`fresh` 执行；`in_progress` 让调用方挂起到完成或超时；`done` 直接返回缓存结果，不重复执行。v1 去重表只在内存中，因此恰好一次仅在一个权威 epoch 内成立（A §28.3）。
### 8.4 有序 lane
- lane 作用域为 `(client_id, target_service_id)`；`lane_id` 由客户端分配，跨重连稳定。
- `lane_seq` 只在 MPSC 入队时由 `fetch_add` 分配一次，不回绕。
- 同一 lane 的帧同一时刻只走一条连接；重连期间 lane 暂停，直到新连接建立。
- 服务端 `ordered_execution = true` 时每 lane 一个 FIFO，同时只有一个 handler 在途。
- 重试原位进行：同 lane、同 `lane_seq`、同幂等键。重试未完成期间不发送后续序号。
- 默认失败后继续；`fail_fast` 取消该 lane 剩余项。
### 8.5 deadline 传播
wire 只携带 `remaining_ns`，绝不携带绝对时间戳（P12）。
```
client:   deadline_abs = monotonic_now + timeout
          remaining_ns = deadline_abs - monotonic_now
server:   if remaining_ns <= min_remaining (1 ms) -> do not execute, return deadline_exceeded
          downstream: remaining_ns' = remaining_ns - elapsed - safety_margin (1 ms)
```
handler 不得在 deadline 之后继续执行。`call_context::remaining()` 是权威值；`derive()` 扣除已用时间并把当前 span 设为 parent。
### 8.6 取消
`call<t>::cancel()` 与 deadline 定时器都调用 `call_slot::try_finish`，终态分别为 `canceled` 与 `deadline_exceeded`。CAS 成功者发送 `RST_STREAM`；失败者读取终态后什么都不做。终态之后到达的响应被丢弃并计数，绝不会交给已回收的槽（generation 校验）。
### 8.7 推送 API
```cpp
enum class delivery_mode : std::uint8_t { reliable, unreliable };
enum class overflow_policy : std::uint8_t { drop_oldest, drop_newest, disconnect };
struct identity { std::array<std::byte, 16> bytes; };
struct subscribe_options {
    std::string key;
    std::size_t max_queued = 1024;
    overflow_policy on_overflow = overflow_policy::drop_oldest;
    delivery_mode mode = delivery_mode::reliable;
};
struct subscription_stats {
    std::uint64_t subscriptions = 0;
    std::uint64_t dropped = 0;
};
class subscription {
public:
    subscription() noexcept = default;
    subscription(subscription&&) noexcept;
    subscription& operator=(subscription&&) noexcept;
    subscription(const subscription&) = delete;
    ~subscription();                     // unsubscribe
    [[nodiscard]] bool valid() const noexcept;
    void unsubscribe() noexcept;
};
class push_hub {
public:
    void publish(std::string_view topic, std::string_view key,
                 byte_span payload, delivery_mode mode) noexcept;
    void publish_to_identity(const identity& who, std::string_view topic,
                             byte_span payload, delivery_mode mode) noexcept;
    [[nodiscard]] subscription_stats stats() const noexcept;
};
class push_channel {
public:
    [[nodiscard]] subscription subscribe(std::string_view topic,
                                         void (*on_msg)(byte_span, void*) noexcept,
                                         void* user, subscribe_options opt = {});
    [[nodiscard]] rpc_error unsubscribe(std::string_view topic) noexcept;
};
```
推送目标是逻辑身份，不是连接。服务端在权威分片上维护 `identity -> connection*` 映射。断开的身份保留订阅 30 秒以支持恢复。可靠推送使用 `PUSH_ACK` 加每订阅 256 条重放缓冲；不可靠推送在溢出时丢弃并计数。慢客户端绝不阻塞分片：发送环高水位策略生效。
### 8.8 流式
```cpp
class stream_reader {
public:
    virtual ~stream_reader() = default;
    [[nodiscard]] virtual rpc_result<std::size_t> read(byte_span dst) = 0;
};
class stream_writer {
public:
    virtual ~stream_writer() = default;
    [[nodiscard]] virtual status write(byte_span src) = 0;
    [[nodiscard]] virtual status finish() = 0;
};
```
流映射为 `DATA` 帧加 `END_STREAM`；HTTP/1.1 映射为 chunked 或 Content-Length；HTTP/2 映射为 DATA 加流控窗口。`RST_STREAM` 取消，取消时释放流槽、订阅与任何临时文件。
## 9. 错误模型 <a id="s9"></a>
### 9.1 `rpc_code`（严格采用 R6）
```cpp
enum class rpc_code : std::uint16_t {
    ok = 0, canceled = 1, deadline_exceeded = 2, unavailable = 3,
    connection_refused = 4, connection_reset = 5, broken_pipe = 6, eof = 7,
    overload = 8, not_found = 9, no_such_method = 10, invalid_argument = 11,
    failed_precondition = 12, resource_exhausted = 13, protocol_error = 14,
    decode_error = 15, encode_error = 16, not_acknowledged = 17,
    unknown_side_effect = 18, internal = 19, shutting_down = 20
};
```
数值即声明顺序，永久稳定。B §14.1 的更大表被 R6 取代；不得在没有新设计决定的情况下增加、改名或重排任何码。
### 9.2 `rpc_error` 与 `rpc_result`
```cpp
using metadata_map = std::vector<std::pair<std::string, std::string>>;
struct rpc_error {
    rpc_code code = rpc_code::ok;
    std::string message;
    bool retryable = false;
    std::chrono::milliseconds retry_after{0};
    metadata_map metadata;
};
struct status {
    rpc_code code = rpc_code::ok;
    [[nodiscard]] bool ok() const noexcept { return code == rpc_code::ok; }
};
template <class t>
class rpc_result {
public:
    rpc_result(t v) : v_(std::move(v)) {}
    rpc_result(rpc_error e) : v_(std::move(e)) {}
    [[nodiscard]] bool ok() const noexcept { return v_.index() == 0; }
    [[nodiscard]] t& value() & { return std::get<0>(v_); }
    [[nodiscard]] const t& value() const& { return std::get<0>(v_); }
    [[nodiscard]] t&& value() && { return std::get<0>(std::move(v_)); }
    [[nodiscard]] rpc_error& error() & { return std::get<1>(v_); }
    [[nodiscard]] const rpc_error& error() const& { return std::get<1>(v_); }
private:
    std::variant<t, rpc_error> v_;
};
```
`rpc_result` 不要求 `t` 可默认构造（D6）。枚举不得直接充当错误对象（R6）。错误路径允许分配，成功路径不允许。`rpc_result<void>` 特化为只携带 `rpc_error`；`call<void>` 用它表达可靠单向调用。
### 9.3 错误 wire 信封
```
RESPONSE payload:
  status          u8 = 1
  code            varint
  retryable       varint (0 or 1)
  retry_after_ms  varint
  message_len     u16 LE + UTF-8 bytes (<= 512)
  meta_len        u16 LE + meta TLV (<= 1024)
```
meta 键：`crpc.raw_code`（未来未知码）、`trace_id`、`rtti_code`、`field`、`upstream_service`、`http.status`。`rtti::error_code` 只是本地细节，绝不作为 RPC 错误域发送；它映射为 `decode_error`/`encode_error` 并附带 `rtti_code`。

截断：`message` 截到 512 字节并加省略号；meta 超过 1 KiB 时按 key 顺序丢弃；协议帧绝不截断，超限在发送前拒绝。
### 9.4 HTTP 状态码映射
| `rpc_code` | HTTP | 说明 |
|---|---|---|
| `ok` | 200 / 201 / 204 | 取决于动词与 body |
| `canceled` | 499 | 非标准；也可直接断开 |
| `deadline_exceeded` | 504 | |
| `unavailable` | 503 | |
| `connection_refused` | 503 | |
| `connection_reset` | 503 | |
| `broken_pipe` | 503 | |
| `eof` | 503 | |
| `overload` | 429 | 带 `Retry-After` |
| `not_found` | 404 | |
| `no_such_method` | 501 | |
| `invalid_argument` | 400 | |
| `failed_precondition` | 412 | |
| `resource_exhausted` | 429 | |
| `protocol_error` | 502 | 上游协议违规 |
| `decode_error` | 请求 400 / 响应 500 | 按方向 |
| `encode_error` | 500 | |
| `not_acknowledged` | 503 | |
| `unknown_side_effect` | 504 | 绝不宣称"未执行" |
| `internal` | 500 | 固定标题，不泄露内部细节 |
| `shutting_down` | 503 | |

无 `rpc_code` 的框架级状态：405（路径命中但动词不允许，带 `Allow`）、406（无可接受 codec）、413（body 过大）、415（Content-Type 或编码不支持）、431（头过大）。错误体为 RFC 7807 `application/problem+json`，含 `type`、`title`、`status`、`detail`、`instance`、`crpc_code`。
## 10. 序列化 <a id="s10"></a>
### 10.1 codec 注册表
```cpp
namespace crpc::codec {
class registry {
public:
    static registry& instance();
    [[nodiscard]] status enable(std::string_view content_type, bool on);
    [[nodiscard]] bool enabled(std::string_view content_type) const noexcept;
    [[nodiscard]] std::string_view canonical(std::string_view ct) const noexcept;
};
template <class t>
struct binding {
    std::string_view content_type;
    rpc_code (*decode)(byte_span, t&);
    rpc_code (*encode)(const t&, buffer&);
};
template <class t>
inline constexpr binding<t> request_codecs[] = {
    {"application/crpc", &binary_decode<t>, &binary_encode<t>},
    {"application/json", &json_decode<t>, &json_encode<t>},
    {"application/xml", &xml_decode<t>, &xml_encode<t>},
    {"application/ini", &ini_decode<t>, &ini_encode<t>},
    {"application/toml", &toml_decode<t>, &toml_encode<t>},
    {"application/x-protobuf", &proto_decode<t>, &proto_encode<t>},
    {"application/x-flatbuffers", &flat_decode<t>, &flat_encode<t>}
};
} // namespace crpc::codec
```
注册表只负责开关与规范化 Content-Type，不做类型擦除。每个 DTO 在编译期实例化自己的绑定数组。请求按 `Content-Type` 选择，无匹配为 415；响应按 `Accept` 的 q 值协商，无匹配为 406，除非配置了 `fallback_identity`。
### 10.2 crpc-binary 布局
payload 体是字段序列：
```
key   = varint((field_id << 3) | wire_type)
value = wire_type-specific
```
| `wire_type` | 含义 | 编码 |
|---|---|---|
| 0 | varint | bool、int32/64、uint32/64、enum |
| 1 | fixed64 | double、fixed64 |
| 2 | length-delimited | string、bytes、嵌套消息、packed repeated |
| 3 | zigzag varint | sint32/sint64（CRPC 扩展） |
| 5 | fixed32 | float、fixed32 |

`field_id` 为声明序号加一，与 `fields_of<t>::members` 顺序一致，编译期已知。`4`、`6`、`7` 非法，产生 `decode_error`。

容器：数值序列 packed；其他序列 length-delimited 并逐元素编码；map 为重复 entry，key 字段 1、value 字段 2。可选字段有值才写。未知字段按 wire_type 跳过；未知 wire_type 拒绝。
### 10.3 RTTI 接线
热路径编码直接使用成员指针，无虚调用、无 DOM：
```cpp
template <class t, class tuple_t, std::size_t... i>
rpc_code encode_members(const t& v, const tuple_t& m, buffer& out,
                        std::index_sequence<i...>) {
    rpc_code ec = rpc_code::ok;
    ((ec = ec == rpc_code::ok
               ? encode_field(static_cast<std::uint32_t>(i + 1), v.*std::get<i>(m), out)
               : ec), ...);
    return ec;
}
template <class t>
rpc_code binary_encode(const t& v, buffer& out) {
    using members_t = typename rtti::fields_of<t>::members_type;
    return encode_members(v, rtti::fields_of<t>::members, out,
                          std::make_index_sequence<std::tuple_size_v<members_t>>{});
}
```
`fields_of<t>::members` 是成员指针的 `std::tuple`，常量初始化（`[READ:third_party/rtti/include\rtti\reflection.hpp:5449]`）。冷路径（HTTP 绑定、管理面、调试）可以使用 `field_access_base::set_value`/`get_value`（`[READ:third_party/rtti/include\rtti\reflection.hpp:451]`、`:461`、`:463`）。

派生类型被 crpc-binary 以 `static_assert` 拒绝：`RTTI_DEFINE_TYPE_DERIVED` 把基类字段放在 `members` 之外（D12，`[READ:third_party/rtti/include\rtti\reflection.hpp:5490]`）。文本格式仍可通过虚调用路径编码派生类型。
### 10.4 文本与 schema 格式
| 格式 | 解码 | 编码 | 证据 |
|---|---|---|---|
| JSON | `json::parse` 后 `rtti::from_value`（保留偏移） | `rtti::to_value` 后 `json::serialize(document)` | `[READ:third_party/rtti/include\rtti\json.hpp:170]`、`:174`、`[READ:third_party/rtti/include\rtti\convert.hpp:1354]`、`:1414` |
| XML | `xml::parse_as<t>` | `rtti::to_value` 后 `xml::serialize(document)` | `[READ:third_party/rtti/include\rtti\xml.hpp:220]`、`:216` |
| INI | `ini::parse_as<t>` | `rtti::to_value` 后 `ini::serialize(document)` | `[READ:third_party/rtti/include\rtti\ini.hpp:346]`、`:320` |
| TOML | `toml::parse_as<t>` | `rtti::to_value` 后 `toml::serialize(document)` | `[READ:third_party/rtti/include\rtti\toml.hpp:297]`、`:271` |
| protobuf | `rtti::proto::parse_as<t>(bytes, cfg)` | `rtti::proto::serialize<t>(obj, out)` | `[READ:third_party/rtti/include\rtti\proto.hpp:2700]`、`:2679` |
| FlatBuffers | `rtti::flat::parse_as<t>(bytes, cfg)` | `rtti::flat::tree_to_wire(&doc, rtti::flat::schema_of<t>(), out, cfg)` | `[READ:third_party/rtti/include\rtti\flat.hpp:346]`、`:290` |

文本格式必须两段式编码：便捷函数 `serialize<T>` 在转换失败时返回空串（D9，`[READ:third_party/rtti/include\rtti\json.hpp:222]`）。JSON 必须两段式解码：`parse_as` 丢失字节偏移（D8，`[READ:third_party/rtti/include\rtti\json.hpp:193]`）。

protobuf 与 FlatBuffers 要求用户显式特化 `rtti::proto::schema<t>`（`[READ:third_party/rtti/include\rtti\proto.hpp:432]`）或 `rtti::flat::schema<t>`（`[READ:third_party/rtti/include\rtti\flat.hpp:198]`）。主模板为空，未注册类型会静默编码成零字节。因此 CRPC 必须要求 `schema_hash`，并在这些格式上对 schema count 为零的类型拒绝注册（D13）。FlatBuffers 的类型化编码经过 DOM，不宣称零拷贝（D14）。
### 10.5 限制、字节序与错误映射
| 限制 | 默认 | 超限 |
|---|---|---|
| 二进制嵌套深度 | 32 | `decode_error` |
| string/bytes | 1 MiB | `decode_error` |
| 帧 payload | 16 MiB | `protocol_error` |
| 文本文档 | 8 MiB | `resource_exhausted` |
| RTTI 格式深度 | 32（可配） | `decode_error` |

所有整数与浮点 wire 值显式小端；文本格式为无 BOM 的 UTF-8，且与 locale 无关。二进制布局没有对齐与填充要求：每个字段逐字节编解码，读者不做对齐加载，写者不插入填充。

`rtti::error_code`（`[READ:third_party/rtti/include\rtti\error.hpp:15]`）在输入侧映射为 `rpc_code::decode_error`，输出侧映射为 `encode_error`，原始码写入 `metadata["rtti_code"]`。`strict_schema = true` 时 schema 不匹配为 `failed_precondition`；否则按字段 id 继续解码并计数。
## 11. 服务 API 与 stub <a id="s11"></a>
### 11.1 声明宏（R8）
```cpp
#define CRPC_ROUTE(path_literal) \
    inline static constexpr ::crpc::http::controller_meta crpc_route_meta{ path_literal }
#define CRPC_SERVICE(controller_t, service_number) \
    template <> inline constexpr ::crpc::service_id ::crpc::service_id_of<controller_t>{ service_number }
#define CRPC_METHOD(controller_t, method, path_literal) \
    inline const ::crpc::rpc::method_registrar<controller_t, &controller_t::method> \
        crpc_method_##controller_t##_##method{ ::crpc::rpc::kind::binary, path_literal, #method, __LINE__ }
#define CRPC_HTTP_GET(controller_t, method, path_literal) \
    inline const ::crpc::rpc::method_registrar<controller_t, &controller_t::method> \
        crpc_get_##controller_t##_##method{ ::crpc::http::verb::get, path_literal, #method, __LINE__ }
#define CRPC_HTTP_POST(controller_t, method, path_literal) \
    inline const ::crpc::rpc::method_registrar<controller_t, &controller_t::method> \
        crpc_post_##controller_t##_##method{ ::crpc::http::verb::post, path_literal, #method, __LINE__ }
#define CRPC_HTTP_PUT(controller_t, method, path_literal) \
    inline const ::crpc::rpc::method_registrar<controller_t, &controller_t::method> \
        crpc_put_##controller_t##_##method{ ::crpc::http::verb::put, path_literal, #method, __LINE__ }
#define CRPC_HTTP_PATCH(controller_t, method, path_literal) \
    inline const ::crpc::rpc::method_registrar<controller_t, &controller_t::method> \
        crpc_patch_##controller_t##_##method{ ::crpc::http::verb::patch, path_literal, #method, __LINE__ }
#define CRPC_HTTP_DELETE(controller_t, method, path_literal) \
    inline const ::crpc::rpc::method_registrar<controller_t, &controller_t::method> \
        crpc_delete_##controller_t##_##method{ ::crpc::http::verb::del, path_literal, #method, __LINE__ }
```
每个动作宏都携带控制器类型与成员，因为 C++17 无法从下一行声明推导成员指针。返回与参数类型由 `decltype(&controller_t::method)` 推导，绝不手写。

字段绑定属性使用 RTTI 属性：
```cpp
#define CRPC_FROM_ROUTE(name)   RTTI_ATTR("FromRoute", name)
#define CRPC_FROM_QUERY(name)   RTTI_ATTR("FromQuery", name)
#define CRPC_FROM_BODY          RTTI_ATTR("FromBody")
#define CRPC_FROM_HEADER(name)  RTTI_ATTR("FromHeader", name)
#define CRPC_FROM_FORM(name)    RTTI_ATTR("FromForm", name)
#define CRPC_FROM_FILE(name)    RTTI_ATTR("FromFile", name)
```
它们写在 `RTTI_DEFINE_FIELD_ATTRS` 内，例如 `RTTI_DEFINE_FIELD_ATTRS(get_user_req, RTTI_FIELD_ATTR_ENTRY(id, CRPC_FROM_ROUTE("id")))`。只使用公开 RTTI API（`[READ:third_party/rtti/include\rtti\reflection.hpp:6791]`、`:6766`、`:1142`）。
### 11.2 方法 traits 与调用
```cpp
namespace crpc::rpc {
enum class kind : std::uint8_t { binary, http };
template <class sig> struct method_traits;
template <class ctl_t, class resp_t, class req_t>
struct method_traits<resp_t (ctl_t::*)(const req_t&)> {
    using controller_type = ctl_t;
    using request_type = req_t;
    using response_type = resp_t;
    static constexpr bool has_context = false;
};
template <class ctl_t, class resp_t, class req_t>
struct method_traits<resp_t (ctl_t::*)(const req_t&, call_context&)> {
    using controller_type = ctl_t;
    using request_type = req_t;
    using response_type = resp_t;
    static constexpr bool has_context = true;
};
template <class ctl_t, class resp_t>
struct method_traits<resp_t (ctl_t::*)()> {
    using controller_type = ctl_t;
    using request_type = void;
    using response_type = resp_t;
    static constexpr bool has_context = false;
};
template <class ctl_t, class req_t>
struct method_traits<void (ctl_t::*)(const req_t&)> {
    using controller_type = ctl_t;
    using request_type = req_t;
    using response_type = void;
    static constexpr bool has_context = false;
};
template <class ctl_t, class req_t>
struct method_traits<void (ctl_t::*)(const req_t&, call_context&)> {
    using controller_type = ctl_t;
    using request_type = req_t;
    using response_type = void;
    static constexpr bool has_context = true;
};
template <class ctl_t>
struct method_traits<void (ctl_t::*)(call_context&)> {
    using controller_type = ctl_t;
    using request_type = void;
    using response_type = void;
    static constexpr bool has_context = true;
};
template <class ctl_t, auto method>
struct method_registrar {
    method_registrar(kind k, const char* path, const char* name, std::uint32_t ordinal) noexcept;
    method_registrar(http::verb v, const char* path, const char* name, std::uint32_t ordinal) noexcept;
};
template <class ctl_t, auto method>
status invoke_binary(ctl_t& self, span<const std::byte> in, buffer& out, call_context& ctx);
template <class ctl_t, auto method>
status invoke_http(ctl_t& self, const http::request& req, http::response& resp, call_context& ctx);
} // namespace crpc::rpc
```
`invoke_binary` 解码 `request_type`，调用 `(self.*method)(req, ctx)` 或 `(self.*method)(req)`，再编码 `response_type`；`void` 返回空 body。handler 边界捕获异常并转为 `rpc_error{rpc_code::internal, "internal error"}`。
### 11.3 `call<t>`
```cpp
template <class t>
class call {
public:
    using value_type = t;
    call() noexcept = default;
    call(call&& other) noexcept;
    call& operator=(call&& other) noexcept;
    call(const call&) = delete;
    call& operator=(const call&) = delete;
    ~call();                                                   // unfinished: cancel and recycle
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] rpc_result<t> get();                         // parks in a coroutine; blocks elsewhere
    [[nodiscard]] bool try_get(rpc_result<t>& out) noexcept;
    template <class fn_t> void then(fn_t&& fn);
    void cancel() noexcept;
};
template <class... calls_t>
auto when_all(calls_t&&... calls)
    -> call<std::tuple<typename std::decay_t<calls_t>::value_type...>>;
```
句柄为 16 字节：`call_slot*` 加 `std::uint32_t generation`。槽来自启动期预分配的每分片 freelist。`then` 存储函数指针加用户指针，或不超过 32 字节的小对象；热路径绝不使用 `std::function`（R7）。回调在拥有连接的分片上、下一轮 pump 执行，绝不在 `try_finish` 内、也绝不在注册者栈上执行。

`get()` 行为：协程内挂起在 `coro_rt_sem_wait`；非分片线程阻塞在条件变量；分片线程但没有协程上下文时返回 `rpc_error{rpc_code::failed_precondition}`，并置 `metadata["reason"] = "would_block"`，绝不阻塞（A §18.6）。
### 11.4 `call_context`
```cpp
struct trace_id { std::array<std::byte, 16> bytes; };
struct span_id { std::array<std::byte, 8> bytes; };
struct lane_token { std::uint64_t lane_id = 0; std::uint64_t seq = 0; };
class call_context {
public:
    [[nodiscard]] std::chrono::nanoseconds remaining() const noexcept;
    [[nodiscard]] bool expired() const noexcept;
    [[nodiscard]] trace_id trace() const noexcept;
    [[nodiscard]] span_id span() const noexcept;
    [[nodiscard]] span_id parent_span() const noexcept;
    [[nodiscard]] std::uint64_t config_version() const noexcept;
    [[nodiscard]] std::uint64_t route_version() const noexcept;
    [[nodiscard]] std::string_view idempotency_key() const noexcept;
    [[nodiscard]] std::optional<lane_token> lane() const noexcept;
    [[nodiscard]] std::string_view metadata(std::string_view key) const noexcept;
    [[nodiscard]] std::uint32_t attempt() const noexcept;
    [[nodiscard]] bool canceled() const noexcept;
    void set_response_metadata(std::string_view key, std::string_view value);
    [[nodiscard]] call_context derive() const;
};
```
`call_context` 每次调用一份，只可移动，按引用传给 handler，handler 返回后失效。它绝不使用 `thread_local`，因为协程会在执行上下文之间迁移（B §4.5）。
## 12. HTTP/1.1、HTTP/2、HTTPS <a id="s12"></a>
### 12.1 同端口分流
```
accept -> peek first bytes ->
  TLS: ALPN "h2" -> HTTP/2; ALPN "http/1.1" -> HTTP/1.1
  cleartext "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" -> h2c
  otherwise -> HTTP/1.1
```
两种协议共用路由、绑定层、handler 池、`call_context` 与容错流水线。TLS 必须使用 ALPN：只提供不支持协议的客户端被拒绝，绝不降级到明文。
### 12.2 HTTP/1.1
| 项 | v1 规则 |
|---|---|
| keep-alive | 默认开启；`Connection: close` 关闭；HTTP/1.0 需显式 `keep-alive` |
| pipelining | 接受但串行处理；响应顺序等于请求顺序 |
| chunked | 支持解码；长度未知时用于编码 |
| Content-Length | 有则使用；无且非 chunked 为 400 |
| CL 与 TE 同时出现 | 400 并关闭（请求走私防护，不做 TE 优先） |
| 多个或非法 CL | 400 并关闭 |
| `Transfer-Encoding: chunked, gzip` | 400；不支持 TE 链 |
| 100-continue | 路由与尺寸检查通过后才发 `100 Continue`；超限为 413 且不发 100 |
| trailers | 解析并通过 `call_context` 暴露；可选 |
| header 上限 | 16 KiB；超限 431 |
| body 上限 | 默认 8 MiB；超限 413 |
| Host | HTTP/1.1 必填；缺失 400 |
| 解析 | 严格 RFC 9112；拒绝裸 LF 与冒号前空格 |
| HEAD | 执行 GET handler 并丢弃 body |
### 12.3 HTTPS
通过 OpenSSL 支持 TLS 1.2 与 1.3。ALPN 宣告 `h2` 与 `http/1.1`。SNI 选择证书上下文；证书热更新原子替换 `shared_ptr<SSL_CTX>`，旧连接用旧上下文走完，新连接用新上下文。服务端会话缓存开启。OCSP stapling 与客户端证书不在 v1 范围。TLS 层压缩禁用。
### 12.4 HTTP/2
必须实现最小正确集（R5），不存在"简化 h2"。

| 领域 | 要求 |
|---|---|
| 帧层 | 9 字节头；DATA/HEADERS/PRIORITY/RST_STREAM/SETTINGS/PUSH_PROMISE/PING/GOAWAY/WINDOW_UPDATE/CONTINUATION |
| HPACK | 61 项静态表、动态表与大小更新、Huffman、整数与字符串原语 |
| 流控 | 连接与流两级窗口、`WINDOW_UPDATE`、初始 65535、`SETTINGS_INITIAL_WINDOW_SIZE` |
| 流状态 | idle、reserved、open、half-closed local、half-closed remote、closed；非法转移为连接错误 |
| 设置 | `HEADER_TABLE_SIZE`、`ENABLE_PUSH`、`MAX_CONCURRENT_STREAMS`、`INITIAL_WINDOW_SIZE`、`MAX_FRAME_SIZE`、`MAX_HEADER_LIST_SIZE`；必须 ACK |
| 错误 | 流错误用 `RST_STREAM`，连接错误用 `GOAWAY`，错误码正确 |
| CONTINUATION | 必须紧跟 `HEADERS`/`PUSH_PROMISE`，中间不得插入其他帧 |
| 限制 | `MAX_FRAME_SIZE` 默认 16384，`MAX_CONCURRENT_STREAMS` 默认 100，header list 16 KiB |
| 互操作 | 发布前 h2spec 必测章节全绿 |

两层流控彼此独立：h2 层拥有连接与流窗口，CRPC 层拥有二进制协议自己的连接与流窗口，二者不共享。h2 `stream_id` 与 CRPC `stream_id` 是独立空间；适配层维护 `h2_stream_id -> call_slot*`。
```
h2 HEADERS :method POST, :path /svc/method, content-type application/crpc
  -> router lookup -> call_slot created with an internal stream_id
h2 DATA frames -> REQUEST body bytes
RESPONSE bytes -> h2 HEADERS (:status) + DATA frames
h2 RST_STREAM -> call_slot try_finish(canceled)
h2 GOAWAY -> no new streams; existing streams drain or cancel
```
HTTP/2 头名必须小写。`x-crpc-*` 是普通头，绝不进入伪头。`x-crpc-trace-id`、`x-crpc-timeout-ms`、`x-crpc-idempotency-key`、`x-crpc-attempt`、`x-crpc-lane-id`、`x-crpc-lane-seq` 在两个 HTTP 版本上语义一致。
### 12.5 状态与错误映射
HTTP 状态按 §9.4 选择。CRPC 生成的 4xx/5xx 错误体统一为 `problem+json`。网关或拦截器可设置 `metadata["http.status"]` 产生 401、403、409、422，因为 R6 未定义这些码；该桥接列入 §22 未决项。
## 13. MVC 与 RESTful <a id="s13"></a>
### 13.1 路由表
```cpp
namespace crpc::http {
enum class verb : std::uint8_t { get, post, put, patch, del, head, options };
struct controller_meta {
    const char* prefix;
    constexpr explicit controller_meta(const char* p) noexcept : prefix(p) {}
};
struct request {
    verb method = verb::get;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    byte_span body;
};
struct response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    buffer body;
};
struct route_entry {
    const char* path;
    const char* name;
    verb method;
    std::uint32_t ordinal;
};
struct action_record {
    std::string pattern;
    std::string name;
    verb method;
    void* controller;
    const std::type_info* controller_type;
    status (*invoke_binary)(void*, span<const std::byte>, buffer&, call_context&);
    status (*invoke_http)(void*, const request&, response&, call_context&);
    std::uint64_t schema_hash;
};
struct match_result;

class http_router {
public:
    static http_router& instance();
    [[nodiscard]] status build();
    [[nodiscard]] match_result match(verb, std::string_view path, std::string* allow) const noexcept;
};
} // namespace crpc::http
```
`route_entry` 常量初始化，存放在静态存储。`build()` 在 `main` 之后运行，检测冲突、绑定控制器实例、构建精确表与参数表。同一 `(verb, normalized_pattern)` 的两条记录是 `route_conflict`，启动中止并打印双方名称与 ordinal。有路由但未注册实例的控制器同样中止启动。
### 13.2 匹配与绑定
v1 规模下，路由树表示为精确匹配表加预排序参数表；trie 只是实现选项，前提是保持相同的优先级与冲突规则。

| 步骤 | 规则 |
|---|---|
| 归一化 | 去掉查询串；折叠一个尾斜杠（`/users/` 变 `/users`；`/` 保留） |
| 精确表 | 无 `:` 或 `*` 的路径进 `unordered_map<string, action_record*>` |
| 参数表 | 线性扫描，按段数降序、静态段数降序预排 |
| 优先级 | 静态段 > `:param` > `*` |
| 大小写 | 默认敏感；`case_insensitive_routes` 可开启 |
| 百分号解码 | 逐段解码；拒绝 `%2F` 与 `%00`；非法转义为 400 |
| 查询串 | `&` 分隔，`+` 为空格，百分号解码，重复键成为列表 |
| 动词不符 | 405，带 `Allow` |
| 未命中 | 404 |

DTO 绑定遍历 `rtti::fields<req>()`（`[READ:third_party/rtti/include\rtti\reflection.hpp:884]`）并读取字段属性。`FromRoute`、`FromQuery`、`FromHeader`、`FromForm` 取对应来源；`FromBody` 或无属性取 body 文档。必填字段缺失为 400 并给出字段名；`std::optional` 缺失留空；未知 body 键默认忽略，`strict_body = true` 时为 400。整数解析使用 `std::from_chars` 且必须消费全部字符；枚举先查 `rtti::enum_table<t>` 标签，再按数值（`[READ:third_party/rtti/include\rtti\reflection.hpp:1837]`）。转换失败为 400，`crpc_code = invalid_argument`。
### 13.3 响应
| handler 返回值 | 默认 |
|---|---|
| `t` | 200 加 body；POST 201，DELETE 200 |
| `void` | 204，无 body |
| `rpc_result<t>` | ok 为 200；错误按 §9.4 映射 |
| `created<t>(location, body)` | 201 加 `Location` |
| `no_content()` | 204 |
| 抛出 `rpc_exception` | 使用其 `rpc_code`；其他异常为 500 `internal` |

`Accept` 协商选择响应 codec，无匹配为 406。`HEAD` 执行 GET handler 并丢弃 body。错误体为 `problem+json`，`instance` 为请求路径。
### 13.4 上传
`multipart/form-data` 增量解析，body 绝不整体缓冲。

| 限制 | 默认 |
|---|---|
| part 数 | 32 |
| 单 part | 64 MiB |
| 上传总量 | 256 MiB |
| part 头 | 8 KiB |
| 落盘阈值 | 1 MiB |

解析状态：`preamble`、`part_header`、`part_body`、`epilogue`。正文扫描保留 `boundary_len + 4` 字节回退，内容中出现边界前缀时不会误切分。畸形输入（缺 boundary、part 未闭合、非法 CRLF）为 400 并关闭连接。超过落盘阈值的内容写入配置根目录下的 `O_EXCL` 临时文件；`file_part` 在析构、取消或错误时删除。客户端文件名净化后仅作元数据，磁盘名随机。`Expect: 100-continue` 只在路由与尺寸检查通过后兑现；超限或不支持直接 413/415，不发 100。
```cpp
struct file_part {
    std::string name;
    std::string filename;
    std::string content_type;
    std::uint64_t size = 0;
    std::filesystem::path temp_path;
    std::shared_ptr<stream_reader> reader;
};
```
### 13.5 下载与 SSE
下载使用 `file_response{path, content_type, allow_range}`。发送 `Content-Length` 与 `Accept-Ranges: bytes`，按 64 KiB 分块，绝不整文件读入。`Range: bytes=a-b` 返回 206 加 `Content-Range`；非法或不可满足返回 416。响应被压缩时忽略 Range（§14.4）。

SSE 响应使用 `text/event-stream`、`Cache-Control: no-cache`、`X-Accel-Buffering: no`。事件格式为 `id: <index>`、`event: <name>`、`data: <json>`、空行；心跳为 `: ping`，每 15 秒一次。重连使用 `Last-Event-ID`；服务端从 1024 条有界缓冲重放，超出则先发 `event: reset` 再发全量快照。慢客户端写阻塞超过 30 秒即断开并释放订阅。
## 14. 压缩 <a id="s14"></a>
### 14.1 HTTP 内容编码
| 项 | v1 |
|---|---|
| 请求 `Content-Encoding` | gzip、deflate；其他 415 |
| 响应协商 | `Accept-Encoding` 按 q 值；gzip 优先，接受 deflate |
| 最小尺寸 | 1 KiB；更小不压缩 |
| 头 | 正确设置 `Content-Encoding`；`Vary: Accept-Encoding` |
| chunked | 先压缩再分块，顺序固定 |
| 嵌套 | 只允许一层编码；多层为 415 |
| `identity;q=0` 且无替代 | 406，除非配置 `fallback_identity` |

压缩与解压是流式 `stream_reader`/`stream_writer` 装饰器，绝不整体缓冲。背压从下游写传递到上游读。
### 14.2 CRPC 帧级压缩
`flags.COMPRESSED` 加 `flags.ALGO` 选择 lz4（1）或 zstd（2）；gzip（3）仅用于兼容。握手 `compress_bitmap` 取双方交集。`compress_min` 默认 512 字节，更小的 payload 不压缩。压缩按帧 payload 进行，批量写不跨帧合并。压缩失败即 fail-closed：关闭连接，绝不静默回退明文。

流控按压缩后的线上字节计数；`WINDOW_UPDATE` 归还的也是压缩后字节，两端口径一致。
### 14.3 大消息
超过 `max_frame` 的 payload 分帧，除最后一帧外 `END_STREAM = 0`。每片独立压缩，压缩前不超过 64 KiB。接收端逐片解压、逐片交付。
### 14.4 解压炸弹防护
| 限制 | CRPC 帧 | HTTP |
|---|---|---|
| 解压后总量 | 64 MiB | 64 MiB |
| 单帧 | 16 MiB | body 8 MiB |
| 压缩率 | 1000:1 | 100:1 |
| CPU 时间 | 每帧 5 ms 软上限 | 计入请求 deadline |
| 嵌套编码 | 1 | 1 |

超限返回 `resource_exhausted` 或 `protocol_error` 并关闭连接（CRPC），或返回 413/400 并关闭（HTTP）。存在 `Content-Encoding` 时忽略 Range：返回 200 与完整压缩实体，且不发送 `Accept-Ranges`。
## 15. 容错与治理 <a id="s15"></a>
### 15.1 流水线
六个固定阶段，启动期组装，运行期只读：
```cpp
enum class resilience_stage : std::uint8_t {
    rate_limit, bulkhead, breaker, load_balance, retry, deadline
};
class resilience_policy {
public:
    virtual ~resilience_policy() = default;
    [[nodiscard]] virtual rpc_error admit(call_context& ctx) noexcept = 0;
    virtual void on_result(call_context& ctx, const rpc_error& e) noexcept = 0;
};
struct rate_limit_config {
    std::uint32_t rate_per_second = 10000;
    std::uint32_t burst = 1000;
};
struct bulkhead_config {
    std::uint32_t max_concurrent = 1024;
};
struct breaker_config {
    std::chrono::seconds window{10};
    std::uint32_t min_requests = 50;
    double failure_ratio = 0.5;
    double slow_ratio = 0.5;
    std::chrono::nanoseconds slow_threshold{std::chrono::microseconds(200)};
    std::uint32_t half_open_max_probes = 3;
    std::uint32_t recovery_successes = 5;
    std::chrono::seconds open_duration{5};
};
struct retry_config {
    std::uint32_t max_attempts = 3;
    std::chrono::nanoseconds base_backoff{std::chrono::milliseconds(2)};
    std::chrono::nanoseconds max_backoff{std::chrono::milliseconds(200)};
    double jitter = 0.2;
    double budget_ratio = 0.1;
    bool retry_on_connection_error = true;
    bool retry_on_timeout = false;
};
using fallback_fn = rpc_error (*)(call_context&, const rpc_error&) noexcept;
struct resilience_config {
    rate_limit_config rate;
    bulkhead_config bulkhead;
    breaker_config breaker;
    retry_config retry;
    fallback_fn fallback;
    std::chrono::nanoseconds default_deadline{std::chrono::seconds(1)};
    bool fail_fast_on_open = true;
};
```
`admit` 顺序为 `rate_limit -> bulkhead -> breaker -> load_balance -> deadline`；`on_result` 逆序。`retry` 包裹单次 attempt，并在每次 attempt 前重新执行 `load_balance`。成功路径总开销预算为 35-65 ns（A §27.8）。
### 15.2 熔断器
```
closed --(failure_ratio >= threshold and requests >= min_requests)--> open
open --(open_duration elapsed)--> half_open
half_open --(recovery_successes consecutive successes)--> closed
half_open --(any probe failure)--> open (reset duration, exponential backoff)
```
| 参数 | 默认 |
|---|---|
| 窗口 | 10 秒，十个 1 秒桶 |
| `min_requests` | 50 |
| `failure_ratio` | 0.5 |
| `slow_ratio` | 0.5 |
| `slow_threshold` | 200 微秒 |
| `half_open_max_probes` | 3 |
| `recovery_successes` | 5 |
| `open_duration` | 5 秒，指数退避到 60 秒 |
| 粒度 | 方法级与实例级开启；服务级与连接级关闭 |

统计为每分片 `alignas(64)` 桶，relaxed 原子自增；桶轮转只用单调时间（P12）。状态转移用 CAS，CAS 成功者执行副作用。`probes_left` 是原子令牌，因此半开最多三个探测。熔断打开时禁止重试。失败统计按逻辑调用最终结果计，不按每次重试计。
### 15.3 限流、隔离舱、降级、重试
| 策略 | 默认 | 超限 |
|---|---|---|
| 客户端令牌桶 | 每 `(target, method)`，容量与速率可配 | `resource_exhausted` 加 `retry_after` |
| 服务端令牌桶 | 每方法与每连接 | `resource_exhausted` |
| 隔离舱（bulkhead） | 每服务、每分片 | 协程挂起到剩余 deadline，然后 `overload` |
| 过载保护 | 每连接 1024、每分片 65536、全局 1,000,000 在途 | 立即 `overload` |
| 降级（fallback） | 每方法一个函数，只在最终失败后调用 | 结果标记 `degraded=true` |
| 重试 | 最多 3 次，基数 2 ms，上限 200 ms，抖动 20%，预算 10% | 停止重试，返回最后一次错误 |

只有幂等调用允许重试（显式标志或幂等键）。`deadline_exceeded` 默认不重试，因为副作用可能已发生。重试 attempt 必须落在剩余 deadline 内。降级函数在把缓存值写入调用的响应缓冲后返回 `ok`；其他返回码替换原始错误。
### 15.4 健康、负载均衡、节点互调
主动健康检查每 5 秒一次（TCP connect、HTTP `/health` 或 RPC `Ping`）；被动健康来自熔断与连接失败。恢复中的实例权重从 1 按连续成功次数升到配置值。

选择顺序：同数据中心、健康、熔断关闭，然后平滑加权轮询。实例级熔断打开时通过原子快照替换把实例移出端点集合。

节点间按需建立连接，没有全连接。节点互调就是发往 `crpc.registry` 服务的普通 CRPC 调用，不是第二套协议。节点身份是持久化的 UUIDv4。
### 15.5 注册中心（Consul 形态，单权威）
| 操作 | HTTP | CRPC 方法 |
|---|---|---|
| 注册/续约 | `PUT /v1/agent/service/register` | `register` |
| 注销 | `PUT /v1/agent/service/deregister/:id` | `deregister` |
| 健康通过/失败 | `PUT /v1/agent/check/pass/:check_id` | `heartbeat` |
| 目录 | `GET /v1/catalog/services`、`GET /v1/catalog/service/:name` | `query` |
| 健康列表 | `GET /v1/health/service/:name?passing=true` | `query` |
| KV 读写/CAS/删除 | `GET/PUT/DELETE /v1/kv/:key` | `kv_get`、`kv_put`、`kv_cas` |
| watch | `GET /v1/watch/kv/:key?index=N&wait=5m` | `watch` |

数据模型：`service_instance{node, endpoint, service, instance_id, tags, version, weight, incarnation, index, health, lease_expiry}`。`index` 每次变更递增，是唯一排序锚。TTL 租约按 `ttl/3` 续约；过期标记 `critical`，再过一个 TTL 删除，并保留 2×TTL 的墓碑。

写入权威只有一个节点。非权威写入返回 `unavailable` 并带 `metadata["leader"]`；R6 没有 `not_leader` 码，此点列入 §22。读使用不可变快照，通过 `shared_ptr<const catalog_snapshot>` 的 `atomic_load`/`atomic_store` 发布；权威不可用时读停留在最后快照（fail-static），写失败（fail-closed）。

KV 值在 HTTP 与 wire 上都是 base64。`cas=N` 仅在 `modify_index == N` 时成功；`cas=0` 表示仅创建；冲突为 409。ACL 使用静态 token 表加读写前缀策略；`require_token=true` 时缺 token 为 401，权限不足为 403。二进制通道执行同样的检查。

watch 使用长轮询：`index == current` 时 handler 协程挂起到变更或等待超时。超时返回 200 与当前值、index 不变，不是 504。HTTP/1.1 每连接最多一个 watch；全局 watch 上限 10k。

两序收敛：快照 index 为 `S`，事件 index 为 `E`。`E <= S` 丢弃；`E == S + 1` 应用；`E > S + 1` 在有界缓存中等待并触发快照拉取。状态绝不回退。
## 16. 可观测性 <a id="s16"></a>
### 16.1 AOP
```cpp
enum class hook_point : std::uint8_t {
    client_before_call, client_after_call, client_encode_before, client_encode_after,
    server_before_call, server_after_call, server_decode_before, server_decode_after,
    conn_open, conn_close, conn_error
};
class interceptor {
public:
    virtual ~interceptor() = default;
    [[nodiscard]] virtual bool on_hook(hook_point p, call_context& ctx) noexcept = 0;
};
class aop_chain {
public:
    void add(hook_point p, interceptor* i) noexcept;
    void freeze() noexcept;
    [[nodiscard]] bool run(hook_point p, call_context& ctx) noexcept;
};
```
`run` 首行为 `if (hooks_[p].empty()) return true;`，未使用切面约 1 ns。链在开始服务前冻结；拦截器启动期构造，钩子内不得分配或阻塞。钩子内异常被捕获并转为 `internal`。
### 16.2 链路追踪
trace 上下文为 16 字节 `trace_id`、8 字节 `span_id`、8 字节 `parent_span_id` 加 flags。HTTP 使用 W3C `traceparent`（`00-<32 hex>-<16 hex>-<2 hex>`）；CRPC 在调用信封中携带同样字段。父子关系由发送/接收对建立，绝不用时间戳。头部采样默认 1%，在调用发起时决定一次。Baggage 上限 8 KiB，绝不参与路由。导出器异步、有界队列 8192，溢出丢弃并计数。未配置导出器时零分配。
### 16.3 日志
热路径：栈上格式化加每分片 8192 项 SPSC 环；无 `malloc`、无锁、无系统调用、无 `std::string`。格式为 `{}` 风格。级别过滤为编译期 `CRPC_LOG_MAX_LEVEL` 加运行期原子。单个后台线程批量 `writev`/`WriteFile`，按大小或时间轮转，写 UTF-8。队列溢出丢弃并递增 `crpc_log_dropped_total`。结构化字段包含 trace id、span id、service、method。字段名含 `token`、`secret`、`password`、`key` 时默认拒绝写入。开启时目标成本不超过 150 ns/条。
### 16.4 指标
计数器：`crpc_calls_total{service,method,code}`、`crpc_call_duration_ns` 直方图、`crpc_unreliable_dropped_total{reason}`、`crpc_log_dropped_total`、`crpc_retry_attempts_total`、`crpc_breaker_state{key}`、`crpc_breaker_open_total`、`crpc_rate_limited_total`、`crpc_bulkhead_rejected_total`、`crpc_registry_watch_total`、`crpc_push_dropped_total{policy}`。直方图使用固定桶，无分配。
## 17. 内存与性能预算 <a id="s17"></a>
### 17.1 内存
| 项 | 预算 | 构成 |
|---|---|---|
| 空闲连接 | <= 2 KiB | 连接对象 256 B、惰性读缓冲 1 KiB、无写缓冲、表项 64 B |
| 活跃连接 | <= 160 KiB | 读 64 KiB、写 64 KiB、调用表指针 |
| 调用槽 | 128 B | 状态 8、信号量 32、结果 16、缓冲指针 16、stream/lane 16、回调 40 |
| 每分片固定 | <= 8 MiB | 64 个 handler 栈 × 64 KiB = 4 MiB，定时轮、MPSC、统计 |
| 10 万在途调用 | <= 64 MiB | 10 万 × 128 B 加多数为空的缓冲 |
| 1 万空闲连接 | <= 20 MiB | 每个 2 KiB |
| 订阅表 | <= 5 MiB | 10 万项 × 48 B |
| 每 lane 队列 | <= 16 KiB | 1024 项 × 16 B，有界环 |
| 每连接发送环 | <= 256 KiB | 有界，高水位回落 |

所有队列有界。池有上限并在低水位释放内存，不得只涨不落。协程栈 64 KiB 带 guard page；handler 池长期存活，因此 256 项默认尺寸栈缓存不在请求路径上。
### 17.2 性能目标与测量
| 指标 | 目标 | 状态 |
|---|---|---|
| 聚合吞吐 | 500 万 RPC/s | 目标，非门禁（R10） |
| 每连接对，批量 | >= 100 万 RPC/s | 审计主机实测 1.05M |
| p99 延迟 | < 100 微秒 | 目标，必须实测 |
| 跨分片交接 | 批量时每条 15-30 ns | 预算 |
| 64 字节消息编解码 | 编解码各 < 100 ns | 预算 |
| 容错流水线 | 每次成功调用 35-65 ns | 预算 |

基准必须报告消息大小、连接数、线程数、是否批量、工具链、CPU 型号，以及 p50/p99/p999。结果必须使用独立请求计数。禁止把"每 RTT 64 帧"折算成 RPC/s（R10）。测量使用 `bench/bench_rpc.cpp`，预热 10 秒，跑五次 30 秒，取五次中位数。
## 18. 并发正确性 <a id="s18"></a>
### 18.1 锁序
| 级别 | 锁 | 持有时允许获取 |
|---|---|---|
| 1 | `registry_catalog_lock`（权威） | 无 |
| 2 | `config_lock`（reload） | 无 |
| 3 | `shard::inbox_lock`（MPSC 内部） | 无 |
| 4 | `connection::send_ring_lock` | 无（单写者，通常不存在） |
| 5 | `dedup_table_lock` | 无 |
| 6 | 熔断状态（原子，非锁） | 不适用 |

只允许按级别递增获取。持有某分片锁时绝不获取另一个分片的锁。持锁期间禁止 I/O、用户代码、协程挂起、`sem_post`、日志 sink、阻塞分配。无法证明无环的路径使用带超时的 `try_lock` 并显式恢复。
### 18.2 一一配对
| 获取 | 释放 | 回收点 |
|---|---|---|
| freelist 取 `call_slot` | 归还 freelist | 终态且引用计数归零 |
| 连接槽 | 槽释放，generation++ | 关闭完成 |
| 订阅项 | 表删除 | 退订、过期或断连 TTL |
| 定时器节点 | 堆移除 | 调用终态 |
| `sem_wait` | `sem_post` | 由 `try_finish` CAS 恰好一次 |
| sink `push` | `completion::complete` | 写完、出错或取消 |
| 临时上传文件 | `std::filesystem::remove` | `file_part` 析构或取消 |
| 注册实例 | 注销或 TTL 墓碑 | 租约过期 |
### 18.3 状态机收敛
```cpp
enum class call_state : std::uint8_t { idle, pending, completed, canceled, timed_out, conn_lost, recycled };
bool try_finish(call_slot& s, call_state terminal, rpc_error e) noexcept {
    std::uint32_t expected = static_cast<std::uint32_t>(call_state::pending);
    if (!s.state.compare_exchange_strong(expected, static_cast<std::uint32_t>(terminal),
                                         std::memory_order_acq_rel)) {
        return false;
    }
    s.result = std::move(e);
    coro_rt_sem_post(s.rt, &s.sem);
    return true;
}
```
所有终态路径（响应、超时、取消、连接丢失、拒绝）都调用 `try_finish`。首个 CAS 成功，后续路径观察到终态后丢弃。由于 `stream_id` 永不复用，迟到响应只能命中存活槽或终态槽，绝不会命中不同 generation 的回收槽。因此终态与到达顺序无关。
### 18.4 D1-D20 缺陷映射
D1-D15 为 B §8 的 RTTI 集成缺陷；D16-D20 为 A §1.1 D1-D5 的 corocc 集成缺陷。

| ID | 缺陷 | 规避 | 测试 |
|---|---|---|---|
| D1 | 方法没有 attribute | CRPC POD 路由表 | `test_router_conflict` |
| D2 | 每个 TU 重复注册方法 | 按 `(controller type, method name)` 去重 | `test_method_registry` |
| D3 | 属性表动态初始化 | `main` 之后 build/freeze | `test_static_init_order` |
| D4 | `fields<T>()` 访问器动态初始化 | 热路径使用 `fields_of<T>::members` | `test_binary_roundtrip` |
| D5 | `reflect_method` 同步且无上下文 | 直接调用 `&T::method` | `test_invoke` |
| D6 | `result<T>` 要求默认构造 | CRPC `rpc_result` | `test_move_only_response` |
| D7 | `error_info` 无消息文本 | CRPC `rpc_error` | `test_error_mapping` |
| D8 | JSON `parse_as` 丢失偏移 | 两段式解析 | `test_json_offset` |
| D9 | 文本 `serialize<T>` 吞掉错误 | 两段式 `to_value` 加 `serialize(document)` | `test_codec_errors` |
| D10 | XML 零拷贝视图超出 body 生命周期 | 复制进 arena 或保持 body 存活 | `test_xml_lifetime` |
| D11 | 文本中 `u64 >= 2^63` 丢精度 | 二进制原生；JSON 用字符串 | `test_u64_text` |
| D12 | 派生类型 `members` 不含基类字段 | crpc-binary 拒绝派生类型 | `test_derived_rejected` |
| D13 | protobuf/FlatBuffers 需手写 schema | 强制 `schema<t>`；空 schema 拒绝 | `test_schema_required` |
| D14 | FlatBuffers 类型化路径建 DOM | 不宣称零拷贝 | 基准报告 |
| D15 | 首次 `method_table<t>()` 分配 | 在 `runtime::init` 预热 | `test_prewarm` |
| D16 | LOOP 模式不暴露最近 deadline | CRPC 定时轮 | `test_timer_precision` |
| D17 | 驱动 wake 只是门铃 | MPSC 加 eventfd/IOCP 投递 | `test_cross_shard_wake` |
| D18 | LOOP runtime 只有 `worker[0]` | 每分片一个 runtime | `test_shard_isolation` |
| D19 | runtime 不能被并发 pump | `pump_active`；单分片线程 | `test_pump_exclusive` |
| D20 | corocc 没有 I/O 完成收割 | `epoll_wait` / `GQCSEx` 批量数组 | `test_batch_completion` |
## 19. 测试策略 <a id="s19"></a>
### 19.1 层次
| 层次 | 内容 | 门禁 |
|---|---|---|
| 单元 | 帧编解码、varint、字节序、路由、HTTP 解析、multipart、codec、错误映射、熔断、令牌桶、去重 | 全绿，x86 与 x64 |
| conformance | fake、inproc、TCP-loopback 三后端同一断言 | 无语义差异 |
| 集成 | 双进程 RPC、HTTP/1.1、h2、TLS、注册中心、上传下载、SSE、推送 | Windows 与 Linux 全绿 |
| 故障注入 | 丢包、重复、乱序、延迟、重置、半开、短写、短读 | 可由 seed 复现 |
| fuzz | 帧、HTTP/1.1、h2 HPACK、JSON/XML/TOML/INI、multipart、路由 | 无崩溃、无超限分配 |
| sanitizer | ASan/LSan、UBSan、TSan、可选 MSan | 零报告 |
| 长稳 | 百万调用、1000 次 start/stop、RSS 平台期、活跃计数归零 | 无增长 |
### 19.2 定律到验证手段矩阵
| 定律 | 验证 | 测试资产 |
|---|---|---|
| 无实时全局状态 | 乱序注入加终态断言 | `test_inproc_reorder` |
| 半开闭是常态 | 超时后检查 `unknown_side_effect` | `test_timeout_semantics` |
| 对等写入必发散 | 两个写者只有一个成功 | `test_single_writer` |
| 外部效果不可撤回 | 重试后 handler 只执行一次 | `test_idempotency` |
| 共识是最后手段 | 权威宕机：写失败、读继续 | `test_registry_failover` |
| 同步重试共振 | 1000 客户端重试，服务端 QPS 有界 | `test_retry_storm` |
| 单调锚点 | `lane_seq` 单调，去重命中 | `test_dedup_window` |
| 规则堆砌 | 未知转移被拒绝 | `test_state_machine` |
| 不对称安全 | 非法帧/版本/证书被拒绝 | `test_fail_closed` |
| 事件先于数据 | 两种到达顺序终态一致 | `test_watch_two_order` |
| NULL 不可穿透 | 注销后陈旧心跳不复活 | `test_tombstone` |
### 19.3 PIN 流程
实现期发现的每个缺陷都先钉住再修：

1. **钉住（pin）**：写一个精确复现缺陷的失败测试，记录实际错误，不写转述。
2. **证明红**：运行并保存失败输出到任务报告。
3. **修复**：最小改动，不做无关重构。
4. **转绿**：同一测试通过，并永久保留为回归守卫。
5. **绝不删除或弱化**已钉住的测试；缺陷有更多分支时只扩展。

项目规则是：失败必须先被证明，修复后测试不得删除。
### 19.4 sanitizer 矩阵
| 工具 | 目标 | job |
|---|---|---|
| ASan + LSan | 帧解析、缓冲复用、槽回收、关闭竞态、multipart、上传取消 | Linux Clang Debug |
| UBSan | 字节序编解码、varint、长度运算、对齐、移位、32 位截断 | Linux Clang，`-fno-sanitize-recover=all` |
| TSan | 多分片、MPSC、调用槽状态、熔断统计、注册中心 watch | Linux Clang `-O1 -g` |
| MSan（可选） | 解码路径未初始化读 | Linux Clang |

corocc 已提供 fiber hook：`COROCC_ASAN` 与 `COROCC_TSAN` 构建选项、ASan 上下文切换、TSan fiber 注解。CRPC 用这些选项构建依赖，不自建 fiber 注解。
## 20. 构建与 CI <a id="s20"></a>
### 20.1 CMake 目标
只构建一个库目标，源文件来自整个 `src/`：

| 目标 | 内容 |
|---|---|
| `crpc` | 库本体：core、io、wire、rpc、http、http2、cluster、mvc、codec、files、push、db、mq、redis、ws、ipc、fs、orm、game、algorithm、crypto、text |
| `crpc::crpc`、`crpc_core` | `crpc` 的别名，供 `target_link_libraries` 与测试使用 |
| `crpc_mpmc` | INTERFACE 目标，只负责传递 vendored mpmc 的头文件目录 |

其余都是"源文件一个可执行文件"，全部输出到构建根目录：`crpc_test_<file>`
（嵌套测试目录为 `crpc_test_<dir>_<file>`）、`crpc_example_<stem>`、
`crpc_bench_<file>`。按子系统拆分库曾被考虑但不采用：它不改变依赖图
（汇总库本来就连全部），只会增加每个目标的链接行维护成本。

选项：`CRPC_BUILD_TESTS`、`CRPC_BUILD_BENCH`、`CRPC_BUILD_EXAMPLES`、
`CRPC_BUILD_SHARED`、`CRPC_WITH_OPENSSL`、`CRPC_WITH_ODBC`、
`CRPC_WARNINGS_AS_ERRORS`、`CRPC_ENABLE_LTO`、
`CRPC_SANITIZER=none|asan|ubsan|tsan`、`CRPC_RTTI_DIR`、`CRPC_COROCC_DIR`。
不在这个列表里的选项都不存在。压缩器是内置的、不是可选项；32 位帧上限是编译期
常量；也没有 fuzz 目标——没有可以打开这些能力的开关。

零警告策略：MSVC `/W4 /WX`；GCC/Clang `-Wall -Wextra -Wpedantic -Werror`；可行处再加 `-Wconversion -Wsign-conversion`。CRPC 代码不得关闭警告；第三方抑制必须写明库名与理由。
### 20.2 脚本与 CI
```
scripts/build.ps1 -Config Debug|Release|Both -Arch x86|x64 -Sanitizer none|asan|ubsan|tsan -Jobs N -Clean
scripts/build.sh  --config debug|release|both --arch x64 --sanitizer none|asan|ubsan|tsan --jobs N --clean
scripts/test.ps1  -Config Debug|Release -Arch x86|x64 -Sanitizer none|all -Filter <substring> -Repeat N
scripts/test.sh   --config debug|release --sanitizer none|all --filter <substring> --repeat N
```
CI job：`linux`（GCC Debug/Release、Clang Debug，以及 GCC/Clang 的 shared）、
`linux-sanitizers`（ASan + UBSan + TSan）、`windows`（MSVC x64 与 x86，Debug/Release）、
`shared`（MSVC shared，含消费方链接门与导出表门）、`linux-bench-compile` 与
`bench-compile`（两个平台都编译 `bench/`，但都不运行）、`examples`。
没有 fuzz job，也没有 golden-bytes job：逐字节期望写在单元测试里
（`test_frame.cpp`、`test_protocol*`、`test_redis_resp.cpp` 等），不是独立的比对步骤。CI 调用开发者同一套脚本，不重复构建逻辑。日志 UTF-8；失败非零退出；构建幂等。
## 21. v1 范围、v1.1、里程碑 <a id="s21"></a>
### 21.1 v1（R5，权威）
| 领域 | v1 内容 |
|---|---|
| 传输 | `source`/`sink`（R1）、epoll ET、IOCP、inproc |
| 并发 | 每核一分片、每分片一个 LOOP runtime、MPSC 加门铃、定时轮 |
| 协程 | reader 协程、每分片 64 个 64 KiB 栈 handler 协程 |
| Wire | 16 字节帧头、varint、握手、流控、lane、可靠/不可靠、有序/普通、压缩 |
| RPC | `call<t>` 同步/异步、deadline 传播、取消、幂等键、重试、熔断、限流、隔离舱、降级、健康检查 |
| 注册中心 | Consul 形态的注册/注销/健康/catalog/KV+CAS/watch/ACL，单权威，单调 index，两序收敛 |
| 集群 | 负载均衡、节点互调 |
| HTTP | HTTP/1.1、HTTP/2（h2 与 h2c）、OpenSSL HTTPS |
| MVC | GET/POST/PUT/DELETE/PATCH、参数绑定、problem+json |
| 编解码 | crpc-binary、JSON、XML、INI、TOML、protobuf、FlatBuffers 全部接线（protobuf/FlatBuffers 需用户 `schema<t>`） |
| 文件 | 流式 multipart 上传、临时文件 RAII、Range/206 下载 |
| 压缩 | HTTP gzip/deflate；CRPC lz4/zstd |
| 推送 | `push_hub`、订阅、流式、SSE、长轮询 |
| 可观测 | AOP、W3C traceparent、高性能日志 |
| 工程 | 一键脚本、sanitizer 矩阵、零警告 |
### 21.2 v1.1（R5）
gossip/Raft 强一致、动态策略下发、io_uring/shm/RDMA、ARM64/大端/Android/macOS。WebSocket 不在 v1 也不在 v1.1：SSE 加推送已覆盖单向需求（A §32、B §19.2）。
### 21.3 里程碑与验收门禁
| 里程碑 | 交付 | 退出条件 |
|---|---|---|
| M0 | 骨架、构建脚本、核心类型、CI | x86/x64 零警告；核心单测 |
| M1 | source/sink、epoll、IOCP、inproc、定时器 | 三后端 conformance；关闭竞态 |
| M2 | wire、握手、调用、槽、deadline、取消 | golden bytes；终态恰好一次；无槽泄漏 |
| M3 | HTTP/1.1、MVC、绑定、上传下载、SSE | HTTP 解析 fuzz；multipart 取消清理 |
| M4 | HTTP/2、HPACK、TLS | h2spec 必测章节；TLS 互操作 |
| M5 | 容错、注册中心、集群、推送 | 重试风暴；注册中心两序；推送重放 |
| M6 | 全部 codec、可观测、加固 | sanitizer 零报告；长稳；基准报告 |

发布门禁（R11）：x86 与 x64 零错误零警告；ASan/LSan/UBSan/TSan 零报告；fuzz 无崩溃；两架构全部测试通过；脚本可复现构建；性能如实测量并报告。
## 22. 未决问题与风险 <a id="s22"></a>
| # | 问题 | 影响 | 阻塞点 / 计划 |
|---|---|---|---|
| O1 | R6 没有 `permission_denied`、`unauthenticated`、`already_exists`、`out_of_range`、`unimplemented`、`data_loss`、`not_leader` | HTTP 401/403/409/422/501 与注册中心 leader 提示无法用 `rpc_code` 表达 | 桥接：HTTP 层直接设置状态并记录 `metadata["http.status"]`；注册中心返回 `unavailable` 加 `metadata["leader"]`。M3/M5 冻结前需裁决确认。 |
| O2 | R1 定义 `source::on_data` 返回信用，但未命名恢复入口 | 没有恢复路径时，ET 会永久保留尾部 | `connection::resume_read()` 位于 `source`/`sink` 之外，两接口约束因此保持完整。 |
| O3 | A §30.1 的 flags 与 B §4.2.1 不同 | 复用 B 的常量会破坏 wire | 采用 A；B 的位分配被取代。任何基于 B 的代码必须重写。 |
| O4 | A §31.1 把压缩算法放在 flags bit5-7；B §21.6 放在 payload 首字节 | 互操作破坏 | 采用 A；B 的布局被取代。 |
| O5 | 帧类型数值此前没有文档固定 | 独立实现可能不一致 | 数值在 §7.3 固定并提交 golden bytes；修改即 wire 破坏。 |
| O6 | `max_frame` 默认值冲突：A 16 MiB，B 1 MiB | 每连接内存与大消息能力的取舍 | 采用 A §30.4：默认 16 MiB，硬上限 64 MiB，32 位构建默认 8 MiB。内存实测后复核。 |
| O7 | 幂等表只在内存 | 恰好一次仅在一个进程 epoch 内成立 | v1 明确边界；持久化留给 v1.1。 |
| O8 | 注册中心单权威 | 权威故障时写入停止 | v1 接受写 fail-closed、读 fail-static；gossip/Raft 为 v1.1（R5）。 |
| O9 | R5 把 HTTP/2 与七种 codec 放进 v1，而审计 B 估算 h2 单独需 15-25 人日 | 进度风险，非设计风险 | M4 单列里程碑；h2spec 为发布门禁。 |
| O10 | protobuf 与 FlatBuffers 需用户特化 `schema<t>` | 未注册类型会编码成零字节 | schema count 为零时拒绝注册；§10.4 已写明。 |
| O11 | crpc-binary 不支持派生类型 | 派生 DTO 编译失败 | `static_assert` 给出明确信息；文本格式仍支持派生类型。 |
| O12 | Windows 高精度定时器要求 Windows 10 1803 及以上 | 旧主机回退到粗粒度定时器，达不到 p99 目标 | 检测 `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` 失败并报告；不静默宣称达标。 |
| O13 | 性能目标未在构建主机验证 | 500 万 RPC/s 与 p99 < 100 微秒可能达不到 | R10 要求如实测量；目标不是门禁。 |
| O14 | MSVC 没有 TSan | Windows 并发竞争可能漏检 | Linux Clang TSan 为权威 job；MSVC 依靠 ASan 加压力测试。 |
| O15 | `then` 使用 32 字节小缓冲 | 更大捕获需要用户自行分配 | 已文档化；`std::function` 只允许在热路径之外。 |
| O16 | corocc 默认栈缓存只缓存默认尺寸栈 | 64 KiB handler 栈不被缓存，但 worker 长期存活 | 不做每请求 spawn；池大小启动期固定。 |
| O17 | HTTP/1.1 pipelining 串行 | 流水线客户端无法超过一个在途请求 | 已文档化；HTTP/2 是多路复用路径。 |
| O18 | Range 与压缩互斥 | 压缩下载忽略 `Range` | 已文档化；压缩开启时 `allow_range` 为 false。 |
| O19 | 不做 WebSocket | 浏览器双向推送不可用 | SSE 加 CRPC 推送覆盖单向；v1.1 也不增加 WebSocket。 |
| O20 | 注册中心 ACL 为静态 token | 无动态策略与角色 | v1 范围；动态策略为 v1.1（R5）。 |
