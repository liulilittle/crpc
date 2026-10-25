# 发布说明

[English](CHANGELOG.md) | **简体中文**

## 未发布

### 错误诊断

- `rpc_error` 现在携带终结失败的来源与逻辑调用尝试次数：`failure_origin origin`（默认 `local_lifecycle`）与 `std::uint32_t attempts`（默认 `0`），追加在 `metadata` 之后（`include/crpc/error.hpp:179-183`）。按名称初始化前五个成员的聚合/位置初始化保持可编译。
- `crpc::to_string(const rpc_error&)` 把码、消息、来源与尝试次数渲染为一行。七个分类谓词（`is_local`、`is_network`、`is_timeout`、`is_circuit_breaker`、`is_rate_limiter`、`is_fallback`、`is_server`）把 `origin` 映射到失败的子系统，调用方询问它关心的问题，而不必在单个枚举上 switch。
- 进程级完成钩子（`diagnostics::set_completion_hook` / `on_completion`）恰好一次地观察每个终结的 `response<Resp>::get()` 结果，且不持任何库锁。当未安装用户钩子时，它在 `after_call` 运行进程级 `obs::interceptor_chain`（`obs::set_default_interceptor_chain`），使已安装的拦截器看到终结错误；未安装观察者时该路径为无锁、无分配的 no-op。
- `response<Resp>::get()` 在调用携带有效 trace 上下文时，把调用的 W3C trace id 挂到终结错误的 metadata 的 `trace_id` 键下，使失败可与产生它的 trace 关联，无需重新推导上下文。
- 影响：行为为 MINOR；下方 ABI 部分为 MAJOR。

### ABI 破坏：`rpc_error` 布局与 `failure_origin` 迁移

- `rpc_error` 由五个数据成员增至七个（`origin`、`attempts`）。公开结构体布局变更属于 MAJOR（[abi.zh-CN.md](abi.zh-CN.md) §1）。两个字段追加在末尾且有默认值，因此*初始化*前五个成员的消费者保持可编译，但按旧 `rpc_error` 分配或复制的消费者必须用同一份头文件重建。
- `failure_origin` 从 `client_detail` 迁入 `crpc`（核心错误头），使 `rpc_error` 无需循环依赖即可携带它。这重命名了两个导出符号：枚举类型现在 mangle 为 `crpc::failure_origin`，而非 `crpc::client_detail::failure_origin`。
  - `ABI 破坏：crpc::client_detail::breaker_record_for(crpc::client_detail::failure_origin, crpc::rpc_code)` 由 `?breaker_record_for@client_detail@crpc@@YA?AW4breaker_record@12@W4failure_origin@12@W4rpc_code@2@@Z` 改为 `?breaker_record_for@client_detail@crpc@@YA?AW4breaker_record@12@W4failure_origin@2@W4rpc_code@2@@Z`。
  - `ABI 破坏：crpc::client_detail::result_origin(const crpc::rpc_error&)` 由 `?result_origin@client_detail@crpc@@YA?AW4failure_origin@12@AEBUrpc_error@2@@Z` 改为 `?result_origin@client_detail@crpc@@YA?AW4failure_origin@2@AEBUrpc_error@2@@Z`。
  `client_detail::failure_origin` 仍是别名，既有拼写仍可编译，但链接的符号已改变。
- 影响：MAJOR。每个消费者必须用同一份头文件重建并重新链接。见 [abi.zh-CN.md](abi.zh-CN.md) §1、§4 与 §8.3，以及 [migration.zh-CN.md](migration.zh-CN.md)。

## 2.0.0 - 2026-10-24

### 变更（MINOR，仅行为）

