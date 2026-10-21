# CRPC ABI 策略

[English](abi.md) | **简体中文**

版本 1.0 / 2026-10-19

本文档定义 CRPC 在二进制层面保证什么、不保证什么，以及破坏二进制兼容的变更如何处理。
它是规范性的：违反它的发布即为发布缺陷。

## 1. 版本语义

CRPC 使用语义化版本：

```
MAJOR.MINOR.PATCH
```

| 变更 | 版本 | ABI 影响 |
|---|---|---|
| 删除或重命名公开符号、修改函数签名、修改结构体布局、修改跨线枚举值 | MAJOR | 破坏 |
| 修改任何公开结构体的布局（含新增字段） | MAJOR | 破坏（尚无尺寸标记选项结构体，见第 4 节） |
| 新增公开符号、末尾新增枚举值 | MINOR | 兼容 |
| 无接口变更的缺陷修复 | PATCH | 兼容 |

线上协议有自己的版本（`protocol_version`，当前为 `1`），与库版本独立：
库的补丁发布永不改变线上格式；线上格式变更必须提升协议版本并附带迁移文档。

## 2. 允许跨越 ABI 边界的类型

导出签名中只允许出现：

- 固定宽度的基本类型（`crpc::u8` … `crpc::u64`、`crpc::i8` … `crpc::i64`、
  `float`、`double`、`bool`）；
- `std::byte`、`std::size_t`、`std::ptrdiff_t`；
- `crpc::span<T>`、`crpc::buffer`、`crpc::rpc_error`、`crpc::result<T>`、
  `crpc::status`；
- 固定周期的 `std::chrono::duration` / `time_point`；
- 指向 CRPC 类型的指针与引用；
- `std::string_view`（两字宽、可平凡拷贝的视图）。

导出签名中禁止出现：

- 标准容器（`std::string`、`std::vector`、`std::map` 等）——其布局由实现定义，
  MSVC STL、libstdc++、libc++ 之间不一致；
- `std::function`、`std::any`、含非平凡备选项的 `std::variant`；
- 跨越边界抛出的异常（本库从不抛出）；
- `long`、`unsigned long`、`wchar_t`、未指定固定宽度的 `size_t`；
- 位域、匿名联合、packed 结构体。

确实需要容器时，API 使用 `span`、或 `const char*` 加长度，
或返回一个被文档标注为不透明的 CRPC 自有类型。

## 3. 符号可见性

- 每个导出符号都用 `CRPC_API` 声明（见 `include/crpc/config.hpp`）。
- Windows：共享库构建只导出 `CRPC_API` 符号，其余按链接器默认行为隐藏。
- Linux/macOS：共享库构建使用 `-fvisibility=hidden -fvisibility-inlines-hidden`，
  只有 `CRPC_API` 符号为默认可见。
- 静态库按定义导出全部符号；静态链接的消费方不受可见性规则约束，
  但仍受第 2 节的布局规则约束。

## 4. 结构体布局

每个跨越 ABI 边界的公开结构体必须：

1. 是标准布局且可平凡拷贝，或被明确标注为不透明、只允许指针/引用访问；
2. 不含虚函数（虚分派仅限传输接口，且其对象从不按值跨越库边界）；
3. 不含条件成员（公开结构体内部禁止 `#if`）。

**头文件目前尚未强制第 1 条。** 公开记录与选项结构体（例如 `frame_header`、
`crpc::connect_options`、`rpc::client_options`、`server::options`）中没有任何
`static_assert(sizeof(...))` 或 `static_assert(alignof(...))`。现存的断言覆盖的是
其它契约：

- `config.hpp:96-102` 固定整数宽度与 IEEE-754 格式；
- `observability/log.hpp:327` 限定 `log_record` 为 512 字节；
- `rpc/call.hpp:559-566` 限定内联回调存储。

若干公开类型（`rpc_error`、`buffer`、`endpoint`）含有标准库拥有型成员，因此不是
可平凡拷贝；它们按引用传递、作为 CRPC 自有类型返回，或被标注为不透明，不按值跨越
C ABI。为每个结构体在头文件中增加 `sizeof`/`alignof` 断言是计划中的工作，**尚未**
实施。在它存在之前，任何公开结构体的布局变更都按 MAJOR 处理（第 7 节），而不是
MINOR 扩展。

