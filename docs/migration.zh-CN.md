# CRPC 迁移指南

[English](migration.md) | **简体中文**

版本 2.0 / 2026-10-24

本指南记录最后一个兼容版本（1.0.0，`bd006b2`）与 2.0.0 之间的二进制与行为变更。ABI 策略是规范性的，见 [abi.md](abi.zh-CN.md)。下文中 MAJOR 变更要求消费方用同一份头文件重建；仅源码级变更会显式标注。

## 1. 1.0.0 -> 2.0.0：破坏点

| 变更 | 类别 | 受影响类型 / 符号 | 1.0.0 | 2.0.0 |
|---|---|---|---|---|
| `http_statistics` 增长 | MAJOR（布局） | `crpc::http::http_statistics` | 72 字节，9 字段 | 224 字节，28 字段 |
| `server::options` 增长 | MAJOR（布局） | `crpc::server::options` | 304 字节（x64）、256 字节（x86） | 320 字节（x64）、264 字节（x86） |
| `server::builder` 增长 | MAJOR（随 options） | `crpc::server::builder` | 376 字节（x64）、296 字节（x86） | 392 字节（x64）、304 字节（x86） |

1.0.0 发布的 9 个字段在 `http_statistics` 中偏移不变（`active_connections` 为 0 … `timed_out_connections` 为 64），`server::options` 的每个既有成员偏移也不变（`send_goaway_on_stop` 在 x64 为 249 / x86 为 225，`push_subscribe` 为 264 / 232）。只有 `sizeof` 增长，因为新字段追加在末尾。这并不使变更兼容：两侧对按值传递的对象尺寸认知不一致，编译器生成的拷贝、新字段之后任何成员的 `offsetof`、以及消费方分配该结构体的栈帧，都按消费方自己的头文件确定尺寸。

### 重建要求

每个消费方必须用 2.0.0 的头文件重建，并链接 2.0.0 的库。把 1.0.0 的头文件与 2.0.0 的二进制混用不受支持，也不向前兼容，因为该变更属于不同的 MAJOR 版本（[abi.md](abi.zh-CN.md) 第 7 节）。静态库场景下重新编译消费方翻译单元即可；共享库必须与消费方一起重新部署。

## 2. 符号新增（MINOR，2.x 内兼容）

以下均为增补，本身不会破坏已重建的消费方：

- `crpc::client::resilience_statistics` 与 `crpc::client::resilience_stats()`。
- `crpc::resilience::fallback_statistics` 与 `crpc::client::fallback_stats()`。
- `crpc::client::call<Resp>::degraded()`（header-only 访问器）。
- `crpc::client::set_fallback`、`crpc::client::run_fallback` 与
  `crpc::client::record_fallbacks`。
- `crpc::server::builder::clock(resilience::clock_source)`。
- 易用性深度改进（方案 C）：`crpc::client::set_fallback_owned` 与
  `crpc::fallback_install`（RAII fallback 安装器）、
  `crpc::rpc::call_context::set_deadline` / `set_timeout` / `set_idempotent`
  与 `with_deadline` / `with_timeout` / `with_idempotent` 构造器、
  `crpc::client::call<Resp>::then`、共享的 `crpc::delivery_mode`、
  `crpc::service_id` / `crpc::method_id`、`crpc::result<T>::error_or` /
  `value_or`、`crpc::publish_result`，以及两参数的
  `CRPC_HTTP_GET("/path", lambda)` MVC 形式。客户端构造器新增
  `default_timeout` 与 `per_connection_inflight` 弃用别名，服务端构造器新增
  `max_inflight_per_connection`；规范名称是 `default_deadline` 与
  `max_inflight_per_connection`。

`server::publish` 现在返回 `publish_result` 而非 `bool`。到 `bool` 的转换是隐式的
（仅 `ok` 为 `true`），因此 `if (server.publish(...))` 与
`CRPC_CHECK(server.publish(...))` 仍可编译且仍表示「ok」；把结果显式写成 `bool`
变量的调用方也可编译。见 [CHANGELOG.zh-CN.md](CHANGELOG.zh-CN.md)。

没有任何导出符号被删除或改签名。共享构建现在导出全部文档化公开 API，见 [CHANGELOG.zh-CN.md](CHANGELOG.zh-CN.md)。

## 3. 不构成 ABI 破坏的行为变更

- 服务端在四个执行前准入拒绝点写出保留响应 meta 条目 `crpc-degradable: true`（服务级熔断器、deadline 仍可达时的服务级令牌桶、服务级 bulkhead 与服务级在途上限）。忽略未知 meta 键的客户端不受影响。
- HTTP MVC 错误响应在 `rpc_error::retry_after > 0` 时携带 `Retry-After`。
- `COMPRESSED` 帧不再被当作明文交付。`decode_frame` 对 `COMPRESSED` 帧返回 `protocol_error`（它没有输出缓冲），`frame_decoder` 在拷贝模式下解压、在借用模式下拒绝。手工构造压缩帧的对端现在会得到协议错误。
- 帧级压缩支持 LZ4 block 编解码器（`ALGO = 1`），其它算法 id 一律 fail-closed。

## 4. 迁移步骤

1. 用 2.0.0 的头文件重建消费方。
2. 用新头文件重新分配或值初始化每个 `http_statistics`、`server::options` 与 `server::builder`；不要反序列化 1.0.0 的数据块。
3. 链接 2.0.0 的库。
4. 重新运行测试套件：`src/abi_check.cpp` 中的布局护栏会在 x86 与 x64 上断言 `sizeof(server::options)` 与 `sizeof(http_statistics)`。

## 5. 未发布：`rpc_error` 布局与 `failure_origin` 迁移

未发布的错误诊断工作在 2.0.0 之上属于 MAJOR 变更：

- `rpc_error` 新增两个数据成员（`origin`、`attempts`），追加在 `metadata` 之后。初始化前五个成员仍可编译；按旧 `rpc_error` 分配或复制则不行。
- `failure_origin` 从 `client_detail` 迁入 `crpc`，重命名了两个导出符号（`breaker_record_for`、`result_origin`）。`client_detail::failure_origin` 别名保持源码兼容，但链接到旧符号的消费方必须重新链接。

每个消费方必须用同一份头文件重建并重新链接。旧/新 mangled 名见 [CHANGELOG.zh-CN.md](CHANGELOG.zh-CN.md)。
