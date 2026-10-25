# CRPC RPC 易用性 ① 实现计划：统一调用与返回

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 提供统一 `response<Resp>`，让一次 RPC 调用同时支持同步、corocc 栈协程、回调、异步句柄、取消、组合与 C++20 `co_await`（条件启用）。

**Architecture:** `response<Resp>` 包装 `rpc::rpc_result<client::call<Resp>>`，转发底层 `call<Resp>` 的等待/回调逻辑，并补充 `value()`/`value_or()`/`when_all`/`when_any`。C++20 `co_await` 通过 `#if __cplusplus >= 202002L` 条件启用。

**Tech Stack:** C++17（兼容 C++20）、MSVC 19.51、corocc 栈协程、CMake/Ninja、ctest。

**Spec:** `docs/superpowers/specs/2026-10-25-crpc-rpc-usability-design.md`

## Global Constraints

- C++17 为主；C++20 `co_await` 仅在 `__cplusplus >= 202002L` 且 `<coroutine>` 可用时启用。
- 零警告（`/W4 /WX`）；不破坏现有 `client::call<Resp>` / `stub::call` 调用代码。
- 不改变 `rpc_result<Resp>` 语义。
- 所有改动须有红/绿测试；构建命令 `scripts/build.ps1 -Config Debug -Arch x64 -Jobs 4`，测试 `ctest`。
- 工作目录 `F:\crpc`；临时构建 `F:\crpc\.opencode\tmp\`；禁止 `C:\` 路径。

## Review Focus

- `response<Resp>` 在 client 销毁后仍可用（生命周期）。
- `get()` 在 corocc 协程内 park，在普通线程阻塞。
- `then()` 回调在完成线程调用，不持锁，恰好一次。
- `when_all` 全成功 vs 任一失败；`when_any` 首完成。
- `cancel()` 幂等；取消后 `get()` 返回 `operation_canceled`。
- C++17 构建不因 C++20 代码而失败。

---

### Task 1: 为 `client::call<Resp>` 增加非阻塞访问器

**Files:**
- Modify: `include/crpc/client_facade.hpp:926-1316`（`client::call<Resp>` 类内）
- Test: `tests/test_rpc_response.cpp`（新增）

**Interfaces:**
- Consumes: `client::call<Resp>::state_`（`std::shared_ptr<client_detail::call_state<Resp>>`）
- Produces: `client::call<Resp>::code() noexcept`、`origin() noexcept`、`error() const noexcept`

- [ ] **Step 1: 写失败测试**

在 `tests/test_rpc_response.cpp` 中：

```cpp
CRPC_TEST(response_call_peek_accessors) {
    // 用一个已完成的 call 验证 code/origin/error 非阻塞可读
    // 具体构造见 Step 3
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_response --output-on-failure`
Expected: FAIL（`code`/`origin`/`error` 未定义）

- [ ] **Step 3: 实现访问器**

在 `client::call<Resp>` 的 `public:` 段（`attempts()` 之后）添加：

```cpp
[[nodiscard]] rpc_code code() const noexcept {
    if (state_ == nullptr) {
        return rpc_code::invalid_argument;
    }
    return state_->core.terminal.load(std::memory_order_acquire);
}

[[nodiscard]] client_detail::failure_origin origin() const noexcept {
    if (state_ == nullptr) {
        return client_detail::failure_origin::local_lifecycle;
    }
    std::lock_guard<std::mutex> lk(state_->core.mutex);
    return state_->core.origin;
}

[[nodiscard]] rpc_error error() const noexcept {
    if (state_ == nullptr) {
        return rpc_error(rpc_code::invalid_argument, "invalid call handle");
    }
    std::lock_guard<std::mutex> lk(state_->core.mutex);
    return state_->core.error;
}
```

- [ ] **Step 4: 运行测试确认通过**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_response --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/client_facade.hpp tests/test_rpc_response.cpp
git commit -m "feat(rpc): add non-blocking accessors to client::call"
```

---

### Task 2: 定义 `response_base` 与 `response<Resp>` 骨架

**Files:**
- Modify: `include/crpc/client_facade.hpp`（文件末尾，`client` 类之后）
- Test: `tests/test_rpc_response.cpp`

**Interfaces:**
- Consumes: `client::call<Resp>`、`rpc::rpc_result`
- Produces: `response_base`、`response<Resp>`

- [ ] **Step 1: 写失败测试**

在 `tests/test_rpc_response.cpp` 中：

```cpp
CRPC_TEST(response_basic_accessors) {
    // response<Resp> 的 valid/ready/attempts/degraded/code/origin/error/cancel
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_response --output-on-failure`
Expected: FAIL（`response` 未定义）

- [ ] **Step 3: 前向声明 `response`**

在 `include/crpc/client_facade.hpp` 中 `class CRPC_API client {` 之前添加：

```cpp
template <typename Resp>
class response;
```

- [ ] **Step 4: 实现 `response_base` 与 `response<Resp>` 骨架**

在 `include/crpc/client_facade.hpp` 末尾（`#endif` 之前，`client` 类定义之后）添加：

```cpp
class response_base {
public:
    virtual ~response_base() = default;
    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual bool ready() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t attempts() const noexcept = 0;
    [[nodiscard]] virtual bool degraded() const noexcept = 0;
    [[nodiscard]] virtual rpc_code code() const noexcept = 0;
    [[nodiscard]] virtual client_detail::failure_origin origin() const noexcept = 0;
    [[nodiscard]] virtual rpc_error error() const noexcept = 0;
    virtual void cancel() noexcept = 0;
};

template <typename Resp>
class [[nodiscard]] response final : public response_base {
public:
    response() = default;
    explicit response(rpc::rpc_result<client::call<Resp>> result)
        : result_(std::move(result)) {}

    [[nodiscard]] bool valid() const noexcept override {
        return result_.ok() && result_.value().valid();
    }
    [[nodiscard]] bool ready() const noexcept override {
        return result_.ok() && result_.value().ready();
    }
    [[nodiscard]] std::uint32_t attempts() const noexcept override {
        return result_.ok() ? result_.value().attempts() : 0;
    }
    [[nodiscard]] bool degraded() const noexcept override {
        return result_.ok() && result_.value().degraded();
    }
    [[nodiscard]] rpc_code code() const noexcept override {
        return result_.ok() ? result_.value().code() : result_.error().code;
    }
    [[nodiscard]] client_detail::failure_origin origin() const noexcept override {
        return result_.ok() ? result_.value().origin() : client_detail::result_origin(result_.error());
    }
    [[nodiscard]] rpc_error error() const noexcept override {
        return result_.ok() ? result_.value().error() : result_.error();
    }
    void cancel() noexcept override {
        if (result_.ok()) {
            result_.value().cancel();
        }
    }

private:
    rpc::rpc_result<client::call<Resp>> result_;
};
```

- [ ] **Step 5: 运行测试确认通过**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_response --output-on-failure`
Expected: PASS

- [ ] **Step 6: 提交**

```bash
git add include/crpc/client_facade.hpp tests/test_rpc_response.cpp
git commit -m "feat(rpc): add response_base and response skeleton"
```

---

### Task 3: 实现 `get()`/`wait()`/`park()`/`try_get()`/`value()`/`value_or()`

**Files:**
- Modify: `include/crpc/client_facade.hpp`（`response<Resp>` 类内）
- Test: `tests/test_rpc_response.cpp`

**Interfaces:**
- Consumes: `response<Resp>::result_`
- Produces: `get()`, `wait()`, `park()`, `try_get()`, `value()`, `value_or()`

- [ ] **Step 1: 写失败测试**

在 `tests/test_rpc_response.cpp` 中：

```cpp
CRPC_TEST(response_get_wait_try_value) {
    // 验证 get()/wait()/park() 等价；try_get 非阻塞；value 抛错；value_or 默认值
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_response --output-on-failure`
Expected: FAIL（方法未定义）

- [ ] **Step 3: 实现等待与取值**

在 `response<Resp>` 的 `public:` 段添加：

```cpp
[[nodiscard]] rpc::rpc_result<Resp> get() {
    if (!result_.ok()) {
        return rpc::rpc_result<Resp>(result_.error());
    }
    return result_.value().get();
}

[[nodiscard]] rpc::rpc_result<Resp> wait() { return get(); }
[[nodiscard]] rpc::rpc_result<Resp> park() { return get(); }

[[nodiscard]] bool try_get(rpc::rpc_result<Resp>& out) {
    if (!result_.ok()) {
        out = rpc::rpc_result<Resp>(result_.error());
        return true;
    }
    return result_.value().try_get(out);
}

[[nodiscard]] Resp value() {
    auto r = get();
    if (!r.ok()) {
        throw r.error();
    }
    return std::move(r).value();
}

[[nodiscard]] Resp value_or(Resp fallback) {
    auto r = get();
    return r.ok() ? std::move(r).value() : std::move(fallback);
}
```

- [ ] **Step 4: 运行测试确认通过**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_response --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/client_facade.hpp tests/test_rpc_response.cpp
git commit -m "feat(rpc): add get/wait/park/try_get/value to response"
```

---

### Task 4: 实现 `then()` 回调

**Files:**
- Modify: `include/crpc/client_facade.hpp`（`response<Resp>` 类内）
- Test: `tests/test_rpc_response.cpp`

**Interfaces:**
- Consumes: `response<Resp>::result_`
- Produces: `then(F&&)`

- [ ] **Step 1: 写失败测试**

在 `tests/test_rpc_response.cpp` 中：

```cpp
CRPC_TEST(response_then_callback) {
    // 验证 then 在完成线程调用，恰好一次；已完成则立即调用
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_response --output-on-failure`
Expected: FAIL（`then` 未定义）

- [ ] **Step 3: 实现 `then()`**

在 `response<Resp>` 的 `public:` 段添加：

```cpp
template <typename F>
[[nodiscard]] response& then(F&& fn) {
    if (result_.ok()) {
        result_.value().then(std::forward<F>(fn));
    }
    return *this;
}
```

- [ ] **Step 4: 运行测试确认通过**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_response --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/client_facade.hpp tests/test_rpc_response.cpp
git commit -m "feat(rpc): add then callback to response"
```

---

### Task 5: 实现 `when_all` / `when_any`

**Files:**
- Modify: `include/crpc/client_facade.hpp`（`response<Resp>` 之后）
- Test: `tests/test_rpc_composition.cpp`（新增）

**Interfaces:**
- Consumes: `response<Resp>`
- Produces: `when_all(std::vector<response<Resp>>&)`、`when_any(std::vector<response<Resp>>&)`

- [ ] **Step 1: 写失败测试**

在 `tests/test_rpc_composition.cpp` 中：

```cpp
CRPC_TEST(response_when_all_all_success) { /* ... */ }
CRPC_TEST(response_when_all_one_failure) { /* ... */ }
CRPC_TEST(response_when_any_first_completion) { /* ... */ }
```

- [ ] **Step 2: 运行测试确认失败**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_composition --output-on-failure`
Expected: FAIL（`when_all`/`when_any` 未定义）

- [ ] **Step 3: 实现组合子**

在 `response<Resp>` 之后添加：

```cpp
template <typename Resp>
[[nodiscard]] response<std::vector<rpc::rpc_result<Resp>>> when_all(
    std::vector<response<Resp>>& responses) {
    std::vector<rpc::rpc_result<Resp>> results;
    results.reserve(responses.size());
    rpc_error first_error;
    bool has_error = false;
    for (auto& r : responses) {
        auto res = r.get();
        if (!res.ok() && !has_error) {
            first_error = res.error();
            has_error = true;
        }
        results.push_back(std::move(res));
    }
    if (has_error) {
        return response<std::vector<rpc::rpc_result<Resp>>>(
            rpc::rpc_result<std::vector<rpc::rpc_result<Resp>>>(first_error));
    }
    return response<std::vector<rpc::rpc_result<Resp>>>(
        rpc::rpc_result<std::vector<rpc::rpc_result<Resp>>>(std::move(results)));
}

template <typename Resp>
[[nodiscard]] response<rpc::rpc_result<Resp>> when_any(
    std::vector<response<Resp>>& responses) {
    for (auto& r : responses) {
        rpc::rpc_result<Resp> out;
        if (r.try_get(out)) {
            return response<rpc::rpc_result<Resp>>(
                rpc::rpc_result<rpc::rpc_result<Resp>>(std::move(out)));
        }
    }
    // 全部未完成：返回一个无效 response，由调用者稍后重试
    return response<rpc::rpc_result<Resp>>();
}
```

- [ ] **Step 4: 运行测试确认通过**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_composition --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/client_facade.hpp tests/test_rpc_composition.cpp
git commit -m "feat(rpc): add when_all and when_any"
```

---

### Task 6: 增加 `client.request<Resp>(...)` 入口

**Files:**
- Modify: `include/crpc/client_facade.hpp`（`client` 类内声明；文件末尾定义）
- Test: `tests/test_rpc_response.cpp`

**Interfaces:**
- Consumes: `client::start_encoded<Resp>`、`rpc::call_codec<Req>::encode`
- Produces: `client::call<Resp>(service_id, method_id, const Req&, rpc::call_context)`

- [ ] **Step 1: 写失败测试**

在 `tests/test_rpc_response.cpp` 中：

```cpp
CRPC_TEST(response_client_call_entry) {
    // client.request<Resp>(service, method, request) 返回 response<Resp>
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_response --output-on-failure`
Expected: FAIL（`client::call` 未定义）

- [ ] **Step 3: 声明并定义入口**

在 `client` 类内（`service` 方法之后）声明：

```cpp
template <typename Resp, typename Req>
[[nodiscard]] response<Resp> call(service_id service, method_id method,
                                  const Req& request,
                                  rpc::call_context ctx = {}) {
    buffer encoded;
    const status encoded_status = rpc::call_codec<Req>::encode(request, encoded);
    if (!encoded_status.ok()) {
        return response<Resp>(rpc::rpc_result<client::call<Resp>>(
            encoded_status.error()));
    }
    auto result = start_encoded<Resp>(service.value, method.value,
                                      encoded.readable(), std::move(ctx));
    return response<Resp>(std::move(result));
}
```

- [ ] **Step 4: 运行测试确认通过**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_response --output-on-failure`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add include/crpc/client_facade.hpp tests/test_rpc_response.cpp
git commit -m "feat(rpc): add client.call entry point"
```

---

### Task 7: corocc 栈协程内 `get()` 测试

**Files:**
- Test: `tests/test_rpc_response_coro.cpp`（新增）

**Interfaces:**
- Consumes: `response<Resp>::get()`
- Produces: 协程内 park 行为验证

- [ ] **Step 1: 写测试**

在 `tests/test_rpc_response_coro.cpp` 中：

```cpp
CRPC_TEST(response_get_parks_in_corocc_coroutine) {
    // 在 corocc 协程内调用 response.get()，验证挂起后恢复
}
```

- [ ] **Step 2: 运行测试确认通过**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug -R crpc_test_rpc_response_coro --output-on-failure`
Expected: PASS

- [ ] **Step 3: 提交**

```bash
git add tests/test_rpc_response_coro.cpp
git commit -m "test(rpc): verify get() parks in corocc coroutine"
```

---

### Task 8: C++20 `co_await` 条件支持

**Files:**
- Modify: `include/crpc/client_facade.hpp`（`response<Resp>` 类内）
- Test: `tests/test_rpc_response_cpp20.cpp`（新增，仅 C++20 构建）

**Interfaces:**
- Consumes: `response<Resp>::get()`
- Produces: `operator co_await()`（仅 C++20）

- [ ] **Step 1: 写测试**

在 `tests/test_rpc_response_cpp20.cpp` 中：

```cpp
// 仅 C++20 构建编译
CRPC_TEST(response_co_await) {
    // auto resp = co_await client.request<Resp>(...);
}
```

- [ ] **Step 2: 运行测试确认通过（C++20 构建）**

Run: `cmake -S . -B .opencode/tmp/out/build/x64-Debug-cpp20 -DCMAKE_CXX_STANDARD=20 ... && ctest ...`
Expected: PASS

- [ ] **Step 3: 实现 `operator co_await()`**

在 `include/crpc/client_facade.hpp` 顶部（其他 `#include` 附近）条件包含：

```cpp
#if defined(__cpp_coroutines) && __cplusplus >= 202002L
#include <coroutine>
#endif
```

在 `response<Resp>` 的 `public:` 段添加（条件编译）：

```cpp
#if defined(__cpp_coroutines) && __cplusplus >= 202002L
    auto operator co_await() {
        struct awaiter {
            response& r;
            bool await_ready() const noexcept { return r.ready(); }
            void await_suspend(std::coroutine_handle<> h) {
                r.then([h](rpc::rpc_result<Resp>) mutable { h.resume(); });
            }
            rpc::rpc_result<Resp> await_resume() { return r.get(); }
        };
        return awaiter{*this};
    }
#endif
```

- [ ] **Step 4: 运行 C++17 构建确认未破坏**

Run: `ctest --test-dir .opencode/tmp/out/build/x64-Debug --output-on-failure`
Expected: PASS（C++17 不声明 `operator co_await`）

- [ ] **Step 5: 提交**

```bash
git add include/crpc/client_facade.hpp tests/test_rpc_response_cpp20.cpp
git commit -m "feat(rpc): conditionally support C++20 co_await"
```

---

### Task 9: 全量验证

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
git commit -m "test(rpc): full verification for response API"
```
