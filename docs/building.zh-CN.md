# CRPC 构建与测试

[English](building.md) | **简体中文**

版本 2.0 / 2026-10-24

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

变量只在缓存值为空时于配置期读取。脚本还会把已存在的环境值显式转成 `-DCRPC_RTTI_DIR=...` / `-DCRPC_COROCC_DIR=...`；当 `CRPC_RTTI_DIR` 缺失时，脚本打印警告并在配置前停止，因为本仓库总是构建测试套件，而使用反射宏的测试在没有它时无法编译。每个根都会校验（`include/rtti/reflect.hpp`、`include/corocc/coro.h`），并以 `add_subdirectory(... EXCLUDE_FROM_ALL)` 加入构建树的 `_deps` 目录。RTTI 自带的测试条目在 `EXCLUDE_FROM_ALL` 下不会构建，因此按名称从 CTest 排除。`COROCC_BUILD_TESTS` 被强制关闭。

严格的告警策略（GCC/Clang 的 `-Wall -Wextra -Wpedantic -Werror`，MSVC 的 `/W4 /WX`）通过 `target_compile_options` 挂在 crpc 自身的目标上，而不是目录上，因此不会传到这些 vendored 仓库：依赖的告警不会让 crpc 构建失败，crpc 的 `-Wpedantic` 也不会强加到第三方代码。

缺少依赖时，对应的 `CRPC_HAVE_*` 宏关闭：依赖 RTTI 的编解码器不注册，MVC 字段 attribute 宏不定义，协程相关组件不参与编译。无条件使用反射宏的测试目标（`test_codec`、`test_mvc`，shared 构建下还有 `test_shared_link`）会以 `STATUS` 提示跳过，其余测试照常构建并运行。当前 CRPC 源码不调用 `coro_rt_*`；corocc 会被链接，但运行时尚未使用。写明这一点是为了避免把“没有 corocc 的构建”误判为损坏的构建。

## 3. CMake 选项 <a id="s3"></a>

| 选项 | 默认 | 含义 |
|---|---|---|
| `CRPC_BUILD_TESTS` | `ON` | 构建 `tests/` 并注册 CTest 条目 |
| `CRPC_BUILD_BENCH` | `OFF` | 构建 `bench/`；基准程序不注册进 CTest，shared 构建下基准可执行文件与 `crpc.dll` 同处构建根目录 |
| `CRPC_BUILD_EXAMPLES` | `OFF` | 构建 `examples/`；每个 `quickstart*.cpp` 生成 `crpc_example_<文件名>` 并注册进 CTest（quickstart、microservice、MVC、game） |
| `CRPC_WARNINGS_AS_ERRORS` | `ON` | `/W4 /WX` 或 `-Werror` |
| `CRPC_ENABLE_LTO` | `ON` | 支持且未启用消毒器时，Release 使用 IPO/LTO |
| `CRPC_BUILD_SHARED` | `OFF` | 构建共享库而不是静态归档 |
| `CRPC_WITH_OPENSSL` | `OFF` | 查找并链接 OpenSSL，定义 `CRPC_HAVE_OPENSSL` |
| `CRPC_SANITIZER` | 空 | `address`、`undefined` 或 `thread` |
| `CRPC_RTTI_DIR` | 空 | RTTI 仓库根（环境回退） |
| `CRPC_COROCC_DIR` | 空 | corocc 仓库根（环境回退） |

`CRPC_WITH_OPENSSL` 链接 `OpenSSL::SSL` 与 `OpenSSL::Crypto` 并定义 `CRPC_HAVE_OPENSSL`。TLS 装饰器实现在 `src/io/tls_openssl.cpp`，经 `include/crpc/io/tls.hpp` 与 `http_server::listen_tls` 暴露；公共头从不包含 OpenSSL 头。关闭该选项时，同一组入口返回 `tls_errc::ssl_not_available`，`test_tls` 打印其文档化的 skip 行。

