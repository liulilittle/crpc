# 发布说明

[English](CHANGELOG.md) | **简体中文**

## 未发布

### 新增（MINOR，仅新增符号）

- `client::resilience_statistics` 与 `client::resilience_stats()`：客户端韧性流水线的增补可观测性（`rate_limited`、`bulkhead_rejected`、`local_gate_rejected`、`endpoint_open`、`lb_unavailable`、`pre_send_retries`、`probe_admitted`、`retries_delayed_by_hint`、`bulkhead_in_use`）。不改变既有结构：`client::statistics` 的字段顺序、大小与含义保持不变，C ABI 入口不变。见 [abi.zh-CN.md](abi.zh-CN.md) §4 与 §8。

### 编译期布局护栏

- 新增内部翻译单元 `src/abi_check.cpp`（不进入任何头文件，也不导出符号），对跨越 ABI 边界的公开记录做编译期断言：13 个记录的标准布局属性、14 个记录的精确成员数量（`CRPC_MEMBER_COUNT`）、可捕获重排/中间插入/删除的相对成员链（`CRPC_STEP`、`CRPC_STEP_SAME`、`CRPC_STEP_MIXED`、`CRPC_AFTER`）、`http_statistics` 的总宽度与尾字段位置，以及仅在 x86 与 x64 上的绝对 `sizeof`/`offsetof` 锚（`CRPC_ABS_SIZE`、`CRPC_ABS_OFF`）。成员数量正是能看到「尾追加被 x86 尾部填充吸收」这一情况的护栏。
- [abi.zh-CN.md](abi.zh-CN.md) 第 4 节现在给出「跨越 ABI 边界」的范围谓词，并列出 19 个仍无编译期护栏的范围内记录。`crpc_test_abi_docs` 会重新扫描 `include/crpc/**/*.hpp` 与 `src/abi_check.cpp`：新增跨边界记录若不加入护栏，也不更新该清单，测试即失败。
- 已写入文档的盲区：成员数量护栏不校验名字（改名只靠相对链捕获）；`rpc_error` 是唯一没有相对链的跨 ABI 记录，因此其内部的重排或改名不会被该单元捕获；`rpc_error` 与 `status` 的绝对 `sizeof` 被刻意不锁定，因为它取决于标准库配置。
- 影响：MINOR（仅构建期校验）。不破坏 ABI：不改变任何公开结构体布局，也不新增导出符号。运行时行为不变；构建约束收紧——护栏覆盖的布局变更现在会直接编译失败。见 [abi.zh-CN.md](abi.zh-CN.md) 第 4 节与第 8 节。

### 客户端韧性流水线

- 每端点 rate limiter 与 bulkhead 准入，通过 `builder::per_endpoint_rate()` / `builder::per_endpoint_bulkhead()` 配置，可用 `builder::resilience_enabled(false)` 整体关闭。
- 实际执行序为 `select + allow -> probe 判定 -> rate -> bulkhead -> register -> pool/slot -> retry -> deadline`；半开探针跳过 rate 与 bulkhead，连接池与在途上限仍生效。
- 本地 gate 拒绝会忽略 breaker permit、携带 limiter/bulkhead 的 `retry_after`，且**不自动重试**。
- 远端 `retry_after` 是重试延迟下界（`max(backoff, hint)`），由调用 deadline 封顶；retry 预算在该判定之后才扣减。`retries_delayed_by_hint` 仅统计延迟确实由 hint 决定的那些重试。
- 选择循环用每端点位图跟踪已试候选，超过 512 个端点时自动扩容，不再静默忽略高位索引。
- `call::cancel()` 立即中断重试等待（plain-thread 用条件变量，分片用 5 ms 分片），并与完成回调注册串行化对低层调用句柄的访问。

### 准入阶段与 HTTP Retry-After