无需改动头文件即可检查的布局不变量集中在一个内部翻译单元 `src/abi_check.cpp`：
它断言公开记录的标准布局属性、每个跨边界记录的精确成员数量、
`http_statistics` / `client::statistics` / `client::resilience_statistics` /
`server::statistics` 各计数器的相对成员链（链条按每个成员自身的宽度推进，因此
混用 `std::size_t` 与 `std::uint64_t` 的 `server::statistics` 在 32 位与 64 位
目标上都能被检查）、`http_statistics` 的总宽度与尾字段位置，并且**仅在 x86 与
x64 上**断言该文件中列出的记录的绝对 `sizeof` 与关键 `offsetof` 数值。它随库一起
编译，因此在这些护栏覆盖的范围内，布局变更会导致构建失败。

这些护栏并不对所有变更类型、所有架构一视同仁，判断某个变更是否会被捕获时必须区分：

- 重排、在两个既有成员之间插入、删除、重命名成员：由相对成员链与成员数量在所有
  架构上捕获，因为这些护栏不依赖绝对数值。
- 在末尾追加字段：由成员数量在所有架构上捕获，并且在 x86 与 x64 上还会被绝对
  `sizeof` 锚额外捕获。x86 之所以必须依赖成员数量，是因为 x86 上
  `server::statistics` 为 104 字节而最后一个字段结束于 100，末尾 4 字节是填充；
  追加一个 `std::size_t` 会被这段填充吸收，`sizeof` 不变。因此**单靠绝对
  `sizeof` 断言在 x86 上抓不到尾追加**，这正是成员数量存在的理由。
- 在 x86 与 x64 之外的架构（例如 ARM64）上，不维护任何绝对 `sizeof` /
  `offsetof` 数值。尾追加仍由成员数量捕获，但如果某个变更同时保持成员数量与相对
  顺序不变、只改变绝对宽度，则在这些目标上**不会**在编译期被捕获。

该翻译单元不锁定含有标准库成员的类型的尺寸（`rpc_error`、`status`、`buffer`、
`endpoint`），因为它们的宽度属于标准库配置而非本库的属性：同一份头文件下，
`rpc_error` 与 `status` 在 x64 Release 为 80 字节、Debug 为 96 字节，在 x86 上
分别为 56 与 64 字节。它们由成员数量、标准布局前置条件以及
`sizeof(status) == sizeof(rpc_error)` 关系来守护。

**当前没有 size-tag 模式。** 没有任何公开头文件声明 `struct_size` 成员；
`crpc::connect_options` 只有超时、缓冲区与 no-delay 字段
（`include/crpc/io.hpp:186-191`）。`src/runtime/runtime.cpp:101` 处的
`struct_size` 赋值作用于 corocc 依赖中局部的 `coro_rt_driver_ops`，不是 CRPC 的
ABI 机制。尺寸标记选项结构体属于计划中的方案，**尚未**实现；在它缺席期间，调用方
与库必须使用同一份头文件构建。


最后一个已发布布局是 1.0.0（`bd006b2`）：`http_statistics` 只有 9 个字段，
`sizeof` 为 72 字节，完全没有任何长轮询计数器。本未发布周期新增了 19 个字段：
整个长轮询块（`longpoll_waiters`、`longpoll_subscribers`、
`longpoll_replay_window`、`longpoll_events`、`longpoll_timeouts`、
`longpoll_ack_clamped`、`longpoll_conflicts`、`longpoll_restarts`、
`longpoll_overloaded`）以及 10 个 SSE/压缩/下载计数器。`sizeof` 从 72（9 字段）
增长到 224（28 字段）。1.0.0 发布的 9 个字段偏移全部不变
（`active_connections` 0 … `timed_out_connections` 64）。公开结构体新增字段属于
MAJOR 变更（第 1 节），因此本周期属 ABI breaking。

`longpoll_replay_window` 的移动只发生在这个未发布周期内部：它最初放在 offset 168
（在 6 个计数器之前），后来被移到末尾（offset 216）。原先排在它后面的 6 个计数器
各前移一个 `std::size_t`（审计所用 64 位构建上为 8 字节）：

| 成员 | 移动前（`4adf359`） | 新偏移 |
|---|---|---|
| `longpoll_events` | 176 | 168 |
| `longpoll_timeouts` | 184 | 176 |
| `longpoll_ack_clamped` | 192 | 184 |
| `longpoll_conflicts` | 200 | 192 |
| `longpoll_restarts` | 208 | 200 |
| `longpoll_overloaded` | 216 | 208 |

