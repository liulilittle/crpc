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
