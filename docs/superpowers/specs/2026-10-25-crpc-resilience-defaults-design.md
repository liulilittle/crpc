# CRPC 弹性默认设计（工作流 ④）

日期：2026-10-25
状态：设计已定稿，待写实现计划

## 背景

当前弹性配置已有默认值（`retry_policy`、`breaker_config`、`rate_limit_config`、`bulkhead_config`），但需要用户在 `builder` 中显式设置，且默认值不随调用特征自适应。用户要求"零配置智能默认"。

## 目标

- 零配置即可用：builder 默认即提供合理超时/重试/熔断/限流。
- 智能默认：按调用特征（idempotent、deadline、延迟 SLO）推导。
- 自适应：熔断阈值基于观测 p99；重试预算基于 deadline；限流基于端点容量。
- 可覆盖：用户仍可显式配置。

## 设计

### 1. `resilience::smart_policy`

```cpp
struct smart_policy {
    std::chrono::nanoseconds timeout;       // 默认 1s
    retry_policy retry;                      // 基于 timeout 推导
    breaker_config breaker;                  // 基于 p99 目标推导
    rate_limit_config rate;                  // 基于端点容量推导
    bulkhead_config bulkhead;                // 基于并发上限推导
};
smart_policy make_smart_policy(const call_traits& traits);
```

### 2. 推导规则

- **timeout**：`traits.deadline` 若显式则用；否则 `1s`（可配置）。
- **retry**：`max_attempts = 3`（idempotent）或 `1`（非 idempotent）；`initial_backoff = timeout/10`；`max_backoff = timeout/2`。
- **breaker**：`window = 10s`；`p99_target = timeout*2`；`slow_call_threshold = timeout`；`open_duration = 5s`。
- **rate**：`rate = 端点容量/秒`；`burst = rate/10`。
- **bulkhead**：`capacity = 端点并发上限`。

### 3. 自适应

- 熔断：运行期用观测 p99 更新 `slow_call_threshold`（指数移动平均）。
- 重试：预算基于剩余 deadline。
- 限流：基于端点健康度动态调整。

### 4. 集成

- `client::builder` 默认使用 `smart_policy`。
- `server::builder` 默认使用 `smart_policy`。
- 用户显式配置覆盖 smart 推导。

## 测试

- 零配置：不设置任何弹性参数，调用成功。
- 智能推导：idempotent 调用 max_attempts=3；非 idempotent=1。
- 自适应：熔断阈值随观测 p99 变化。
- 覆盖：显式配置覆盖 smart。

## 兼容

- 保留现有 `retry_policy`/`breaker_config` 等类型。
- 新增 `smart_policy` 与 `make_smart_policy`。
- 跑 `abi_check`。