该移动前后 `sizeof` 都是 224，原因是本次没有增删字段：`4adf359` 中 6 个计数器
紧跟 `longpoll_replay_window`（结束于 offset 176），正好填满 224 字节，移动前的
尾部填充为 0，移动后仍为 0。没有任何已发布头文件使用过 offset 168 的布局
（无 tag、无 1.0.1），所以该内部移动不构成对已发布消费者的破坏；真正破坏兼容的是
72 → 224 的增长。在尺寸标记选项结构体可用之前，任何公开结构体布局变更都按
MAJOR 处理（见第 8 节）。


增补类型不改变既有布局：`client::resilience_statistics`（标准布局、可平凡拷贝的值类型，见第 4 节）与 `client::resilience_stats()` 是 MINOR 新增符号，只增加导出；`client::statistics` 的字段顺序、大小与含义保持不变。

## 5. 枚举值

- 写入线上的枚举值永久固定，永不重新编号或复用。
- 新值追加在末尾。
- 解码器收到未知值必须 fail-closed（`protocol_error`），禁止映射为默认值。

## 6. C ABI 层

C++ API 是主接口。为语言绑定与插件作者提供 C ABI（`include/crpc/c_api.h`）：

- 不透明句柄（`crpc_client*`、`crpc_server*`）；
- 签名中不出现 C++ 类型；
- 所有字符串为 `(const char*, size_t)` 对；
- 状态以 `int` 返回，枚举有文档；
- 回调携带 `void* user` 上下文，永不抛出。

导出修饰在编译期选择，与 `CRPC_API` 一致：

| 构建形态 | 宏 | Windows | GCC/Clang |
|---|---|---|---|
| 静态库（默认） | 均无 | 无修饰 | 无修饰 |
| 动态库，构建中 | `CRPC_SHARED` + `CRPC_BUILDING` | `dllexport` | 默认可见性 |
| 动态库，消费方 | `CRPC_SHARED` | `dllimport` | 无修饰 |

C ABI 独立版本化（`CRPC_C_ABI_VERSION`），遵循相同的 MAJOR/MINOR 规则。

全部函数在 `src/c_api.cpp` 中基于 C++ 门面实现，不存在只有声明没有定义的符号。
`tests/test_c_api.c` 以 C 翻译单元编译（CMake 目标设置 `LINKER_LANGUAGE CXX`），
因此导出修饰写错或实现缺失会在构建期失败，而不是拖到下游消费者。

## 7. 兼容矩阵

针对版本 X 构建的二进制在版本 Y 上必须能链接并运行，当且仅当：

| X → Y | 保证 |
|---|---|
| 同一 MAJOR，Y.MINOR ≥ X.MINOR | 是（向前兼容） |
| 同一 MAJOR，Y.MINOR < X.MINOR | 否（新构建可能使用新符号） |
| 不同 MAJOR | 否 |

编译器与标准库：CRPC 不承诺跨标准库（MSVC STL / libstdc++ / libc++）
或跨不同 `_ITERATOR_DEBUG_LEVEL` / `_GLIBCXX_DEBUG` 设置的 ABI 兼容。
共享库必须与消费方使用同一工具链族与调试设置。这一点只写进构建文档：
没有 CMake 配置期的检查，增加该检查属于**尚未**实现的工作。

## 8. 变更流程

1. 违反第 2–5 节的变更必须提升 MAJOR 版本。
2. 同一个提交必须更新本文档与变更日志（[CHANGELOG.zh-CN.md](CHANGELOG.zh-CN.md)）。
   迁移指南（`docs/migration.md` / `docs/migration.zh-CN.md`）**均不存在**；
   可行时增加兼容垫片。
3. 变更日志必须明确写出："ABI breaking: <符号> 从 … 变为 …"。
4. **未实现 ABI 检查器**：`scripts/abi_check.ps1` 与 `scripts/abi_check.sh`
   都不存在，CI 也不比较导出符号表或布局断言。内部翻译单元
   `src/abi_check.cpp` 在编译期守护公开记录的布局，但它不是与上一发布版本的
   符号表对比。在该任务存在之前，比对方式是人工审阅与上一版本的 diff。

## 9. 不保证的内容

- 模板实例化：`result<T>`、`span<T>` 与拦截器链是仅头文件的，会内联进消费方；
  它们的 ABI 属于消费方。
- 头文件中的内联函数：使用不同编译选项的消费方可能内联到不同版本；
  因此任何影响正确性的行为都不会只存在于内联函数里。
- `result<T>` 内部 `std::variant` 的布局（当 `T` 是用户类型时）：
  对给定工具链与 `T` 稳定，跨工具链不稳定。
