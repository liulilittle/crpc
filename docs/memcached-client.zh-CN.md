# memcached 客户端

[English](memcached-client.md) | **简体中文**

基于 CRPC 字节传输层的异步 memcached 客户端。支持文本协议、meta 协议，以及二进制协议的帧层，
并用与 libketama 兼容的哈希环在多台服务器之间分片。

## 快速开始

```cpp
#include <crpc/memcached/client.hpp>
#include <crpc/runtime.hpp>

crpc::inproc_backend backend;           // 也可以换成 tcp 后端
crpc::memcached::client::options opts;
opts.pool.connection.ep = crpc::endpoint::tcp("127.0.0.1", 11211);
opts.pool.max_size = 4;

crpc::memcached::client cache(opts, backend);

const auto stored = cache.store(crpc::memcached::store_kind::set, "user:1", "hello");
if (!stored) { /* stored.error() 同时携带错误码与服务端原文 */ }

const auto values = cache.get({"user:1", "user:2"});
if (values) {
    for (const auto& entry : values.value()) {
        // entry.key、entry.flags、entry.data、entry.cas（仅 gets/gats）
    }
}
```

在协程内调用会挂起当前协程，而不是阻塞线程；在普通线程上则按池配置的超时等待条件变量。
库内没有隐藏的工作线程。

## 多台服务器

`client` 只管一个端点；`router` 管多台，并用 libketama 环决定键落在哪台。因此即使同一部署里
混用本客户端与其他 memcached 客户端，同一个键仍会落到同一台服务器。

```cpp
#include <crpc/memcached/router.hpp>

std::vector<crpc::memcached::router::server> servers;
for (const char* name : {"10.0.0.1:11211", "10.0.0.2:11211"}) {
    crpc::memcached::connection_transport_config cfg;
    cfg.pool.connection.ep = crpc::endpoint::tcp(name);
    servers.push_back({name,
                       std::make_shared<crpc::memcached::connection_transport>(cfg, backend),
                       1});
}
crpc::memcached::router cache(std::move(servers));

// 按键路由。
cache.store(crpc::memcached::store_kind::set, "user:1", "hello");

// 扇出：每个持有键的服务器各一次请求，结果合并。
const auto values = cache.get({"user:1", "user:2", "user:3"});
```

`server::name` 就是哈希环要哈希的字符串。同一部署内所有客户端必须用同一种写法
（`host:port`），否则不同客户端的落点会不一致。

## 不是错误的「结果」

memcached 对常见情形用一个词作答，而那个词并不表示失败。客户端把它们作为值返回，
因为若当作错误，每个调用点都得特判：

| 命令 | 回包 | 结果 |
| --- | --- | --- |
| `add` 撞到已存在的键 | `NOT_STORED` | `store_status::not_stored` |
| `cas` 的令牌已过期 | `EXISTS` | `store_status::exists` |
| 对不存在的键 `append`/`prepend`/`cas` | `NOT_FOUND` | `store_status::not_found` |
| 删除不存在的键 | `NOT_FOUND` | `store_status::not_found` |
| `touch` 成功 | `TOUCHED` | `store_status::touched` |
| 对不存在的键 `incr`/`decr` | `NOT_FOUND` | **错误** `not_found` |

计数器回包是数字，所以「计数器不存在」无法用数字表达，只能作为错误返回。

真正的失败按下表映射，且始终保留服务端原文：

| 回包 | `rpc_code` |
| --- | --- |
| `CLIENT_ERROR <message>` | `invalid_argument`（请求本身不合法） |
| `SERVER_ERROR <message>` | `internal`（服务端故障，不是调用方的问题） |
| `ERROR` | `protocol_error` |

## meta 协议

meta 命令的 flag 列表原样透传，因此本库未建模的 flag 也能用：

```cpp
const auto reply = cache.meta_get("user:1", {"v", "f", "t", "c"});
if (reply && reply.value().type == crpc::memcached::text_reply::kind::meta_hit) {
    reply.value().value;      // 载荷
    reply.value().meta_cas;   // 若请求了 c<cas> 令牌
}
```

`meta_set`、`meta_delete` 与 `meta_noop`（`mn`）返回解析后的结果。其他命令可用
`client::execute()` 原样写出并取回解析后的回包。

## 限制与失败即关闭

* 键超过 250 字节（含配置的前缀）在写出前就被拒绝——服务端本来也会拒绝。
* 值超过 `options::max_value_size`（默认 1 MiB）在写出前就被拒绝。只有在服务端调高了
  `-I` 时才应调高该值。
* 回包单行超过 `max_line_size`（64 KiB）或值块超过 `max_value_size`，以
  `resource_exhausted` 失败，而不是继续涨内存。
* 回包类型与命令不符（例如 `get` 收到 `STORED`）即 `protocol_error`：流已错位。
* 回包后流上仍有多余字节即 `protocol_error`，该连接被关闭丢弃、不归还连接池。
  错位的流绝不会交给下一个调用者。

## 未包含

* **重试**。是否可重试取决于命令本身，以及连接是在写出请求之前还是之后失败，
  因此策略属于调用方。
* **SASL 认证**与二进制协议各命令的 extras 布局。二进制帧头与状态码映射已提供，
  命令编码器尚未提供。
* **UDP 传输**。
* **跨服务器 multi-get 的原子性**。扇出 `get` 在任一服务器失败时整体失败，但它不是快照：
  两次回包之间其他客户端仍可能改动某个键。
