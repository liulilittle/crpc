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
| 新增公开符号、末尾新增枚举值、通过"指针 + struct_size"传递的选项结构体末尾新增可选字段 | MINOR | 兼容 |
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
2. 在头文件中有 `sizeof` 与 `alignof` 的 `static_assert`；
3. 不含虚函数（虚分派仅限两个传输接口，且其对象从不按值跨越库边界）；
4. 不含条件成员（公开结构体内部禁止 `#if`）。

选项结构体采用尺寸标记模式：

```cpp
struct connect_options {
    std::uint32_t struct_size;   // = sizeof(connect_options)
    // ... fields ...
};
```

新版本库读取 `struct_size`，只访问调用方保证存在的字段。
这就是 MINOR 版本在不破坏旧调用方的前提下新增字段的方式。

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
共享库必须与消费方使用同一工具链族与调试设置；这一点写进构建文档，
并在 CMake 配置期可检测到不匹配时给出警告。

## 8. 变更流程

1. 违反第 2–5 节的变更必须提升 MAJOR 版本。
2. 同一个提交必须更新本文档、迁移指南（`docs/migration.zh-CN.md`），
   并在可行时增加兼容垫片。
3. 变更日志必须明确写出："ABI breaking: <符号> 从 … 变为 …"。
4. CI 运行 ABI 检查任务（`scripts/abi_check.ps1` / `.sh`），
   对比当前构建与上一个发布的导出符号表与布局断言。
   除非提交带有 MAJOR 提升标记，否则不匹配即构建失败。

## 9. 不保证的内容

- 模板实例化：`result<T>`、`span<T>` 与拦截器链是仅头文件的，会内联进消费方；
  它们的 ABI 属于消费方。
- 头文件中的内联函数：使用不同编译选项的消费方可能内联到不同版本；
  因此任何影响正确性的行为都不会只存在于内联函数里。
- `result<T>` 内部 `std::variant` 的布局（当 `T` 是用户类型时）：
  对给定工具链与 `T` 稳定，跨工具链不稳定。
