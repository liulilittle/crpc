# CRPC documentation

[简体中文](README.zh-CN.md) | **English**

This is the entry point for the English documentation. Every document also
exists in Chinese as a separate file, linked from the top of each page; a page
never mixes the two languages.

## Index

| # | Document |
|---|---|
| 1 | [ABI policy](abi.md) |
| 2 | [Architecture](architecture.md) |
| 3 | [Wire protocol](protocol.md) |
| 4 | [Build and test](building.md) |
| 5 | [Engineering principles](principles.md) |
| 6 | [Performance report](performance.md) |
| 7 | [Migration guide](migration.md) |
| 8 | [API reference](api/README.md) |
| 9 | [Tutorial](tutorial.md) |
| 10 | [Examples](../examples/README.md) |
| 11 | [Release notes](CHANGELOG.md) |
| 12 | [Distributed testing and benchmarks](distributed-testing.md) |
| 13 | [Redis client](redis-client.md) |
| 14 | [Database client](db-client.md) |
| 15 | [Memcached client](memcached-client.md) |
| 16 | [Proxy headers](proxy-headers.md) |
| 17 | [Oracle TTC evidence](oracle-ttc-evidence.md) |
| 18 | [TDS TLS evidence](tds-tls-evidence.md) |

## Reading order

- **Using the library**: 9 (tutorial) → 8 (API reference) → 2 (architecture) → 3
  (protocol). Add 13 (Redis client) when the program talks to Redis.
- **Measuring and reproducing**: 6 (performance) → 12 (distributed testing and
  benchmarks).
- **Contributing**: 5 (principles) → 2 (architecture) → 4 (build and test).
- **Integrating at the binary level**: 1 (ABI policy).

## Status legend

The status matrix in document 2 (architecture) is the single authoritative
per-module status record.

| Mark | Meaning |
|---|---|
| implemented | Code exists, tests pass, documented. |
| partial | Code exists and is tested, but a documented part is missing. |
| specified | Documented behaviour without a code path. |
