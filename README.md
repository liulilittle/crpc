# CRPC

**English** | [简体中文](README.zh-CN.md)

A modern C++17 framework for building large server systems: remote calls and
server push over one connection, one frame format and one coroutine model.

> Status: **v2.0.0.** The core primitives (wire, frame, codec, transport
> contract) and the call/server facade are in place. See
> [docs/README.md](docs/README.md) for the document index and the current
> implementation status.

## Documentation index

| Document | English | 中文 |
|---|---|---|
| Design specification | [docs/superpowers/specs/2026-10-19-crpc-design.md](docs/superpowers/specs/2026-10-19-crpc-design.md) | [中文](docs/superpowers/specs/2026-10-19-crpc-design.zh-CN.md) |
| ABI policy | [docs/abi.md](docs/abi.md) | [docs/abi.zh-CN.md](docs/abi.zh-CN.md) |
| Architecture overview | [docs/architecture.md](docs/architecture.md) | [架构概览](docs/architecture.zh-CN.md) |
| Wire protocol | [docs/protocol.md](docs/protocol.md) | [线上协议](docs/protocol.zh-CN.md) |
| Build and test | [docs/building.md](docs/building.md) | [构建与测试](docs/building.zh-CN.md) |
| Engineering principles | [docs/principles.md](docs/principles.md) | [工程原则](docs/principles.zh-CN.md) |
| Performance report | [docs/performance.md](docs/performance.md) | [性能报告](docs/performance.zh-CN.md) |
| Migration guide | [docs/migration.md](docs/migration.md) | [迁移指南](docs/migration.zh-CN.md) |
| API reference | [docs/api/README.md](docs/api/README.md) | [API 参考](docs/api/README.zh-CN.md) |
| Tutorial | [docs/tutorial.md](docs/tutorial.md) | [教程](docs/tutorial.zh-CN.md) |
| Examples | [examples/README.md](examples/README.md) | [示例](examples/README.zh-CN.md) |
| Release notes | [docs/CHANGELOG.md](docs/CHANGELOG.md) | [发布说明](docs/CHANGELOG.zh-CN.md) |

## What this is

CRPC carries two capabilities that are usually separate products:

1. **Calls** — request/response RPC. Inside a coroutine a call looks and reads
   like a local function call; it suspends the coroutine instead of blocking the
   thread. An asynchronous handle form is available when the caller wants to
   start several calls before waiting.
2. **Push** — server-to-client messages and streams, with subscriptions,
   ordering, backpressure and optional delivery guarantees.

Both ride on the same connection, the same multiplexed binary frame format and
the same shard/coroutine runtime, so a service can answer an RPC and push an
event to the same client without a second transport.

On top of that transport the library provides the parts a production service
needs and that are usually re-implemented badly in every project:

- a reflection-driven service API (no IDL, no code-generation step),
- an HTTP/1.1, HTTP/2 and HTTPS front door sharing the same handlers,
- MVC/RESTful routing declared with attributes,
- all RTTI serialization formats plus a compact binary codec,
- service discovery, load balancing, retries, circuit breaking, rate limiting,
  bulkheads, health checks and graceful degradation,
- distributed tracing, AOP hooks, structured high-performance logging,
- file upload/download, streaming responses and compression.

## Design constraints

These are not aspirations; they are enforced by the test suite and the build.

- **No deadlock, no crash, no leak.** Every lock path has a documented order;
  every externally supplied byte is validated; every acquire has exactly one
  release. ASan, LSan, UBSan and TSan are merge gates.
- **Explicit over implicit.** Byte order, integer width, alignment, ownership
  and lifetimes are stated in the type and checked at compile time.
- **Platform-independent wire format.** Multi-byte fields are little-endian on
  the wire on every host, encoded and decoded explicitly. Two nodes on different
  platforms and different CPUs must interoperate.
- **Fail closed.** An unknown frame type, version or capability is rejected, not
  guessed at.
- **Causality, not wall clocks.** Ordering is defined by message send/receive
  pairs. No cross-node decision is made by comparing timestamps.

## Requirements

- CMake 3.16 or newer
- A C++17 compiler: MSVC 2022, GCC 9+, or Clang 10+
- Dependencies (consumed read-only through their public headers):
  - [RTTI](https://github.com/liulilittle/RTTI) — reflection and serialization
  - [corocc](https://github.com/liulilittle/corocc) — stackful coroutines

## Build

```powershell
# Windows
.\scripts\build.ps1 -Config Debug -Arch x64
.\scripts\test.ps1  -Config Debug -Arch x64
```

```sh
# Linux / macOS
./scripts/build.sh --config Debug --arch x64
./scripts/test.sh  --config Debug --arch x64
```

Both scripts are the same ones CI uses; they are idempotent, write UTF-8 logs,
and exit non-zero on any failure. Warnings are errors.

Point the build at the dependency checkouts with the environment variables the
CMake configure step reads:

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

## License

MIT. See [LICENSE](LICENSE).