- 服务端现在会在四个执行前准入拒绝点写出保留的响应 meta 条目 `crpc-degradable: true`：服务级熔断器（`unavailable`）、deadline 仍可达时的服务级令牌桶（`resource_exhausted`）、服务级 bulkhead（`overload`）与服务级在途上限（`overload`）。只有当请求剩余的线上 deadline 至少 1 ms 时才写（`resilience::min_remaining_ns`，现已与 dispatcher 自身的执行前 deadline 拒绝共用，不再有第二个字面量）。它让客户端能区分「handler 执行前被拒绝，降级值有意义」与「handler 执行后以同一个码失败」：handler 返回的 `overload` 与 bulkhead 返回的 `overload` 在线上除该条目外完全相同。五条错误路径永不携带它（连接级泛洪令牌桶、连接级在途上限、令牌桶的 deadline 不满足出口、dispatcher 的 deadline 拒绝、以及所有 handler 产生的错误），one-way 请求因没有响应帧也无法携带。`crpc-degraded` 在服务端仍无人写入，服务端仍从不调用 `fallback_execute`。
- 影响：MINOR，且属于**行为变更，不是 ABI 变更**。不新增、不移除、不改签名任何导出符号；没有公开记录改变布局或大小；不新增 `server::options` 字段，因此消费者在栈上分配 `server::builder` 不受影响。只有四个既有错误响应的负载各增长 23 字节（两字节 `meta_count` 由 `0x0000` 变为 `0x0001`，并追加一个 TLV 条目）。忽略未知 meta 键的客户端不受影响。见 [protocol.zh-CN.md](protocol.zh-CN.md) §10 与 [abi.zh-CN.md](abi.zh-CN.md) §4。

### 修复（MINOR，仅新增符号）

- `CRPC_BUILD_SHARED=ON` 构建现在导出全部文档化公开 API。此前只有 27 个类带
  `CRPC_API`，因此按 README 用法链接 `crpc.dll` / `crpc.so` 的消费者会在链接期
  因未解析外部符号失败：builder、进程内后端、全部韧性原语、push hub、MVC router、
  logger 与 tracer、HTTP 报文/编码/文件层、流控估计器、codec registry 以及 `obs`
  的 span 类型。现在 46 个公开类（43 个顶层类，加上嵌套的 `server::builder`、
  `client::builder`、`http::http_server::builder`）与 51 个含 out-of-line 定义的
  命名空间级函数，都在自己的声明上带上了装饰。另有 7 个跨 DLL 接口类
  （`io::completion`、`io::source`、`io::sink`、`io::connection`、`io::accept_sink`、
  `io::listener`、`io::backend_factory`）一并导出：消费者会继承它们并把对象传回库内，因此其析构
  vtable 槽与 RTTI 必须导出。
- 影响：MINOR，且是**纯符号变化**。没有符号被删除或改签名，没有公开记录改变布局
  或大小，也没有行为变化。Windows x64 上共享构建的导出表从 586 个名字增长到 1258
  个；静态构建不受影响，因为静态库按定义导出全部符号。
- 新增构建期门禁 `tests/test_shared_link.cpp`，仅在 `CRPC_BUILD_SHARED=ON` 时构建
  并运行。它是一个消费者：链接导入库并直接调用文档化 API，包括函数体引用
  out-of-line 成员的头内联模板，因此缺少装饰会在构建期失败，而不是在下游消费者
  那里。见 [abi.zh-CN.md](abi.zh-CN.md) 第 3 节与
  [building.zh-CN.md](building.zh-CN.md) 第 4 节。

### 新增（MINOR，仅新增符号）

- `client::resilience_statistics` 与 `client::resilience_stats()`：客户端韧性流水线的增补可观测性（`rate_limited`、`bulkhead_rejected`、`local_gate_rejected`、`endpoint_open`、`lb_unavailable`、`pre_send_retries`、`probe_admitted`、`retries_delayed_by_hint`、`bulkhead_in_use`）。不改变既有结构：`client::statistics` 的字段顺序、大小与含义保持不变，C ABI 入口不变。见 [abi.zh-CN.md](abi.zh-CN.md) §4 与 §8。
- `resilience::fallback_statistics` 与 `client::fallback_stats()`：`crpc/resilience/fallback.hpp` 中 fallback 原语的七计数器快照（`ineligible`、`skipped_deadline`、`attempted`、`failed`、`canceled`、`overrun`、`applied`），以及 `client::call<Resp>::degraded()`——它报告最终结果是否来自 fallback 函数（本切片恒为 false）。fallback 的实际接线见下文「fallback 接线与服务端时钟注入」，因此计数不再恒为 0。`client::fallback_stats()` 是该切片唯一新增的导出符号；按值返回的类型与 `degraded()` 访问器都是 header-only。见 [abi.zh-CN.md](abi.zh-CN.md) §4。

