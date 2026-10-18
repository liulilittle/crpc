# CRPC 文档

**English** | [简体中文](README.zh-CN.md)

这里是 CRPC 全部文档的入口。每份文档都有中英两个版本，成对文件在各自顶部互相链接。

## 索引

| # | 文档 | English |
|---|---|---|
| 1 | [设计规格](superpowers/specs/2026-10-19-crpc-design.zh-CN.md) | [Design specification](superpowers/specs/2026-10-19-crpc-design.md) |
| 2 | [ABI 策略](abi.zh-CN.md) | [ABI policy](abi.md) |
| 3 | [架构概览](architecture.zh-CN.md) | [Architecture overview](architecture.md) |
| 4 | [线上协议](protocol.zh-CN.md) | [Wire protocol](protocol.md) |
| 5 | [构建与测试](building.zh-CN.md) | [Build and test](building.md) |
| 6 | [工程原则](principles.zh-CN.md) | [Engineering principles](principles.md) |
| 7 | [性能报告](performance.zh-CN.md) | [Performance report](performance.md) |

索引中的文档均已存在。各模块状态见第 3 份文档的状态矩阵与设计规格第 21 章；
索引缺项或链接失效视为文档缺陷，不是疏漏。

## 阅读顺序

- **使用这个库**：3（架构）→ 4（协议）→ 设计规格中的服务 API 章节。
- **参与开发**：6（工程原则）→ 1（设计规格）→ 5（构建与测试）。
- **二进制层集成**：2（ABI 策略）。

## 状态标记

| 标记 | 含义 |
|---|---|
| implemented | 代码存在、测试通过、有文档。 |
| in progress | 代码存在，但契约尚未冻结。 |
| specified | 设计已冻结，尚无代码。 |

各模块的权威状态见设计规格第 21 章。
