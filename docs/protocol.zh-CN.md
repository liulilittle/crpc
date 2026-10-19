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

`COMPRESSED = 0` 要求 `ALGO = 0`；`COMPRESSED = 1` 要求已知且非零的 `ALGO`；大于 3 的值被拒绝；`ONE_WAY` 要求 `UNRELIABLE`。标志已定义，但代码树没有 lz4/zstd/gzip 编解码器，因此压缩属于**已设计，未实现**。

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

本节对已实现的单分片、进程内子集具有规范效力。PUSH 与 REQUEST/RESPONSE 走同一条连接。跨分片路由、ordered/gap 处理与 `stream_transport` 集成不属于本子集；HTTP SSE 见第 20 节。

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

本节对已实现的 SSE 子集具有规范效力：HTTP/1.1、单分片、进程内。长轮询、HTTP/2 SSE、跨分片发布、鉴权与 `content_coding` 不属于本子集。分帧辅助位于 `include/crpc/http/sse.hpp` / `src/http/sse.cpp`；服务端接线位于 `src/http/server.cpp`。

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

### 21.2 响应压缩

启用 `enable_response_coding` 时，响应体按 `Accept-Encoding`（q 值；gzip 优先于 deflate，再优先 identity）协商。即使最终不压缩，也会进行协商，并携带 `Vary: Accept-Encoding`。若客户端拒绝所有支持的编码（`identity;q=0` 且 gzip/deflate 不可接受），返回 `406`。

仅当以下条件全部满足时才真正编码：选中了压缩编码；体不小于 `coding_limits::min_response_bytes`（默认 1024）；媒体类型不是已压缩类型（`image/*`、`video/*`、`audio/*`、`application/zip|gzip|zstd|br|octet-stream|...`）；状态不是 `204/304/206`；未自带 `Content-Encoding`；且编码后的结果**严格小于** identity 体。否则保持 identity 且**不发送** `Content-Encoding`（禁止静默膨胀）。`Content-Length` 反映线上（编码后）长度；`HEAD` 与 `GET` 采用同一决策，发送相同响应头但不发送体。

### 21.3 编解码器

编码器输出定长 Huffman deflate 块，内含最小 LZ77 匹配器（3 字节哈希链、32 KiB 环形历史、贪心匹配最长 258 字节；匹配可跨 `write` 边界），并封装为 gzip 或 zlib 容器。其输出可被任何合规解码器解压；解码器以 zlib 向量独立验证，编码器以 Python `zlib`/`gzip` 独立验证。

### 21.4 HTTP/2

本切片中 HTTP/2 不使用这些编码：请求携带需要解码的 `Content-Encoding` 返回 `501`；响应协商会选中 gzip/deflate 时返回 `501`（fail-closed，禁止静默降级为 identity）。`Accept-Encoding` 选中 identity（且未拒绝）时正常响应；拒绝 identity 返回 `406`。

## 22. HTTP 文件下载与 Range（已实现子集） <a id="s22"></a>

本节对服务器门面的 `download_route`（HTTP/1.1、进程内）具有规范效力。`download_source` 与 Range 解析位于 `include/crpc/http/file_transfer.hpp` / `src/http/file_transfer.cpp`；会话级流式接线位于 `src/http/server.cpp`。

### 22.1 路由与路径安全

路由注册请求路径前缀 `path` 与文件系统 `root`。剩余部分先做百分号解码，再在 `root` 下解析。空路径、绝对路径、反斜杠、`.`/`..` 段、目录目标、缺失/不可读文件，以及规范化（解析符号链接）后不在规范化 `root` 之下的结果，均返回 `404`。仅服务 `GET` 与 `HEAD`；其他方法返回 `405` 并携带 `Allow: GET, HEAD`。

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

响应头为手写（`206` 的长度为所选区间长度），随后按 `download_chunk_bytes`（默认 64 KiB）分块读取，并以已知 `Content-Length` 的普通字节写入（非 chunked）。输出由写完成驱动，并受 `egress_cap_bytes` 限制，慢对端会对文件读取形成背压而不是增长内存。下载保留连接的 idle 截止时间（由写进展刷新），并使用独立的 `output_stall_timeout`（默认 30 秒）：暂存输出在该时间内未排空即关闭连接。该计时器与 SSE 的 stall 截止时间相互独立；与 SSE 不同，下载保留 `idle_timeout`。`HEAD` 发送相同响应头但不发送体（含 `HEAD` + `Range` → `206` 头）。keep-alive 下载完成后，连接可继续服务下一个请求。

### 22.4 HTTP/2

HTTP/2 下命中下载路由的请求返回完整 `501`（fail-closed），不进行文件流式传输。
