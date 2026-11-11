# 工作产物位置（硬性约束）

本仓库的一切**工作产物**必须留在本仓库目录内，**禁止写到 C 盘**（含
`C:\Users\<user>\AppData\Local\Temp\opencode`、`%TEMP%`、`%APPDATA%`，以及任何
`C:\` 下的路径）。主代理与所有子代理（`task` / `manage_subagent` 派生的会话）都受此约束。

## 唯一允许的位置

| 用途 | 路径 |
|---|---|
| 临时脚本、探针、变异脚本、中间输出 | `F:\crpc\.opencode\tmp\` |
| 构建目录（含 ODBC / x86 / bench 变体） | `F:\crpc\.build-*\` |
| 审计/评审报告、设计稿、执行报告 | `F:\crpc\.opencode\`（沿用既有 `<role>-<topic>.md` 命名） |
| 备份（改码前） | `F:\crpc\.opencode\backups\<file>.<timestamp>` |
| 记忆四文件 | `F:\crpc\.opencode\{memory,plan,process,discover}.md` |
| 仓库外副本（做变异/审计时需要隔离） | `F:\crpc\.opencode\sandbox\<topic>\` |

## 具体规则

1. **临时文件**一律写在 `F:\crpc\.opencode\tmp\`，用完即删（`Remove-Item`）。
2. **子代理**在自己的任务提示里必须收到这条约束；派发时写明
   "所有产物写到 `F:\crpc\.opencode\tmp\<your-topic>\`，禁止使用 C 盘临时目录"。
3. **隔离副本**（审计/变异要用干净树）放在 `F:\crpc\.opencode\sandbox\`，
   不要复制到 `%TEMP%`。用完删除，避免仓库膨胀。
4. **构建目录**保持在 `F:\crpc\.build-*`（已是现状）。
5. 唯一例外：工具自身强制的路径（如 ODBC 驱动要求 `.mdb` 必须在可写目录——
   `F:\crpc\.opencode\tmp\` 同样可写，仍不需要 C 盘）。

## 为什么

C 盘临时目录在上一轮积累到 **5.6 GB / 700+ 文件**，且与本仓库的清理、审计记录完全脱节：
真实产物散落在两处，事后无法判断哪份是证据、哪份是垃圾。把产物收进 `.opencode`
既便于随仓库一起清理，也让"这次任务的证据在哪"只有一个答案。

---

# 异常捕获（硬性约束）

## 规则

**任何可执行体（探针、变异程序、单元测试、example）在自动化流程中运行时，必须用 SEH
捕获异常并禁止弹出任何对话框。禁止直接运行裸 exe。**

用户已明确要求：所有异常测试必须用 SEH 捕获。直接 `Start-Process <exe>` /
`& <exe>` 运行未包装的程序是违规操作。

## 为什么必须这样做

Debug CRT 的断言（如 `_CrtIsValidHeapPointer` 失败）会调用 `_CrtDbgReport`，在未配置
报告模式时**弹出模态对话框并阻塞进程**。表现：

- 进程不退出、CPU 占用 0，`Start-Process -Wait` 或 `WaitForExit` 永久挂起；
- 工具调用超时，整个自动化流程卡死；
- 同样的缺陷在 Release 下 `abort()`，行为完全不同，掩盖真实问题。

对话框是 GUI 状态，无法被脚本关闭，只能人工点「中止」。这使自动化失效。

## 必须实现的最小防御（缺一不可）

1. **重定向 CRT 报告到 stderr，禁止弹框**
   ```cpp
   _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
   _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
   _CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
   _CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);
   _CrtSetReportMode(_CRT_WARN,   _CRTDBG_MODE_FILE);
   _CrtSetReportFile(_CRT_WARN,   _CRTDBG_FILE_STDERR);
   _set_error_mode(_OUT_TO_STDERR);
   ```
2. **禁止系统错误对话框**
   ```cpp
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                SEM_NOOPENFILEERRORBOX);
   ```
3. **捕获未处理异常与 SEH**
   ```cpp
   SetUnhandledExceptionFilter(&on_unhandled);
   __try { body(); }
   __except (EXCEPTION_EXECUTE_HANDLER) { /* 记录 code，返回非零 */ }
   ```
4. **给进程设置硬超时**：调用方一律 `Start-Process` + `WaitForExit(ms)`，
   超时立刻 `Stop-Process -Force`，并判为失败。**禁止无上限等待。**

## 唯一允许的执行方式

```powershell
$p = Start-Process -FilePath <exe> -RedirectStandardOutput <out> `
     -RedirectStandardError <err> -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit(180000)) { Stop-Process -Id $p.Id -Force; <判失败> }
```

`-WindowStyle Hidden` 只隐藏窗口，**不阻止模态对话框阻塞**，因此不能替代 SEH 包装。

## 适用范围

主代理与所有子代理（`task` / `manage_subagent` 派生的会话）均受此约束。派发任务时
必须写明："探针/测试必须用 SEH 包装 + `SetErrorMode` + `_CrtSetReportMode`，
并且调用方必须用 `WaitForExit` 超时兜底；禁止直接运行裸 exe。"

## 参考实现

可复用包装见 `tests/support/seh_guard.hpp`（提供 `crpc::test::run_guarded(fn)`）。
