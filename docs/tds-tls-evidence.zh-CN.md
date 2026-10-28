# TDS over TLS：线路证据

[English](tds-tls-evidence.md) | **简体中文**

这份文档存在的原因是：TDS 适配器有两轮都在加密时失败，而当时的结论（"载荷不同"、
"记录分帧不对"）都是错的。真正的差异在于 **TLS 记录本身的封装方式**，它是通过把可用
客户端的完整密文与我们的逐记录比对才找到的。

## TDS 实际使用的规则

| 阶段 | 线上字节 |
| --- | --- |
| TLS 握手（ClientHello … Finished） | 每条 TLS 记录是 **TDS PRELOGIN 包的包体**：类型 `0x12`、状态 `0x01`（EOM），带正常的 8 字节 TDS 包头 |
| 握手之后 | **裸 TLS 记录**：记录类型 `0x16`/`0x14`/`0x17`/`0x15` 直接写进套接字，**完全不带 TDS 包头** |
| TLS 流内部 | 恢复普通的 TDS 包分帧（LOGIN7 `0x10`、SQL 批 `0x01`、应答 `0x04`） |

`0x17` 是 **TLS** 的应用数据内容类型，不是 TDS 包类型。本仓库此前携带的
`packet_type::ssl = 0x17`（注释称记录被包在 TDS 包**内**）是错误模型，也是这个故障被
误判两次的原因之一：握手后给记录加一个 TDS 包头，会把加密后的 LOGIN7 整体后移八个字节，
SQL Server 于是回 `17832, Severity 20, State 2 — The login packet used to open the
connection is structurally invalid`。

## 方法

把已知可用的客户端 `pytds` 与本适配器都指向一个字节拷贝 TCP 代理（位于 SQL Server 2022
的 `127.0.0.1:1433` 之前），把两个方向的完整密文导出并解析。只比对分帧元数据（记录类型、
长度、扩展、cipher suite、边界）；因为分帧在明文可见，不需要密钥材料。

## 客户端 → 服务端，逐记录

### pytds（可用参照），702 字节

| # | TDS 包头 | 内部 TLS 记录 |
| --- | --- | --- |
| 1 | `12 01 00 3a 00 00 00 00` PRELOGIN，长度 58，id 0 | —（明文 PRELOGIN：version、encryption、thread id、MARS、MSSQLServer instopt） |
| 2 | `12 01 00 db 00 00 01 00` PRELOGIN，长度 219，id 1 | `16 03 01 00 ce` 握手 **ClientHello**，206 字节 |
| 3 | `12 01 00 65 00 00 02 00` PRELOGIN，长度 101，id 2 | `16 03 03 00 25` **ClientKeyExchange** 37 字节、`14 03 03 00 01` **ChangeCipherSpec** 1 字节、`16 03 03 00 28` **Finished** 40 字节 |
| 4 | *无包头* | `17 03 03 00 f4` 应用数据，244 字节（LOGIN7） |
| 5 | *无包头* | `17 03 03 00 46` 应用数据，70 字节（SQL 批） |

### 此前实现（被 17832 拒绝）

旧代码在本次比对之前就被删除了，因此没有它自己的抓包。它做了什么记录在它留下、并被
本次提交删除的注释里：`packet_type::ssl = 0x17` 的注释写着"a TLS record carried inside
a TDS packet, which is how encryption is framed after the handshake"。这正是上文所说的
八字节位移，也与服务端对 LOGIN7（字节比对已证明其正确）报出的 `State 2` 拒绝吻合。

### 修复后的我方实现，786 字节

| # | TDS 包头 | 内部 TLS 记录 |
| --- | --- | --- |
| 1 | `12 01 00 29 00 00 01 00` PRELOGIN，长度 41，id 1 | —（明文 PRELOGIN） |
| 2 | `12 01 00 de 00 00 02 00` PRELOGIN，长度 222，id 2 | `16 03 01 00 d1` ClientHello，209 字节 |
| 3 | `12 01 00 65 00 00 03 00` PRELOGIN，长度 101，id 3 | `16 03 03 00 25` CKE 37 字节、`14 03 03 00 01` CCS 1 字节、`16 03 03 00 28` Finished 40 字节 |
| 4 | *无包头* | `17 03 03 00 d4` 应用数据，212 字节（LOGIN7） |
| 5 | *无包头* | `17 03 03 00 52` 应用数据，82 字节（SELECT 42） |
| 6 | *无包头* | `17 03 03 00 52` 应用数据，82 字节（SELECT 43） |
| 7 | *无包头* | `15 03 03 00 1a` alert，26 字节（close_notify） |