- 新增头文件 `crpc/resilience/admission.hpp`（header-only），把准入（admission）词汇集中到一处：`resilience::stage_kind`（10 个阶段）、`resilience::admission_decision`（`admit`、`reject`、`probe_bypass`；`probe_bypass` 为保留值，尚无阶段消费）、`reject_code(stage_kind, bool deadline_met = true)`——唯一的「阶段到 `rpc_code`」表，包含 rate 阶段的两种结果（deadline 仍可达时为 `resource_exhausted`，否则为 `deadline_exceeded`）——以及 `fallback_eligible(rpc_code, stage_kind, bool one_way)`。
- 服务端与客户端的拒绝路径改为从该表取线上码。码值不变，消息、`retryable`、`retry_after` 与各阶段计数器也不变：这一部分是零行为变化的重构。
- HTTP MVC 错误响应在 `rpc_error::retry_after > 0` 时新增 `Retry-After`：毫秒向上取整为整秒、下限为 1，hint 为 0 时不写该头。状态码映射不变。
- 影响：MINOR（新增头文件与符号）。不破坏 ABI：header-only，`stage_kind`/`admission_decision` 不是跨 ABI 类型，既有记录布局不变。行为变化仅限新增的 HTTP `Retry-After` 响应头；RPC 线上码与消息不变。

### HTTP 长轮询

- 服务器自有 housekeeping 定时器在之后没有 poll 到达时也会清理空闲订阅；由
  `start()` 装配、`stop()` 在拆 hub 之前取消，并由回调自行重新装配。
- `start()` 把前端重放上限收敛到队列上限（`max_replay < max_messages` 会被抬高，
  反之被拒绝）。条数上限与每订阅字节预算相互独立；游标缺口超过任一上限时返回
  409 Restart，而不是静默丢弃未确认数据。`http_statistics::longpoll_replay_window`
  报告生效上限，服务器未运行（含 `start()` 失败）时为 0。
- `stop()` 保留监听配置，之后 `start()` 可重新绑定同样的端点。保存的
  `backend_factory` 指针是借用的：backend 必须活过整个 server 生命周期，包括每次
  `stop()` -> `start()`（已写在 `http_server::builder::add_listener()` 与
  `server::builder::add_listener()` 上）。同时修复 `rpc::server`：其第二次 `start()`
  过去会返回 ok 却不绑定任何监听。

### 异常与编译器特性契约