### 易用性深度改进（MINOR，仅符号）

易用性差距分析中的九个缺口被补齐。每一项都是增补式的：底层形式保留其签名，新名称是规范名称。

- `client::set_fallback_owned` 与 `crpc::fallback_install`：RAII fallback 安装器。`fallback_install` 用 `std::shared_ptr` 持有 `self`，并在销毁或 `reset` 时卸载该策略，因此五参数的 `set_fallback(function, self, min_remaining, retain, release)` retain/release 契约不再是安装策略的唯一方式。
- `rpc::call_context::set_deadline` / `set_timeout` / `set_idempotent` 与 `with_deadline` / `with_timeout` / `with_idempotent` 构造器：设置 deadline 时同时设置 `has_deadline`，因此只赋值 `deadline` 而不置标志位不再静默地表示「无超时」。
- `client::call<Resp>::then`：门面层的完成回调，与 `rpc::call<Resp>::then` 对应。调用终止时恰好触发一次，包括本地观测到的终止。
- `crpc::delivery_mode`：由 `crpc::rpc` 与 `crpc::push` 共享的一个枚举；两个命名空间别名是同一类型，因此 RPC 模式与推送模式无需转换即可互通。
- 构造器命名：`max_inflight_per_connection` 是客户端与服务端构造器上每连接在途上限的规范名称；服务端的 `per_connection_inflight` 是弃用别名。`default_deadline` 是客户端构造器的规范名称，`default_timeout` 是弃用别名。
- `crpc::service_id` 与 `crpc::method_id`：两个 id 的强类型 newtype。`service_id` 可从 `u64` 字面量隐式转换，但绝不从 `method_id` 转换，因此参数顺序颠倒会成为编译错误。
- `result<T>::error_or` 与 `value_or` 针对左值与 const 左值结果的重载；`call<Resp>::get()` 对无效句柄返回 `invalid_argument`，与协程外等待的 `failed_precondition` 区分开。
- `CRPC_HTTP_GET("/path", lambda)`：无需控制器类型的两参数 MVC 形式；路由序号与对象名来自 `__COUNTER__`，因此同一源行上的两次注册不会在 `__LINE__` 上冲突。
- `crpc::publish_result` 与 `server::publish`：`publish` 返回可区分的结果（`ok`、`empty_topic`、`not_running`、`unsupported_configuration`、`no_push_hub`、`internal_error`）。到 `bool` 的转换是隐式的，因此既有的 `if (server.publish(...))` 仍可编译且仍表示「ok」。
- 影响：MINOR，仅符号与行为。没有导出符号被删除或改签名，没有公开记录改变布局或大小（弃用的构造器别名是方法而非字段），没有线路值变化。不使用新名称的消费者不受影响。

### 文档与示例

- `examples/` 现在是已接入的构建目标。`examples/CMakeLists.txt` 在
  `CRPC_BUILD_EXAMPLES=ON` 下构建 `crpc_example_quickstart`——一个进程内程序，
  启动服务器、注册一个类型化方法、发起一次调用并处理一个推送订阅——并把
  它注册到 CTest；`examples/README.md` 说明如何构建与运行。此前该目录没有
  `CMakeLists.txt`，因此 `CRPC_BUILD_EXAMPLES` 必须保持关闭。
- `docs/api/README.md` 索引公开头文件与关键类型；`docs/tutorial.md` 从消息类型
  到一次调用与一个推送订阅逐步构建。两者均有中英版本，且 `README.md`、
  `README.zh-CN.md`、`docs/README.md`、`docs/README.zh-CN.md` 都链接了它们。
- 影响：仅文档与示例。没有公开头文件、符号、布局或行为变化；示例位于
  `src/` 与库目标之外。

### fallback 接线与服务端时钟注入