验证状态：guarded 分支已在 WSL + OpenSSL 3.0.13 与 Windows + OpenSSL 4.0.3（OpenSSL-Win64）下以 `CRPC_WITH_OPENSSL=ON` 构建并运行；两种配置的 `crpc_test_tls` 均为 8 cases / 80 assertions 全绿。该套件覆盖握手与 ALPN、验证失败、干净 EOF、背压与恢复、截断记录、会话恢复、证书重载以及成对的装饰器入口。Windows HTTPS 在该配置下已验证。

### 编译器特性要求

**异常。** 公共头可在无异常下构建；库本身不行。

- *使用预编译库、`CRPC_HAVE_RTTI=0`：* 52 个公共头都可用 `-fno-exceptions` 与 MSVC `/EHs-c-` 编译。库本身必须是默认的启用异常构建；只有消费方关闭异常。
- *在无异常下重建库不受支持：* `src/` 的 46 个 TU 中有 13 个含 `try`/`catch` 语句（例如 `src/observability/log.cpp` catch `:270` / try `:268`、`src/rpc/dispatcher.cpp` catch `:368` / try `:366`、`src/c_api.cpp` catch `:291` / try `:248`；MSVC 报的是 `try` 所在行）。失败数按目标不同：Linux GCC 46 个中 11 个、MinGW GCC 12 个、MSVC 13 个。算式：`src/io/iocp_backend.cpp` 只在**非 Windows** GCC 上是空 TU；MinGW GCC 会编译它，并在 `:1257` 的 `try`/`catch` 处失败，因此 MinGW GCC 在 46 个中失败 12 个。相对 Linux GCC，MSVC 多出 `src/http2/connection.cpp`——它自身没有 `try`/`catch`，但会触发 MSVC `<vector>(1224)` 里的 C4530，在 `/WX` 下失败；同时少了 `src/io/tls_openssl.cpp`——没有 OpenSSL 时其 `catch` 被预处理掉：11 + 1 + 1 = 13。CMake 没有移除异常处理的开关。
- *`CRPC_RTTI_DIR` + `-fno-exceptions`：* 在**头阶段**失败（早于任何 `src/` 文件）：MSVC 在 `rtti/convert.hpp:2597` 的 `try` 报 C4530（`/WX` 下为 C2220），GCC 在 `:2599` 的 `catch` 报 "exception handling disabled"。

