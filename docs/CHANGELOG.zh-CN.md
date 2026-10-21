# 发布说明

[English](CHANGELOG.md) | **简体中文**

## Unreleased

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
