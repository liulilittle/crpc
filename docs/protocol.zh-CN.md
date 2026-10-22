# CRPC 线上协议

[English](protocol.md) | **简体中文**

版本 1.0 / 2026-10-19

本文规定 TCP 与进程内后端承载的 CRPC 二进制流。帧布局取自 `include/crpc/frame.hpp` 与 `src/frame/frame.cpp`；调用信封与响应布局取自同一个头文件以及 `src/rpc/client.cpp` / `src/rpc/dispatcher.cpp`。HTTP/2 映射取自 `http2/*`。设计规格与代码不一致时，两者都会写明，并注明代码是当前行为。

## 目录

| # | 章节 |
|---|---|
| [1](#s1) | 范围与权威 |
| [2](#s2) | 字节序与对齐 |
| [3](#s3) | 固定帧头 |
| [4](#s4) | 帧类型 |
| [5](#s5) | 帧标志 |
| [6](#s6) | 有序扩展 |
| [7](#s7) | 负载长度与流作用域 |
| [8](#s8) | 调用信封 |
| [9](#s9) | 追踪、截止时间与幂等 |
| [10](#s10) | 响应与错误负载 |
| [11](#s11) | varint 与 zigzag |
| [12](#s12) | 长度限制 |
| [13](#s13) | fail-closed 规则 |
| [14](#s14) | 黄金字节 |
| [15](#s15) | 增量解码 |
| [16](#s16) | 会话握手 |
| [17](#s17) | HTTP/2 映射 |
| [18](#s18) | 版本与验证 |
| [19](#s19) | 推送投递（已实现子集） |
| [20](#s20) | HTTP/1.1 Server-Sent Events（已实现子集） |
| [21](#s21) | HTTP 内容编码（已实现子集） |
| [22](#s22) | HTTP 文件下载与 Range（已实现子集） |
| [23](#s23) | HTTP/1.1 长轮询（已实现子集） |
| [24](#s24) | CRPC 帧压缩（已实现子集） |

## 1. 范围与权威 <a id="s1"></a>

CRPC 二进制协议是带长度前缀的多路复用帧流。一帧由 16 字节固定头、可选的 16 字节有序扩展和负载组成。帧层不了解 RPC 方法；`REQUEST`、`RESPONSE`、`ONE_WAY` 的负载携带调用信封。

权威来源：`include/crpc/frame.hpp` 规定字段宽度、数值、标志、限制与 CRC；`src/frame/frame.cpp` 规定校验顺序与信封编码；`src/rpc/client.cpp` 规定客户端写什么、接受什么；`src/rpc/dispatcher.cpp` 规定响应编码。设计规格 §7 是目标契约。有一处偏差需要明确：信封使用 64 位 service/method id 和不同的扩展布局（§8、§9）。截止时间按设计使用相对值（§9）。

## 2. 字节序与对齐 <a id="s2"></a>

CRPC 帧中的每个多字节字段都是**小端**，与宿主无关。值通过 `crpc::endian` 与 `byte_reader`/`byte_writer` 逐字节读写；不把结构体 `memcpy` 到线上，也不通过非对齐转换读取整数。没有填充，也没有对齐要求：字段从表格给出的偏移开始。

负载中的无符号整数使用 base-128 varint；有符号整数先 zigzag 再 varint。IEEE-754 浮点以固定宽度小端整数承载其位模式，因此 NaN 载荷与负零可以逐字节往返。

HTTP/2 是例外：它的头部和整数是大端。CRPC 二进制帧与 HTTP/2 帧是不同对象，见 §17。

## 3. 固定帧头 <a id="s3"></a>

固定头恰好 16 字节。`payload_len` 只计算固定头与有序扩展之后的负载。

| 偏移 | 宽度 | 字段 | 类型 | 字节序 | 语义 |
|---|---|---|---|---|---|
| 0 | 4 | `payload_len` | `u32` | 小端 | 固定头与有序扩展之后的字节数；`<= max_frame` |
| 4 | 1 | `type` | `u8` | — | §4 之一；未知即拒绝 |
| 5 | 1 | `flags` | `u8` | — | §5 的位 |
| 6 | 2 | `reserved` | `u16` | 小端 | 必须为零；非零即拒绝 |
| 8 | 4 | `stream_id` | `u32` | 小端 | bit 31 必须为零；0 表示连接级 |
| 12 | 4 | `ext` | `u32` | 小端 | `flags.CRC32C` 时为 CRC32C，否则为零 |

`frame_header` 不存储 `reserved`；编码器总是写零，解码器拒绝非零值。`max_frame` 默认 16 MiB，硬上限 64 MiB。`max_stream_id` 为 `0x7FFFFFFF`。

`ext` 双向检查：没有 `CRC32C` 却非零是协议错误；`CRC32C` 帧的计算校验和与 `ext` 不同也是协议错误。校验和是 CRC32C（Castagnoli，反射多项式 `0x82F63B78`），查表实现且不分配。

## 4. 帧类型 <a id="s4"></a>

数值永久固定；修改即破坏线上兼容。

| 值 | 名称 | 流作用域 | 负载 |
|---|---|---|---|
| `0x00` | `DATA` | 非零 | 流式分块或推送正文片段 |
| `0x01` | `REQUEST` | 非零 | 调用信封 + 请求正文 |
| `0x02` | `RESPONSE` | 非零 | 状态字节 + 正文或错误信封 |
| `0x03` | `ONE_WAY` | 非零 | 调用信封 + 正文；必须置 `UNRELIABLE` |
| `0x04` | `RST_STREAM` | 非零 | `rpc_code`，`u32` 小端 |
| `0x05` | `SETTINGS` | 零 | settings TLV（设计） |
| `0x06` | `SETTINGS_ACK` | 零 | 空 |
| `0x07` | `PING` | 零 | 8 个不透明字节 |
| `0x08` | `PONG` | 零 | 8 个不透明字节 |
| `0x09` | `GOAWAY` | 零 | `last_stream_id` `u32` + `rpc_code` `u32` |
| `0x0A` | `WINDOW_UPDATE` | 零或非零 | `increment` `u32` |
| `0x0B` | `PUSH` | 非零 | 推送信封 + 正文（第 19 节） |
| `0x0C` | `PUSH_ACK` | 非零 | `push_seq` `u64` |

帧编解码器校验头部和每种类型的作用域，但不解析 `SETTINGS`、`GOAWAY`、`WINDOW_UPDATE` 的负载；这些负载语义在这一层属于**已设计，未实现**。客户端实现 `REQUEST`/`ONE_WAY` 写出、`RESPONSE` 解码、`PING`/`PONG` 回显、`GOAWAY` 处理，并在第 19 节的单分片子集中投递 `PUSH`、对可靠消息回 `PUSH_ACK`；客户端收到的 `PUSH_ACK` 会被忽略，因为本切片只由服务器推送。

## 5. 帧标志 <a id="s5"></a>

| 位 | 掩码 | 名称 | 含义 |
|---|---|---|---|
| 0 | `0x01` | `END_STREAM` | 该流的最后一帧 |
| 1 | `0x02` | `COMPRESSED` | 负载已压缩；算法在 5–7 位 |
| 2 | `0x04` | `CRC32C` | `ext` 携带负载的 CRC32C |
| 3 | `0x08` | `ORDERED` | 头部之后跟 16 字节 lane 扩展 |
| 4 | `0x10` | `UNRELIABLE` | 尽力投递；不重传 |
| 5–7 | `0xE0` | `ALGO` | 0 none，1 lz4，2 zstd，3 gzip，4–7 保留 |

`COMPRESSED = 0` 要求 `ALGO = 0`；`COMPRESSED = 1` 要求非零 `ALGO`（`1` lz4 已实现；`2` zstd 与 `3` gzip 保留，本版本以 `protocol_error` 失败关闭；`4`-`7` 保留）；`ONE_WAY` 要求 `UNRELIABLE`。帧级压缩的规范见第 24 节。解码器无法解压所宣告的算法时必须 fail-closed 返回 `protocol_error`，绝不把压缩字节当作 payload 交付。压缩仅定义于 `REQUEST`、`RESPONSE`、`DATA`、`ONE_WAY`、`PUSH`；控制帧带 `COMPRESSED` 是 `protocol_error`。头部校验仍接受这样的帧头（只校验标志一致性），拒绝发生在整帧路径。

## 6. 有序扩展 <a id="s6"></a>

当 `ORDERED` 置位时，固定头之后恰好 16 字节：

| 头后偏移 | 宽度 | 字段 | 类型 | 字节序 |
|---|---|---|---|---|
| 0 | 8 | `lane_id` | `u64` | 小端 |
| 8 | 8 | `lane_seq` | `u64` | 小端 |

`payload_len` 不包含这些字节。`ORDERED` 未置位时不存在扩展；假设其存在的解码器不符合规范。`lane_seq` 在入队时按 lane 分配一次并保持单调；绝不跨 lane 比较。

## 7. 负载长度与流作用域 <a id="s7"></a>

- `payload_len` 在任何分配之前对照 `max_frame` 检查。默认 16 MiB，硬上限 64 MiB；更大的构造参数会被钳制。
- `stream_id` bit 31 保留且必须为零；`0` 是连接级。
- `REQUEST`、`RESPONSE`、`DATA`、`ONE_WAY`、`PUSH`、`RST_STREAM`、`PUSH_ACK` 要求非零 id。
- `SETTINGS`、`SETTINGS_ACK`、`PING`、`PONG`、`GOAWAY` 要求 id 为 0。
- `WINDOW_UPDATE` 对连接和流都合法。
- 一条连接上的 id 永不复用。未知或已终态 id 的响应会被丢弃，绝不投递给已回收的槽。

- 对 `COMPRESSED` 帧，`payload_len` 仍是**线上**字节数，payload 以 4 字节 `uncompressed_len`（小端）开头；逻辑 payload 为 `uncompressed_len <= max_frame` 字节。压缩绝不放大帧：编码后（含 4 字节前缀）不严格更小时，发送方回退为未压缩帧（§24.4）。

## 8. 调用信封 <a id="s8"></a>

`REQUEST` 与 `ONE_WAY` 在负载开头携带调用信封：

| 偏移 | 宽度 | 字段 | 类型 | 字节序 | 语义 |
|---|---|---|---|---|---|
| 0 | 8 | `service_id` | `u64` | 小端 | 已注册服务号 |
| 8 | 8 | `method_id` | `u64` | 小端 | 已注册方法号 |
| 16 | 1 | `call_flags` | `u8` | — | bit 0 `has_extensions`；1–7 位必须为零 |
| 17 | 2 | `codec` | `u16` | 小端 | 0 binary，1 json，2 xml，3 ini，4 toml，5 protobuf，6 flatbuffers |
| 19 | 1 | `ext_flags` | `u8` | — | 仅在 bit 0 置位时存在；见 §9 |
| … | … | 扩展字段 | … | … | 顺序见 §9 |
| … | 其余 | `body` | 字节 | — | 应用负载 |

最小信封 19 字节。当前代码中 `RESPONSE` 以 §10 的状态字节开头，不携带信封；设计中的成功路径信封属于**已设计，未实现**。设计 §7.6 画的还是 32 位 id、`remaining_ns` varint、meta TLV 和版本字段；代码使用 64 位 id 和上述扩展块。代码是当前线上格式；要完全匹配设计需要破坏线上兼容并提升版本。

`call_flags` 只有一个已定义位；未知位在解码时是协议错误，在编码时是无效参数。`codec` 必须是七个已知值之一。

## 9. 追踪、截止时间与幂等 <a id="s9"></a>

当 `call_flags` bit 0 置位时，codec 字段之后是扩展块。其 `ext_flags` 控制哪些字段存在，顺序固定：

| 位 | 名称 | 字段 | 宽度 |
|---|---|---|---|
| 0 | `has_trace_id` | `trace_id` | 16 字节 |
| 1 | `has_span_id` | `span_id` | 8 字节 |
| 2 | `has_parent_span_id` | `parent_span_id` | 8 字节 |
| 3 | `has_deadline` | `deadline_ns` | `u64` 小端 |
| 4 | `has_idempotency_key` | `key_len` `u8` + key | 1 + 0…128 字节 |

5–7 位保留且必须为零。`span_id` 要求 `trace_id`；`parent_span_id` 要求 `span_id`。幂等键最多 128 字节；当前客户端写入 32 字节的 R6 三元组 `(client_id[16], client_epoch u64, seq u64)`。`call_flags` bit 0 未置位时，`ext_flags` 必须为零且不得有键。存在位未置位时对应字节不存在；没有该位却有非空键会被拒绝。

**截止时间语义。** `deadline_ns` 是 `remaining_ns`：编码该帧时剩余的时间，绝不是绝对时刻。steady_clock 的纪元因进程和主机而异，绝对值在对端没有意义（P12，设计 §8.5）。客户端通过注入的单调时钟计算 `slot.deadline - now`，截止时间已过则写 `0`；服务端按 `steady_clock::now() + deadline_ns` 重建本地截止时间。因此传输耗时不计入对端预算，剩余值为零时必须不执行处理器直接返回 `deadline_exceeded`。

## 10. 响应与错误负载 <a id="s10"></a>

`RESPONSE` 负载以状态字节开头：0 表示成功，其余字节是响应正文；1 表示错误，随后是错误信封。其他值是协议错误。

| 错误字段 | 编码 |
|---|---|
| `code` | varint |
| `retryable` | `u8`，0 或 1 |
| `retry_after_ms` | varint |
| `message_len` | `u16` 小端 |
| `message` | `message_len` 字节，UTF-8 |
| `meta_count` | `u16` 小端 |
| 重复项 | `key_len u16`、key、`value_len u16`、value |

编码器把 `message_len`、`key_len`、`value_len`、`meta_count` 钳制到 `uint16_t`；设计中的 512 字节消息和 1 KiB 元数据限制是目标策略，不是此编解码器强制的限制。大于 `application`（21）的未知 `rpc_code` 解码为 `internal`，原值放入 `metadata["crpc.raw_code"]`。`RST_STREAM` 负载是 `u32` 小端的 `rpc_code`；客户端取消时发送它，并忽略同一 stream 的迟到响应。

**服务端准入错误。** 准入阶段复用该错误信封，不新增线上字段。可选的连接级令牌桶返回 `resource_exhausted`（13）和非零 `retry_after_ms`。服务级令牌桶返回同样的码；若等待会越过请求 deadline，则返回 `deadline_exceeded`（2）且 `retry_after_ms = 0`。服务级 bulkhead 满载返回 `overload`（8）并带其提示；breaker open 返回 `unavailable`（3）并带剩余打开时长；dispatcher 的执行前 deadline 拒绝仍是 `deadline_exceeded` 且 `retry_after_ms = 0`。HTTP 映射：`resource_exhausted`/`overload` 为 429，`unavailable` 为 503，`deadline_exceeded` 为 504。`retry_after_ms` 是服务端给出的建议下界，不保证重试一定成功。未知服务 id 仍是 `not_found`（9），且不会创建服务端状态。

**`crpc-degradable` 标记。** handler 执行前的拒绝可以由持有降级值的客户端用降级值回答；handler 执行后的失败则不能。两者可能带同一个 `rpc_code`，所以服务端用一个保留的响应 meta 条目声明差别：`crpc-degradable: true`（key 长 15，value 长 4）。它只在恰好四个执行前拒绝点写出，且要求请求剩余的线上 deadline 至少 1 ms：

| 拒绝点 | `rpc_code` |
|---|---|
| 服务级熔断器打开 | `unavailable`（3） |
| 服务级令牌桶为空、deadline 仍可达 | `resource_exhausted`（13） |
| 服务级 bulkhead 满载 | `overload`（8） |
| 服务级在途上限已满 | `overload`（8） |

另外五条错误路径永不携带它：连接级泛洪令牌桶（`resource_exhausted`）、连接级在途上限（`overload`）、服务级令牌桶的「无法满足 deadline」出口（`deadline_exceeded`）、dispatcher 自身的执行前 deadline 拒绝（`deadline_exceeded`），以及所有 handler 产生的错误（包括 `overload` 与 `unavailable`）。one-way 请求也永不携带：它没有响应帧。该标记**不可由 code 推导**——handler 返回的 `overload` 与 bulkhead 返回的 `overload` 除该条目外完全相同。客户端把它用作降级闸的前置条件：是否可降级（`(code, stage)` 表）、入口时间下界与超时检查仍决定降级是否可以执行。`crpc-degraded`（声明「降级值确实产生」的伴随键）在服务端仍无人写入，服务端自身也仍不执行降级。

## 11. varint 与 zigzag <a id="s11"></a>

varint 是 base-128，最低 7 位组在前，bit 7 是续位。`u64` 最多 10 字节。读取器拒绝超长编码：在 shift 63 时最后一组只能是 0 或 1；第 11 个续位字节也被拒绝。

```
encode: (u64(value) << 1) ^ u64(value >> 63)
decode: (u >> 1) ^ (~(u & 1) + 1)
```

所有算术使用无符号类型，避免有符号溢出未定义行为。浮点先按位转换为 `u32`/`u64`，再按小端写出。

## 12. 长度限制 <a id="s12"></a>

| 项 | 限制 | 执行点 |
|---|---|---|
| 帧头 / 有序扩展 | 16 / 16 字节 | 固定 |
| 帧负载 | 默认 16 MiB，硬上限 64 MiB | `max_frame`，分配前检查 |
| stream id | `0x7FFFFFFF` | bit 31 保留 |
| 幂等键 | 128 字节 | 长度前缀 + 解码检查 |
| 调用信封 | 最小 19 字节 | 截断即 `protocol_error` |
| 解码缓冲 | `max_frame + 32`，至少 64 MiB | `frame_decoder` |
| `buffer` 上限 | 默认 64 MiB | `buffer::default_max_capacity` |
| crpc-binary 深度 | 32 | `binary_codec` |
| HTTP/1.1 行 / 头 / 正文 | 8 KiB / 64 KiB + 100 字段 / 8 MiB | `parser_limits` |
| multipart | 32 个 part、8 KiB part 头、64 MiB part、256 MiB 总量、70 字节 boundary | `multipart_limits` |
| h2 帧 / 头列表 | 默认 16 KiB（最大 16 MiB − 1）/ 16 KiB | `http2` settings |
| 压缩 payload 前缀 | 4 字节 | 固定（`uncompressed_len` u32 小端） |
| `uncompressed_len` | `1..max_frame` | `protocol_error` |
| 压缩比 | 默认 300:1（`max_compression_ratio`，0 关闭） | `resource_exhausted` |
| 解压后帧 | `<= max_frame`；且 `<=` 解码器 payload 缓冲 | `protocol_error` / `resource_exhausted` |

限制是包含性的，并在分配或指针运算之前检查。

## 13. fail-closed 规则 <a id="s13"></a>

解码器对下列条件返回 `rpc_code::protocol_error`。`rpc::client` 把解码错误视为致命：停止读取、标记关闭，并用 `protocol_error` 使所有在途调用失败。设计中的 `GOAWAY(protocol_error)` 发送在客户端属于**已设计，未实现**。

| 条件 | 结果 |
|---|---|
| 未知 `type` | `protocol_error` |
| `reserved` 非零 | `protocol_error` |
| `stream_id` bit 31 置位 | `protocol_error` |
| 违反流作用域（§7） | `protocol_error` |
| `payload_len > max_frame` | `protocol_error`，分配前拒绝 |
| `COMPRESSED`/`ALGO` 不一致 | `protocol_error` |
| 未知压缩算法 | `protocol_error` |
| 非零 `ext` 但没有 `CRC32C` | `protocol_error` |
| CRC32C 不匹配 | `protocol_error` |
| 解码长度与 `payload_len` 不一致 | `protocol_error` |
| 信封截断 | `protocol_error` |
| 未知 `call_flags` 位 | `protocol_error` |
| 未知 `codec` | `protocol_error` |
| 未知 `ext_flags` 位 | `protocol_error` |
| span 没有 trace，parent 没有 span | `protocol_error` |
| 幂等键超过 128 字节 | `protocol_error` |
| 没有 `has_extensions` 却有扩展字段 | `protocol_error` |
| 响应状态不是 0/1 | `protocol_error` |
| `ONE_WAY` 没有 `UNRELIABLE` | `protocol_error` |
| 不支持的 `ALGO`（zstd、本版本 gzip、保留值） | `protocol_error` |
| 压缩 payload 短于 5 字节 | `protocol_error` |
| `uncompressed_len` 为零或大于 `max_frame` | `protocol_error` |
| 码流畸形或解出长度不符 | `protocol_error` |
| 控制帧类型带 `COMPRESSED` | `protocol_error` |
| 解压后大小或比值超过配置上限 | `resource_exhausted` |
| `COMPRESSED` 帧到达借用模式解码器 | `protocol_error` |

未知或已终态 stream 的响应不是协议错误；它会被丢弃。

## 14. 黄金字节 <a id="s14"></a>

`REQUEST`，`payload_len = 16`，`stream_id = 1`，无标志：

```
10 00 00 00 01 00 00 00 01 00 00 00 00 00 00 00
```

同一请求带 `ORDERED`，`lane_id = 7`，`lane_seq = 9`：

```
10 00 00 00 01 08 00 00 01 00 00 00 00 00 00 00
07 00 00 00 00 00 00 00 09 00 00 00 00 00 00 00
```

`PING`，负载 `01 02 03 04 05 06 07 08`：

```
08 00 00 00 07 00 00 00 00 00 00 00 00 00 00 00
01 02 03 04 05 06 07 08
```

`REQUEST`，`service_id = 1`，`method_id = 2`，codec `binary`，无扩展，正文 `61 62`（`"ab"`）。信封 19 字节，因此负载 21 字节（`0x15`）：

```
15 00 00 00 01 00 00 00 01 00 00 00 00 00 00 00
01 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00
00 00 00 61 62
```

帧头与 `PING` 向量由 `tests/test_frame.cpp` 断言；信封字段顺序由该文件的 `call_envelope_round_trip_all_fields` 用例断言。

## 15. 增量解码 <a id="s15"></a>

`frame_decoder` 接受任意切分的块，包括逐字节输入和一块多帧。`feed()` 把输入复制到内部缓冲区；调用方的 span 不必活过本次调用。`next()` 返回 `need_more`、`frame_ready` 或 `error`。借用模式下负载指向解码器缓冲区，下一次 `feed()`/`next()` 后失效；复制模式下负载被复制到调用方缓冲区，下一次 `next()` 后失效，超长负载返回 `resource_exhausted`。

解码器不会为头部尚未通过校验的负载分配内存。只有当死前缀至少 4 KiB 且至少占缓冲区一半时才压缩，避免二次复制。`decode_frame` 要求输入大小恰好等于编码大小；尾随字节是长度不匹配。`decode_frame_header` 只校验头部，CRC 留给能访问负载的解码器。`decode_call_envelope` 与 `decode_response` 借用输入中的正文（以及幂等键）；视图只在输入有效期间有效。

## 16. 会话握手 <a id="s16"></a>

**已设计，未实现。** 设计定义 12 字节客户端 hello（`"CRPC"`、`u16` version、`u16` flags、`u16` tlv_len、`u16` reserved），随后是 `max_frame`、`initial_window`、`codec`、`compress_bitmap`、`compress_min`、`keepalive_ms`、`max_streams`、`identity`、`resume_token`、`hmac` 等 TLV。服务端 hello 使用相同前缀并带状态字节。限制 4 KiB、超时 3 秒；未知 TLV/codec/能力位、非零 reserved 或版本不匹配均 fail-closed。

`protocol_version` 为 `1`，但当前代码不写也不解析握手。帧头没有版本字段，因此版本协商尚不存在；两端必须由兼容源码构建。

压缩能力（`compress_bitmap`、`compress_min`、`max_decompressed_frame`）在当前实现中不由该握手协商；连接级 `SETTINGS` 帧承载同样字段，见 §24.5。对端宣告之前不压缩。

## 17. HTTP/2 映射 <a id="s17"></a>

HTTP/2 是独立的协议层。固定头 9 字节且为大端：

| 偏移 | 宽度 | 字段 | 语义 |
|---|---|---|---|
| 0 | 3 | `length` | 负载长度，24 位大端 |
| 3 | 1 | `type` | 帧类型 |
| 4 | 1 | `flags` | 类型相关标志 |
| 5 | 4 | `R` + `stream_id` | 1 位保留 + 31 位 id，大端 |

已实现类型：`DATA`（0x0）、`HEADERS`（0x1）、`PRIORITY`（0x2）、`RST_STREAM`（0x3）、`SETTINGS`（0x4）、`PUSH_PROMISE`（0x5）、`PING`（0x6）、`GOAWAY`（0x7）、`WINDOW_UPDATE`（0x8）、`CONTINUATION`（0x9）。HPACK 实现 61 项静态表、带大小更新的动态表、Huffman 编码以及整数/字符串原语。连接状态机实现 settings/ACK、流状态、CONTINUATION 顺序、连接与流窗口、停车发送、GOAWAY 与 RST_STREAM。默认：16 KiB 帧、100 流、16 KiB 头列表、65535 初始窗口。

设计中的 CRPC 映射（规格 §12.4）：

| CRPC 概念 | HTTP/2 表示 |
|---|---|
| 请求 | `HEADERS` 带 `:method POST`、`:path`、`content-type: application/crpc`，随后 `DATA` 承载 CRPC `REQUEST` 负载 |
| 响应 | `HEADERS` 带 `:status`，随后 `DATA` 承载 CRPC `RESPONSE` 负载 |
| 取消 | `RST_STREAM(CANCEL)`；适配器把调用槽主张为 `canceled` |
| 关闭 | `GOAWAY`；现有流排空或取消 |
| 追踪/超时/幂等/lane | `x-crpc-trace-id`、`x-crpc-timeout-ms`、`x-crpc-idempotency-key`、`x-crpc-attempt`、`x-crpc-lane-id`、`x-crpc-lane-seq` |

两个 stream id 空间相互独立；适配器必须维护 `h2_stream_id -> call_slot*` 映射。h2 与 CRPC 的流控层也相互独立、不共享。

HTTP 服务端门面（`http/server.hpp`）自行实现 HTTP/1.1、h2c 与 h2 传输：明文连接首字节匹配 HTTP/2 连接前导（`PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`）即按 HTTP/2 服务，其余明文连接按 HTTP/1.1，TLS 连接按 ALPN 结果（`h2` 或 `http/1.1`）路由。请求经 MVC 路由器分派而非 CRPC 调用槽适配器，因此上面的 `h2_stream_id -> call_slot*` 映射属于 CRPC-on-h2 映射，仍为**已设计，未实现**。HTTP 服务端复用本节描述的同一套帧、HPACK、连接层与流控规则。

## 18. 版本与验证 <a id="s18"></a>

线上协议有自己的版本 `protocol_version = 1`，独立于库版本。帧类型、标志、codec id 与 `rpc_code` 数值永久固定；新值追加在末尾，未知值被拒绝而不是映射为默认值。线上变更需要提升协议版本并附带迁移说明；ABI 策略是另一份文档（[ABI 策略](abi.zh-CN.md)）。

验证位于 `tests/test_frame.cpp`（帧头与有序/PING 黄金字节、CRC32C、fail-closed 规则、信封往返）、`tests/test_wire.cpp`（varint/zigzag 边界）、`tests/test_http2.cpp`（h2 帧、HPACK RFC 向量、流状态、流控）以及 `tests/test_rpc_core.cpp`（请求、响应、取消、超时路径）。无法按本文精确解码字段的读者必须拒绝该帧；不存在“尽力而为”的解码路径。

## 19. 推送投递（已实现子集） <a id="s19"></a>

本节对已实现的单分片、进程内子集具有规范效力。PUSH 与 REQUEST/RESPONSE 走同一条连接。跨分片路由、ordered/gap 处理与 `stream_transport` 集成不属于本子集；HTTP SSE 与 HTTP 长轮询分别见第 20、23 节。

### 19.1 PUSH 信封

`PUSH`（类型 `0x0B`）的 `stream_id` 必须非零；每个订阅分配一条推送流。负载如下：

| 偏移 | 宽度 | 字段 | 类型 | 语义 |
|---|---|---|---|---|
| 0 | 1 | `version` | `u8` | 必须为 `1` |
| 1 | 1 | `mode` | `u8` | 0 可靠，1 不可靠 |
| 2 | 2 | `topic_id` | `u16` | 服务器分配，非零 |
| 4 | 8 | `publisher_id` | `u64` | 发布者标识 |
| 12 | 8 | `epoch` | `u64` | 发布者 epoch |
| 20 | 8 | `seq` | `u64` | 每订阅可靠序号；不可靠时为 0 |
| 28 | 8 | `topic_seq` | `u64` | 每 topic 单调序号 |
| 36 | 4 | `body_len` | `u32` | 必须等于剩余负载长度 |
| 40 | `body_len` | `body` | 字节 | 应用负载 |

未知 version、未知 mode、`topic_id = 0`、可靠信封却 `seq = 0`、固定字段被截断，或 `body_len` 与剩余字节数不符，都是协议错误，必须在投递前拒绝。

### 19.2 订阅控制与流映射

推送控制复用普通 REQUEST 路径而不是新帧类型，保留 `service_id = 0xFFFFFFFFFFFFFFFF`、`method_id = 1`（subscribe）：

```
请求：u64 subscriber_id, u8 mode, u8 ordered, u16 topic_len, topic 字节
应答：u32 stream_id, u16 topic_id, u8 mode, u8 ordered, u16 topic_len, topic
```

应答就是 `stream_id`/`topic_id` 映射。每个订阅独占一个非零推送 `stream_id`，因此 `PUSH_ACK` 可以保持第 4 节冻结的格式不变：类型 `0x0C`、订阅的 `stream_id`、负载 `push_seq` `u64`（累积确认）。服务器维护 `stream_id → (subscriber_id, topic, mode, ordered)` 映射，并用 `hub.ack(subscriber_id, topic, seq)` 应用确认。

### 19.3 客户端行为

收到格式正确的 PUSH 后，客户端调用已注册的 push sink。对可靠消息，客户端按推送流记录已接受的最大 `seq`，并丢弃 `seq` 不大于它的帧（至少一次的重放因此在 sink 处变为至多一次投递）。可靠帧在 sink 返回后回 `PUSH_ACK`；不可靠帧（`seq = 0`）不回确认。信封非法则按协议失败处理。

### 19.4 服务器泵与出站上限

每条连接沿用既有发送队列暂存 PUSH 帧。暂存的 PUSH 字节（在途加排队）由 `server::options::push_egress_max_bytes` 限制（默认 1 MiB），与响应在途上限相互独立。达到上限时泵停止调用 `try_take`；已经取出的消息停靠在连接上而不是被丢弃。因此所有丢弃决策都由 hub 的每订阅 `max_messages` / `max_bytes` 与 `overflow_policy` 作出，`publish()` 永不因慢订阅者阻塞。

关闭连接时，必须在销毁连接及其推送令牌之前先 `hub.detach`，使迟到的 `PUSH_ACK` 不可能指向已死连接。订阅在 detach 后保留，因此使用同一 `subscriber_id` 重连时会重放未确认的可靠消息并继续投递。

本子集未接 `hub::tick` 的慢订阅检测：不产生 `slow_subscriber` 事件，内存由 hub 的每订阅有界队列及其溢出策略兜底。

## 20. HTTP/1.1 Server-Sent Events（已实现子集） <a id="s20"></a>

本节对已实现的 SSE 子集具有规范效力：HTTP/1.1、单分片、进程内。长轮询见第 23 节；HTTP/2 SSE、跨分片发布、鉴权与 `content_coding` 不属于本子集。分帧辅助位于 `include/crpc/http/sse.hpp` / `src/http/sse.cpp`；服务端接线位于 `src/http/server.cpp`。

### 20.1 请求

`GET <sse-path>`；`Accept: text/event-stream` 可选。查询参数覆盖已注册路由：`topic`（必须非空）、`mode`（`reliable` 或 `unreliable`）、`ordered`（`0`/`1` 或 `false`/`true`）与 `subscriber`（非零 u64；缺省时分配新 id）。可选的 `Last-Event-ID: <epoch>:<seq>` 用于续订。路由通过 `http_server::builder::add_sse_route()` 注册。SSE 要求 `http_server_options::shards == 1`，否则 `start()` 以 `failed_precondition` 失败。

### 20.2 响应与事件

响应为 `200`，携带 `Content-Type: text/event-stream; charset=utf-8`、`Cache-Control: no-cache`、`Connection: keep-alive`、`X-Accel-Buffering: no` 与 `Transfer-Encoding: chunked`（没有 `Content-Length`；响应头为手写，因为 `http::serialize` 总会追加终止块）。事件帧为 `id: <epoch>:<seq>\ndata: <payload>\n\n`。非合法 UTF-8 文本的负载会做 base64 编码并标记为 `data: b64:<...>`；只有二进制事件带 `b64:` 前缀。每 `sse_options::heartbeat`（默认 15 秒，0 关闭）发送一次注释心跳 `:hb\n\n`。配置的 `retry:` 提示仅在开头发送一次。

### 20.3 投递、背压与关闭

已打开的 SSE 响应独占其连接：不使用 HTTP/1.1 流水线，且禁用 idle 截止时间。存活检测来自心跳写（写失败即关闭会话）以及 `sse_options::slow_timeout`：当暂存输出非空而对端停止排空时关闭。暂存输出（写队列加在途写）由 `sse_options::egress_cap_bytes` 限制（默认取 `http_server_options::egress_cap_bytes`，1 MiB）。达到上限时泵停止调用 `try_take`，并把已取出的消息停靠（而非丢弃）；所有丢弃决策由 hub 的 `subscribe_options`（`max_messages`、`max_bytes`、`overflow_policy`）作出，`http_server::publish()` 永不阻塞。关闭时必须在传输与其令牌被销毁之前先从 hub detach；终止块与关闭绑定，不在事件之间发送。

### 20.4 重连与重放

以同一 `subscriber` 重连会复用既有订阅：服务端绝不重复订阅。服务端把 `Last-Event-ID` 序号 clamp 到它为该 (subscriber, topic) 已交付过的最高序号，然后调用 `hub.ack(cursor)`（累积）与 `hub.attach()`，后者重放未确认的可靠消息。epoch 与当前服务端 epoch 不一致的游标不会被确认。因此事件 id 同时携带 epoch 与序号，客户端必须以 `(epoch, seq)` 去重。

### 20.5 HTTP/2

本子集不支持 HTTP/2 SSE。命中 SSE 路由的请求会收到一个完整的 `501` 响应，不会打开流。

## 21. HTTP 内容编码（已实现子集） <a id="s21"></a>

本节对服务器门面的内容编码接线（HTTP/1.1、进程内）具有规范效力。原语位于 `include/crpc/http/content_coding.hpp` / `src/http/content_coding.cpp`；服务端接线位于 `src/http/server.cpp`。SSE 响应（第 20 节）、HTTP/2 响应与文件下载永不参与内容编码。

### 21.1 请求解码

携带 `Content-Encoding: gzip` 或 `deflate`（仅单层）的请求，其请求体在路由前被解码。解码后的体替换线上体并移除 `Content-Encoding`；若存在 `Content-Length`，改写为解码后的长度。不支持的编码或分层编码返回 `415`；损坏或截断的流返回 `400`；超过 `http_server_options::max_decoded_body`（默认 8 MiB）返回 `413` 并关闭连接。`max_decoded_ratio` 限制解码/编码比（0 为默认值，表示关闭该检查：合法请求可以高度可压缩）。请求解码由 `enable_request_decoding` 控制。

CRPC 通道的帧级压缩是独立机制，拥有自己的容器、上限与能力交换，见第 24 节；本节内容不适用于 CRPC 帧。

### 21.2 响应压缩

启用 `enable_response_coding` 时，响应体按 `Accept-Encoding`（q 值；gzip 优先于 deflate，再优先 identity）协商。即使最终不压缩，也会进行协商，并携带 `Vary: Accept-Encoding`。已有 `Vary`（如 `Origin`）与 `Accept-Encoding` 合并（不重复、不覆盖）。若客户端拒绝所有支持的编码（`identity;q=0` 且 gzip/deflate 不可接受），返回 `406`，该响应同样携带 `Vary: Accept-Encoding`。

仅当以下条件全部满足时才真正编码：选中了压缩编码；体不小于 `coding_limits::min_response_bytes`（默认 1024）；媒体类型不是已压缩类型（`image/*`、`video/*`、`audio/*`、`application/zip|gzip|zstd|br|octet-stream|...`）；状态不是 `204/304/206`；未自带 `Content-Encoding`；且编码后的结果**严格小于** identity 体。否则保持 identity 且**不发送** `Content-Encoding`（禁止静默膨胀）。`Content-Length` 反映线上（编码后）长度；`HEAD` 与 `GET` 采用同一决策，发送相同响应头但不发送体。

### 21.3 编解码器

编码器输出定长 Huffman deflate 块，内含最小 LZ77 匹配器（3 字节哈希链、32 KiB 环形历史、贪心匹配最长 258 字节；匹配可跨 `write` 边界，且每次 write 的全部字节在下次 write 前已写入历史环），并封装为 gzip 或 zlib 容器。哈希链位置为 32 位，流式输入超过 2 GiB 后退化为字面量（不会损坏数据）。最大匹配距离为 32767 字节：距离恰为 32768 时会与当前位置刚写入的环形槽位重合，因此永不选用。其输出可被任何合规解码器解压；解码器以 zlib 向量独立验证，编码器以 Python `zlib`/`gzip` 独立验证。

### 21.4 HTTP/2

本切片中 HTTP/2 不压缩响应：请求携带需要解码的 `Content-Encoding` 返回 `501`。普通 h2 客户端即使提供 `gzip`/`deflate`，也返回 identity `200`（不压缩）并携带 `Vary: Accept-Encoding`；仅当拒绝 identity（`identity;q=0` 且无可用编码）时返回 `406`，且该响应携带 `Vary`。

## 22. HTTP 文件下载与 Range（已实现子集） <a id="s22"></a>

本节对服务器门面的 `download_route`（HTTP/1.1、进程内）具有规范效力。`download_source` 与 Range 解析位于 `include/crpc/http/file_transfer.hpp` / `src/http/file_transfer.cpp`；会话级流式接线位于 `src/http/server.cpp`。

### 22.1 路由与路径安全

路由注册请求路径前缀 `path` 与文件系统 `root`。剩余部分先做百分号解码，再在 `root` 下解析。空路径、绝对路径、反斜杠、`.`/`..` 段、目录目标、缺失/不可读文件，以及规范化（解析符号链接）后不在规范化 `root` 之下的结果，均返回 `404`。仅服务 `GET` 与 `HEAD`；其他方法返回 `405` 并携带 `Allow: GET, HEAD`。`HEAD` 的错误响应（404/405/416）与 `GET` 同头但绝无体，保证 keep-alive 帧同步。路由的 `content_type`（默认及空串回退均为 `application/octet-stream`）作为 `Content-Type` 发送，且不影响路径安全规则。stat/open 之间的 TOCTOU 窗口仍是已记录限制，本切片未加固。

### 22.2 Range 语义

`Range` 作用于文件实体：

| 头值 | 结果 |
|---|---|
| 缺失 / 未知单位 | `200`，完整实体 |
| `bytes=a-b` 且 `b >= size` | `206`，`b` clamp 为 `size-1` |
| `bytes=a-` | `206` |
| `bytes=-n`（`n > 0`） | `206` 后缀区间 |
| `bytes=a-b` 且 `a >= size` | `416` + `Content-Range: bytes */size` |
| `bytes=-0` | `416` |
| 缺 `=`、缺 `-`、数字不可解析、`last < first` | 忽略，`200` |
| 多区间（`bytes=0-1,3-4`） | 忽略，`200`（未实现 `multipart/byteranges`） |

空实体上的 Range 返回 `416`（`bytes */0`）。`206` 携带 `Content-Range` 与 `Accept-Ranges: bytes`；`200` 携带 `Accept-Ranges: bytes`，且启用响应编码时携带 `Vary: Accept-Encoding`（下载路径本身永不参与内容编码）。`416` 体为空且 `Content-Length: 0`。

本切片不产生验证器（`ETag` / `Last-Modified`），因此 `If-Range` 被忽略：存在该头时忽略 `Range` 并返回完整实体（`200`），符合 RFC 9110 13.1.5 在无验证器时的要求。

### 22.3 流式与存活

响应头为手写（`206` 的长度为所选区间长度），随后按 `download_chunk_bytes`（默认 64 KiB）分块读取，并以已知 `Content-Length` 的普通字节写入（非 chunked）。输出由写完成驱动，并受 `egress_cap_bytes` 限制，慢对端会对文件读取形成背压而不是增长内存。下载保留连接的 idle 截止时间，并使用独立的 `download_stall_timeout`（默认 30 秒）。stall 死线在每次进展事件（入队一个分块或一次写完成）时刷新，队列空时清零，因此持续缓慢排空的对端不会被误杀；只有整个超时内毫无进展才关闭。该计时器与 SSE 的 stall 截止时间相互独立；与 SSE 不同，下载保留 `idle_timeout`。`HEAD` 发送相同响应头但不发送体（含 `HEAD` + `Range` → `206` 头，以及 HEAD 404/405/416 错误）。keep-alive 下载完成后，连接可继续服务下一个请求。

### 22.4 HTTP/2

HTTP/2 下命中下载路由的请求返回完整 `501`（fail-closed），不进行文件流式传输。

## 23. HTTP/1.1 长轮询（已实现子集） <a id="s23"></a>

本节对已实现的长轮询子集具有规范效力：HTTP/1.1、单分片、进程内。路由通过 `http_server::builder::add_longpoll_route()` 注册；会话状态机位于 `src/http/server.cpp`。长轮询要求 `http_server_options::shards == 1`；多分片服务器上的请求返回 `503`。长轮询、SSE 与下载三组路径互斥（`start()` 拒绝冲突）。SSE 行为不变。

### 23.1 请求与订阅标识

`GET <longpoll-path>`。`HEAD` 及其他方法返回 `405`；HTTP/1.0 返回 `400`。查询参数覆盖已注册路由：`topic`（必须非空）、`mode`（`reliable` 或 `unreliable`）、`ordered`（`0`/`1` 或 `false`/`true`）、`subscriber` 与 `timeout`。`timeout` 以毫秒计，默认 `longpoll_options::timeout`（25 秒），并 clamp 到 `[0, longpoll_options::max_timeout]`（30 秒）；`0` 表示只做一次非阻塞取消息，队列为空时立即 `204`。

游标为 `Last-Event-ID: <epoch>:<seq>` 或 `?cursor=<epoch>:<seq>`。`Last-Event-ID` 优先；两者同时出现且不同则 `400`。解析复用严格的 SSE 解析器。长轮询 subscriber id 位于 `[2^32, 2^33)`（SSE 从 1 分配）；客户端自带 id 越界返回 `400`。缺省时由服务端分配。每个 `200`/`204` 都携带 `X-CRPC-Subscriber`，客户端必须持久化。订阅因溢出/TTL 丢失后重建会返回新的服务端 id，客户端据此重置去重状态；该命名空间只是避免冲突的约定，不是安全边界。

### 23.2 响应

| 情况 | 状态 | Body | 关键头 |
|---|---|---|---|
| 有事件 | `200` | 恰好一条消息原始字节 | `Content-Type`（默认 `application/octet-stream`）、`Content-Length`、`Cache-Control: no-store, no-cache`、`X-CRPC-Event-ID: <epoch>:<seq>`、`X-CRPC-Subscriber`、`Connection: keep-alive|close` |
| 超时 | `204` | 空 | `Cache-Control: no-store, no-cache`、`X-CRPC-Subscriber`、`Connection: keep-alive|close`；无 `Content-Length`、无 `X-CRPC-Event-ID` |
| 零长度事件 | `200` | 空（`Content-Length: 0`） | 与事件相同，含 `X-CRPC-Event-ID` |
| 参数/游标非法 | `400` | problem/plain | - |
| 未知/不属于本 shard 主题 | `404` | plain | - |
| 同 subscriber 已有活跃 transport | `409` | plain | - |
| 重放窗口缺口/重传预算耗尽 | `409` | plain（`Restart`） | - |
| 等待者上限 | `503` | plain | `Retry-After: 1` |
| HTTP/2 | `501` | plain | 完整响应，不开流 |

可靠模式的事件 id 使用每订阅 `seq`；不可靠模式使用 `topic_seq`，且没有确认。响应永不参与内容编码、不携带 `Vary`（特殊路由在响应编码之前分派）。零长度可靠事件是 `200` + `Content-Length: 0` + 事件 id，可与无 body 的 `204` 超时区分。

### 23.3 确认与 at-least-once 投递

`200` 不确认消息；下一次请求的游标即累积 ACK：服务端先把它 clamp 到该 `(subscriber, topic)` 已交付的最高序号，再调用 `hub.ack(cursor)`。过大的游标被 clamp（计入 `longpoll_ack_clamped`）；epoch 与 `longpoll_options::epoch` 不一致的游标不确认。去重键为 `(subscriber, epoch, seq)`。

正常 `200`/`204` 从不 detach：token、订阅与队列跨 poll 保留。会话结束（对端关闭、停机、stall）使用 hub 的 keep-queue `release(token)`：token 失效，但队列消息保持 `in_queue`，且不计为重传。若客户端游标落后于已交付高水位，服务端调用 `retransmit_front`，把已取出但未确认的可靠消息按 seq 升序**前插**到队头，再取一条。`retransmit_budget` 只统计真正发送过的消息。若重放窗口已被 `max_replay` 裁剪、不再包含首个未确认序号，或预算耗尽，当前 poll 返回 `409 Restart`，绝不静默跳过数据；客户端应重建订阅。`retransmit_front` 依赖 `subscribe_options::max_messages >= subscribe_options::max_replay`。长轮询 `start()` 在该不等式不成立（`max_replay > max_messages`）时以 `invalid_argument` 拒绝配置，绝不静默抬高队列的内存上界；`max_replay < max_messages` 时保持现有逻辑，把重放上限提升到队列上限。注意：条数上限与字节上限彼此独立，`start()` 既不检查也不改写 `subscribe_options::max_bytes`，因此满足 `max_messages >= max_replay` 仍可能装不下整个窗口；此时 `retransmit_front` 返回 `resource_exhausted`，当前 poll 返回 `409 Restart`，绝不静默丢弃未确认消息。按文档默认值（`max_messages = max_replay = 1024`、`max_bytes = 1 MiB`）连续 publish 3 条 600 KiB 消息后，队列只保留最新一条，交付后前插重放需重新入队三条（1.8 MiB），故首次交付后带游标缺口的 poll 实测返回 `409 Restart`。该要求只针对前插重放：尾插重放（SSE）没有此约束，`hub::subscribe` 既不拒绝也不改写调用方的边界；直接调用 `retransmit_front` 且 `max_messages < max_replay` 时返回 `resource_exhausted`（安全重启），绝不静默跳过。

### 23.4 等待、存活与上限

取消息与等待注册在同一把 hub 锁内完成，`publish` 也持同一把锁，因此消息不会落在“队列为空”与“等待已注册”之间。等待期间连接的 idle 与 request 截止时间被禁用，改用独立的 poll 截止时间。`publish` 唤醒只投递 shard 任务；shard 取一条后立即响应。截止时间到点时先再取一次（shard inbox 满时唤醒可能被丢），确认仍无消息才返回 `204`，随后重新武装 idle 截止时间。连接保持 keep-alive；poll 响应入队之前不解析 HTTP/1.1 流水线字节。

同一 subscriber 至多一个活跃 transport：另一连接上的并发 poll 返回 `409`，第一个 token 不会被重定向。等待中的 poll 总数受 `longpoll_options::max_waiters` 限制，超出返回 `503` + `Retry-After: 1`。`http_server_options::max_connections` 仍是 accept 层的套接字上限。对端停止排空 poll 响应时由 `longpoll_options::slow_timeout` 关闭；已取消息保持未确认，由下一次 poll 的游标缺口重放。超过 `longpoll_options::subscriber_ttl`（5 分钟）没有 poll 的 subscriber 会被 `unsubscribe_all`，并清理其高水位/owner 记录；清理由 shard housekeeping 定时器执行（周期 `min(subscriber_ttl, 30s)`），不依赖后续 poll。`stop()` 会唤醒等待中的 poll 并关闭会话；keep-queue release 在 hub 销毁之前完成。

### 23.5 与 SSE（第 20 节）的差异

| 维度 | SSE | 长轮询 |
|---|---|---|
| 分帧 | chunked `text/event-stream` 事件 | 每个 `200` 一条带 `Content-Length` 的原始 body |
| 超时 | 注释心跳；无 204 | `204`，无 body |
| ACK | 重连 `Last-Event-ID` | 下一次 poll 游标（累积） |
| 连接归属 | 独占流，禁用 idle | keep-alive，每次 poll 后重新武装 idle |
| 并发 | 重连会重定向 token | 同 subscriber 活跃 transport → `409` |
| 资源清理 | 现有子集无 TTL（D5） | TTL + `unsubscribe_all` |

## 24. CRPC 帧压缩（已实现子集） <a id="s24"></a>

帧级压缩独立于 HTTP 内容编码（第 21 节）。它作用于单帧 payload，绝不跨帧，且只用于承载应用数据的帧类型。

### 24.1 容器

置位 `COMPRESSED` 的帧承载：

| 偏移 | 宽度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | `uncompressed_len` | `u32` 小端，`1..max_frame`；逻辑 payload 长度 |
| 4 | N | `codec_stream` | `lz4` block 或单个 `gzip` member |

`payload_len = 4 + N <= max_frame`。置位 `CRC32C` 时，校验和覆盖**含前缀**的整个 payload，
接收方在消耗解压 CPU 之前先验证完整性。payload 短于 5 字节、`uncompressed_len` 为零、或
`uncompressed_len > max_frame` 都是 `protocol_error`。

4 字节前缀与参考 LZ4 block API 在 `store_size=True` 下的长度前缀约定逐字节一致，
因此第三方实现可以把 `payload + 4` 与声明长度直接交给 `LZ4_decompress_safe`。

### 24.2 算法

| `ALGO` | 编解码 | 状态 |
|---|---|---|
| 0 | none | 必需 |
| 1 | LZ4 **block** 格式（无 frame magic、无 frame 校验和；容器已提供两者） | 已实现 |
| 2 | zstd | 保留，**未实现**：一律 `protocol_error` |
| 3 | gzip member（RFC 1952：deflate + CRC32 + ISIZE） | 留待下一阶段；本构建一律 `protocol_error` |
| 4-7 | 保留 | `protocol_error` |

`ALGO = 1` 是 LZ4 block 格式，不是 LZ4 frame 格式。`tests/test_frame_compression.cpp` 的
golden 向量钉死字节布局，供第三方实现对照；它们由参考实现 LZ4 v1.10.0 产出，
与本实现在相同输入下的输出逐字节相同。

压缩仅定义于承载应用数据的帧：`REQUEST`、`RESPONSE`、`DATA`、`ONE_WAY`、`PUSH`。
控制帧（`RST_STREAM`、`SETTINGS`、`SETTINGS_ACK`、`PING`、`PONG`、`GOAWAY`、`WINDOW_UPDATE`、
`PUSH_ACK`）带 `COMPRESSED` 是 `protocol_error`：这些 payload 是定长控制字段，压缩只会膨胀，
且 `PUSH_ACK` 还会被服务端做尺寸校验。`decode_frame_header` 仍接受这样的帧头（只校验标志一致性），
拒绝发生在**三个**整帧入口：`decode_frame`、`decode_frame_compressed` 与
`frame_decoder::next`。该闸门在编解码器运行之前生效，因此携带完全合法 LZ4 码流的
控制帧仍被拒绝；在 `frame_decoder::next` 上它是**锁存**的 `protocol_error`（绝不是
`frame_dropped`），`PUSH_ACK` 也一样，因此对端无法把违规降级为静默丢帧。

### 24.3 fail-closed 规则

| 条件 | 结果 |
|---|---|
| 不支持的 `ALGO`（zstd、本构建的 gzip、保留值） | `protocol_error` |
| `payload_len < 5`、`uncompressed_len == 0` | `protocol_error` |
| `uncompressed_len > max_frame` | `protocol_error` |
| 码流畸形或截断 | `protocol_error`（可靠）/ 显式丢弃（不可靠 `ONE_WAY`） |
| 解出字节数与 `uncompressed_len` 不符 | `protocol_error`（可靠）/ 显式丢弃（不可靠 `ONE_WAY`） |
| `CRC32C` 不符 | `protocol_error`（可靠）/ 显式丢弃（不可靠 `ONE_WAY`） |
| `uncompressed_len` 超过 `min(max_frame, limits.max_payload, payload 缓冲)` | `resource_exhausted`（解压前） |
| 控制帧类型带 `COMPRESSED` | `protocol_error` |
| 压缩/明文比值超过 `max_compression_ratio`（默认 300:1） | `resource_exhausted` |
| `COMPRESSED` 帧到达借用模式解码器（无输出缓冲） | `protocol_error` |

比值那一行只是 CPU 守卫，不是内存上界。内存上界是声明长度：解码器在解压前检查 uncompressed_len <= payload_out.size()，随后把 payload_out.first(uncompressed_len) 交给编解码器，因此每个字面量与匹配在写出前都要对照声明长度检查，且码流必须恰好产出这么多字节。所以「4 MiB 码流 + 100 字节声明」会失败且绝不写到第 100 字节之后，即使调用方缓冲是 16 MiB。

可靠帧把每一行都视为连接级致命错误，与第 13 节其它行一致。`ONE_WAY` 帧额外允许上表中标注的
**帧内**失败走「显式丢弃」：该帧被丢弃、连接继续、丢弃被计数。判据是帧**类型**，不是
`UNRELIABLE` 标志：对端可以在 `REQUEST` 上置该标志，绝不能让它把致命协议违规降级为静默丢帧。
算法与上限类错误在所有帧类型上都保持致命：它们是连接级威胁，不是单帧损坏。

解码器绝不把压缩字节当作 payload 返回：要么交付解压后的 payload（清 `COMPRESSED`/`ALGO`，
并把 `payload_len`、以及存在时的 `CRC32C` 改写为逻辑 payload 的对应值），要么丢弃该帧
（仅 `ONE_WAY`，且显式计数），要么失败。

### 24.4 发送策略

仅当对端宣告了该算法（§24.5）、payload 至少 `max(local.compress_min, peer.compress_min)` 字节
（默认 512）、且编码后**含 4 字节前缀**严格小于原始 payload 时才压缩；否则发送未压缩帧并保持
`COMPRESSED` 清零：发送方绝不放大帧，因此启用压缩不会破坏任何帧大小预算。

算法是传给 `encode_frame_compressed` 的运行时选择（`none` 表示要求不压缩）。不支持的取值（zstd、本构建的 gzip、保留 id）返回 `invalid_argument`，绝不静默回退到别的算法：「配置了 gzip 却发 lz4」正是本节要消除的标志与载荷不符。

`encode_frame_compressed` 在写出任何字节之前，按与 `encode_frame` 完全相同的规则校验帧头
（已知类型、stream 作用域、`stream_id` bit 31 为 0、`ONE_WAY` 需 `UNRELIABLE`、非零 `ext` 需 `CRC32C`、
`payload_len <= max_frame`），压缩分支与 identity 分支都一样。违规时返回 `invalid_argument` 且不写入任何字节，
因此该入口永远不会产出本实现自己的 `decode_frame_header` 会拒绝的帧。调用方提供的 `ext` 会被
校验，而不是在缺少 `CRC32C` 时被静默归一为 0。

### 24.5 能力交换

压缩用连接级 `SETTINGS` 帧（`0x05`，`stream_id = 0`）协商；该帧类型早于本节存在，此前未被解析：

| 偏移 | 宽度 | 字段 |
|---|---|---|
| 0 | 1 | `version` = 1 |
| 1 | 1 | `flags` = 0（保留） |
| 2 | 2 | `reserved` = 0 |
| 4 | 4 | `compress_bitmap`：bit0 lz4、bit1 zstd、bit2 gzip；bits 3-31 必须为 0 |
| 8 | 4 | `compress_min`：小于该值的 payload 不压缩；0 = 512 |
| 12 | 4 | `max_decompressed_frame`：可接受的解压后单帧上限；0 = 16 MiB |

双方在传输建立后、任何请求之前各发一次 `SETTINGS`。收到对端 `SETTINGS` 之前一律不压缩。
生效发送集是双方位图的交集。`SETTINGS` 是全量快照：后到的覆盖先到的，只影响后续帧，无需确认。
本切片尚未实现 `SETTINGS` payload 解析；当前双方都会忽略该帧，这正是「对端宣告前不压缩」
这一安全默认值成立的原因。

接收侧接受任何它**能解**的算法，无论是否宣告过；只有不可解的算法才是协议错误。这让混合部署可用：
早于本节的旧端忽略 `SETTINGS`、从不宣告，因此永远不会收到压缩帧。

### 24.6 与其它标志的交互

- `ORDERED` 正交：lane 扩展位于 payload 之外，永不压缩。
- `END_STREAM` 标记最后一个分片；每个分片独立压缩，分片之间可以混合压缩与未压缩。
- `UNRELIABLE` 帧可以压缩。帧内解压失败会显式丢弃该帧并保持连接；丢弃以
  `rpc_code::decode_error` 报告（经逐帧报告通道，而不是解码器锁存的错误状态）。
  压缩的不可靠帧建议带 `CRC32C`，让位翻转在解压前被拦下。
- 压缩逐帧独立：没有跨帧字典或历史，因此丢帧或乱序绝不会污染后续帧。

### 24.7 验证

`tests/test_frame_compression.cpp` 覆盖容器、编解码、fail-closed 表、不可靠丢弃路径、
比值与大小上限、永不放大策略、golden 向量与变异 fuzz 循环。LZ4 block 编解码在 golden 向量上
另与参考实现做了逐字节一致性对拍。