各边界上抛出的语义见[架构 §15](architecture.zh-CN.md#s15)；这些开关只决定什么能编译，不决定异常如何被恢复。

**RTTI。** `-fno-rtti` 的实际行为取决于反射依赖是否存在，两种情形的失败方式差别很大。

- *`CRPC_HAVE_RTTI=0`：* `encode_into`/`decode_from` 是**编译期错误**，不是返回错误状态。switch 的 `crpc_binary` 分支会无条件实例化 `binary_codec::encode_typed`，其 `static_assert` 直接触发："crpc-binary requires the RTTI dependency"（`include/crpc/codec.hpp:1073`）。
- *反射开启 + `-fno-rtti`：* 编译在任何 CRPC 模板实例化之前就死在依赖里。在 GCC 13.3 与 Clang 18.1.3 上首个错误都是依赖自身的 `rtti/reflection.hpp:2183`（`typeid`）；GCC 共报 34 条（32 处 `typeid` 加 2 处 `rtti/proto.hpp:2527,2529` 的 `dynamic_cast`），Clang 默认在 20 条错误处截断，只有加 `-ferror-limit=0` 才会看到 `proto.hpp` 那两条。更新的 Clang 可能先报 `proto.hpp` 那对错误；无论哪种，失败都在依赖里，不在 CRPC 头里。
- *`CRPC_CAN_USE_TYPEID=0` 分支：* 未识别的自定义 codec id 返回 `failed_precondition`（HTTP 409）而不是走到 `typeid`（`include/crpc/codec.hpp:1553-1557`）；内置 id 仍正常编解码。该分支**当前在所有已文档化的配置下都不可达**：`CRPC_HAVE_RTTI=0` 会先撞上 `codec.hpp:1073` 的 `static_assert`（自定义 id 的调用同样会实例化 `crpc_binary` 分支），反射开启但无 `typeid` 则死在依赖里，MSVC 与 clang-cl 恒推导为 `1`，自行定义该宏会触发硬 `#error`（`codec.hpp:156-165`）。保留它是为了将来某个缺少 `typeid` 但具备反射库的工具链。
- *MSVC `/GR-`：* 在 `CRPC_HAVE_RTTI=0` 时可用（cl 在 `/GR-` 下仍接受本库使用的静态 `typeid(T)`）。启用反射库时，`/GR-` 在 `rtti/proto.hpp:2527,2529` 报 C4541（`/GR-` 下的 `dynamic_cast` 属未定义行为）：不加 `/WX` 时编译 EXIT=0 但带这两条警告——能编译，但含 UB；加 `/WX` 后转为 C2220。

另有两条一致性要求，构建错误都无法替你兜住：

- `codec.hpp` 的 inline 模板按 `CRPC_HAVE_RTTI` 分支，因此消费方该宏的值必须与库一致；库侧由 `CMakeLists.txt:117-120` 根据 `CRPC_RTTI_DIR` 决定。
- 同一个二进制内所有翻译单元的 RTTI 开关必须一致。flag 级混编对预处理器不可见，没有 `#error` 能抓住；链接器只会保留一份 inline 函数体，保留哪一份取决于链接顺序。用两个按 `__cpp_rtti` 分支的 TU 实测：`A(-frtti) B(-fno-rtti)` 输出 `b=1 a=1`，反序 `B(-fno-rtti) A(-frtti)` 输出 `b=0 a=0`。

`CRPC_HAVE_RTTI` 的含义是「rtti 反射库可用」，不是编译器 RTTI；两者正交。没有反射仓库时它为 `0`：文本/模式编解码器不注册，MVC 字段 attribute 宏不定义，MVC 绑定返回 `failed_precondition`（HTTP 409）"binding requires the RTTI dependency"（`mvc/binding.hpp:533`）。

```bat
rem MSVC：52 个头、无异常、无编译器 RTTI、告警即错误。
for /r include %f in (*.hpp *.h) do @cl /nologo /std:c++17 /EHs-c- /GR- /W4 /WX /TP /Zs /I include %f
```

```sh
# GCC：52 个头、无异常、告警即错误。
find include -name '*.hpp' -o -name '*.h' | while read -r f; do
  g++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic -Werror \
      -fno-exceptions -I include "$f" || echo "FAIL $f"
done
# 重建库在无异常下必须失败（GCC 上 46 个 TU 中 11 个）：
g++ -std=c++17 -I include -fno-exceptions -c src/observability/log.cpp -o /tmp/log.o
```

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

共享构建带有**链接门禁**：只要 `CRPC_BUILD_SHARED=ON` 且存在 RTTI 仓库（探针会注册反射 DTO），`tests/test_shared_link.cpp`
就会针对导入库编译，并以 `crpc_test_shared_link` 注册进 CTest。该文件是**消费者**
而不是单元测试：它只包含公开头文件并直接调用文档化的 API，因此共享构建漏导出的
任何公开符号都会在链接期变成未解析外部符号。它覆盖三类手写调用清单无法覆盖的
情况：头内联模板 `client_detail::wake_functor`（会把对 `call_core::signal` 的调用
编译进消费者自己的目标文件）、按值持有 `std::optional<bulkhead_permit>` 的类型
（其隐式析构函数在消费者侧生成）、以及 `parse_query`、`default_router`、`serialize`
等命名空间级辅助函数。`CRPC_BUILD_SHARED=OFF` 时该目标根本不会创建：静态库没有
导出表可出错。

`scripts/build.ps1 -Shared` 与 `scripts/build.sh --shared` 选择共享构建；
`scripts/test.ps1 -Shared` / `scripts/test.sh --shared` 还会直接运行该链接门禁，
因此带过滤的 CTest 调用无法跳过它。这条门禁强制的装饰规则见
[ABI 策略](abi.zh-CN.md) 第 3 节。

## 5. 一键脚本 <a id="s5"></a>

Windows：

```powershell
.\scripts\build.ps1 [-Config Debug|Release|Both] [-Arch x86|x64]
                    [-Sanitizer none|asan|ubsan|tsan] [-Jobs N] [-Clean] [-Shared]
                    [-Examples] [-NoTests] [-Bench]
                    [-SourceDir <path>] [-BuildRoot <path>]
.\scripts\test.ps1  [-Config Debug|Release|Both] [-Arch x86|x64]
                    [-Sanitizer none|asan|ubsan|tsan|all] [-Filter <regex>]
                    [-Repeat N] [-Shared] [-Examples] [-NoTests]
                    [-SourceDir <path>] [-BuildRoot <path>]
```

Linux/macOS：

```sh
./scripts/build.sh [--config Debug|Release|Both] [--arch x86|x64]
                   [--compiler gcc|clang] [--sanitizer none|asan|ubsan|tsan]
                   [--cxx-standard 17|20]
                   [--jobs N] [--clean] [--shared]
                   [--examples] [--no-tests] [--bench]
                   [--source DIR] [--build-root DIR]
./scripts/test.sh  [--config Debug|Release|Both] [--arch x86|x64]
                   [--compiler gcc|clang] [--sanitizer none|asan|ubsan|tsan|all]
                   [--cxx-standard 17|20]
                   [--filter REGEX] [--repeat N] [--shared]
                   [--examples] [--no-tests]
                   [--source DIR] [--build-root DIR]
```

构建行为：`Config Both` 在独立目录中构建 Debug 与 Release。
`-CxxStandard`/`--cxx-standard` 选择语言标准。`17` 是默认值，也是其余所有作业使用的值。
`20` 存在的原因有两条：库本身必须能在 C++20 下编译（`response<Resp>::operator co_await`
需要它）；且若没有任何构建请求它，`tests/test_rpc_response_cpp20.cpp` 在所有配置下都是
空操作 —— 在加入这个开关之前，它的用例从未在任何地方运行过。标准会进入构建目录名
（`...-cxx20`），因此 C++20 的配置不会静默地重配 C++17 的目录。
`-Jobs`/`--jobs` 默认为处理器数量。`-Clean`/`--clean` 只删除所选构建目录，绝不删除源码或整个产物根。Windows 脚本在检测到 `vcvars` 时于其中运行 CMake；POSIX 脚本显式选择 `g++`/`clang++`，`--arch x86` 会传 `-m32`。

测试行为：`test.ps1`/`test.sh` 先构建（调用构建脚本），然后在每个构建目录运行 CTest 并检查输出中的消毒器报告。`-Filter`/`--filter` 是 CTest 正则。`-Repeat`/`--repeat` 把整套 CTest 运行 N 次。`-Sanitizer all` 在独立目录中构建并运行 ASan、UBSan、TSan；这是 Linux/Clang 流程，因为 MSVC 没有 TSan。

参数语义：

- `-Config`/`--config`：`Debug`、`Release` 或 `Both`；`Both` 生成两个独立目录。
- `-Arch`/`--arch`：`x86` 或 `x64`；POSIX 的 x86 通过 `-m32` 实现。
- `-Sanitizer`/`--sanitizer`：`none`、`asan`、`ubsan`、`tsan`，测试脚本额外接受 `all`。
- `-Jobs`/`--jobs`：并行编译任务数；零或省略表示处理器数量。
- `-Filter`/`--filter`：传给 CTest `-R` 的正则，只影响测试选择。
- `-Clean`/`--clean`：只删除所选构建目录，可安全重复执行。
- `-Shared`/`--shared`：选择共享构建（`CRPC_BUILD_SHARED=ON`），产物目录带 `-shared`
  后缀；测试脚本在该模式下额外直接运行 `crpc_test_shared_link` 链接门禁。
- `-SourceDir`/`--source` 与 `-BuildRoot`/`--build-root`：覆盖源码根与产物根。

每个脚本写出带时间戳的 UTF-8 日志，并在出现失败汇总时以非零退出。PowerShell 使用 `[System.IO.File]::AppendAllText` 写 UTF-8，而不是 `Tee-Object`，后者在 PowerShell 5.1 上写 UTF-16。

## 6. 构建布局与产物 <a id="s6"></a>

默认产物根是 `<source>/.opencode/tmp/out`。构建目录是 `build/<arch>-<config>[-<sanitizer>]`，例如 `.opencode/tmp/out/build/x64-Debug-asan`。日志位于 `.opencode/tmp/out/logs/build-<arch>-<config>-<sanitizer>-<timestamp>.log` 与 `test-...log`。测试期间的 minidump 写入 `.opencode/tmp/out/dumps`。

库文件是 Windows 的 `crpc.lib`/`crpc.dll` 或 Linux 的 `libcrpc.a`/`libcrpc.so`，位于所选构建目录。测试可执行文件为 `crpc_test_<area>`，由 `tests/CMakeLists.txt` 按源文件逐个注册。所有消费者可执行文件（测试以及启用后的基准）通过 `RUNTIME_OUTPUT_DIRECTORY` 输出到构建根目录，与库同目录；不存在复制步骤，因此只重建库的增量构建也总是让可执行文件加载新链接出的 `crpc.dll`。依赖构建位于 `<build>/_deps`。当前没有 `install()` 规则，因此 `cmake --install` 尚不是受支持的部署路径。

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

`.github/workflows/ci.yml` 已提交，每个 job 都是「检出 + 按文档化参数调用 `scripts/build.*` / `scripts/test.*`」——脚本之外没有复制任何构建逻辑，这正是脚本作为唯一事实来源的前提（需求 25）。

| Job | 矩阵 |
| --- | --- |
| `linux` | gcc x64 与 clang x64，各 Debug 与 Release，静态与共享 |
| `linux-sanitizers` | `test.sh --sanitizer all`（asan + ubsan + tsan） |
| `linux/gcc/cxx20` | `test.sh --cxx-standard 20`，使仅 C++20 的代码被编译、其用例真正运行 |
| `windows` | MSVC x64 与 x86，各 Debug 与 Release |
| `windows/shared` | MSVC x64 共享构建加导出闸门 |
| `windows/examples` | 以 `CRPC_BUILD_TESTS=OFF` 构建并运行 `examples/` |
| `windows/bench-compile` | 编译全部基准，不运行 |
| `linux/bench-compile` | POSIX 编译器同上 |

两个平台都跑两种优化级，因为只在 Release 出现的缺陷已经发生过两次；Linux 同时跑两个编译器，因为 clang 的 `-Werror` 诊断并不是 MSVC `/W4` 的超集：有 5 处只在 clang 报的警告（未使用的常量、未使用的内部声明）藏在 Windows 上完全干净的代码里。

尚未进入 workflow、仍需本地执行的部分：MSVC x86、fuzz smoke、黄金字节与基准报告。本地等价命令是 `.\scripts\test.ps1 -Config Both -Arch x64 -Sanitizer none`、`.\scripts\build.ps1 -Config Debug -Arch x86` 与 `./scripts/test.sh --config Debug --arch x64 --sanitizer all --compiler clang`。

“零警告”声明要求构建日志中没有 `warning:` 行；即使编译器没有报错，脚本也会把警告视为失败。请重新运行脚本确认当前代码树；套件数量会随子系统增加而变化，因此这里不写死数字。

## 10. 故障排查 <a id="s10"></a>

| 症状 | 原因 / 处理 |
|---|---|
| `CRPC_RTTI_DIR does not look like an RTTI checkout` | 指向仓库根，而不是 `include/` |
| `CRPC_COROCC_DIR does not look like a corocc checkout` | 指向包含 `include/corocc/coro.h` 的根 |
| `examples` 未构建 | 示例现已可用：设置 `CRPC_BUILD_EXAMPLES=ON`；每个 `quickstart*.cpp` 生成 `crpc_example_<文件名>` 并注册进 CTest |
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