- 52 个公共头现在都能在关闭异常的情况下编译（`-fno-exceptions`、MSVC `/EHs-c-`）。以无异常方式重建库仍不受支持：`src/` 的 46 个翻译单元中，Linux GCC 有 11 个失败、MinGW GCC 12 个、MSVC 13 个（见 [building.zh-CN.md](building.zh-CN.md) 第 3 节）。
- 异常契约按边界分别说明。头内联边界（`obs::interceptor_chain::run`、`mvc::invoke_http`）跟随消费方的异常选项；启用异常时抛出的异常会被捕获并转为 `rpc_code::internal` / HTTP 500 `internal error`。库翻译单元边界（`mvc::router::handle`、`rpc::dispatcher::dispatch`）跟随库的选项；以异常构建的库会捕获抛出的 handler 并转为 500/`internal`。无异常支持时，抛出不在本库契约内，实践中会 terminate。见 [architecture.zh-CN.md](architecture.zh-CN.md#s15)。
- 新增宏 `CRPC_CAN_USE_TYPEID`：由编译器派生（`__cpp_rtti`；非 clang-cl 的 MSVC 恒为 `1`），外部定义即触发硬 `#error`。所有 `typeid` 使用都在该宏之后；编译器 RTTI 不可用时，type-erased codec 路径返回 `failed_precondition`（HTTP 409），而不是构造 `std::type_info`。`-fno-rtti` 支持仍然受限：启用反射依赖（`CRPC_RTTI_DIR`）时构建会死在依赖内部，该兜底分支目前在任何已文档化配置下都不可达；保留它是为了将来的工具链。见 [building.zh-CN.md](building.zh-CN.md) 第 3 节。
- 影响：MINOR（新增宏与新的消费方配置）。不破坏 ABI：无布局或符号变更；该宏只在编译期选择内联分支。行为变化：消费方现在可以用无异常方式编译自己的翻译单元；此类构建中抛出的 hook 或 handler 会离开调用点，而不是被捕获。

### ABI breaking：`http_statistics` 布局

- 相对已发布的 1.0.0（`bd006b2`），`http_statistics` 在本未发布周期新增了 19 个
  字段：整个长轮询块（`longpoll_waiters`、`longpoll_subscribers`、
  `longpoll_replay_window`、`longpoll_events`、`longpoll_timeouts`、
  `longpoll_ack_clamped`、`longpoll_conflicts`、`longpoll_restarts`、
  `longpoll_overloaded`）以及 10 个 SSE/压缩/下载计数器。`sizeof` 从 72（9 字段）
  增长到 224（28 字段）；1.0.0 发布的 9 个字段偏移保持不变（0 … 64）。公开
  结构体新增字段属于 MAJOR 变更（[abi.zh-CN.md](abi.zh-CN.md) 第 1/4 节）。
- 在该周期内部，`longpoll_replay_window` 最初放在 offset 168，后来被移到末尾
  （offset 216）；原先排在它后面的 6 个计数器各前移 8 字节
  （`longpoll_events` 176→168 … `longpoll_overloaded` 216→208）。移动前后
  `sizeof` 都是 224，因为移动前结构体尾部同样没有填充。没有任何已发布头文件
  使用过 offset 168 的布局（无 tag、无 1.0.1），因此该内部移动不构成对已发布
  消费者的破坏；真正破坏兼容的是 72 → 224 的增长。

### 行为变更：COMPRESSED 帧不再被当作明文交付

帧级压缩现已实现 LZ4 block 编解码（`ALGO = 1`），其余情况一律 fail-closed。此前行为不安全：
`encode_frame` 接受 `COMPRESSED` 标志却写出未压缩 payload；`decode_frame` 与 `frame_decoder`
把压缩字节当作 payload 返回；任何算法 id 都被接受。`COMPRESSED` 的 8 字节 `PING` 甚至会被
原样回送为明文 `PONG`。

变更内容：

- `encode_frame` 对 `COMPRESSED` 标志或非零 `ALGO` 返回 `invalid_argument`；压缩帧只能由
  `encode_frame_compressed` 产生。
- `encode_frame_compressed` 在写出任何字节之前执行与 `encode_frame` 相同的帧头校验
  （已知类型、stream 作用域、`stream_id` bit 31、`ONE_WAY`/`UNRELIABLE`、`ext`/`CRC32C`、
  `payload_len <= max_frame`），并新增 `max_frame` 形参；违规时在压缩分支与 identity 分支
  都返回 `invalid_argument` 且不写入任何字节。未设 `CRC32C` 时的非零 `ext` 现在被拒绝，
  而不是被静默置 0；设了 `CRC32C` 时该字段按设计存放 payload 校验和（明文与压缩形式都一样）。
- `decode_frame` 对 `COMPRESSED` 帧返回 `protocol_error`（它没有输出缓冲）；`frame_decoder`
  在拷贝模式下解压、借用模式下拒绝。解码后的帧头被规范化：清 `COMPRESSED`/`ALGO`，
  `payload_len` 与 `CRC32C` 改写为逻辑 payload 的对应值。
- zstd（2）与 gzip（3）在本构建中是被识别但 fail-closed 的 id；`4`-`7` 直接拒绝。
- `COMPRESSED` 只允许出现在 `REQUEST`、`RESPONSE`、`DATA`、`ONE_WAY`、`PUSH` 上；
  控制帧（含 `PUSH_ACK`）带 `COMPRESSED` 是 `protocol_error`。
- `ONE_WAY` 帧上的畸形压缩 payload 属于显式丢弃：解码器返回 `result::frame_dropped`，
  `error()` 保持 `ok`，原因经 `frame_compression_info::drop_reason` 报告。判据是帧类型而非
  `UNRELIABLE` 标志，因此对端无法把 `REQUEST` 上的致命违规降级为静默丢帧。
- `frame_decoder::result` 末尾追加 `frame_dropped`（既有值编号不变）。对 `result` 做无 `default`
  的 `switch` 的代码必须补上该分支：GCC/Clang 会报 `-Werror=switch`，MSVC `/W4` 不告警。

兼容性：生产路径不写 `COMPRESSED` 帧，两处生产解码器都是借用模式，因此既有对端行为不变。
手工构造压缩帧的对端现在会得到协议错误，而不是被静默误交付——这正是有意的收紧。详见
`docs/protocol.md` 第 24 节。

## 1.0.0 - 2026-10-19

CRPC 首个正式版本。本版本覆盖核心原语、CRPC 调用/服务器门面，以及
HTTP/1.1、HTTP/2 与 HTTPS 前门。这不表示所有设计目标均已达成；实测差距见“已知限制”。

### 主要能力

- **传输后端**：进程内、边缘触发 epoll 与 IOCP 三种后端，统一实现
  `source`/`sink` 两接口契约，提供基于额度的背压与恰好一次的完成回调。
- **协程与分片**：`runtime` 分片原语（corocc LOOP 运行时、有界跨分片 inbox、
  有界 handler 就绪队列、64 个 handler 协程池、单调时钟定时器轮），以及
  `client_facade` 的协程调用写法；corocc 缺失时提供 nofiber 回退。
- **协议**：多路复用 CRPC 二进制帧、crpc-binary 编解码器、类型化派发器与服务注册表。
- **HTTP/1.1 与 HTTP/2**：消息解析/序列化、分块传输、HPACK、h2c 与 h2、
  HTTP 服务器门面、keep-alive 与请求上限。
- **HTTPS**：基于 OpenSSL 的 TLS 装饰器与 ALPN，由 `CRPC_WITH_OPENSSL` 启用。
- **MVC**：attribute 声明的路由、DTO 绑定与响应映射。
- **序列化**：crpc-binary，以及 RTTI 驱动的 JSON、XML、INI、TOML 与 schema 适配器。
- **推送原语**：主题、订阅、有界溢出策略、可靠重放与流式收发形状。
- **容灾原语**：熔断、限流、舱壁、负载均衡与重试预算；其中负载均衡、重试与熔断
  已接入客户端门面。
- **可观测性**：W3C 追踪上下文、拦截器原语、结构化异步日志与有界 tracer；
  AOP 尚未接入调用路径。
- **文件与压缩**：multipart 解析、上传落盘与 Range 下载，以及 HTTP gzip/deflate
  流式内容编码原语（尚未接入 HTTP 服务器路径）。

### 本轮修复摘要

- **借用 source 的生命周期契约**：借用的 source 由 `bound_guard` 跟踪，
  连接不会向正在销毁的 source 投递数据。
- **server/client 解绑**：`server` 与 `client_facade` 在销毁借用的 source 前
  解绑并等待在途投递结束。
- **定时器 keep-alive**：定时器回调持有 keep-alive 引用，避免弹出/停机竞态中
  回调访问已释放的上下文。

### 验证状态

- MSVC x64 Debug 与 x86 Debug：CTest 26/26 套件通过，零告警。
- TSan：26 套件，0 竞争（nofiber/Linux 配置）。
- ASan/LSan：26 套件，0 消毒器报告（nofiber/Linux 配置）。
- 独立双审：在审计基线上会签通过，无未关闭的 P0/P1 缺陷。
- 性能证据与实测差距见 [performance.zh-CN.md](performance.zh-CN.md)。

### 已知限制

- 5M RPC/s 聚合吞吐目标未达：最佳实测 0.275M RPC/s
  （进程内、MSVC、流水线深度 64）。
- IOCP 回环 p99 为 170.2 us，高于 100 us 目标。
- call slot 为 440 字节，高于 128 字节预算。
- IOCP 仅支持 Windows，未做 Linux 运行时验证。
- 跨平台与跨字节序验证（ARM64、大端、macOS）尚未完成。
- 尚未端到端完成：`PUSH`/`PUSH_ACK` 与 HTTP SSE/长轮询、HTTP 压缩与下载接入
  服务器路径、服务端幂等去重表、注册中心服务化。