- 客户端现在在调用的最后一次尝试被终结、且其熔断结果被记录之后运行已安装的 fallback，
  落点是 `call<Resp>::get()` 的两个终态失败出口。`client::set_fallback(function, self,
  min_remaining, retain, release)` 安装策略，`client::run_fallback` 与
  `client::record_fallbacks` 是库内入口。保活 `self` 的引用在每次安装策略时取一次，
  在策略被替换或客户端销毁时释放一次；`self` 非空却缺少 `noexcept` 的
  `retain`/`release` 对会被拒绝，且什么都不安装。服务端四处执行前拒绝写下的
  `crpc-degradable` 是远端拒绝可降级的闸门；**已发送且非幂等**的调用永不降级，因为
  替换它的错误可能掩盖已经发生的副作用。超过 deadline 才返回的降级值报
  `deadline_exceeded`，不是降级成功。`crpc-degraded` 仍然没有线上写入者：可观测载体是
  `call<Resp>::degraded()` 与 `fallback_stats().applied`。
- 影响：新增符号属 MINOR，**下面的布局变更属 MAJOR**。既有成员偏移不变，但消费方必须
  用同一份头文件重建。
- `server::options` 尾部新增 `resilience::clock_source clock_source`（默认无效，库内替换
  为 `resilience::monotonic_clock()`），并新增 `server::builder::clock()` 设置它。同一个
  时钟现在同时供给每连接 rate 桶、每服务 rate 桶、每服务熔断器与 dispatcher 的 deadline
  判定，因此同一个请求不会再被两套「现在」判定。`sizeof(server::options)` 从 304 增长到
  **320**（x64）、从 256 增长到 **264**（x86）；`sizeof(server::builder)` 随之增长。按
  [abi.zh-CN.md](abi.zh-CN.md) 第 1 节，给公开结构体加字段属 MAJOR，因此本周期的 MAJOR
  记录也覆盖它。默认值完全保留旧行为：字段保持无效的 options 读 `monotonic_clock()`，
  与注入前服务端使用的 `std::chrono::steady_clock` 相同。
- ABI breaking：`server::options` 从 304 增至 320 字节（x64）、从 256 增至 264 字节
  （x86）；`server::builder` 随之增长（376 -> 392 / 296 -> 304）。没有导出符号改签名；
  新增三个（`client::set_fallback`、`client::run_fallback`、
  `client::record_fallbacks`）。

### 编译期布局护栏

- 新增内部翻译单元 `src/abi_check.cpp`（不进入任何头文件，也不导出符号），对跨越 ABI 边界的一部分公开记录做编译期断言：15 个记录的标准布局属性；15 个记录的精确成员数量（`CRPC_MEMBER_COUNT`，可捕获追加、中间插入或删除）；可捕获重排或改名的相对成员链（`CRPC_STEP`、`CRPC_STEP_SAME`、`CRPC_STEP_MIXED`、`CRPC_AFTER`）；`http_statistics` 的总宽度与尾字段位置；以及仅在 x86 与 x64 上的绝对 `sizeof`/`offsetof` 锚（`CRPC_ABS_SIZE`、`CRPC_ABS_OFF`）。成员数量正是能看到「尾追加被 x86 尾部填充吸收」这一情况的护栏。
- [abi.zh-CN.md](abi.zh-CN.md) §4 现在给出「跨越 ABI 边界」的范围谓词，并列出 36 个仍无编译期护栏的范围内记录（`crpc_test_abi_docs` 扫描确认的确切数量）。`crpc_test_abi_docs` 会重新扫描 `include/crpc/**/*.hpp` 与 `src/abi_check.cpp`：新增跨边界记录若不加入护栏，也不更新该清单，测试即失败。
- 已写入文档的盲区：成员数量护栏不校验名字（若记录存在相对链，则由相对链捕获；在 `rpc_error` 中则由 `src/abi_check.cpp` 的成员类型断言捕获）；在 15 个有成员数量护栏的记录中，`rpc_error` 是唯一没有相对链的，因此其内部的重排不会被该单元捕获。其构造函数只点名 `code`、`message`、`retryable`，所以不会捕获 `retry_after` 或 `metadata` 的改名；该单元的七条 `decltype` 断言点名了全部七个成员，因此其中任何一个改名都会在那里失败。`rpc_error` 与 `status` 的绝对 `sizeof` 被刻意不锁定，因为它取决于标准库配置。见 [abi.zh-CN.md](abi.zh-CN.md) §4。
- 影响：MINOR（仅构建期校验）。不破坏 ABI：不改变任何公开结构体布局，也不新增导出符号。运行时行为不变；构建约束收紧——护栏覆盖的布局变更现在会直接编译失败。见 [abi.zh-CN.md](abi.zh-CN.md) §4 与 §8。

