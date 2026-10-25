# CRPC 服务定义体验 ② 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 CRPC 增加声明式 RPC 服务定义：`CRPC_RPC_METHOD` 宏 + `add_controller<Ctl>` 自动注册 + 确定性 ID + RTTI 参数绑定。

**Architecture:** 新增 `method_meta`（确定性 ID）与 `CRPC_RPC_METHOD` 宏；`server::builder::add_controller<Ctl>` 编译期遍历声明方法并复用现有 `register_typed_method`。

**Tech Stack:** C++17、RTTI 反射、CMake/Ninja、ctest。

**Spec:** `docs/superpowers/specs/2026-10-25-crpc-service-definition-design.md`

## Global Constraints

- C++17 为主；零警告（`/W4 /WX`）。
- **头文件最小化**：能放 `.cpp` 的实现必须放 `.cpp`；头文件只保留模板、内联与声明。模板实现若必须放头文件，须有明确理由并尽量缩短。
- 保留现有 `add_service` / `add_method` / `add_raw_method`。
- 不改变 `rpc_result<Resp>` 语义。
- 工作目录 `F:\crpc`；构建 `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4`；测试 `ctest`。
- 环境变量 `CRPC_RTTI_DIR=F:\RTTI`、`CRPC_COROCC_DIR=F:\corocc`。

## Review Focus

- `CRPC_RPC_METHOD` 宏展开是否与现有宏冲突。
- `add_controller<Ctl>` 是否注册全部声明方法。
- 确定性 ID 是否无碰撞。
- 参数绑定是否复用现有 RTTI 路径。
- 旧 API 是否不受影响。

---

### Task 1: `method_meta` 与确定性 ID

**Files:**
- Create: `include/crpc/rpc/method_meta.hpp`
- Test: `tests/test_rpc_method_meta.cpp`

**Interfaces:**
- Produces: `crpc::rpc::method_id_from<Ctl>()`、`crpc::rpc::method_meta`

- [ ] **Step 1: 写失败测试**

在 `tests/test_rpc_method_meta.cpp` 中验证 `method_id_from<Ctl>()` 确定性且不同类型不同。

- [ ] **Step 2: 运行测试确认失败**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_method_meta --output-on-failure`
Expected: FAIL（`method_id_from` 未定义）

- [ ] **Step 3: 实现 `method_meta`**

在 `include/crpc/rpc/method_meta.hpp` 中实现：
```cpp
namespace crpc::rpc {
template <typename Ctl>
[[nodiscard]] constexpr u64 method_id_from() noexcept {
    // FNV-1a of __FUNCSIG__/__PRETTY_FUNCTION__ 中的 Ctl 名
    ...
}
struct method_meta {
    u64 service_id;
    const char* name;
};
}
```

- [ ] **Step 4: 运行测试确认通过**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_method_meta --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/rpc/method_meta.hpp tests/test_rpc_method_meta.cpp
git commit -m "feat(rpc): add method_meta and deterministic method id"
```

---

### Task 2: `CRPC_RPC_METHOD` 宏

**Files:**
- Modify: `include/crpc/mvc/attributes.hpp`（或新 header）
- Test: `tests/test_rpc_method_meta.cpp`

**Interfaces:**
- Consumes: `crpc::rpc::method_meta`、`method_id_from<Ctl>()`
- Produces: `CRPC_RPC_METHOD(Ctl, Method, name)`

- [ ] **Step 1: 写失败测试**

验证 `CRPC_RPC_METHOD` 声明后，`Ctl::crpc_rpc_meta_##Method` 存在且携带正确 service_id/name。

- [ ] **Step 2: 运行测试确认失败**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_method_meta --output-on-failure`
Expected: FAIL（宏未定义）

- [ ] **Step 3: 实现宏**

```cpp
#define CRPC_RPC_METHOD(Ctl, Method, name_literal)                              \
    inline static constexpr ::crpc::rpc::method_meta crpc_rpc_meta_##Method{    \
        ::crpc::rpc::method_id_from<Ctl>(), name_literal};                      \
    friend class ::crpc::rpc::service_registrar<Ctl>;
```

- [ ] **Step 4: 运行测试确认通过**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_method_meta --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/mvc/attributes.hpp tests/test_rpc_method_meta.cpp
git commit -m "feat(rpc): add CRPC_RPC_METHOD macro"
```

---

### Task 3: `add_controller<Ctl>` 自动注册

**Files:**
- Modify: `include/crpc/server.hpp`（`builder`）
- Modify: `src/server/server.cpp`（`builder::add_controller` 定义）
- Test: `tests/test_rpc_service_definition.cpp`

**Interfaces:**
- Consumes: `CRPC_RPC_METHOD`、`rpc::register_typed_method`
- Produces: `builder::add_controller<Ctl>(Ctl&)`

- [ ] **Step 1: 写失败测试**

在 `tests/test_rpc_service_definition.cpp` 中：用 `CRPC_RPC_METHOD` 声明 `Ctl`，`builder.add_controller(ctl)`，验证方法可调用。

- [ ] **Step 2: 运行测试确认失败**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_service_definition --output-on-failure`
Expected: FAIL（`add_controller` 未定义）

- [ ] **Step 3: 实现 `add_controller`**

在 `server::builder` 中：
```cpp
template <typename Ctl>
builder& add_controller(Ctl& instance) {
    rpc::register_controller<Ctl>(registrations_, instance);
    return *this;
}
```
`register_controller` 编译期遍历 `Ctl` 的 `crpc_rpc_meta_*`，逐个调用 `register_typed_method`。

- [ ] **Step 4: 运行测试确认通过**

Run: `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4` 然后 `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_service_definition --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/server.hpp src/server/server.cpp tests/test_rpc_service_definition.cpp
git commit -m "feat(rpc): add add_controller auto-registration"
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
git commit -m "test(rpc): full verification for service definition"
```