仍存在的结构性差异是有意为之且无害的：明文 PRELOGIN 这里是 41 字节、pytds 是 58 字节
（pytds 多发了我们不发的 INSTOPT 选项）；packet id 从 1 开始而不是 0。两者在此前轮次里
都已被证明无关——用相同 PRELOGIN 与相同 id 起点跑的明文会话能正常登录——现在加密会话
也同样能登录。

## 服务端 → 客户端，逐记录

### pytds

| # | TDS 包头 | 内部 TLS 记录 |
| --- | --- | --- |
| 1 | `04 01 00 2b ...` tabular result，长度 43 | —（明文 PRELOGIN 应答；encryption = `0x01`） |
| 2 | `12 01 04 d7 ...` PRELOGIN，长度 1239，id 0 | ServerHello 65 字节、Certificate 842 字节、ServerKeyExchange 300 字节、ServerHelloDone 4 字节 |
| 3 | `12 01 00 ea ...` PRELOGIN，长度 234，id 0 | Finished 170 字节、CCS 1 字节、Finished 40 字节 |
| 4 | *无包头* | `17 03 03 01 99` 409 字节、`17 03 03 00 3d` 61 字节 |

### 修复后的我方实现，2060 字节

| # | TDS 包头 | 内部 TLS 记录 |
| --- | --- | --- |
| 1 | `04 01 00 25 ...` tabular result，长度 37 | —（明文 PRELOGIN 应答；encryption = `0x01`） |
| 2 | `12 01 04 d7 ...` PRELOGIN，长度 1239，id 0 | 同样四条握手消息，长度完全相同（65/842/300/4） |
| 3 | `12 01 00 ea ...` PRELOGIN，长度 234，id 0 | Finished 170 字节、CCS 1 字节、Finished 40 字节 |
| 4 | *无包头* | `17 03 03 01 99` 409 字节（登录应答）、`17 03 03 00 3f` 63 字节、`17 03 03 00 3f` 63 字节 |

两条 63 字节记录是两个 `SELECT` 的应答；pytds 只有一条 61 字节记录，因为它只发了一个查询。

## ClientHello 比对

两个客户端给出完全相同的 27 个 cipher suite、顺序一致
（`c02c c030 009f cca9 cca8 ccaa c02b c02f 009e c024 c028 006b c023 c027 0067
c00a c014 0039 c009 c013 0033 009d 009c 003d 003c 0035 002f`），legacy version 为
`03 03`，session id 为空，压缩方法 `00`，扩展集合也相同：

| 扩展 | pytds | 我方 |
| --- | --- | --- |
| `renegotiation_info` (65281) | 长度 1 | 长度 1 |
| `ec_point_formats` (11) | `01 00` | `01 00` |
| `supported_groups` (10) | 7 个组 `001d 0017 001e 0018 0019 0100 0101` | 相同 |
| `session_ticket` (35) | 长度 0 | 长度 0 |
| `encrypt_then_mac` (22) | 长度 0 | 长度 0 |
| `extended_master_secret` (23) | 长度 0 | 长度 0 |
| `signature_algorithms` (13) | 同样的 20 组 | 相同 |
| `server_name` (0) | `127.0.0.1`（14 字节） | `172.28.18.36`（17 字节，实际拨号地址） |

没有 ALPN、没有 `supported_versions`、没有压缩：两者都锁定 TLS 1.2，也就是 SQL Server
的 TDS 端点协商出的版本。扩展逐项相等，正是握手从来就不是问题的原因。

## 比对结论

三个假设被检验，前两个被证据否证：

1. ~~LOGIN7 载荷不同~~ —— 更早的轮次已否证：明文与可用客户端逐字节相同，156 字节，hex 全等。
2. ~~packet id 起点不同~~ —— 更早的轮次已否证：两种起点都试过。
3. **握手后的记录封装不同** —— 此处确证：记录必须裸写进套接字，而不是放进 TDS 包里。

## 服务端侧验证

客户端自己的 `encryption_negotiated()` 标志并不能证明服务端同意。在上面这些查询所在的
同一条会话上执行：

```sql
SELECT encrypt_option, protocol_type, auth_scheme
FROM sys.dm_exec_connections WHERE session_id = @@SPID;
```

```
encrypt_option = TRUE    protocol_type = TSQL    auth_scheme = SQL
```

`encrypt_option` 是 SQL Server 自己对这条连接的判定，因此加密是由对端确认的，而不只是
我们状态机的自述。集成用例 `tds_it_require_encryption_logs_in_and_queries` 对这三列都做
断言，所以分帧一旦回归，用例会带着服务端的答案失败。