### 客户端韧性流水线

- 每端点 rate limiter 与 bulkhead 准入，通过 `builder::per_endpoint_rate()` / `builder::per_endpoint_bulkhead()` 配置，可用 `builder::resilience_enabled(false)` 整体关闭。
- 实际执行序为 `select + allow -> probe 判定 -> rate -> bulkhead -> register -> pool/slot -> retry -> deadline`；半开探针跳过 rate 与 bulkhead，连接池与在途上限仍生效。
- 本地 gate 拒绝会忽略 breaker permit、携带 limiter/bulkhead 的 `retry_after`，且**不自动重试**。
- 远端 `retry_after` 是重试延迟下界（`max(backoff, hint)`），由调用 deadline 封顶；retry 预算在该判定之后才扣减。`retries_delayed_by_hint` 仅统计延迟确实由 hint 决定的那些重试。
- 选择循环用每端点位图跟踪已试候选，超过 512 个端点时自动扩容，不再静默忽略高位索引。
- `call::cancel()` 立即中断重试等待（plain-thread 用条件变量，分片用 5 ms 分片），并与完成回调注册串行化对低层调用句柄的访问。

### 准入阶段与 HTTP Retry-After

- 新增头文件 `crpc/resilience/admission.hpp`（header-only），把准入（admission）词汇集中到一处：`resilience::stage_kind`（10 个阶段）、`resilience::admission_decision`（`admit`、`reject`、`probe_bypass`；`probe_bypass` 为保留值，尚无阶段消费）、`reject_code(stage_kind, bool deadline_met = true)`——唯一的「阶段到 `rpc_code`」表，包含 rate 阶段的两种结果（deadline 仍可达时为 `resource_exhausted`，否则为 `deadline_exceeded`）——以及 `fallback_eligible(rpc_code, stage_kind, bool one_way)`；它只回答某个 (code, stage) 对是否可降级，fallback 的实际执行尚未实现。
- 本切片转换的 breaker、服务/端点 rate 与 bulkhead、服务容量、deadline 与预发送 dispatch 拒绝分支现已从该表取线上码；每连接洪泛桶、每连接在途上限、客户端连接池容量与「cannot track endpoint candidates」分支仍保留字面量。对已转换分支，码值不变，消息、`retryable`、`retry_after` 与各阶段计数器也不变：这一部分是零行为变化的重构。
- HTTP MVC 错误响应在 `rpc_error::retry_after > 0` 时新增 `Retry-After`：毫秒向上取整为整秒、下限为 1，hint 为 0 时不写该头。状态码映射不变。
- 影响：MINOR（新增头文件与内联 API；不新增导出符号）。不破坏 ABI：header-only，`stage_kind`/`admission_decision` 不是跨 ABI 类型，既有记录布局不变。行为变化仅限新增的 HTTP `Retry-After` 响应头；RPC 线上码与消息不变。

### HTTP 长轮询

- 服务器自有 housekeeping 定时器在之后没有 poll 到达时也会清理空闲订阅；由
  `start()` 装配、`stop()` 在拆 hub 之前取消，并由回调自行重新装配。
- `start()` 把前端重放上限收敛到队列上限（`max_replay < max_messages` 会被抬高，
  反之被拒绝）。条数上限与每订阅字节预算相互独立；游标缺口超过任一上限时返回
  409 Restart，而不是静默丢弃未确认数据。`http_statistics::longpoll_replay_window`
  报告生效上限，服务器未运行（含 `start()` 失败）时为 0。
