# CRPC 构建与测试

[English](building.md) | **简体中文**

版本 1.0 / 2026-10-19

本文说明如何在 Windows 与 Linux 上配置、构建、测试和诊断 CRPC。一键脚本是受支持的入口；CI 设计为调用同一批脚本，而不是复制构建逻辑。

## 目录

| # | 章节 |
|---|---|
| [1](#s1) | 环境要求 |
| [2](#s2) | 依赖仓库 |
| [3](#s3) | CMake 选项 |
| [4](#s4) | 静态与共享构建 |
| [5](#s5) | 一键脚本 |
| [6](#s6) | 构建布局与产物 |
| [7](#s7) | 消毒器 |
| [8](#s8) | Windows 无人值守测试 |
| [9](#s9) | CI 复用 |
| [10](#s10) | 故障排查 |
| [11](#s11) | 验证 |

## 1. 环境要求 <a id="s1"></a>

- CMake 3.16 或更高。
- C++17 编译器：MSVC 2022、GCC 9+ 或 Clang 10+。
- 警告策略：构建启用 `-Wall -Wextra -Wpedantic -Werror`（GCC/Clang）或 `/W4 /WX`（MSVC）；警告即错误。
- Ninja 优先；没有时 Windows 回退到 NMake Makefiles，Unix 使用默认生成器。
- 编译器自动探测：MSVC 通过 `vswhere`/`vcvars`，GCC/Clang 通过 `PATH`。
- 请求 `Both` 时需要容纳两棵构建树的磁盘空间。
- 默认所有产物位于 `.opencode/tmp` 下；脚本不会把构建输出放进源码树或 `C:` 盘。

已测试目标是 Windows x86/x64 与 Linux x64。`config.hpp` 也接受 macOS，但在那里只有进程内后端；epoll 后端仅限 Linux，IOCP 后端仅限 Windows。没有 GPU、数据库或外部服务依赖；测试全部在本机完成。

## 2. 依赖仓库 <a id="s2"></a>

两个可选依赖通过公开头文件与 CMake 目标消费：

- **RTTI** —— 反射与序列化（`include/rtti/reflect.hpp`）。
- **corocc** —— 有栈协程（`include/corocc/coro.h`）。

用缓存变量或环境变量指向它们的仓库根：

```powershell
$env:CRPC_RTTI_DIR   = 'F:\RTTI'
$env:CRPC_COROCC_DIR = 'F:\corocc'
.\scripts\build.ps1 -Config Debug -Arch x64
```

```sh
export CRPC_RTTI_DIR=/path/to/RTTI
export CRPC_COROCC_DIR=/path/to/corocc
./scripts/build.sh --config Debug --arch x64
```

变量只在缓存值为空时于配置期读取；脚本转发环境，不需要额外参数。每个根都会校验（`include/rtti/reflect.hpp`、`include/corocc/coro.h`），并以 `add_subdirectory(... EXCLUDE_FROM_ALL)` 加入构建树的 `_deps` 目录。RTTI 自带的测试条目在 `EXCLUDE_FROM_ALL` 下不会构建，因此按名称从 CTest 排除。`COROCC_BUILD_TESTS` 被强制关闭。

缺少依赖时，对应的 `CRPC_HAVE_*` 宏关闭：依赖 RTTI 的编解码器不注册，MVC 字段 attribute 宏不定义，协程相关组件不参与编译。当前 CRPC 源码不调用 `coro_rt_*`；corocc 会被链接，但运行时尚未使用。写明这一点是为了避免把“没有 corocc 的构建”误判为损坏的构建。

## 3. CMake 选项 <a id="s3"></a>

| 选项 | 默认 | 含义 |
|---|---|---|
| `CRPC_BUILD_TESTS` | `ON` | 构建 `tests/` 并注册 CTest 条目 |
| `CRPC_BUILD_BENCH` | `OFF` | 加入 `bench/`；该目录目前没有 `CMakeLists.txt`，保持关闭 |
| `CRPC_BUILD_EXAMPLES` | `OFF` | 加入 `examples/`；同样如此 |
| `CRPC_WARNINGS_AS_ERRORS` | `ON` | `/W4 /WX` 或 `-Werror` |
| `CRPC_ENABLE_LTO` | `ON` | 支持且未启用消毒器时，Release 使用 IPO/LTO |
| `CRPC_BUILD_SHARED` | `OFF` | 构建共享库而不是静态归档 |
| `CRPC_WITH_OPENSSL` | `OFF` | 查找并链接 OpenSSL，定义 `CRPC_HAVE_OPENSSL` |
| `CRPC_SANITIZER` | 空 | `address`、`undefined` 或 `thread` |
| `CRPC_RTTI_DIR` | 空 | RTTI 仓库根（环境回退） |
| `CRPC_COROCC_DIR` | 空 | corocc 仓库根（环境回退） |

`CRPC_WITH_OPENSSL` 会链接 `OpenSSL::SSL` 与 `OpenSSL::Crypto`，但尚无 TLS 源码消费该宏；HTTPS 属于**已设计，未实现**。启用该选项不应被理解为启用了 HTTPS。

库只有一个目标 `crpc`（别名 `crpc::crpc` 与 `crpc_core`）。源码用 `CONFIGURE_DEPENDS` 从 `src` 递归通配，因此新增文件无需编辑 `CMakeLists.txt`。

不经脚本的最小配置示例：

```powershell
cmake -S . -B .opencode/tmp/out/build/x64-Debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug -DCRPC_BUILD_TESTS=ON `
  -DCRPC_RTTI_DIR=F:/RTTI -DCRPC_COROCC_DIR=F:/corocc
cmake --build .opencode/tmp/out/build/x64-Debug --parallel
ctest --test-dir .opencode/tmp/out/build/x64-Debug --output-on-failure
```

## 4. 静态与共享构建 <a id="s4"></a>

默认是静态归档。`-DCRPC_BUILD_SHARED=ON` 构建共享库，并定义 `CRPC_SHARED`（公开）与 `CRPC_BUILDING`（私有）。共享构建设置隐藏可见性、隐藏内联符号、关闭 Windows 的全符号导出，并在非 MSVC 上加入 `-Wl,--no-undefined`。MSVC 上抑制 C4251，因为私有 STL 成员从不出现在签名中；[ABI 策略](abi.zh-CN.md)中的布局规则仍然适用，共享库必须与消费方使用同一工具链族与调试设置。

## 5. 一键脚本 <a id="s5"></a>

Windows：

```powershell
.\scripts\build.ps1 [-Config Debug|Release|Both] [-Arch x86|x64]
                    [-Sanitizer none|asan|ubsan|tsan] [-Jobs N] [-Clean]
                    [-SourceDir <path>] [-BuildRoot <path>]
.\scripts\test.ps1  [-Config Debug|Release|Both] [-Arch x86|x64]
                    [-Sanitizer none|asan|ubsan|tsan|all] [-Filter <regex>]
                    [-Repeat N] [-SourceDir <path>] [-BuildRoot <path>]
```

Linux/macOS：

```sh
./scripts/build.sh [--config Debug|Release|Both] [--arch x86|x64]
                   [--compiler gcc|clang] [--sanitizer none|asan|ubsan|tsan]
                   [--jobs N] [--clean] [--source DIR] [--build-root DIR]
./scripts/test.sh  [--config Debug|Release|Both] [--arch x86|x64]
                   [--compiler gcc|clang] [--sanitizer none|asan|ubsan|tsan|all]
                   [--filter REGEX] [--repeat N] [--source DIR] [--build-root DIR]
```

构建行为：`Config Both` 在独立目录中构建 Debug 与 Release。`-Jobs`/`--jobs` 默认为处理器数量。`-Clean`/`--clean` 只删除所选构建目录，绝不删除源码或整个产物根。Windows 脚本在检测到 `vcvars` 时于其中运行 CMake；POSIX 脚本显式选择 `g++`/`clang++`，`--arch x86` 会传 `-m32`。

测试行为：`test.ps1`/`test.sh` 先构建（调用构建脚本），然后在每个构建目录运行 CTest 并检查输出中的消毒器报告。`-Filter`/`--filter` 是 CTest 正则。`-Repeat`/`--repeat` 把整套 CTest 运行 N 次。`-Sanitizer all` 在独立目录中构建并运行 ASan、UBSan、TSan；这是 Linux/Clang 流程，因为 MSVC 没有 TSan。

参数语义：

- `-Config`/`--config`：`Debug`、`Release` 或 `Both`；`Both` 生成两个独立目录。
- `-Arch`/`--arch`：`x86` 或 `x64`；POSIX 的 x86 通过 `-m32` 实现。
- `-Sanitizer`/`--sanitizer`：`none`、`asan`、`ubsan`、`tsan`，测试脚本额外接受 `all`。
- `-Jobs`/`--jobs`：并行编译任务数；零或省略表示处理器数量。
- `-Filter`/`--filter`：传给 CTest `-R` 的正则，只影响测试选择。
- `-Clean`/`--clean`：只删除所选构建目录，可安全重复执行。
- `-SourceDir`/`--source` 与 `-BuildRoot`/`--build-root`：覆盖源码根与产物根。

每个脚本写出带时间戳的 UTF-8 日志，并在出现失败汇总时以非零退出。PowerShell 使用 `[System.IO.File]::AppendAllText` 写 UTF-8，而不是 `Tee-Object`，后者在 PowerShell 5.1 上写 UTF-16。

## 6. 构建布局与产物 <a id="s6"></a>

默认产物根是 `<source>/.opencode/tmp/out`。构建目录是 `build/<arch>-<config>[-<sanitizer>]`，例如 `.opencode/tmp/out/build/x64-Debug-asan`。日志位于 `.opencode/tmp/out/logs/build-<arch>-<config>-<sanitizer>-<timestamp>.log` 与 `test-...log`。测试期间的 minidump 写入 `.opencode/tmp/out/dumps`。

库文件是 Windows 的 `crpc.lib`/`crpc.dll` 或 Linux 的 `libcrpc.a`/`libcrpc.so`，位于所选构建目录。测试可执行文件为 `crpc_test_<area>`，由 `tests/CMakeLists.txt` 按源文件逐个注册。依赖构建位于 `<build>/_deps`。当前没有 `install()` 规则，因此 `cmake --install` 尚不是受支持的部署路径。

## 7. 消毒器 <a id="s7"></a>

脚本名称到 CMake 值的映射：

| 脚本名 | CMake `CRPC_SANITIZER` | 编译选项 |
|---|---|---|
| `asan` | `address` | `-fsanitize=address` / `/fsanitize=address` |
| `ubsan` | `undefined` | `-fsanitize=undefined` / `/fsanitize=undefined` |
| `tsan` | `thread` | `-fsanitize=thread`（仅 GCC/Clang） |

GCC/Clang 上构建会加入 `-fno-omit-frame-pointer`，并把消毒器加入链接选项。消毒器构建禁用 LTO。`test.ps1`/`test.sh` 检测 `AddressSanitizer`、`LeakSanitizer`、`UndefinedBehaviorSanitizer`、`ThreadSanitizer` 报告以及 `runtime error:`，即使进程以零退出也计为失败。

目标合并矩阵是 ASan+LSan（帧解析、缓冲区、槽回收、关闭竞争）、UBSan（Linux 上 `-fno-sanitize-recover=all`）与 TSan（多分片并发、Linux Clang `-O1 -g`）。当前代码没有分片，因此 TSan 覆盖仅限已实现的线程安全原语；这是覆盖缺口，不是通过声明。MSVC 支持 ASan/UBSan 选项但不支持 TSan；TSan 门禁请使用 POSIX 脚本。

常用调用：

```powershell
.\scripts\build.ps1 -Config Debug -Arch x64 -Sanitizer asan
.\scripts\test.ps1  -Config Debug -Arch x64 -Sanitizer asan -Repeat 3
```

```sh
./scripts/build.sh --config Debug --arch x64 --sanitizer ubsan
./scripts/test.sh  --config Debug --arch x64 --sanitizer all --compiler clang
```

## 8. Windows 无人值守测试 <a id="s8"></a>

Debug 构建遇到 CRT 断言、无效参数、纯虚调用或访问违例时可能弹出模态对话框，挂起无人值守运行。三层机制防止这一点：

1. `test.ps1` 设置 `HKCU\Software\Microsoft\Windows\Windows Error Reporting` 的 `DontShowUI=1` 与 `Disabled=1`，并导出 `CRPC_TEST_MINIDUMP_DIR` 指向 dump 目录。
2. `tests/test_framework.hpp` 把 `_CRT_WARN`/`_CRT_ERROR`/`_CRT_ASSERT` 重定向到 stderr，关闭 abort 的 report-fault，安装无效参数、纯虚调用与 `std::terminate` 处理器，设置 `SetErrorMode` 并安装未处理异常过滤器。过滤器打印异常码、地址与线程，写出 minidump（`MiniDumpWithDataSegs`、`MiniDumpWithHandleData`、`MiniDumpWithThreadInfo`、卸载模块、间接引用内存），然后以非零退出。
3. `test.ps1` 给每个 CTest 条目 300 秒超时，并在每轮后杀死残留的 `crpc_test_*` 进程，避免卡死的子进程污染下一轮。**`test.sh` 不添加这个逐测试超时与残留进程清理；** POSIX 使用 CTest 默认值。

这些护栏在依赖 MSVC CRT 的地方仅限 Debug。Release 崩溃仍应导致进程失败，只是可能不留下 minidump。

## 9. CI 复用 <a id="s9"></a>

设计的 CI 矩阵是 MSVC x64 Debug/Release、MSVC x86 Debug/Release、GCC x64、Clang x64、ASan、UBSan、TSan、fuzz smoke、黄金字节与基准报告。CI 应像开发者一样调用 `scripts/build.*` 与 `scripts/test.*`；目前没有提交 CI YAML，脚本之外没有复制构建逻辑。本地等价命令是 `.\scripts\test.ps1 -Config Both -Arch x64 -Sanitizer none` 与 `./scripts/test.sh --config Debug --arch x64 --sanitizer all --compiler clang`。

“零警告”声明要求构建日志中没有 `warning:` 行；即使编译器没有报错，脚本也会把警告视为失败。`.opencode/process.md` 记录的 2026-10-19 状态是 x64 Debug 与 x86 Debug 上 16 个套件全绿；请重新运行脚本确认当前代码树。

## 10. 故障排查 <a id="s10"></a>

| 症状 | 原因 / 处理 |
|---|---|
| `CRPC_RTTI_DIR does not look like an RTTI checkout` | 指向仓库根，而不是 `include/` |
| `CRPC_COROCC_DIR does not look like a corocc checkout` | 指向包含 `include/corocc/coro.h` 的根 |
| 配置在 `bench`/`examples` 失败 | 保持 `CRPC_BUILD_BENCH`/`CRPC_BUILD_EXAMPLES` 关闭；目录为空 |
| 在 MSVC 上请求消毒器 | TSan 不可用；TSan 任务使用 Linux/Clang |
| 找不到 `cmake` 或 `ninja` | 脚本搜索 `PATH`、Program Files 与 VS 包；安装或加入 `PATH` |
| Windows 上测试挂起 | 检查 `.opencode/tmp/out/dumps`、测试日志与 300 秒 CTest 超时 |

## 11. 验证 <a id="s11"></a>

构建通过的标准是：

- 配置成功，且日志中没有 `warning:` 或 `error:`；
- 每个目标零警告零错误编译；
- `ctest` 报告所有注册测试通过，没有 `Not Run` 或超时；
- 消毒器运行没有 ASan/LSan/UBSan/TSan 诊断；
- 生成的库能链接进目标架构的测试可执行文件。

把命令、构建目录、日志路径与结果记录在任务报告中，而不是在没有输出的情况下断言成功。
