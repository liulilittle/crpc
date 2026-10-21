# CRPC 架构

[English](architecture.md) | **简体中文**

版本 1.0 / 2026-10-19

本文描述分层、传输契约、执行模型、调用状态机，以及每项需求在代码树中的位置。它区分“仓库今天已经实现的内容”和“设计已冻结、但代码尚未提供的内容”。**已设计，未实现** 表示设计承诺，不是当前行为。

## 目录

| # | 章节 |
|---|---|
| [1](#s1) | 范围与状态 |
| [2](#s2) | 两项一等能力 |
| [3](#s3) | 分层图 |
| [4](#s4) | 依赖图 |
| [5](#s5) | 基础层 |
| [6](#s6) | 传输契约 |
| [7](#s7) | 传输后端 |
| [8](#s8) | 分片线程模型 |
| [9](#s9) | RPC 核心 |
| [10](#s10) | 调用状态机 |
| [11](#s11) | 编解码层 |
| [12](#s12) | HTTP 与 MVC |
| [13](#s13) | 集群层 |
| [14](#s14) | 韧性层 |
| [15](#s15) | 可观测性 |
| [16](#s16) | 推送与流 |
| [17](#s17) | 流控 |
| [18](#s18) | 需求映射 |
| [19](#s19) | 状态矩阵 |
| [20](#s20) | 验证映射 |

## 1. 范围与状态 <a id="s1"></a>

CRPC 是面向大型服务器系统的 C++17 框架。公开接口以头文件为先：`include/crpc/**` 定义使用者能命名的每个类型，`src/**` 放置非内联实现。项目处于 v0.1：各模块是独立测试过的构件，还不是完整的网络服务器。当前没有安装目标，也没有提交到仓库的 CI 配置。

- **implemented（已实现）** —— 代码存在、可编译，并且 `tests/` 中有测试套件。
- **partial（部分实现）** —— 代码存在且有测试，但契约中有明确缺失的部分。
- **specified, not implemented（已设计，未实现）** —— 设计冻结了行为，但没有任何代码路径提供它。

权威目标是[设计规格](superpowers/specs/2026-10-19-crpc-design.zh-CN.md)。如果本文与规格不一致：关于“今天的事实”，以代码为准；关于“目标契约”，以规格为准。

## 2. 两项一等能力 <a id="s2"></a>

CRPC 在同一条连接、同一套帧格式、同一个执行模型上承载两项能力：

1. **调用（calls）。** 一个请求恰好产生一个响应。在协程内，调用应当看起来像同步调用；在协程外，它是一个句柄（`rpc::call<T>`），可以等待、轮询、取消或注册回调。两种形式共用同一个状态机。
2. **推送（push）。** 服务器向一个逻辑订阅者身份发布消息。订阅者可以重连并恢复；可靠投递使用重放缓冲与确认，不可靠投递允许丢弃并计数。

两者共用 `source`/`sink`、帧、错误值、截止时间、追踪上下文与背压。它们不共用调用槽：推送拥有的是订阅，而不是在途调用。这也是 `include/crpc/rpc/**` 与 `include/crpc/push/**` 各自拥有独立状态机的原因。

调用路径由 `rpc::client` 与 `rpc::dispatcher` 实现；推送路径由 `push::hub` 与 `push::stream` 实现。两者尚未在帧层接到同一条连接：客户端处理 `RESPONSE`、`PING`、`GOAWAY`，不处理 `PUSH`/`PUSH_ACK`。这部分接线属于**已设计，未实现**。

## 3. 分层图 <a id="s3"></a>

代码树比设计中的示意布局更扁平：公开头文件直接位于 `include/crpc/` 下（是 `frame.hpp`，不是 `wire/frame.hpp`），`src/` 按模块名镜像。分层本身仍然清晰：

| 层 | 公开头文件 | 实现 | 职责 |
|---|---|---|---|
| 基础 | `config.hpp`、`span.hpp`、`buffer.hpp`、`error.hpp`、`endian.hpp` | `src/error.cpp` | 固定宽度、平台检查、借用视图、缓冲区、错误、字节序 |
| 协议 | `wire.hpp`、`frame.hpp` | `src/frame/frame.cpp` | varint/zigzag、带边界检查的读写、16 字节帧编解码、调用信封、增量解码器 |
| 传输 | `io.hpp`、`io/epoll_backend.hpp`、`io/iocp_backend.hpp` | `src/io/*` | 双接口契约、连接/工厂、inproc、epoll ET、IOCP |
| RPC | `rpc/call.hpp`、`rpc/client.hpp`、`rpc/dispatcher.hpp` | `src/rpc/*` | 调用槽、状态机、截止时间、取消、请求/响应、分发 |
| 编解码 | `codec.hpp` | `src/codec/codec.cpp` | crpc-binary、RTTI 文本/模式适配器、注册表 |
| HTTP | `http/message.hpp`、`http/multipart.hpp`、`http/file_transfer.hpp` | `src/http/*` | HTTP/1.1 消息、解析器、multipart、上传 sink、下载/Range |
| HTTP/2 | `http2/frame.hpp`、`http2/hpack.hpp`、`http2/connection.hpp` | `src/http2/*` | 帧、HPACK、流与流控状态 |
| MVC | `mvc/attributes.hpp`、`mvc/router.hpp`、`mvc/binding.hpp` | `src/mvc/*` | 路由、DTO 绑定、响应映射 |
| 集群 | `cluster/registry.hpp`、`cluster/lease.hpp`、`cluster/election.hpp`、`cluster/health.hpp`、`cluster/watch.hpp` | `src/cluster/*` | 目录、租约、选举、健康检查、watch |
| 韧性 | `resilience/*.hpp` | `src/resilience/*` | 熔断、限流、舱壁、负载均衡、重试 |
| 可观测性 | `observability/context.hpp`、`observability/interceptor.hpp`、`observability/log.hpp`、`observability/tracer.hpp` | `src/observability/*` | 追踪上下文、AOP、日志、span |
| 推送 | `push/hub.hpp`、`push/stream.hpp` | `src/push/*` | 主题、订阅、重放、流 |
| 流控 | `flow/estimator.hpp` | `src/flow/estimator.cpp` | RTT 基线、排队信号、pacer、窗口 |

设计中的 `transport` 与 `wire` 上下文对应代码树中的 `io` 与 `frame`/`wire`。名称不同，依赖方向相同。

## 4. 依赖图 <a id="s4"></a>

依赖方向严格向下，依据是公开头文件中的 `#include <crpc/...>`：

```
                    rpc          mvc
                  /  |  \       /   \
              codec frame  io  http  codec
                 \   |     |    |    /
                  \  |     |   http2
                   \ |     |   /
                    base + wire
                         |
                 config / span / buffer / error / endian

  cluster ---- resilience ---- observability ---- flow
      \__________  all depend on base only  __________/
```

当前成立的规则：

- `base` 不包含任何更高层的头文件。
- `io` 只依赖 `base`；RPC 层只接受 `backend_factory&`，从不判断传输是 TCP 还是进程内。
- `rpc/client.hpp` 包含 `frame.hpp`、`io.hpp`、`rpc/call.hpp`、`rpc/dispatcher.hpp`；`rpc/dispatcher.hpp` 包含 `frame.hpp` 与 `rpc/call.hpp`。
- `mvc/router.hpp` 包含 `http/message.hpp`、`codec.hpp`、`mvc/binding.hpp`；`http2/connection.hpp` 包含 `http2/frame.hpp`、`http2/hpack.hpp`、`http/message.hpp`。
- `cluster`、`observability`、`push`、`flow` 今天独立于 `rpc`。`resilience` 已部分接入：`client_facade` 接入了负载均衡、每端点 rate/bulkhead/breaker、重试和 deadline；`rpc::server` 接入了准入半边流水线（连接级 rate、服务级 rate、bulkhead、breaker、并发上限）。fallback 仍未实现。
- 没有脚本强制这些方向。设计中的上下文依赖规则是评审规则；构建期依赖检查属于**已设计，未实现**。

## 5. 基础层 <a id="s5"></a>

`config.hpp` 在编译期拒绝不支持的目标：8 位字节、恰好 8/16/32/64 位的整数、IEEE-754 float 与 double、可确定的宿主字节序。它定义 `u8`…`u64`、`i8`…`i64`、`CRPC_API`，以及后端使用的平台宏。

`span<T>` 是 C++17 中 `std::span` 的替代：两个字宽、可平凡拷贝、借用语义、`subspan` 越界时钳制为空。`as_bytes`/`as_writable_bytes` 是唯一允许的重解释辅助函数，并要求元素类型可平凡拷贝。

`buffer` 是唯一的动态字节容器。它按 1.5 倍几何增长，有调用方给定的上限（默认 64 MiB），`clear()` 保留容量，分配失败通过 `failed()` 报告而不是抛出异常。

`error.hpp` 定义 `rpc_code`（0–21）、`rpc_error`（错误码、消息、可重试标志、重试等待、有序元数据）、用于 void 结果的 `status`，以及值或错误的 `result<T>`。库从不跨协程、线程或 ABI 边界抛出异常；`result<T>` 不要求 `T` 可默认构造。

`endian.hpp` 与 `wire.hpp` 是唯一允许接触字节的途径。多字节整数使用显式小端读写，绝不对结构体 `memcpy`，不使用位域或非对齐 `reinterpret_cast`。`byte_reader`/`byte_writer` 是全函数：每次读取要么得到值，要么返回 `rpc_code::protocol_error`；每次写入要么成功，要么返回 `rpc_code::encode_error`。

## 6. 传输契约 <a id="s6"></a>

传输只有两个接口，外加生命周期对象：

- `io::source` —— 后端到框架：`on_data(span) -> size_t`、`on_eof()`、`on_error(std::error_code)`。
- `io::sink` —— 框架到后端：`push(span, completion*)`。
- `io::completion` —— 每次 push 恰好一次 `complete(io_result)`。
- `connection` / `listener` / `backend_factory` —— 建立、关闭、取消、选项、`bind_source`、`resume_read`。

`on_data` 返回的是**信用（credit）**，不是布尔值：`n == data.size()` 表示全部消费；`0 < n < data.size()` 表示消费了前缀，后端保留未消费的尾部，并且只在 `resume_read()` 之后再次投递；`0` 表示暂停读取。这就是全部背压机制，source 侧没有完成对象。`resume_read()` 位于 `connection` 上、在两个接口之外；设计将其记录为开放问题 O2，代码遵循了这一放置。

| # | 后端强制的不变量 |
|---|---|
| I1 | 每次 `completion::complete` 恰好运行一次（成功、失败或取消）。 |
| I2 | 完成回调可以运行在任意线程。 |
| I3 | 完成回调不得阻塞、不得抛出（`noexcept`）。 |
| I4 | `close()` 之后不开始新的 source 投递或 sink push；在途完成仍恰好运行一次，状态为取消。 |
| I5 | 每个方向最多一个在途 sink push。 |
| I6 | 部分写由后端完成；框架绝不重新提交尾部。 |
| I7 | source 的 span 在 `on_data` 返回前有效；sink 的 span 在 `complete` 返回前有效。 |
| I8 | 任意时刻每条连接一个读者、一个写者。 |
| I9 | `on_data` 的返回值不超过 `data.size()`；违反者会被钳制。 |
| I10 | 信用不足时保留尾部，并且只在 `resume_read()` 后投递。 |

建立、关闭、取消与选项位于 `connection` 和 `backend_factory`，绝不在 `source`/`sink` 上。在这个头文件之上没有 `if (is_tcp)`。

背压是分层的：内核接收窗口通过返回零信用关闭；发送环有界；客户端对超过 `max_pending_write_bytes` 的帧直接拒绝而不是阻塞；推送订阅有每订阅的消息/字节上限。默认策略是快速失败或显式停车，而不是无界排队。

## 7. 传输后端 <a id="s7"></a>

| 后端 | 头文件 | 模型 | 边界 |
|---|---|---|---|
| 进程内 | `io.hpp`（`inproc_pair`、`inproc_backend`） | 有界队列对、串行投递、确定性故障 | `max_queued_bytes` 每方向 64 KiB |
| epoll | `io/epoll_backend.hpp` | 一个循环线程、epoll + eventfd 门铃 + timerfd、仅 ET | 发送环 64 KiB、每轮写预算 256 KiB、4096 连接、每批 256 事件 |
| IOCP | `io/iocp_backend.hpp` | 一个完成端口、一个泵线程、AcceptEx、每连接一收一发、高精度定时器线程 | 发送队列 256 KiB、64 个预投递 accept、每批 64 完成 |

进程内后端是同等公民，不是 mock：它实现同一套 `connection`、`sink`、`source`、`completion` 契约，串行投递，传播 EOF 与错误，遵守信用规则，并提供确定性故障钩子（`drop_bytes`、`delay_chunks`、`reorder_pairs`、`inject_write_error`、`inject_read_error`）。大多数契约测试使用它。

epoll 后端为每个 fd 注册 `EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP`；没有电平触发路径，也没有毫秒回退。事件 token 是槽索引加 generation，绝不是裸指针，因此过期事件不可能访问已释放内存。写循环有每轮字节预算；当预算耗尽而 socket 仍可写时，它会自投递一次唤醒（R4）；否则在发送环为空时消费掉的边沿会让后续数据搁浅。`timerfd` 在同一个 epoll 集合中承载亚毫秒截止时间。

IOCP 后端每个实例一个完成端口、一个使用 `GetQueuedCompletionStatusEx` 的泵线程。每条连接最多一个在途 `WSARecv` 和一个 `WSASend`，操作记录来自池，`AcceptEx` 预投递，`CancelIoEx` 被视为请求：被取消的操作仍然恰好产生一次完成，并映射为 `operation_canceled`。独立的高精度可等待定时器线程投递完成包，因此泵从不以毫秒粒度睡眠。

两个网络后端各自拥有循环/泵线程。source 回调按连接串行。`close`、`resume_read`、`is_open`、`remote` 可从其他线程调用；`push` 同一时刻可来自另一个线程，但同一条连接不得并发发起两次 push。

## 8. 分片线程模型 <a id="s8"></a>

目标模型是每个物理核一个分片：

```
n_shards = clamp(physical_cores - 1, 1, 32)
per shard:
  1 OS thread
  1 corocc LOOP runtime
  1 epoll set or IOCP
  1 MPSC inbox + 1 doorbell (eventfd / PostQueuedCompletionStatus)
  1 timer wheel
  64 long-lived handler coroutines with 64 KiB stacks
```

分片是亲和性与单线程所有权的单位。它的循环轮询 I/O、到期定时器、清空收件箱并泵自己的运行时。属于另一个分片的完成会被投递到那个分片的收件箱；没有线程泵其他分片的运行时。跨线程流量是 MPSC 加一次合并的门铃。

**状态：已设计，未实现。** 代码树中没有 `shard` 类型、协程 handler 池或 MPSC 收件箱。`corocc` 作为可选 CMake 依赖被接受，并在设置 `CRPC_COROCC_DIR` 时链接，但没有任何 CRPC 翻译单元调用 `coro_rt_*`。当前实现的执行模型是：每个网络后端拥有一个循环/泵线程；`rpc::client` 不创建线程，必须由驱动其后端的线程调用；`call<T>::get()` 在条件变量上阻塞；请求分发在读者线程上同步执行。

这是一个阶段性的落点，不是另一种架构。单写者规则、“持有锁时不执行 I/O 或用户回调”规则、基于信用的背压已经生效，因此加入分片不会改变契约。定时轮、handler 池和跨分片唤醒路径仍属于**已设计，未实现**。

## 9. RPC 核心 <a id="s9"></a>

`rpc::client` 拥有一条 `connection`、一个 `frame_decoder`、固定容量的 `call_slot_pool`、以 `stream_id` 为键的在途表，以及截止时间多重映射。它是该连接上 stream id 与槽状态的唯一写者。它只分配一次临时缓冲区并复用；调用句柄本身不分配。

`rpc::dispatcher` 是服务端半边。`service_registry` 把数字服务 id 映射到服务、把方法 id 映射到类型擦除的 handler。注册在 `freeze()` 之前单线程完成；冻结之后查找只读不可变映射，不加锁。`dispatch()` 解码调用信封，在调用 handler 之前拒绝已过期的截止时间，申请方法级舱壁，在 handler 边界捕获异常，并编码响应负载。

`rpc::method_traits` 从成员指针推导请求/响应类型；类型化 trampoline 用 `call_codec<T>` 解码并编码响应。`call_codec` 对不支持的类型没有主定义，因此缺少特化是编译错误，而不是静默的空负载。设计中的 RTTI 驱动服务宏在这一层属于**已设计，未实现**；RTTI 编解码器独立存在于 `codec.hpp`。

服务端连接循环在 `src/server/server.cpp` 中实现。`server::start()` 在 `registry.freeze()` 后构建冻结的每服务 guard 表，再创建分片运行时并启动监听；accept sink 把每条连接包装成 `reader_source`，`on_data` 解码 REQUEST/ONE_WAY 并提交到分片的 handler 池。`server::impl::handle_request` 先校验服务 id，再执行 §14 的服务端准入流水线，调用 `rpc::dispatcher::dispatch`，最后把 RESPONSE 或错误帧放入该连接的有界发送路径。客户端流水线已按端点接线；fallback 仍属于**已设计，未实现**。证据：`src/server/server.cpp:295-380,882-1145,1339-1385`；`include/crpc/server.hpp:78-180`。

## 10. 调用状态机 <a id="s10"></a>

已实现的状态：

```
idle --(acquire)--> queued --(request written)--> sent
                                              /  |  \       \
                                             /   |   \       \
                                     completed timed_out canceled failed
```

只有所属客户端线程执行 `idle -> queued -> sent`。终态转换可能由四个竞争方尝试：响应、超时、取消、连接丢失。`try_claim_terminal` 对状态字做一次比较交换；恰好一个赢家写入 `code`、`error`、`response`。失败者观察到终态后不得再触碰结果字段。

`result_ready` 在结果字段写完之后以 release 语义发布；`ready()`/`get()` 以 acquire 语义读取。未知或已终态 stream 的迟到响应会被丢弃，绝不投递给已回收的槽。槽带 generation；`call<T>` 携带槽指针、generation 和池指针，并在析构时归还槽。

终态路径：响应调用 `decode_response`，然后 `finish(completed, ...)`；`client::poll()` 遍历截止时间多重映射并主张 `timed_out` 与 `deadline_exceeded`，对已写出且非幂等的调用附加 `metadata["unknown_side_effect"] = "1"`；`call<T>::cancel()` 主张 `canceled`，如果请求已写出则发送 `RST_STREAM(canceled)`；连接丢失、EOF 或协议失败时，`fail_all()` 为所有在途槽主张 `failed`。单向调用是带 `UNRELIABLE` 的 `ONE_WAY` 帧；客户端在帧入队后立即主张 `completed`。

设计中的 `call_slot` 预算是 128 字节。当前槽包含互斥量、条件变量、缓冲区、`rpc_error`、回调存储和状态字；在 MSVC x64 上的编译期探测给出 `sizeof(call_slot) = 440` 字节。预算未达成。协程停车设计可以去掉互斥量/条件变量并缩小它。

## 11. 编解码层 <a id="s11"></a>

`codec.hpp` 定义类型擦除的 `codec` 边界，以及类型化的 `encode_into<T>` / `decode_from<T>` 辅助函数。类型化路径只做一次 `id()` 检查，然后直接调用具体编解码器的成员模板；没有 `std::function`、没有逐字段虚分派、没有中间文档树。

`crpc-binary` 是默认且完整的编解码器。每个字段是 `varint field_id, varint payload_len, payload`；字段 id 是声明索引加一，派生类型先编码基类字段。有符号整数用 zigzag，浮点是固定宽度小端 IEEE-754，容器采用 packed 或长度前缀，map 是重复的键/值项，optional 带一个存在字节。缺失字段保留原值，除非类型用 `RTTI_DEFINE_TYPE_WITH_DEFAULT` 注册；重复 id、id 0、截断、长度越界、深度超过 32 都会被拒绝。未知 id 被跳过，这正是格式向前兼容的原因。

文本/模式适配器（`json_codec`、`xml_codec`、`ini_codec`、`toml_codec`、`protobuf_codec`、`flatbuffers_codec`）由 `CRPC_HAVE_RTTI_<FORMAT>` 条件编译。protobuf 与 FlatBuffers 要求用户显式特化 `schema<T>`，缺失时给出清晰错误。没有 RTTI 时，binary 编解码器是报告编码/解码错误的桩，而不是假装成功。`codec_registry` 负责添加、查找与冻结；类型化路径不需要它。

## 12. HTTP 与 MVC <a id="s12"></a>

HTTP/1.1 在消息层实现：增量解析器与序列化器、严格的 chunked 解码/编码器，以及方法/目标/头部模型。解析器拒绝行内裸 CR、obs-fold、`Content-Length` 与 `Transfer-Encoding` 并存、重复 `Content-Length`，以及不以 `chunked` 结尾的传输编码链。起始行与头部接受 CRLF 和裸 LF，chunk 边界只接受严格 CRLF。限制：请求行 8 KiB、头部块 64 KiB、100 个头部、正文 8 MiB、trailer 64 KiB。

`http/multipart.hpp` 是增量 `multipart/form-data` 解析器，限制 part 数、头部和总字节，清洗文件名，并保留 `boundary_len + 4` 字节回滚尾部。`http/file_transfer.hpp` 增加 `upload_sink`（1 MiB 以内驻留内存，之后以独占创建方式溢写到随机临时文件，RAII 删除）和 `download_source`（流式读取、`Range`/206/416、仅单区间）。

HTTP/2 在协议层实现：9 字节大端帧编解码、包含静态表/动态表/Huffman 的 HPACK，以及带 settings、CONTINUATION 规则、流状态、连接与流窗口、停车发送、GOAWAY 与 RST_STREAM 的连接状态机。它不拥有 socket、TLS、ALPN 或 CRPC 调用适配器。

MVC 是路由表加绑定：`CRPC_ROUTE` 与 `CRPC_HTTP_*` 在静态初始化时注册条目；`router::freeze` 编译模式、拒绝畸形模式与冲突，并构建精确匹配表加按优先级排序的参数列表。绑定按固定顺序从 route/query/header/form/file/body 填充反射 DTO，并显式映射 400。`router::handle` 执行匹配、Accept 协商、绑定、调用与响应写出。

仍缺失：服务端路径中的 HTTP 压缩接线、SSE/长轮询，以及 CRPC 调用到 HTTP 的适配器，均属于**已设计，未实现**。

`http/server.hpp` 实现把上述各层接通的服务端门面。`http_server` 绑定一个或多个 `io::backend_factory` 监听器，把每条被接受的连接分配到某个 shard 的 `runtime`，并只在该线程上触碰全部协议状态：传输 source 只把字节拷入有界 ingress 缓冲并投递任务，因此不存在每连接线程，缓冲满时暂停读取（R1 credit）而不是丢弃。明文连接做嗅探——HTTP/2 连接前导选择 h2c，其余为 HTTP/1.1。请求经 MVC 路由分派（HEAD 执行 GET 处理器并丢弃正文），序列化后的响应入队；每连接写队列保证同一时刻至多一个后端 push 在途，队列增长则对读取方背压。HTTP/1.1 处理 keep-alive、流水线、`100-continue`、chunked 正文与 400/404/405/411/413/431/500 路径，并通过消息层拒绝请求走私。HTTP/2 原样复用帧/HPACK/连接层：遵守连接与流窗口，对超出对端窗口的响应停车并在 `WINDOW_UPDATE` 后恢复（对端窗口长期为零时周期性 PING 探测，R18），应答 PING/SETTINGS/GOAWAY，并把流错误映射为 RST_STREAM、连接错误映射为 GOAWAY。`listen_tls` 用 `io::tls_stream` 包装监听器，并按 ALPN 结果路由（`h2` 走 HTTP/2，`http/1.1` 走 HTTP/1.1）；公共头不出现 OpenSSL 类型，未定义 `CRPC_HAVE_OPENSSL` 时 `listen_tls` 返回明确错误。`stop()` 停止接受、在 HTTP/2 上发送 GOAWAY、在 `drain_timeout` 内排空在途请求后关闭，且幂等。它同时保留 builder 的监听配置，使之后的 `start()` 能重新绑定同样的端点；保存的 `backend_factory` 指针是借用的，因此 backend 必须活过整个 server 生命周期，包括每次 `stop()` -> `start()` 循环（与 MVC router 同一契约）。资源上限按服务器实例显式给出：`max_connections`（超出的 accept 直接关闭并计数，绝不排队）、`idle_timeout`（静默连接被关闭）与 `request_timeout`（未完成的请求回 408），因此连接洪泛与 slowloris 都无法长期占用内存或连接。出站队列同样有界：HTTP/1.1 一旦越过 cap 就停止解析并把未解析字节退回有界入站缓冲，HTTP/2 响应则停在每流队列中、其上限被强制。DTO 绑定依赖反射：无 RTTI 构建服务的是请求类型不含反射 DTO 的路由；文档编解码在 JSON 缺失时回退到其他已注册格式，而不是对所有请求回 406。

## 13. 集群层 <a id="s13"></a>

`cluster::registry` 是目录的唯一权威。实例以 `(service, node_id)` 为键；每次被接受的变更都会递增全局单调 `index`。读者原子加载不可变的 `registry_snapshot`；写者用一个互斥量串行化变更并发布新快照。只有属主 `node_id` 或携带管理 token 的写入会被接受。过期 incarnation 被拒绝；墓碑拒绝所有不是严格更大的 incarnation。每次被接受的变更返回一个由单调 index 派生的 fence，绝不来自墙钟。

`cluster::lease_manager` 授予 TTL 租约、续租，并通过 registry 权威路径应用到期：先 critical，宽限期后 deregistered。`cluster::election` 提供 PreVote、Vote 和带 fencing epoch 的租约式领导权；它明确不是 Raft，不提供日志、持久化或成员变更。`cluster::health` 提供 TTL、TCP 连接和回调检查，带连续成功/失败阈值。`cluster::watch_hub` 把 registry sink 事件转换为按单调 index 排序的快照加增量流；缺口把订阅标记为需要重新同步，慢订阅者被移除并产生可观察事件。

缺失项均属于**已设计，未实现**：网络 registry 服务及其 HTTP/CRPC 方法、节点身份/对等池、发现、Consul 兼容 HTTP 端点、KV+CAS+ACL 存储、gossip/Raft、多节点故障转移。原语存在，服务不存在。

## 14. 韧性层 <a id="s14"></a>

五个组件独立实现：

- `circuit_breaker`：`alignas(64)` 桶中的滑动窗口计数、CAS 状态转换、半开探测令牌、仅使用单调时间。
- `rate_limiter`：显式补充的令牌桶、非阻塞获取、有界 `retry_after`。
- `bulkhead`：无锁计数信号量加 RAII 许可。
- `load_balancer`：在不可变 `instance_set` 上实现轮询、随机、最少在途、一致性哈希和平滑加权轮询。
- `retry_sequence` / `retry_budget`：带指数退避与抖动的逐调用尝试，以及共享令牌桶预算；`unknown_side_effect` 仅在幂等时可重试。

设计命名六个**逻辑阶段**：`rate_limit -> bulkhead -> load_balance -> breaker -> retry -> deadline`。服务端准入半边已经接线；客户端按端点接线同样的阶段，但不按字面顺序执行：必须先选出端点（负载均衡）并通过其 breaker 准入，才能应用该端点的 rate 与 bulkhead，因此实际执行序为 `select + allow -> probe 判定 -> rate -> bulkhead -> register -> pool/slot -> retry -> deadline`。fallback 函数仍属于**已设计，未实现**，全局 rate 阶段属于 slice 2c。

客户端的一次逻辑调用依次经过：

1. 选择循环向负载均衡器取候选，并通过该端点的 breaker 准入。`allow()` 发放半开探针，并把 open 时长已过的端点提升为 half-open，因此候选被拒时循环会换下一个端点，而不是让调用失败。
2. 半开探针跳过每端点 rate 与 bulkhead；连接池与在途上限仍作用于探针，被 cap 拒绝的探针不计端点失败。
3. closed 路径先过每端点 rate（感知 deadline；等待会越过调用 deadline 时返回 `deadline_exceeded`），再过每端点 bulkhead（满载返回 `overload`），然后取连接并启动低层调用。
   `mvc::write_error` 将正值 `rpc_error.retry_after` 投影为 `Retry-After` 头，向上取整到秒且最小为 1；提示为 0 时不写该头。HTTP 长轮询的 503 路径单独处理：固定发送 `Retry-After: 1`，不投影 `rpc_error.retry_after`。
4. 本地 gate 拒绝调用 `ignore()`，且**不自动重试**：配额属于调用方。连接池饱和、低层 slot 耗尽、选择循环后全部 open、connect 失败属于 pre-send 失败，即使调用非幂等也可重试；已发送/远端失败要求幂等或幂等键。`retry_after` 是延迟下界（`max(backoff, hint)`），由 deadline 封顶；retry 预算在 deadline 判定之后才扣减。
5. 所有 gate 默认 `nullopt`（一次可预测分支；不构造对象、不加锁、不分配）。`resilience_enabled=false` 不创建也不执行 gates；失败来源分类仍然生效。close/cancel 会终结当前 attempt 并恰好一次释放其 bulkhead 与 breaker permit。
6. `call::cancel()` 无论低层调用是否在途都会置位取消标志，并中断重试等待：plain-thread 路径等待 cancel 会通知的条件变量，shard 路径按 5 ms 分片睡眠，因此观测延迟约为一个分片加调度（仅当分片内无法切分睡眠时才退化为受配置退避上界约束）。已取消的调用不再发起下一次 attempt。

`server::start()` 在 `registry.freeze()` 之后构建冻结、只读的 `service_guard` 表（每个已注册服务一项：可选 rate limiter、可选 bulkhead、breaker、无锁在途计数）。未知服务 id 没有表项，也不会创建任何状态，因此线上传入的任意 id 都无法撑大内存。handler 路径不取 guard 锁，也不分配。

每个请求的顺序：

1. `submit`（连接 reader）在 per-connection 在途上限之前、任何业务分配之前执行可选的**连接级**令牌桶。拒绝返回 `resource_exhausted` + `retry_after`（HTTP 429）；此时只分配必要的 forced-reject job。这是连接级洪泛保护，不是服务级 rate 阶段。
2. `handle_request` **先**检查 breaker。open 直接返回 `unavailable`（503）及其 `retry_after`。半开探针令牌会跳过服务级 rate 与 bulkhead 阶段，受 `half_open_max_probes` 限制，因此饱和的 rate 桶或 bulkhead 不会饿死 breaker。服务级在途上限是真实容量边界，探针同样受其约束：cap 满时探针返回 `overload`。探针仍受连接级令牌桶约束，不保证跨连接的探针供给。
3. closed 路径：服务级 rate（可选、感知 deadline；若等待会越过调用 deadline 则返回 `deadline_exceeded`，HTTP 504）-> 服务级 bulkhead（满载返回 `overload`，429）-> 现有服务级在途上限 -> `rpc::dispatcher::dispatch`。
4. 结果分类：本地准入（rate、bulkhead、连接池、并发上限）与应用错误调用 `ignore()`；`internal`、`unavailable`、`not_acknowledged` 调用 `failure()`；`deadline_exceeded` 在 handler 已运行时记 `slow()`、在 dispatcher 执行前拒绝时记 `ignore()`；`ok` 由 permit 实测耗时分为 `success` 或 `slow`。

两个服务级阶段默认都是 `nullopt`（一次可预测分支；不构造对象、不加锁、不分配）。bulkhead 需要显式配置 `per_service_bulkhead`；capacity 为 0 时由 `per_service_inflight` 派生。`resilience_enabled=false` 跳过新的 rate/bulkhead 阶段并保留 breaker 与并发上限，行为与接线前一致；此模式没有探针特判，cap 同样作用于探针。`stop()` 先停止 accept、drain、关闭连接，并在 join 全部分片之后才释放 guard 表；Debug 构建断言每个计数与 bulkhead 都回到零。64 位统计包括 `rate_limited_connection`、`rate_limited_service`、`bulkhead_rejected`、`breaker_open`、`deadline_rejected`、`probe_admitted`；`rejected_overload` 统计 per-connection cap、per-service cap 与 per-method 上限（rate/bulkhead/breaker 仍在各自计数器）。

客户端路径按端点经过同样的阶段：负载均衡 + breaker 感知的重新选择、每端点 rate/bulkhead（探针跳过）、带共享预算且遵守 `retry_after` 的重试、每端点 breaker（`src/client/client_facade.cpp`）。服务端流水线证据：`src/server/server.cpp:295-380,882-1145,1339-1385`；`include/crpc/server.hpp:78-180`。

## 15. 可观测性 <a id="s15"></a>

`obs::trace_context` 携带 16 字节 trace id、8 字节 span id、8 字节 parent 和 W3C flags；`parse_traceparent`/`format_traceparent` 实现 W3C 头，`baggage` 有序且有界。`obs::deadline` 是相对时间（`after(duration)`），符合跨机传播要求。

`obs::interceptor_chain` 是每个 hook 点的固定钩子表。空链只花一次可预测分支；钩子在启动时注册并冻结。该链尚未被 `rpc::client`/`rpc::dispatcher` 调用；这部分接线属于**已设计，未实现**。服务端门面另有一个 `server_interceptor` 钩子包住 `dispatch`（`src/server/server.cpp:333-337,428-431`），它不是可观测性链。

**异常契约。** HTTP 与 RPC 路径跨越两种不同的边界，它们的行为并不相同。头内联边界编译进消费方，跟随**消费方**的异常选项；库翻译单元是预编译的，保留**库**的选项。

- **头内联边界：** `obs::interceptor_chain::run`（`include/crpc/observability/interceptor.hpp:194-231`）与 MVC 的 handler 适配器 `mvc::invoke_http`（`include/crpc/mvc/router.hpp:385-399`）。有 C++ 异常（定义了 `__cpp_exceptions` 或 `_CPPUNWIND`）时，抛出的异常由本层捕获并转换为 `rpc_code::internal`：`run()` 停止并返回 false，`invoke_http` 写出 HTTP 500 `internal error` 响应，因此没有任何异常逃逸到 RPC 或 HTTP 路径。无异常支持（`-fno-exceptions`，或 MSVC 未启用任何 `/EH` 选项，或使用 `/EHc-`、`/EHs-`、`/EHa-`、`/EHs-c-`、`/EHa-c-`）时完全没有 `try`/`catch`：抛出的 hook 或 handler 离开调用点，其结果不由本库定义（见列表后的段落）。实践中调用方自带 `try`/`catch` 时仍可能捕获；否则进程调用 `std::terminate`。
- **库翻译单元边界：** `mvc::router::handle`（`src/mvc/router.cpp:617-659`，声明为 `noexcept`）与 `rpc::dispatcher::dispatch`（`src/rpc/dispatcher.cpp:366-372`）。这些帧是预编译的，跟随**库**的选项而不是消费方的选项。以异常构建的库在路由调用与 handler 调用外层包了 `try`/`catch (...)`，因此抛出的 handler 在库内被捕获，变成 HTTP 500 `internal error` 或 `rpc_code::internal`；不会 terminate，消费方也观察不到该异常。以无异常支持构建的库没有这些帧：同样的抛出是未定义行为，实践中会 terminate。

这是 `-fno-exceptions` 的**限制**，不是本库的保证：标准没有定义在没有栈展开信息的代码里 `throw` 会发生什么。实践中它可能以 `std::terminate` 结束，也可能被调用方的 `try`/`catch` 捕获，但**栈展开与析构不会正常执行**——以无异常支持编译的帧不携带清理信息，其局部对象（以及它们持有的锁或许可）不会被销毁。因此，在无异常边界外层使用调用方 `try`/`catch` 不是受支持的恢复手段。

因此契约要求 hook 与 handler 的实现**不得抛出**：回调按策略是 `noexcept`（[原则](principles.zh-CN.md#s8) 的「回调 `noexcept`」条款）。

`obs::logger` 在栈缓冲区格式化，把固定大小的记录复制进每线程 SPSC 环（默认 8192 条），由一个后台线程批量 `WriteFile`/`writev`，支持按大小/时间轮转、脱敏和可选压缩钩子。禁用级别只花一次原子读和比较；高于 `CRPC_LOG_MAX_LEVEL` 的宏展开为空语句。

`obs::tracer` 使用确定性头部采样；没有 exporter 时，启动 span 不读时钟、不生成 id、不分配。有 exporter 时，span 经过有界队列和后台导出器；溢出计数。父子关系由发送/接收对建立，绝不依据时间戳。指标属于**已设计，未实现**：代码树中没有计数器/直方图注册表。

## 16. 推送与流 <a id="s16"></a>

`push::hub` 是推送分片的唯一写者。主题映射到订阅者；订阅者 id 是比单条连接更长命的逻辑身份。`publish` 只复制一次负载并在订阅者间共享，绝不在订阅者上阻塞，并在队列边界处应用订阅的溢出策略（`drop_oldest`、`drop_newest`、`disconnect_subscriber`）。可靠消息携带 `(publisher_id, epoch, seq)`，在 `ack` 之前保持未确认；重连时从有界重放缓冲重放未确认消息。有序订阅使用每主题单调 `topic_seq`；缺口或重复会产生可观察事件。

`push::stream_state`、`stream_reader`、`stream_writer` 实现客户端流、服务端流和双向流。写操作最多接受传输当前的信用并返回接受字节数；零表示背压，不是错误。Reset 幂等。

缺失：`rpc::client` 中的 `PUSH`/`PUSH_ACK` 帧路径、HTTP SSE 与长轮询、CRPC 与 HTTP 共享的发布路径。这些属于**已设计，未实现**。

## 17. 流控 <a id="s17"></a>

`flow::rtt_estimator` 只维护一个基线，不建立过程模型。更低的样本立即把基线拉低（排队不可能造成更低样本）；向上样本按有界比例（默认 12.2%）上移，并以上述观测值为上限；持续偏移需要多次确认才会被视为路径变化。超过天花板的样本被当作测量伪影忽略。`queue_delay_ns(observed)` 是唯一的拥塞信号。

`flow::pacer` 是带 2 ms 突发的字节令牌桶；`congestion_window` 只在确认时增长（自时钟），在排队信号出现时收缩；`rtt_tracker` 把发送时间戳映射为样本，并拒绝过期或倒退的 token。所有时钟都是注入的单调纳秒函数。

估计器、pacer 和窗口尚未接入 `io` 或 `rpc`。集成、自适应背压和每 lane 配额属于**已设计，未实现**。

## 18. 需求映射 <a id="s18"></a>

| 需求 | 所在位置 | 状态 |
|---|---|---|
| 双接口传输 | `io.hpp`（`source`、`sink`、`completion`） | 已实现，有测试 |
| 进程内后端 | `io.hpp`、`src/io/inproc.cpp` | 已实现，有测试 |
| 仅 ET 的 epoll | `io/epoll_backend.hpp`、`src/io/epoll_backend.cpp` | 已实现，Linux 测试 |
| IOCP | `io/iocp_backend.hpp`、`src/io/iocp_backend.cpp` | 已实现，Windows 测试 |
| 多路复用二进制帧 | `frame.hpp`、`src/frame/frame.cpp` | 已实现，有测试 |
| crpc-binary 编解码 | `codec.hpp`、`src/codec/codec.cpp` | 已实现，有测试 |
| RTTI 文本/模式编解码 | `codec.hpp` 适配器 | 已实现，条件编译 |
| 调用与类型化桩 | `rpc/call.hpp`、`rpc/client.hpp`、`rpc/dispatcher.hpp` | 部分：无协程或桩宏 |
| 截止时间与取消 | `rpc/client.cpp`、`rpc/call.cpp` | 已实现，有测试 |
| 幂等键 | `rpc/call.hpp`（`call_id`）、调用信封 | 部分：键已携带，无去重表 |
| 有序 lane | `rpc/call.hpp` 中的 `lane_token` | 已设计，未实现 |
| HTTP/1.1 | `http/message.hpp`、`http/server.hpp` | 已实现：消息层 + 服务端门面 |
| HTTP/2 与 HPACK | `http2/*`、`http/server.hpp` | 已实现：协议层 + h2c/h2 服务端 |
| HTTPS/TLS | `http/server.hpp`、`io/tls.hpp` | `CRPC_WITH_OPENSSL` 下已实现；未启用时返回明确错误 |
| MVC/REST | `mvc/*` | 已实现，经 `http_server` 接线 |
| multipart 与文件 | `http/multipart.hpp`、`http/file_transfer.hpp` | 已实现，无专用测试套件 |
| Registry | `cluster/registry.hpp`、`src/cluster/registry.cpp` | 已实现，有测试 |
| 租约/选举/健康/watch | `cluster/*` | 已实现的原语 |
| 韧性策略 | `resilience/*` | 已实现的原语；服务端准入流水线与每端点客户端流水线均已接线；fallback 缺失 |
| 推送 hub/流 | `push/*` | 已实现，未接入帧/HTTP |
| 追踪/日志/AOP | `observability/*` | 已实现，AOP 未接线 |
| 流控 | `flow/estimator.hpp` | 已实现原语，未接线 |
| 分片/协程/MPSC | 设计规格 §4 | 已设计，未实现 |

## 19. 状态矩阵 <a id="s19"></a>

| 区域 | 状态 | 证据 / 缺口 |
|---|---|---|
| 基础、wire、frame | 已实现 | `test_endian.cpp`、`test_wire.cpp`、`test_frame.cpp` |
| inproc / epoll / IOCP | 已实现 | `test_io_inproc.cpp`、`test_io_epoll.cpp`、`test_io_iocp.cpp` |
| RPC 核心 | 已实现 | `test_rpc_core.cpp`、`test_server_client.cpp`；低层客户端为回调/轮询式，PUSH/流帧集成仍缺失 |
| 编解码 | 已实现 | `test_codec.cpp`；文本格式依赖 RTTI |
| HTTP/1.1、multipart、文件 | 部分 | `test_http_message.cpp`、`test_http_server.cpp`；无 multipart/文件套件 |
| HTTP/2 + HPACK | 已实现 | `test_http2.cpp`；`test_http_server.cpp` 覆盖 inproc 上的 h2c |
| MVC | 已实现 | `test_mvc.cpp`；`test_http_server.cpp` 经 inproc 走路由器分派 |
| 集群原语 | 已实现 | `test_cluster.cpp`；无网络 registry 服务 |
| 韧性原语 | 已实现 | `test_resilience.cpp`、`test_server_client.cpp`；服务端准入流水线与每端点客户端流水线均已接线；fallback 缺失 |
| 可观测性 | 部分 | `test_observability.cpp`；AOP 未接线，无指标 |
| 推送 | 部分 | `test_push.cpp`；无 PUSH 帧/HTTP 集成 |
| 流控 | 部分 | `test_flow.cpp`；未接入传输 |
| 分片/协程 | 已设计 | 无 `shard` 类型或 `coro_rt_*` 调用 |
| 压缩 | 部分 | HTTP gzip/deflate 流式编解码已实现并测试（`test_content_coding.cpp`），尚未接入 `http_server`；CRPC 帧压缩：lz4 block 已实现并测试（`test_frame_compression.cpp`），zstd 保留且 fail-closed，gzip 留待下一阶段；`COMPRESSED` 帧现在会被解压或拒绝，不再当作明文交付（此前会原样交付） |
| SSE / 长轮询 | 已设计 | 无 SSE writer 或 HTTP 推送适配器 |

## 20. 验证映射 <a id="s20"></a>

测试树为每个 `tests/test_<area>.cpp` 发现一个可执行文件，并注册到 CTest。支撑上述论断的套件：

| 套件 | 证明内容 |
|---|---|
| `test_endian.cpp` | 显式小端读写、浮点位模式、字节交换 |
| `test_wire.cpp` | varint/zigzag 边界、截断、超长编码拒绝 |
| `test_frame.cpp` | 黄金字节、有序扩展、CRC32C、fail-closed 规则、信封 |
| `test_io_inproc.cpp` | 信用、关闭竞争、有界队列、故障、完成恰好一次 |
| `test_io_epoll.cpp` | ET 就绪、暂停/恢复、半关闭、100 连接、1 万消息、亚毫秒定时器 |
| `test_io_iocp.cpp` | AcceptEx、暂停/恢复、取消、500 accept、1 万消息、定时器 |
| `test_rpc_core.cpp` | 四路终态竞争、槽回收、往返、超时/取消、分派错误 |
| `test_codec.cpp` | binary 黄金字节、容器、深度、派生字段、文本格式 |
| `test_http_message.cpp` | 增量解析、chunked、走私防御、序列化往返 |
| `test_http2.cpp` | 帧、HPACK 向量、settings/流状态、流控、fuzz 不崩溃 |
| `test_mvc.cpp` | 路由优先级/冲突/405/404、绑定来源、分派、multipart 上传 |
| `test_cluster.cpp` | registry 权威/fence/墓碑、租约到期、PreVote、健康、watch 缺口 |
| `test_resilience.cpp` | 熔断、限流、舱壁、重试预算、负载均衡器 |
| `test_observability.cpp` | traceparent、baggage、拦截器链、日志、追踪 |
| `test_push.cpp` | 扇出、溢出、重放/去重、有序缺口、慢订阅者、流 |
| `test_flow.cpp` | 有向 RTT 基线、排队信号、pacer、窗口、过期样本 |

这些是功能测试，不能替代消毒器矩阵。`scripts/test.ps1` 与 `scripts/test.sh` 负责构建并运行它们；ASan/LSan/UBSan/TSan 是[构建与测试](building.zh-CN.md)中描述的合并门禁。模糊测试目前只以 HTTP/2 帧与连接路径的“永不崩溃”用例存在；独立 fuzz 目标属于**已设计，未实现**。
