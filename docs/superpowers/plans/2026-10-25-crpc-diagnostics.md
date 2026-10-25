# CRPC 错误与诊断 ③ 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 CRPC 增加完整错误与诊断能力：`rpc_error` 携带 origin/attempts、`to_string`、分类谓词、完成诊断钩子、追踪元数据。

**Architecture:** 扩展 `error.hpp` 的 `rpc_error`；新增 `diagnostics.hpp`（分类谓词 + `to_string` + 完成钩子）；`response::get()` 完成时调用钩子。

**Tech Stack:** C++17、CMake/Ninja、ctest。

**Spec:** `docs/superpowers/specs/2026-10-25-crpc-diagnostics-design.md`

## Global Constraints

- C++17 为主；零警告（`/W4 /WX`）。
- **头文件最小化**：能放 `.cpp` 的实现必须放 `.cpp`。
- 不改变 `rpc_code` 编号；保留既有 `rpc_error` 聚合初始化兼容（新增字段有默认值）。
- 工作目录 `F:\crpc`；构建 `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4`；测试 `ctest`。
- 环境变量 `CRPC_RTTI_DIR=F:\RTTI`、`CRPC_COROCC_DIR=F:\corocc`。

## Review Focus

- `rpc_error` 布局变化是否破坏 ABI。
- `to_string` 是否覆盖全部字段。
- 分类谓词是否覆盖全部 `failure_origin`。
- `on_completion` 是否恰好一次、不持锁。
- `response.get()` 是否在所有完成路径调用钩子。

---

### Task 1: 扩展 `rpc_error`（origin/attempts）

**Files:**
- Modify: `include/crpc/error.hpp`
- Modify: `src/error.cpp`
- Test: `tests/test_diagnostics.cpp`

**Interfaces:**
- Produces: `rpc_error::origin`、`rpc_error::attempts`

- [ ] **Step 1: 写失败测试**

在 `tests/test_diagnostics.cpp` 中验证 `rpc_error` 可设置/读取 `origin`/`attempts`。

- [ ] **Step 2: 运行测试确认失败**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_diagnostics --output-on-failure`
Expected: FAIL（字段未定义）

- [ ] **Step 3: 扩展 `rpc_error`**

在 `include/crpc/error.hpp` 的 `rpc_error` 中增加：
```cpp
failure_origin origin = failure_origin::local_lifecycle;
std::uint32_t attempts = 0;
```
（`failure_origin` 定义在 `client_facade.hpp`；为避免循环依赖，将 `failure_origin` 移到 `error.hpp` 或前向声明。）

- [ ] **Step 4: 运行测试确认通过**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_diagnostics --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/error.hpp src/error.cpp tests/test_diagnostics.cpp
git commit -m "feat(diagnostics): add origin and attempts to rpc_error"
```

---

### Task 2: `to_string` 与分类谓词

**Files:**
- Create: `include/crpc/diagnostics.hpp`
- Create: `src/diagnostics.cpp`
- Test: `tests/test_diagnostics.cpp`

**Interfaces:**
- Consumes: `rpc_error`、`failure_origin`
- Produces: `to_string(const rpc_error&)`、`is_local`、`is_network`、`is_timeout`、`is_circuit_breaker`、`is_rate_limiter`、`is_fallback`、`is_server`

- [ ] **Step 1: 写失败测试**

在 `tests/test_diagnostics.cpp` 中验证 `to_string` 与全部分类谓词。

- [ ] **Step 2: 运行测试确认失败**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_diagnostics --output-on-failure`
Expected: FAIL（符号未定义）

- [ ] **Step 3: 实现**

在 `include/crpc/diagnostics.hpp` 声明，`src/diagnostics.cpp` 实现（**头文件最小化**）。

- [ ] **Step 4: 运行测试确认通过**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_diagnostics --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/diagnostics.hpp src/diagnostics.cpp tests/test_diagnostics.cpp
git commit -m "feat(diagnostics): add to_string and classification predicates"
```

---

### Task 3: 完成诊断钩子

**Files:**
- Modify: `include/crpc/diagnostics.hpp`
- Modify: `src/diagnostics.cpp`
- Modify: `include/crpc/client_facade.hpp`（`response::get()` 完成路径）
- Test: `tests/test_diagnostics.cpp`

**Interfaces:**
- Consumes: `diagnostics::on_completion`
- Produces: `diagnostics::set_completion_hook`、`on_completion`

- [ ] **Step 1: 写失败测试**

在 `tests/test_diagnostics.cpp` 中验证 `on_completion` 触发钩子恰好一次；`response.get()` 完成时调用。

- [ ] **Step 2: 运行测试确认失败**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_diagnostics --output-on-failure`
Expected: FAIL

- [ ] **Step 3: 实现钩子**

```cpp
namespace crpc::diagnostics {
using completion_hook = void (*)(const rpc_error&, void* self) noexcept;
void set_completion_hook(completion_hook hook, void* self) noexcept;
void on_completion(const rpc_error& err) noexcept;
}
```
在 `response<Resp>::get()` 的完成路径调用 `diagnostics::on_completion(err)`。

- [ ] **Step 4: 运行测试确认通过**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_diagnostics --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/diagnostics.hpp src/diagnostics.cpp include/crpc/client_facade.hpp tests/test_diagnostics.cpp
git commit -m "feat(diagnostics): add completion hook"
```

---

### Task 4: 追踪元数据

**Files:**
- Modify: `include/crpc/client_facade.hpp`（`response::get()` 写 trace_id 到 metadata）
- Test: `tests/test_diagnostics.cpp`

**Interfaces:**
- Consumes: `call_context` 的 `trace_id`
- Produces: error metadata 中的 `trace_id`

- [ ] **Step 1: 写失败测试**

验证 `response.get()` 失败时 `error().metadata` 含 `trace_id`。

- [ ] **Step 2: 运行测试确认失败**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_diagnostics --output-on-failure`
Expected: FAIL

- [ ] **Step 3: 实现**

在 `response::get()` 完成时把 `ctx.trace_id` 写入 `rpc_error.metadata`。

- [ ] **Step 4: 运行测试确认通过**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_diagnostics --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/client_facade.hpp tests/test_diagnostics.cpp
git commit -m "feat(diagnostics): propagate trace_id into error metadata"
```

---

### Task 5: 全量验证

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
Expected: PASS（若 `rpc_error` 布局变化，更新 abi 基线）

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "test(diagnostics): full verification"
```
