# 反向代理头

[English](proxy-headers.md) | **简体中文**

在 nginx、HAProxy、云负载均衡或 API 网关之后，套接字对端是代理而不是客户端。
客户端地址来自请求头，而该请求头在写出它的代理不被信任时完全由攻击者控制。

`crpc::http::resolve_forwarded()` 只回答一个问题：**在我实际连接的这个对端之下，
还能信任哪一跳？**

```cpp
#include <crpc/http/forwarded.hpp>

crpc::http::trusted_proxies trusted;
trusted.networks.push_back(*crpc::net::ip_network::parse("10.0.0.0/8"));
trusted.networks.push_back(*crpc::net::ip_network::parse("2001:db8::/32"));

// `peer` 必须是请求到达时套接字的地址。
const auto client = crpc::http::resolve_forwarded(request, peer, trusted);
if (!client) {
    // Forwarded 头格式错误：回 400，或忽略这些头。
}
if (client.value().has_address()) {
    client.value().address;      // 链路能证明的客户端地址
    client.value().port;         // 头里没带端口时为 0
    client.value().is_https();   // 仅当可信跳报告了协议
    client.value().host;         // 客户端访问的 Host
    client.value().trusted_hops; // 跳过了几个代理
}
```

## 信任模型

1. **对端不是可信代理** → 完全忽略所有转发头，对端就是客户端。这是唯一安全的默认值：
   否则任何客户端都能自称 `127.0.0.1`。
2. **对端可信** → 从右向左遍历链路，跳过本身可信的跳。第一个不可信的跳就是客户端。
3. **无法解析的跳**（`for=_hidden`、`for=unknown` 或垃圾值）会终止遍历。此时
   `has_address()` 为 false：解析器拒绝为它无法证明的客户端命名，也不会退回对端地址——
   那是另一个、并且是错误的答案。
4. **所有跳都可信** → 报告最左侧的跳，`trusted_hops` 记录有几个代理为其背书。

`trusted_proxies::trust_any` 用于代理地址无法事先确定的部署。它意味着任何客户端都能伪造
自己的地址，因此必须是显式决定，绝不能是默认值。

## 头的优先级

两者同时存在时，RFC 7239 的 `Forwarded` 优先于 `X-Forwarded-*`：前者是标准形式，
也是现代代理实际发出的形式。同一个头内部：

| 字段 | 用途 |
| --- | --- |
| `Forwarded: for=` | 客户端地址与端口 |
| `Forwarded: proto=` | 协议，仅取最左元素 |
| `Forwarded: host=` | 客户端访问的 `Host`，仅取最左元素 |
| `X-Forwarded-For` | 客户端地址与端口 |
| `X-Forwarded-Proto` | 协议 |
| `X-Forwarded-Host` | 主机 |
| `X-Forwarded-Port` | 端口 |

重复出现的字段在解析前按线序拼接：HTTP 允许列表型字段拆成多个字段，只有按序拼接
链路才是正确的。

`Forwarded: by=` 会被校验但不报告：它标识的是代理，而解析器已经从对端知道代理是谁。

## 接受的语法

* IPv4 点分四段，禁止前导零（`010.0.0.1` 被拒绝：对某些解析器是八进制，对另一些是十进制）。
* RFC 4291 的 IPv6，支持 `::` 压缩与内嵌 IPv4 尾部。zone id（`%eth0`）属于传输元数据而非地址，被拒绝。
* 端口：`for=198.51.100.7:6321`、`for="[2001:db8::1]:4711"`，以及 RFC 7239 允许的引号转义值。
* 混淆令牌 `_name` 与 `unknown`，它们有意不产生地址。

格式错误的 `Forwarded` 头返回 `rpc_code::invalid_argument`，而不是被跳过。
`X-Forwarded-For` 中格式错误的条目被视为不可解析的跳，终止遍历而不是让请求失败——
该头并非标准，真实部署里确实会写进垃圾。

## 为什么不是「取 X-Forwarded-For 的第一项」

最左项是**客户端自己声称**的，客户端可以声称任何东西。只有你信任的代理写下的条目才是证据。
从可信对端开始遍历，是唯一每一步都由你已经信任的跳背书的顺序。

## 限制

* 解析器只验证跳的地址是否落在可信范围内，不验证它是否真的是代理。既做 NAT 又跑代理的
  共享出口需要把它的网段列进来。
* `Forwarded` 元素中的未知参数会被接受并忽略；只有结构性错误（缺 `=`、`for` 无法解析）
  才让请求失败。
* 只要对端可信，协议与主机就会被报告，即使链路提前终止。用它做安全决策的调用方还应要求
  `trusted_hops > 0`。
