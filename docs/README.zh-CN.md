# CRPC 文档

[English](README.md) | **简体中文**

这里是 CRPC 中文文档的入口。每份文档都有中英两个独立文件，各自顶部互相链接；
单个文件只用一种语言。

## 索引

| # | 文档 |
|---|---|
| 1 | [ABI 策略](abi.zh-CN.md) |
| 2 | [架构](architecture.zh-CN.md) |
| 3 | [线上协议](protocol.zh-CN.md) |
| 4 | [构建与测试](building.zh-CN.md) |
| 5 | [工程原则](principles.zh-CN.md) |
| 6 | [性能报告](performance.zh-CN.md) |
| 7 | [迁移指南](migration.zh-CN.md) |
| 8 | [API 参考](api/README.zh-CN.md) |
| 9 | [教程](tutorial.zh-CN.md) |
| 10 | [示例](../examples/README.zh-CN.md) |
| 11 | [发布说明](CHANGELOG.zh-CN.md) |
| 12 | [分布式测试与基准](distributed-testing.zh-CN.md) |
| 13 | [Redis 客户端](redis-client.zh-CN.md) |
| 14 | [数据库客户端](db-client.zh-CN.md) |
| 15 | [Memcached 客户端](memcached-client.zh-CN.md) |
| 16 | [代理头](proxy-headers.zh-CN.md) |
| 17 | [Oracle TTC 证据](oracle-ttc-evidence.zh-CN.md) |
| 18 | [TDS TLS 证据](tds-tls-evidence.zh-CN.md) |

## 阅读顺序

- **使用这个库**：9（教程）→ 8（API 参考）→ 2（架构）→ 3（协议）；
  程序要访问 Redis 时加读 13（Redis 客户端）。
- **测量与复现**：6（性能报告）→ 12（分布式测试与基准）。
- **参与开发**：5（工程原则）→ 2（架构）→ 4（构建与测试）。
- **二进制层集成**：1（ABI 策略）。

## 状态标记

各模块的权威状态见第 2 份文档（架构）中的状态矩阵。

| 标记 | 含义 |
|---|---|
| implemented | 代码存在、测试通过、有文档。 |
| partial | 代码存在且有测试，但契约中有明确缺失的部分。 |
| specified | 有行为描述，但没有代码路径。 |
