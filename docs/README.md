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
| 3 | [Design](design.md) |
| 4 | [Wire protocol](protocol.md) |
| 5 | [Build and test](building.md) |
| 6 | [Engineering principles](principles.md) |
| 7 | [Performance report](performance.md) |
| 8 | [Migration guide](migration.md) |
| 9 | [API reference](api/README.md) |
| 10 | [Tutorial](tutorial.md) |
| 11 | [Examples](../examples/README.md) |
| 12 | [Release notes](CHANGELOG.md) |
| 13 | [Distributed testing and benchmarks](distributed-testing.md) |
| 14 | [Redis client](redis-client.md) |
| 15 | [Database client](db-client.md) |
| 16 | [Memcached client](memcached-client.md) |
| 17 | [Proxy headers](proxy-headers.md) |
| 18 | [Oracle TTC evidence](oracle-ttc-evidence.md) |
| 19 | [TDS TLS evidence](tds-tls-evidence.md) |

## Reading order

- **Using the library**: 10 (tutorial) → 9 (API reference) → 2 (architecture) → 3
  (design) → 4 (protocol). Add 14 (Redis client) when the program talks to Redis.
- **Measuring and reproducing**: 7 (performance) → 13 (distributed testing and
  benchmarks).
- **Contributing**: 6 (principles) → 2 (architecture) → 3 (design) → 5 (build and
  test).
- **Integrating at the binary level**: 1 (ABI policy).

## How the two core documents divide the work

**Architecture** describes what exists: the module map, the contracts between
layers, and the authoritative per-module status matrix. **Design** records the
decisions behind that structure -- why the layering is what it is, which
alternatives were rejected, and the invariants each choice protects. Neither
restates the other; where they touch, architecture is the record of the present
and design is the record of the reasoning.

## Status legend

The status matrix in document 2 (architecture) is the single authoritative
per-module status record.

| Mark | Meaning |
|---|---|
| implemented | Code exists, tests pass, documented. |
| partial | Code exists and is tested, but a documented part is missing. |
| specified | Documented behaviour without a code path. |
