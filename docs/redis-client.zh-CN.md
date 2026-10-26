# CRPC Redis 客户端

[English](redis-client.md) | **简体中文**

版本 1.0 / 2026-10-26

本文说明 [`include/crpc/redis`](../include/crpc/redis) 下的异步 Redis 客户端。它是
API 地图与可运行配方，不替代头文件：下文每个公开名字都在对应头文件中声明，头文件是
权威来源。

客户端与传输无关。它通过与其他 CRPC 模块相同的 `crpc::io::source` /
`crpc::io::sink` 契约与 Redis 通信，因此同一份代码可以跑在测试用的进程内后端、
Windows 的 IOCP 和 Linux 的 epoll 上。

## 目录

| # | 章节 |
|---|---|
| [1](#s1) | 快速开始 |
| [2](#s2) | API 地图 |
| [3](#s3) | 执行与异步模型 |
| [4](#s4) | 错误处理 |
| [5](#s5) | 配方 |
| [6](#s6) | 构建与测试 |
| [7](#s7) | 已知限制 |
| [8](#s8) | 另见 |

## 1. 快速开始 <a id="s1"></a>

`crpc::redis::client` 组合连接池与类型化命令门面，是最短的可用路径。

```cpp
#include <chrono>
#include <iostream>
#include <string>

#include <crpc/io/iocp_backend.hpp> // Windows；Linux 用 epoll_backend.hpp
#include <crpc/redis/client.hpp>

int main() {
    crpc::io::iocp_backend backend;

    crpc::redis::client::options options;
    options.pool.connection.ep = crpc::endpoint::tcp("127.0.0.1", 6379);
    options.pool.min_size = 1;
    options.pool.max_size = 8;
    options.pool.acquire_timeout = std::chrono::seconds(2);
    options.pool.connection.operation_timeout = std::chrono::seconds(2);
    options.protocol = crpc::redis::resp_protocol::resp3;

    crpc::redis::client redis(options, backend);
    if (const crpc::status connected = redis.connect(); !connected.ok()) {
        std::cerr << "connect failed: " << connected.message() << "\n";
        return 1;
    }

    const crpc::result<crpc::redis::resp_value> set =
        redis.set("greeting", "hello");
    if (!set.ok()) {
        std::cerr << "SET failed: " << set.error().message << "\n";
        return 1;
    }

    const crpc::result<crpc::redis::resp_value> get = redis.get("greeting");
    if (get.ok() && get.value().is_bulk_string()) {
        std::cout << get.value().as_bulk_string() << "\n"; // hello
    }

    redis.close();
    return 0;
}
```

Linux 上用 `crpc::io::epoll_backend::create(crpc::io::epoll_options{})` 创建并
move 进 `std::unique_ptr`；其余代码不变。后端只是 `crpc::backend_factory&`，Redis
代码不依赖具体后端。

`connect()` 建立一条池化连接并用 `PING` 验证；`close()` 关闭空闲连接，并让后续
acquire 以 `rpc_code::shutting_down` 失败。

测试与同进程示例可以直接传 `crpc::inproc_backend` 与
`endpoint::inproc(name)`；Redis 层不关心字节流来自哪个后端。

## 2. API 地图 <a id="s2"></a>

| 头文件 | 关键类型 | 职责 |
|---|---|---|
| [`resp.hpp`](../include/crpc/redis/resp.hpp) | `resp_value`、`resp_parser`、`resp_encoder`、`resp_protocol` | RESP2/RESP3 值模型、增量解析器与编码器；不做 I/O。 |
| [`connection.hpp`](../include/crpc/redis/connection.hpp) | `connection`、`connection_pool`、`connection_config`、`pool_config` | 基于 `io::source`/`io::sink` 的字节传输：异步读写、有界缓冲、重连/退避与连接池。 |
| [`pipeline.hpp`](../include/crpc/redis/pipeline.hpp) | `command`、`command_executor`、`message_channel`、`command_channel`、`pipeline` | 单条命令流：单请求/响应、批量，以及持有 RESP 编解码器的传输通道。 |
| [`pubsub.hpp`](../include/crpc/redis/pubsub.hpp) | `pubsub`、`pubsub_message`、`pubsub_kind` | Pub/Sub 订阅/退订、消息解码、`PUBLISH` 与 `PUBSUB` 自省。 |
| [`sentinel.hpp`](../include/crpc/redis/sentinel.hpp) | `sentinel`、`sentinel_options`、`sentinel_status`、`sentinel_connection_executor` | 主节点发现、主观/客观下线、领导者选举与自动故障转移。 |
| [`stream.hpp`](../include/crpc/redis/stream.hpp) | `stream`、`stream_id`、`stream_entry`、`stream_read` | Redis Streams：`XADD`/`XREAD`/`XRANGE`、消费者组、pending/claim。 |
| [`transaction.hpp`](../include/crpc/redis/transaction.hpp) | `transaction`、`transaction_exec_result` | `WATCH`/`MULTI`/`EXEC`/`DISCARD` 与队列错误、watch 冲突语义。 |
| [`types.hpp`](../include/crpc/redis/types.hpp) | `value`、`string_type`、`list_type`、`hash_type`、`set_type`、`zset_type`、`stream_type`、`redis_stream_entry`、`bitmap_type`、`hyperloglog_type`、`geo_type`、`decode_*`、迭代器 | 类型化值模型、命令构造器与回复投影。 |
| [`client.hpp`](../include/crpc/redis/client.hpp) | `client`、`client::options` | 连接池 + 类型化门面：`execute`、`execute_batch`、`ping`/`get`/`set` 与 hash/list/set/zset 便捷方法。 |
| [`cluster.hpp`](../include/crpc/redis/cluster.hpp) | `cluster`、`cluster_topology`、`cluster_options`、`cluster_slot_executor`、`cluster_pool_provider` | 16384 槽路由、`CLUSTER SHARDS`/`SLOTS` 发现、`MOVED`/`ASK`、CROSSSLOT 拒绝与集群安全 pin。 |
| [`rtti.hpp`](../include/crpc/redis/rtti.hpp) | `encode_to_value`、`decode_from_value`、`hash_set_command`、`hash_mget_command`、`decode_hash_reply`、`value_from_resp` | 反射驱动的 DTO <-> `redis::value` 与 DTO <-> Redis hash 绑定；需要 `CRPC_HAVE_RTTI`。 |

`types.hpp` 与 `pipeline.hpp` 都把命令类型命名为 `command`；它是同一个别名
（`resp_command`），因此两个头文件可以同时使用。

## 3. 执行与异步模型 <a id="s3"></a>

- **没有隐藏线程。** 协程运行在 CRPC 分片上时，`connection::write`、`read_exact`
  与 `read_until` 通过 `crpc::runtime::park()` 挂起；普通线程上（或分片线程上的
  非协程任务）回退为受 `connection_config::operation_timeout` 限制的条件变量等待。
- **每条命令流一个所有者。** `command_channel`、`pipeline`、`transaction`、
  `pubsub` 与 `cluster` 都是单所有者；不要从多个协程并发调用。每条池化连接配一个
  通道；`connection_pool` 本身才是共享、线程安全的组件。
- **池化。** `client::execute` 取一条池化连接，包成 `command_channel`，执行命令后
  归还。需要在同一连接上连续执行多条命令时，显式调用 `client::acquire()` 与
  `client::release()`。
- **背压。** `connection_config::max_buffered_bytes` 限制应用读缓冲；缓冲满时读侧
  停止从后端消费，直到应用排空。
- **重连。** 终态错误把连接标记为关闭；下一次操作按
  `reconnect_backoff`/`reconnect_max_backoff` 指数退避惰性重连。`max_reconnects`
  限制连续失败重连次数，0 表示无限。`close()` 是终态；只有显式 `reconnect()`
  能复活对象。
- **超时。** `operation_timeout` 限制普通线程的读写；`pool_config::acquire_timeout`
  限制普通线程的池等待。协程路径用挂起而不是本地超时。

## 4. 错误处理 <a id="s4"></a>

有两条不同的失败通道，不能混淆：

| 失败 | 表示 |
|---|---|
| Redis 错误回复（`-ERR ...`、`-WRONGTYPE ...`） | `result` 成功，`resp_value::type() == resp_type::error`。 |
| 传输、分帧、本地限制或关闭 | `result`/`status` 本身失败，携带 `rpc_code`（`connection_reset`、`eof`、`deadline_exceeded`、`protocol_error`、`resource_exhausted`、`shutting_down` 等）。 |

`client::connect()` 与 `client::health_check()` 更严格：它们把 Redis 错误回复视为
`rpc_code::application` 失败，因为健康探测必须区分「服务器回答了 PING」与
「服务器回答了一个错误」。

门面从不抛异常。两处底层异常是刻意且已文档化的：

- 简单字符串或错误载荷包含 CR/LF 时，`resp_encoder::encode_to_string` 抛出
  `std::invalid_argument`，因为那会产出解析器拒绝的帧。
- `redis::value` 的 `as_*` 在类型不符时抛出 `std::bad_variant_access`，与
  `std::get` 一致；需要非抛出形状判断时用 `if_*`。

事务有两种协议级结果，它们不是传输错误：`transaction_exec_result::aborted`
表示 `EXEC` 因 watched key 变化返回 null（协议成功）；`replies` 中的单个条目
可能是 RESP 错误值，因为 Redis 会执行每个排队命令并逐条报告运行时错误。

## 5. 配方 <a id="s5"></a>

### 5.1 Pipeline

```cpp
crpc::redis::connection_pool pool(pool_config, backend);
auto connection = pool.acquire();
if (!connection) {
    return;
}
crpc::redis::command_channel channel(*connection.value());

crpc::redis::pipeline pipe(channel);
pipe.add("SET", "counter", "0")
    .add("INCR", "counter")
    .add("GET", "counter");

const crpc::result<std::vector<crpc::redis::resp_value>> replies = pipe.run();
// replies.value().size() == 3；run() 会消费已排队的命令。
```

Pipeline 不是事务：其他客户端的命令可以插入，且某条命令失败不会阻止后续命令。

### 5.2 事务

```cpp
crpc::redis::transaction tx(channel);
if (tx.watch({"balance"}).ok() && tx.multi().ok()) {
    (void)tx.queue(crpc::redis::command{"DECRBY", "balance", "10"});
    const crpc::result<crpc::redis::transaction_exec_result> exec = tx.exec();
    if (exec.ok()) {
        if (exec.value().aborted) {
            // WATCH 冲突：重试整个读-改-写。
        } else {
            // exec.value().replies 按排队顺序排列。
        }
    }
}
```

排队阶段某条命令被拒绝时事务变脏；`exec()` 会发送 `DISCARD` 并返回原始队列错误，
而不是提交。析构函数不做 I/O；活跃事务必须显式关闭。

### 5.3 Pub/Sub

订阅后的连接退出请求/响应模式，因此要给它独立连接：

```cpp
crpc::redis::connection_pool sub_pool(sub_config, backend);
auto sub_connection = sub_pool.acquire();
crpc::redis::command_channel sub_channel(*sub_connection.value());
crpc::redis::pubsub subscription(sub_channel);

if (subscription.subscribe({"news"}).ok()) {
    const crpc::result<crpc::redis::pubsub_message> message =
        subscription.next();
    if (message.ok()) {
        // message.value().channel、.payload、.pattern、.kind
    }
}

// 在另一个 executor 上发布（通常是另一条池化连接）。
const crpc::result<std::int64_t> receivers =
    crpc::redis::pubsub::publish(channel, "news", "hello");
```

`pubsub::matches` 与 Redis 为 `PSUBSCRIBE` 使用的 glob 匹配器一致。订阅期间到达的
确认推送会排队，由 `next()` 先排空。

### 5.4 Streams

```cpp
crpc::redis::stream streams(channel);

const crpc::result<std::string> id = streams.add(
    "events", {{"type", "login"}, {"user", "alice"}});
if (!id.ok()) {
    return;
}

const crpc::result<std::vector<crpc::redis::stream_entry>> entries =
    streams.range("events", "-", "+", 10);
if (entries.ok()) {
    for (const crpc::redis::stream_entry& entry : entries.value()) {
        // entry.id、entry.parsed_id、entry.fields
    }
}

(void)streams.group_create("events", "workers", "$", true);
crpc::redis::stream_read_options read_options;
read_options.count = 16;
read_options.block = std::chrono::milliseconds(1000);
const auto batch = streams.read_group(
    "workers", "worker-1", {"events"}, {">"}, read_options);
```

`stream_id::parse` 拒绝范围标记 `-`、`+`、`$` 与 `*`；它们是范围语法，不是条目
ID。`stream_entry::id` 保留服务器原始拼写，`parsed_id` 是数值形式。

### 5.5 Cluster

```cpp
crpc::redis::cluster_options cluster_options;
cluster_options.seeds.push_back(
    crpc::redis::cluster_address{"127.0.0.1", 7000});
cluster_options.max_redirects = 5;

crpc::redis::cluster cluster(cluster_options, backend);

const crpc::result<crpc::redis::resp_value> reply =
    cluster.execute(crpc::redis::command{"GET", "user:{42}:name"});
if (!reply.ok()) {
    return;
}

// 事务必须固定在一个节点。pin() 会一次性校验键集合。
const auto pinned = cluster.pin({"user:{42}:name", "user:{42}:age"});
if (pinned.ok()) {
    crpc::redis::transaction tx(*pinned.value());
    // MULTI/EXEC 与所有排队命令现在都发往同一槽的所有者。
}
```

单条命令的键跨槽、或 pipeline 的命令跨槽时，客户端在发生任何 I/O 之前本地返回
`failed_precondition` 与 `CROSSSLOT` 消息。`MOVED` 更新槽表；`ASK` 先发
`ASKING`，不更新槽表。客户端始终路由到 master，没有读副本策略。

### 5.6 Sentinel

```cpp
crpc::redis::sentinel_options sentinel_options;
sentinel_options.master_name = "mymaster";
sentinel_options.sentinels.push_back(
    crpc::redis::sentinel_address{"127.0.0.1", 26379});
sentinel_options.quorum = 2;

crpc::redis::sentinel_connection_executor executor(backend);
crpc::redis::sentinel monitor(sentinel_options, executor);
monitor.set_event_handler([](const crpc::redis::sentinel_event& event) {
    // sdown / odown / promoted / master_switch / ...
});

if (monitor.start()) {
    const crpc::redis::sentinel_address master = monitor.master();
    // 让连接池或 cluster seed provider 指向 `master`。
    monitor.stop();
}
```

Monitor 从不拥有 socket：它驱动一个 `sentinel_command_executor`。
`poll_once_at()` 是测试使用的确定性入口；`start()` 以协程运行同一轮扫描。不要与
后台循环并发调用 `poll_once*()`。Executor 是借用的，必须活过 monitor 的协程。

### 5.7 RTTI 绑定

```cpp
#include <crpc/redis/rtti.hpp>

struct profile {
    std::string id;
    int age = 0;
};
RTTI_DEFINE_TYPE(profile, id, age)

profile value{"u1", 30};
const crpc::result<crpc::redis::command> hset =
    crpc::redis::hash_set_command("user:u1", value);
if (hset.ok()) {
    (void)redis.execute(hset.value());
}

const crpc::result<crpc::redis::resp_value> reply =
    redis.execute(crpc::redis::hash_get_command("user:u1"));
if (reply.ok()) {
    crpc::redis::value decoded;
    if (crpc::redis::value_from_resp(reply.value(), decoded).ok()) {
        profile out;
        const crpc::status status =
            crpc::redis::decode_hash_reply(decoded, out);
        // status.ok() 且 out.id == "u1"
    }
}
```

反射结构体变成一个 Redis hash：每个反射字段一个 hash 字段，基类字段在前。
`bind_options` 控制 schema 漂移：`on_unknown = reject`（默认）或 `ignore`，
`on_missing = keep`（默认）或 `reject`。容器值字段无法存进单个 hash 字段；
`hash_set_command` 对它返回 `rpc_code::encode_error`。`std::string_view` 不可解码
（会悬垂），在编译期拒绝。

### 5.8 直接使用 RESP 与类型化辅助

```cpp
crpc::redis::resp_parser parser;
parser.feed(bytes); // bytes 是 span<const std::byte>

crpc::redis::resp_value value;
for (;;) {
    switch (parser.next(value)) {
    case crpc::redis::resp_parser::result::value_ready:
        // 消费 value
        break;
    case crpc::redis::resp_parser::result::need_more:
        return;
    case crpc::redis::resp_parser::result::error:
        return;
    }
}
```

`types.hpp` 增加 RESP3 形状的 `redis::value`，以及
`crpc::redis::string_type::set("k", crpc::redis::value{"v"})` 之类的构造器与
`decode_int`、`decode_string_array`、`decode_scored_members`、
`decode_stream_entries` 等解码器。

## 6. 构建与测试 <a id="s6"></a>

Redis 客户端是库的一部分，不需要额外 CMake 选项。

```powershell
$env:CRPC_RTTI_DIR   = 'F:\RTTI'
$env:CRPC_COROCC_DIR = 'F:\corocc'
.\scripts\build.ps1 -Config Debug -Arch x64
.\scripts\test.ps1  -Config Debug -Arch x64 -Filter redis
```

`-Filter redis` 运行 Redis 套件：`resp`、`connection`、`pipeline`、
`transaction`、`pubsub`、`stream`、`types`、`cluster`、`sentinel`、`client`、
`rtti` 与 `header_conflict`。RTTI 依赖缺失时 `rtti` 为空，与其他依赖 RTTI 的
测试一致。

同时构建基准：

```powershell
cmake -S . -B .opencode/tmp/out-redis -G Ninja `
      -DCMAKE_BUILD_TYPE=Debug -DCRPC_BUILD_TESTS=ON -DCRPC_BUILD_BENCH=ON `
      -DCRPC_RTTI_DIR=F:\RTTI -DCRPC_COROCC_DIR=F:\corocc
cmake --build .opencode/tmp/out-redis --parallel 2
ctest --test-dir .opencode/tmp/out-redis -R redis --output-on-failure
.\.opencode\tmp\out-redis\crpc_bench_redis.exe
```

基准参数是子串过滤器：`crpc_bench_redis.exe redis/pipeline` 只运行 pipeline
用例。[performance.md §2.5](performance.zh-CN.md#s2-5) 的数字是 Debug、单进程、
传输隔离的回归基线，不是网络容量。

## 7. 已知限制 <a id="s7"></a>

- **`stream_entry` 命名。** `stream.hpp` 提供 `crpc::redis::stream_entry`
  （带 `parsed_id` 解析后 ID 的条目）；`types.hpp` 提供
  `crpc::redis::redis_stream_entry`（`decode_stream_entries` 返回的原始回复投影）。
  两者是不同类型，两个头文件可在同一翻译单元共存，
  `tests/test_redis_header_conflict.cpp` 钉住这一区分。
- **shared 构建导出。** shared 构建已覆盖 Redis 头文件导出。在
  `CRPC_BUILD_SHARED=ON` 下 `ctest -R 'redis|abi_docs|shared_link'` 通过；
  `scripts/export_gate.py` 校验每个带装饰的 Redis 头文件确实导出了其符号。
  静态归档仍是默认构建；shared 构建是同一 API 下受支持的配置。
- **基准范围。** `crpc_bench_redis` 用空传输隔离连接池，用线缆形状的 executor
  隔离 pipeline；它不测量真实 Redis 服务器或网络往返，且记录的是 Debug 数字。
- **Cluster。** 没有读副本策略，也没有后台拓扑刷新；刷新是惰性的（未知槽、
  `CLUSTERDOWN`、`TRYAGAIN`）或显式的（`refresh()`）。跨槽命令与批量在本地拒绝。
- **Sentinel。** Monitor 是建立在调用方 executor 上的状态机，自己不打开 socket；
  `poll_once*()` 不得与后台循环竞争。
- **Pub/Sub。** 订阅后的连接只接受订阅命令族；用独立连接与独立 executor 执行
  `PUBLISH`。
- **RTTI。** 需要 `CRPC_HAVE_RTTI`。DTO 的 hash 字段必须编码为标量；
  `std::string_view` 不可解码。默认拒绝未知回复字段、保留缺失 DTO 字段。
- **RESP 限制。** 解析器默认单个 bulk 值/容器元素计数上限 512 MiB、嵌套深度
  128；两者都是构造参数。
- **类型化门面覆盖。** `client` 只提供字符串、hash、list、set 与有序集合的便捷
  方法；其余命令走 `execute(command)` 或 `types.hpp` 构造器，绝不根据命令名猜 API。

## 8. 另见 <a id="s8"></a>

- [API 参考](api/README.zh-CN.md) —— 头文件索引，含 Redis 表。
- [线上协议 §24](protocol.zh-CN.md#s24) —— 同一连接层使用的帧压缩。
- [性能报告 §2.5](performance.zh-CN.md#s2-5) —— 压缩、RPC 与 Redis 基准实测数字。
- [分布式测试与基准](distributed-testing.zh-CN.md) —— 多节点与基准场景的运行方法。
