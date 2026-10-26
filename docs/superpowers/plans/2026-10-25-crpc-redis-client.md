# CRPC 自研 Redis 客户端库 总体规划

日期：2026-10-25
状态：待执行
工作树：`F:\crpc\.worktrees\redis`（分支 `feature/redis-client`）

## 目标

完整实现自研 Redis 客户端库，**所有接口与 CRPC 结合，底层全部异步通信**：

- 基于 CRPC 的 `io::source`/`io::sink` 异步传输，非裸 socket。
- 基于 corocc 栈协程的异步 API。
- RESP2/RESP3 协议。
- 主从/热备自动切换（Sentinel）。
- Redis Cluster 支持。
- 全数据类型封装（string/list/hash/set/zset/stream/bitmap/hyperloglog/geo）。
- RTTI 绑定（序列化/反序列化）。
- 事务/流水线/Pub-Sub/Lua。

## 架构

```
include/crpc/redis/
  resp.hpp          // RESP2/RESP3 协议
  connection.hpp    // 异步连接（基于 CRPC io）
  client.hpp        // 异步 Redis 客户端
  types.hpp         // 数据类型封装
  rtti.hpp          // RTTI 绑定
  sentinel.hpp      // 主从/热备自动切换
  cluster.hpp       // Redis Cluster
src/redis/
  resp.cpp
  connection.cpp
  client.cpp
  types.cpp
  sentinel.cpp
  cluster.cpp
tests/test_redis_*.cpp
```

## 工作流（按依赖顺序）

1. **RESP 协议**（无依赖）
2. **异步连接管理**（依赖 CRPC io）
3. **数据类型封装**（依赖 RESP）
4. **RTTI 绑定**（依赖数据类型 + RTTI）
5. **Sentinel 主从/热备自动切换**（依赖连接管理）
6. **Cluster**（依赖连接管理 + Sentinel）
7. **事务/流水线/Pub-Sub/Lua**（依赖客户端）
8. **测试与验证**

## 约束

- C++17 为主；零警告（`/W4 /WX`）。
- **头文件最小化**：能放 `.cpp` 的实现必须放 `.cpp`。
- 底层异步：使用 CRPC `io::source`/`io::sink` + corocc 栈协程。
- 所有接口与 CRPC 结合（复用 `rpc::call_context`、`runtime`、`io` 抽象）。
- 工作目录 `F:\crpc\.worktrees\redis`；构建 `scripts/build.ps1`。

## 并行派发

- 子代理 A：RESP 协议。
- 子代理 B：异步连接管理。
- 子代理 C：数据类型封装（待 A 完成后）。
- 子代理 D：RTTI 绑定（待 C 完成后）。
- 子代理 E：Sentinel（待 B 完成后）。
- 子代理 F：Cluster（待 B/E 完成后）。
