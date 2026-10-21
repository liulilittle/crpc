# 发布说明

[English](CHANGELOG.md) | **简体中文**

## 未发布

### 新增（MINOR，仅新增符号）

- `client::resilience_statistics` 与 `client::resilience_stats()`：客户端韧性流水线的增补可观测性（`rate_limited`、`bulkhead_rejected`、`local_gate_rejected`、`endpoint_open`、`lb_unavailable`、`pre_send_retries`、`probe_admitted`、`retries_delayed_by_hint`、`bulkhead_in_use`）。不改变既有结构：`client::statistics` 的字段顺序、大小与含义保持不变，C ABI 入口不变。见 [abi.zh-CN.md](abi.zh-CN.md) §4 与 §8。

### 客户端韧性流水线

- 每端点 rate limiter 与 bulkhead 准入，通过 `builder::per_endpoint_rate()` / `builder::per_endpoint_bulkhead()` 配置，可用 `builder::resilience_enabled(false)` 整体关闭。
- 实际执行序为 `select + allow -> probe 判定 -> rate -> bulkhead -> register -> pool/slot -> retry -> deadline`；半开探针跳过 rate 与 bulkhead，连接池与在途上限仍生效。
- 本地 gate 拒绝会忽略 breaker permit、携带 limiter/bulkhead 的 `retry_after`，且**不自动重试**。
- 远端 `retry_after` 是重试延迟下界（`max(backoff, hint)`），由调用 deadline 封顶；retry 预算在该判定之后才扣减。`retries_delayed_by_hint` 仅统计延迟确实由 hint 决定的那些重试。
- 选择循环用每端点位图跟踪已试候选，超过 512 个端点时自动扩容，不再静默忽略高位索引。
- `call::cancel()` 立即中断重试等待（plain-thread 用条件变量，分片用 5 ms 分片），并与完成回调注册串行化对低层调用句柄的访问。

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
