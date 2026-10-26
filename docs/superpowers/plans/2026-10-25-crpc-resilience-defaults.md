# CRPC 弹性默认 ④ 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 CRPC 提供零配置智能弹性默认（超时/重试/熔断/限流/隔舱）。

**Architecture:** 新增 `resilience::smart_policy` 与 `make_smart_policy`，按调用特征推导；`client/server::builder` 默认使用。

**Tech Stack:** C++17、CMake/Ninja、ctest。

**Spec:** `docs/superpowers/specs/2026-10-25-crpc-resilience-defaults-design.md`

## Global Constraints

- C++17 为主；零警告（`/W4 /WX`）。
- **头文件最小化**：能放 `.cpp` 的实现必须放 `.cpp`。
- 保留现有 `retry_policy`/`breaker_config`/`rate_limit_config`/`bulkhead_config`。
- 工作目录 `F:\crpc`；构建 `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4`；测试 `ctest`。
- 环境变量 `CRPC_RTTI_DIR=F:\RTTI`、`CRPC_COROCC_DIR=F:\corocc`。

## Review Focus

- smart 推导是否覆盖 idempotent/deadline/latency。
- 自适应是否收敛。
- 显式配置是否覆盖 smart。
- 默认值是否合理（无回归）。

---

### Task 1: `smart_policy` 与推导

**Files:**
- Create: `include/crpc/resilience/smart_policy.hpp`
- Create: `src/resilience/smart_policy.cpp`
- Test: `tests/test_resilience_smart.cpp`

**Interfaces:**
- Produces: `crpc::resilience::smart_policy`、`make_smart_policy(const call_traits&)`

- [ ] **Step 1: 写失败测试**

在 `tests/test_resilience_smart.cpp` 中验证 `make_smart_policy` 对 idempotent/非 idempotent 的推导。

- [ ] **Step 2: 运行测试确认失败**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_resilience_smart --output-on-failure`
Expected: FAIL（符号未定义）

- [ ] **Step 3: 实现**

在 `include/crpc/resilience/smart_policy.hpp` 声明，`src/resilience/smart_policy.cpp` 实现。

- [ ] **Step 4: 运行测试确认通过**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_resilience_smart --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/resilience/smart_policy.hpp src/resilience/smart_policy.cpp tests/test_resilience_smart.cpp
git commit -m "feat(resilience): add smart_policy derivation"
```

---

### Task 2: builder 默认集成

**Files:**
- Modify: `include/crpc/client_facade.hpp`（`client::builder` 默认）
- Modify: `include/crpc/server.hpp`（`server::builder` 默认）
- Test: `tests/test_resilience_smart.cpp`

**Interfaces:**
- Consumes: `smart_policy`
- Produces: builder 默认使用 smart_policy

- [ ] **Step 1: 写失败测试**

验证不设置弹性参数时，builder 使用 smart 默认。

- [ ] **Step 2: 运行测试确认失败**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_resilience_smart --output-on-failure`
Expected: FAIL

- [ ] **Step 3: 实现**

在 `client::builder`/`server::builder` 默认使用 `smart_policy`。

- [ ] **Step 4: 运行测试确认通过**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_resilience_smart --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/client_facade.hpp include/crpc/server.hpp tests/test_resilience_smart.cpp
git commit -m "feat(resilience): use smart_policy by default in builders"
```

---

### Task 3: 自适应熔断

**Files:**
- Modify: `include/crpc/resilience/circuit_breaker.hpp`
- Modify: `src/resilience/circuit_breaker.cpp`
- Test: `tests/test_resilience_smart.cpp`

**Interfaces:**
- Produces: 熔断阈值随观测 p99 自适应

- [ ] **Step 1: 写失败测试**

验证熔断阈值随观测 p99 变化。

- [ ] **Step 2: 运行测试确认失败**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_resilience_smart --output-on-failure`
Expected: FAIL

- [ ] **Step 3: 实现**

在 `circuit_breaker` 中用观测 p99 更新 `slow_call_threshold`（EMA）。

- [ ] **Step 4: 运行测试确认通过**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_resilience_smart --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/resilience/circuit_breaker.hpp src/resilience/circuit_breaker.cpp tests/test_resilience_smart.cpp
git commit -m "feat(resilience): adaptive breaker threshold"
```

---

### Task 4: 全量验证

**Files:**
- Test: 全部

- [ ] **Step 1: 全量构建**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4`
Expected: `failures=0, warnings=0`

- [ ] **Step 2: 全量 ctest**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug --output-on-failure`
Expected: `100% tests passed`

- [ ] **Step 3: ABI 检查**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_abi_docs --output-on-failure`
Expected: PASS

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "test(resilience): full verification"
```
