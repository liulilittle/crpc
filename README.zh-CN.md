# CRPC

[English](README.md) | **简体中文**

一个用现代 C++17 构建大型服务器系统的框架：远程调用与服务器推送共用同一条连接、
同一套帧格式、同一个协程模型。

> 状态：**v2.0.0**。核心原语（wire、帧、编解码、传输契约）与 call/server 门面均已就位。
> 文档索引与当前实现状态见 [docs/README.zh-CN.md](docs/README.zh-CN.md)。

## 文档索引

| 文档 | 中文 | English |
|---|---|---|
| 设计规格 | [docs/superpowers/specs/2026-10-19-crpc-design.zh-CN.md](docs/superpowers/specs/2026-10-19-crpc-design.zh-CN.md) | [English](docs/superpowers/specs/2026-10-19-crpc-design.md) |
| ABI 策略 | [docs/abi.zh-CN.md](docs/abi.zh-CN.md) | [docs/abi.md](docs/abi.md) |
| 架构概览 | [docs/architecture.zh-CN.md](docs/architecture.zh-CN.md) | [Architecture overview](docs/architecture.md) |
| 线上协议 | [docs/protocol.zh-CN.md](docs/protocol.zh-CN.md) | [Wire protocol](docs/protocol.md) |
| 构建与测试 | [docs/building.zh-CN.md](docs/building.zh-CN.md) | [Build and test](docs/building.md) |
| 工程原则 | [docs/principles.zh-CN.md](docs/principles.zh-CN.md) | [Engineering principles](docs/principles.md) |
| 性能报告 | [docs/performance.zh-CN.md](docs/performance.zh-CN.md) | [Performance report](docs/performance.md) |
| 迁移指南 | [docs/migration.zh-CN.md](docs/migration.zh-CN.md) | [Migration guide](docs/migration.md) |
| API 参考 | [docs/api/README.zh-CN.md](docs/api/README.zh-CN.md) | [API reference](docs/api/README.md) |
| 教程 | [docs/tutorial.zh-CN.md](docs/tutorial.zh-CN.md) | [Tutorial](docs/tutorial.md) |
| 示例 | [examples/README.zh-CN.md](examples/README.zh-CN.md) | [Examples](examples/README.md) |
| 发布说明 | [docs/CHANGELOG.zh-CN.md](docs/CHANGELOG.zh-CN.md) | [Release notes](docs/CHANGELOG.md) |
| 分布式测试与基准 | [docs/distributed-testing.zh-CN.md](docs/distributed-testing.zh-CN.md) | [Distributed testing and benchmarks](docs/distributed-testing.md) |
| Redis 客户端 | [docs/redis-client.zh-CN.md](docs/redis-client.zh-CN.md) | [Redis client](docs/redis-client.md) |

## 这是什么

CRPC 把通常属于两个产品的两种能力放在同一个框架里：

1. **调用** —— 请求/响应式 RPC。在协程内部，一次调用写起来和读起来都像本地函数调用；
   它挂起协程而不是阻塞线程。当调用方希望先发起多个调用再统一等待时，可以使用异步句柄形式。
2. **推送** —— 服务器到客户端的消息与流，支持订阅、有序、背压和可选的投递保证。

两者共用同一条连接、同一套多路复用二进制帧格式和同一个分片/协程运行时，
因此一个服务可以在同一条连接上既应答 RPC，又向同一个客户端推送事件，而不需要第二套传输。

在这套传输之上，库还提供了生产服务需要、但在每个项目里往往被重复实现且实现得不好的部分：

- 反射驱动的服务 API（无 IDL、无代码生成步骤）；
- HTTP/1.1、HTTP/2 与 HTTPS 前门，与 RPC 共用同一批 handler；
- 用 attribute 声明的 MVC/RESTful 路由；
- RTTI 的全部序列化格式，外加一个紧凑的二进制编解码器；
- 服务发现、负载均衡、重试、熔断、限流、舱壁隔离、健康检查与优雅降级；
- 分布式链路追踪、AOP 切面、结构化高性能日志；
- 文件上传/下载、流式响应与压缩。

## 设计约束

以下不是愿景，而是由测试套件与构建强制执行的规则。

- **绝不死锁、绝不崩溃、绝不泄露。** 每条加锁路径都有文档化的锁序；每一个外部输入的字节
  都被校验；每一次获取都恰好对应一次释放。ASan、LSan、UBSan、TSan 是合并门禁。
- **显式优于隐式。** 字节序、整数宽度、对齐、所有权与生命周期都写进类型并在编译期检查。
- **线上格式与平台无关。** 多字节字段在任何宿主上都按小端编码与解码，且显式进行。
  不同平台、不同 CPU 上的两个节点必须能够互相调用。
- **未知即拒绝（fail-closed）。** 未知帧类型、版本或能力位一律拒绝，不做猜测。
- **因果优先，而非墙钟。** 先后顺序由消息收发对定义；任何跨节点判定都不比较时间戳。

## 环境要求

- CMake 3.16 或更高
- C++17 编译器：MSVC 2022、GCC 9+ 或 Clang 10+
- 依赖（只读消费其公开头文件）：
  - [RTTI](https://github.com/liulilittle/RTTI) —— 反射与序列化
  - [corocc](https://github.com/liulilittle/corocc) —— 有栈协程

## 构建

```powershell
# Windows
$env:CRPC_RTTI_DIR   = 'F:\RTTI'
$env:CRPC_COROCC_DIR = 'F:\corocc'
.\scripts\build.ps1 -Config Debug -Arch x64
.\scripts\test.ps1  -Config Debug -Arch x64
```

```sh
# Linux / macOS
export CRPC_RTTI_DIR=/path/to/RTTI
export CRPC_COROCC_DIR=/path/to/corocc
./scripts/build.sh --config Debug --arch x64
./scripts/test.sh  --config Debug --arch x64
```

两个脚本与 CI 使用同一份实现；幂等、输出 UTF-8 日志、任何失败都以非零退出码结束。
警告视为错误。

## 许可证

MIT。见 [LICENSE](LICENSE)。
