# CRPC 错误与诊断设计（工作流 ③）

日期：2026-10-25
状态：设计已定稿，待写实现计划

## 背景

当前错误模型已较完整：`rpc_code`（21 个码）、`rpc_error`（code/message/retryable/retry_after/metadata）、`result<T>`、`status`、`std::error_code` 互操作、HTTP 映射。`failure_origin` 与 `attempts` 存在于 `call_core`，但未进入 `rpc_error`。

缺失：
- `rpc_error` 无 `origin`/`attempts` 字段。
- 无 `to_string(const rpc_error&)` 可读串。
- 无统一错误分类谓词（local/network/timeout/circuit_breaker/rate_limiter/fallback）。
- 完成时无统一诊断钩子（interceptor 已存在但未与错误路径绑定）。

## 目标

- `rpc_error` 增加 `failure_origin origin` 与 `uint32_t attempts`（默认 `local_lifecycle`/`0`）。
- `to_string(const rpc_error&)` 返回可读串。
- 统一分类谓词：`is_local`、`is_network`、`is_timeout`、`is_circuit_breaker`、`is_rate_limiter`、`is_fallback`、`is_server`。
- 完成诊断钩子：`diagnostics::on_completion(...)`，默认触发 interceptor；可选 debug 日志。
- 追踪：`trace_id`/`span_id` 自动传播（复用现有 tracer）。

## 设计

### 1. 扩展 `rpc_error`

```cpp
struct rpc_error {
    rpc_code code = rpc_code::ok;
    std::string message;
    bool retryable = false;
    std::chrono::milliseconds retry_after{0};
    failure_origin origin = failure_origin::local_lifecycle;
    std::uint32_t attempts = 0;
    metadata_list metadata;
    ...
};
```

### 2. 可读串与分类

```cpp
CRPC_API std::string to_string(const rpc_error& err);
CRPC_API bool is_local(const rpc_error&) noexcept;
CRPC_API bool is_network(const rpc_error&) noexcept;
CRPC_API bool is_timeout(const rpc_error&) noexcept;
CRPC_API bool is_circuit_breaker(const rpc_error&) noexcept;
CRPC_API bool is_rate_limiter(const rpc_error&) noexcept;
CRPC_API bool is_fallback(const rpc_error&) noexcept;
CRPC_API bool is_server(const rpc_error&) noexcept;
```

### 3. 完成诊断钩子

```cpp
namespace diagnostics {
using completion_hook = void (*)(const rpc_error&, void* self) noexcept;
void set_completion_hook(completion_hook hook, void* self) noexcept;
void on_completion(const rpc_error& err) noexcept;  // 触发钩子 + 可选日志
}
```

- `response<Resp>::get()` 完成时调用 `diagnostics::on_completion(err)`。
- 默认钩子触发 `observability::interceptor`（若已安装）。
- 可选 debug 日志（编译期开关）。

### 4. 追踪

- `trace_id`/`span_id` 已在 `call_context` 中；`rpc_error.metadata` 携带 `trace_id`。
- `response` 完成时确保 `trace_id` 写回 metadata。

## 测试

- `to_string(rpc_error)` 包含 code 名、message、origin、attempts。
- 分类谓词覆盖全部 `failure_origin`。
- `on_completion` 触发钩子恰好一次。
- `response.get()` 完成时调用 `on_completion`。
- 追踪：`trace_id` 在 error metadata 中。

## 兼容

- `rpc_error` 新增字段有默认值，不破坏既有聚合初始化（除非用位置初始化）。
- 保留 `rpc_code` 编号。
- 跑 `abi_check`。

## ABI

- `rpc_error` 布局变化需评估；若跨 ABI，记录并跑 `abi_check`。
