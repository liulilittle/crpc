# CRPC RPC 易用性设计（工作流 ①：调用与返回体验）

日期：2026-10-25
状态：设计已批准（分节通过），待写实现计划

## 背景

CRPC 是 modern C++ RPC 框架，构建在 RTTI 反射/序列化与 corocc 栈协程之上。目标 C++ 标准为 C++17，同时兼容 C++20 编译；协程为 corocc 栈协程，不使用 C++20 `co_await`。

当前调用路径：

```cpp
auto result = client.service<Resp, Req>(service_id, method_id).call(args...);
auto resp = result.get();   // 普通线程阻塞；corocc 协程内 park
```

问题：同步、协程、回调、异步句柄、组合、取消等用法分散，样板多，错误处理不统一，缺少 `when_all`/`when_any` 等组合子。

## 目标

提供一个统一 `response<Resp>` 类型，让一次 RPC 调用同时支持：

- 同步阻塞获取（普通线程）
- corocc 栈协程内 `get()` 自动 park
- 显式 `wait()` / `park()`
- 非阻塞 `try_get()`
- 回调 `then()`
- 取值 `value()`（失败抛 `rpc_error`）/ `value_or(def)`
- 取消 `cancel()`
- 组合 `when_all` / `when_any`
- 诊断：错误码、origin、attempts、degraded、message
- 可观测性：interceptor、trace、可选日志
- 生命周期安全：client 销毁后 response 仍可用

## 非目标

- 不强制要求 C++20：C++17 构建必须能编译且运行；C++20 `co_await` 支持仅在 `__cplusplus >= 202002L` 且 `<coroutine>` 可用时启用。
- 不改变现有 `rpc_result<Resp>` 语义。
- 不破坏现有 `client::call<Resp>` / `stub::call` 调用代码。
- 不在本工作流实现服务定义、弹性默认、性能调优等其他工作流。

## 设计

### 1. 公共类型与入口

- `response_base`：类型擦除基类，持有 `std::shared_ptr<call_state_base>`。
  - `valid()` / `ready()` / `attempts()` / `degraded()`
  - `error()` / `code()` / `origin()`
  - `cancel()`（转发底层 `call::cancel()`，幂等）
  - `try_get()` 类型擦除（虚函数，由派生类实现）
  - `then()` 类型擦除（虚函数，由派生类实现）
- `response<Resp>`：继承 `response_base`，持有 `std::shared_ptr<call_state<Resp>>`。
  - `get()` / `wait()` / `park()`：返回 `rpc_result<Resp>`
  - `try_get(rpc_result<Resp>& out)`：非阻塞
  - `then(F&&)`：回调
  - `value()`：返回 `Resp`，失败抛 `rpc_error`
  - `value_or(def)`：失败返回默认值
- 入口：
  - `client.request<Resp>(service, method, args...)`：一步发起调用，返回 `response<Resp>`；序列化路径与 `stub::call` 完全一致（同一编码函数）。
  - `client.service<Resp, Req>(service, method).call(args...)`：保留 stub，返回 `response<Resp>`
  - `client.start_encoded<Resp>(...)`：返回 `response<Resp>`
- 构造后自动发起调用；`response` 持有 `shared_ptr`，client 销毁后仍安全。

### 2. 等待/获取/回调

- `get()` / `wait()` / `park()` 等价：
  - corocc 栈协程内：park 当前协程直到完成
  - 普通线程：带 deadline 阻塞直到完成
  - 返回 `rpc_result<Resp>`；幂等；已完成立即返回
- C++20 `co_await`（条件启用）：当 `__cplusplus >= 202002L` 且 `<coroutine>` 可用时，`response<Resp>` 提供 `operator co_await()`，返回 awaiter；awaiter 挂起并在完成时恢复，语义与 `get()` 一致。C++17 构建不声明该运算符。
- `try_get(out)`：非阻塞；未完成返回 false。
- `then(F)`：
  - 完成后回调 `void(rpc_result<Resp>)`
  - 已完成则立即调用
  - 回调在完成后台程调用，不持锁
  - 支持链式