- `stop()` 保留监听配置，之后 `start()` 可重新绑定同样的端点。保存的
  `backend_factory` 指针是借用的：backend 必须活过整个 server 生命周期，包括每次
  `stop()` -> `start()`（已写在 `http_server::builder::add_listener()` 与
  `server::builder::add_listener()` 上）。同时修复 `rpc::server`：其第二次 `start()`
  过去会返回 ok 却不绑定任何监听。

### 异常与编译器特性契约

- 在受支持的无异常消费方配置下（预编译的异常启用库、`CRPC_HAVE_RTTI=0`），52 个公共头可在关闭异常时编译（`-fno-exceptions`、MSVC `/EHs-c-`）；带反射依赖（`CRPC_RTTI_DIR`）且关闭异常时，头阶段仍会失败（见 [building.zh-CN.md](building.zh-CN.md) §3）。以无异常方式重建库仍不受支持：`src/` 的 46 个翻译单元中，Linux GCC 有 11 个失败、MinGW GCC 12 个、MSVC 13 个。
- 异常契约按边界分别说明。头内联边界（`obs::interceptor_chain::run`、`mvc::invoke_http`）跟随消费方的异常选项；启用异常时抛出的异常会被捕获并转为 `rpc_code::internal` / HTTP 500 `internal error`。库翻译单元边界（`mvc::router::handle`、`rpc::dispatcher::dispatch`）跟随库的选项；以异常构建的库会捕获抛出的 handler 并转为 500/`internal`。无异常支持时，抛出不在本库契约内，实践中会 terminate。见 [architecture.zh-CN.md](architecture.zh-CN.md#s15)。
- 新增宏 `CRPC_CAN_USE_TYPEID`：由编译器派生（`__cpp_rtti`；非 clang-cl 的 MSVC 恒为 `1`），外部定义即触发硬 `#error`。所有 `typeid` 使用都在该宏之后；编译器 RTTI 不可用时，type-erased codec 路径返回 `failed_precondition`（HTTP 409），而不是构造 `std::type_info`。`-fno-rtti` 支持仍然受限：启用反射依赖（`CRPC_RTTI_DIR`）时构建会死在依赖内部，该兜底分支目前在任何已文档化配置下都不可达；保留它是为了将来的工具链。见 [building.zh-CN.md](building.zh-CN.md) §3。
- 影响：MINOR（新增宏与新的消费方配置：无异常、`CRPC_HAVE_RTTI=0`）。不破坏 ABI：无布局或符号变更；该宏只在编译期选择内联分支。行为变化：消费方现在可以用无异常方式编译自己的翻译单元；在头内联边界（`run`、`invoke_http`）上，抛出的 hook 或 handler 会离开调用点而不是被捕获；库翻译单元边界仍跟随库的选项。

### ABI breaking：`http_statistics` 布局

- 相对已发布的 1.0.0（`bd006b2`），`http_statistics` 在本未发布周期新增了 19 个
  字段：整个长轮询块（`longpoll_waiters`、`longpoll_subscribers`、
  `longpoll_replay_window`、`longpoll_events`、`longpoll_timeouts`、
  `longpoll_ack_clamped`、`longpoll_conflicts`、`longpoll_restarts`、
  `longpoll_overloaded`）以及 10 个 SSE/压缩/下载计数器。`sizeof` 从 72（9 字段）
  增长到 224（28 字段）；1.0.0 发布的 9 个字段偏移保持不变（0 … 64）。公开
  结构体新增字段属于 MAJOR 变更（[abi.zh-CN.md](abi.zh-CN.md) §1/§4）。
- 在该周期内部，`longpoll_replay_window` 最初放在 offset 168，后来被移到末尾
  （offset 216）；原先排在它后面的 6 个计数器各前移 8 字节
  （`longpoll_events` 176→168 … `longpoll_overloaded` 216→208）。移动前后
  `sizeof` 都是 224，因为移动前结构体尾部同样没有填充。没有任何已发布头文件
  使用过 offset 168 的布局（无 tag、无 1.0.1），因此该内部移动不构成对已发布
  消费者的破坏；真正破坏兼容的是 72 → 224 的增长。

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
`docs/protocol.md` §24。

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
