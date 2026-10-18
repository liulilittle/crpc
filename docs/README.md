# CRPC documentation

[简体中文](README.zh-CN.md) | **English**

This is the entry point for all CRPC documentation. Every document exists in two
languages; the pairs are linked at the top of each file.

## Index

| # | Document | 中文 |
|---|---|---|
| 1 | [Design specification](superpowers/specs/2026-10-19-crpc-design.md) | [设计规格](superpowers/specs/2026-10-19-crpc-design.zh-CN.md) |
| 2 | [ABI policy](abi.md) | [ABI 策略](abi.zh-CN.md) |
| 3 | [Architecture overview](architecture.md) | [架构概览](architecture.zh-CN.md) |
| 4 | [Wire protocol](protocol.md) | [线上协议](protocol.zh-CN.md) |
| 5 | [Build and test](building.md) | [构建与测试](building.zh-CN.md) |
| 6 | [Engineering principles](principles.md) | [工程原则](principles.zh-CN.md) |
| 7 | [Performance report](performance.md) | [性能报告](performance.zh-CN.md) |

Documents 3, 4, 7 are produced as the corresponding modules land; the index is
updated in the same commit that adds them. A missing entry is a documentation
defect, not an oversight.

## Reading order

- **Using the library**: 3 (architecture) → 4 (protocol) → the design
  specification's service API chapter.
- **Contributing**: 6 (principles) → 1 (design specification) → 5 (build and
  test).
- **Integrating at the binary level**: 2 (ABI policy).

## Status legend

| Mark | Meaning |
|---|---|
| implemented | Code exists, tests pass, documented. |
| in progress | Code exists but the contract is not frozen. |
| specified | Design frozen, no code yet. |

The design specification's chapter 21 carries the authoritative per-module
status.