- `value()` / `value_or(def)`：便捷取值。
- 组合：
  - `when_all(std::vector<response<Resp>>&)` → `response<std::vector<rpc_result<Resp>>>`；全部完成后返回；任一失败则整体失败（保留首个失败）。
  - `when_any(std::vector<response<Resp>>&)` → `response<rpc_result<Resp>>`；首个完成即返回，无论成功或失败；其余 response 仍可独立等待。

### 3. 错误/诊断/生命周期

- `rpc_result<Resp>` 保持不变：`ok()` / `value()` / `error()` / `code()` / `origin()` / `attempts()` / `degraded()`。
- `response<Resp>` 转发上述字段。
- 错误分类：`failure_origin` 映射到 `local_lifecycle` / `network` / `server` / `timeout` / `circuit_breaker` / `rate_limiter` / `fallback`。
- `rpc_error` 携带 `message` / `code` / `origin` / `attempts`；`to_string(rpc_error)` 可读。
- 可观测性：完成时触发 interceptor；可选 debug 日志；`trace_id` / `span_id` 自动传播。
- 取消：`cancel()` 幂等；取消后 `get()` 返回 `operation_canceled`。
- 生命周期：`response` 持有 `shared_ptr<call_state_base>`；`call_state` 持有 `shared_ptr<client_liveness>`；client 销毁后仍安全。

### 4. 兼容/迁移/测试

- 保留旧 `client::call<Resp>` / `stub::call` 返回类型不变。
- 新增 `client.request<Resp>(...)` 与 `response<Resp>`。
- 旧 `call<Resp>` 可隐式转 `response<Resp>`（或 `.response()`）。
- 迁移：旧代码不改；新代码用 `response`。
- 测试：
  - 单元：`get` / `wait` / `park` / `try_get` / `then` / `value` / `cancel`
  - 协程：corocc 协程内 `get()` 正确 park
  - 普通线程：`get()` 阻塞
  - 回调：`then` 在完成线程调用，不持锁
  - 组合：`when_all` / `when_any`
  - 生命周期：client 销毁后 response 仍可用
  - 错误路径：超时、熔断、取消、服务端错误
  - 兼容性：旧 `call<Resp>` 与 `response` 互转
- ABI：`client` 新增成员函数不改布局；`response` 为新类型；跑 `abi_check` 确认。

## 实现计划大纲

1. 新增 `include/crpc/rpc/response.hpp`：`response_base` / `response<Resp>` / `when_all` / `when_any`；条件 `operator co_await()`（`#if __cplusplus >= 202002L`）。
2. 修改 `include/crpc/client_facade.hpp`：新增 `client.request<Resp>(...)`；`stub::call` 返回 `response<Resp>`；`start_encoded` 返回 `response<Resp>`。
3. 修改 `src/client/client_facade.cpp`：实现新入口的类型擦除转发。
4. 新增 `tests/test_rpc_response.cpp`：覆盖全部行为。
5. 新增 `tests/test_rpc_response_coro.cpp`：corocc 栈协程内 park。
6. 新增 `tests/test_rpc_response_cpp20.cpp`：C++20 `co_await`（仅 C++20 构建编译）。
7. 新增 `tests/test_rpc_composition.cpp`：`when_all` / `when_any`。
8. 运行 `build.ps1 Debug x64`、`ctest`、`abi_check`；并验证 C++17 与 C++20 两种构建均通过。

## 风险

- 类型擦除的 `try_get` / `then` 虚函数可能增加开销；仅用于非热路径。
- `when_all` / `when_any` 的组合语义需明确（全成功 vs 首完成）。
- 与现有 `call<Resp>` 的隐式转换可能引起歧义；优先显式 `.response()`。
