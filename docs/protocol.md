# CRPC wire protocol

[简体中文](protocol.zh-CN.md) | **English**

Version 1.0 / 2026-10-19

This document specifies the CRPC binary stream carried by TCP and by the in-process backend. The frame layout is derived from `include/crpc/frame.hpp` and `src/frame/frame.cpp`; the call envelope and response layout from the same header and from `src/rpc/client.cpp` / `src/rpc/dispatcher.cpp`. The HTTP/2 mapping is derived from `http2/*`. Where the design and the code differ, both are stated and the code is identified as current behaviour.

## Contents

| # | Section |
|---|---|
| [1](#s1) | Scope and authority |
| [2](#s2) | Byte-order and alignment |
| [3](#s3) | Fixed frame header |
| [4](#s4) | Frame types |
| [5](#s5) | Frame flags |
| [6](#s6) | Ordered extension |
| [7](#s7) | Payload length and stream scope |
| [8](#s8) | Call envelope |
| [9](#s9) | Trace, deadline and idempotency |
| [10](#s10) | Response and error payload |
| [11](#s11) | Varint and zigzag |
| [12](#s12) | Length limits |
| [13](#s13) | Fail-closed rules |
| [14](#s14) | Golden bytes |
| [15](#s15) | Incremental decoding |
| [16](#s16) | Session handshake |
| [17](#s17) | HTTP/2 mapping |
| [18](#s18) | Versioning and verification |
| [19](#s19) | Push delivery (implemented subset) |
| [20](#s20) | HTTP/1.1 Server-Sent Events (implemented subset) |
| [21](#s21) | HTTP content coding (implemented subset) |
| [22](#s22) | HTTP file download and Range (implemented subset) |

## 1. Scope and authority <a id="s1"></a>

The CRPC binary protocol is a length-prefixed, multiplexed frame stream. A frame is a 16-byte fixed header, an optional 16-byte ordered extension, and a payload. The frame layer knows nothing about RPC methods; `REQUEST`, `RESPONSE` and `ONE_WAY` payloads carry a call envelope.

Authority: `include/crpc/frame.hpp` for field widths, numeric values, flags, limits and CRC; `src/frame/frame.cpp` for validation order and envelope encoding; `src/rpc/client.cpp` for what the client writes and accepts; `src/rpc/dispatcher.cpp` for response encoding. The design specification §7 is the target contract. One deviation is called out: the envelope uses 64-bit service/method ids and a different extension layout (§8, §9). The deadline is relative as the design requires (§9).

## 2. Byte-order and alignment <a id="s2"></a>

Every multi-byte field in a CRPC frame is **little-endian**, regardless of host. Values are read and written byte-wise through `crpc::endian` and `byte_reader`/`byte_writer`; no struct is `memcpy`-ed to the wire and no integer is loaded through an unaligned cast. There is no padding or alignment requirement: a field starts at the offset the tables give.

Unsigned integers in a payload use base-128 varints; signed integers use zigzag then varint. IEEE-754 floats travel as their bit pattern in a fixed-width little-endian integer, so NaN payloads and negative zero survive a round trip.

HTTP/2 is the exception: its header and integers are big-endian. A CRPC binary frame and an HTTP/2 frame are different objects; see §17.

## 3. Fixed frame header <a id="s3"></a>

The fixed header is exactly 16 bytes. `payload_len` counts only the payload after the fixed header and the ordered extension.

| Offset | Width | Field | Type | Endianness | Semantics |
|---|---|---|---|---|---|
| 0 | 4 | `payload_len` | `u32` | LE | bytes after the fixed header and ordered extension; `<= max_frame` |
| 4 | 1 | `type` | `u8` | — | one of §4; unknown is rejected |
| 5 | 1 | `flags` | `u8` | — | bits of §5 |
| 6 | 2 | `reserved` | `u16` | LE | must be zero; non-zero is rejected |
| 8 | 4 | `stream_id` | `u32` | LE | bit 31 must be zero; 0 is connection-level |
| 12 | 4 | `ext` | `u32` | LE | CRC32C when `flags.CRC32C`, otherwise zero |

`frame_header` does not store `reserved`; the encoder always writes zero and the decoder rejects a non-zero value. `max_frame` defaults to 16 MiB and is capped at 64 MiB. `max_stream_id` is `0x7FFFFFFF`.

`ext` is checked both ways: non-zero without `CRC32C` is a protocol error, and a `CRC32C` frame whose computed checksum differs is a protocol error. The checksum is CRC32C (Castagnoli, reflected polynomial `0x82F63B78`), table-driven and allocation-free.

## 4. Frame types <a id="s4"></a>

Numeric values are fixed forever; a change is a wire break.

| Value | Name | Stream scope | Payload |
|---|---|---|---|
| `0x00` | `DATA` | non-zero | streaming chunk or push body fragment |
| `0x01` | `REQUEST` | non-zero | call envelope + request body |
| `0x02` | `RESPONSE` | non-zero | status byte + body or error envelope |
| `0x03` | `ONE_WAY` | non-zero | call envelope + body; `UNRELIABLE` required |
| `0x04` | `RST_STREAM` | non-zero | `rpc_code` as `u32` LE |
| `0x05` | `SETTINGS` | zero | settings TLV (design) |
| `0x06` | `SETTINGS_ACK` | zero | empty |
| `0x07` | `PING` | zero | 8 opaque bytes |
| `0x08` | `PONG` | zero | 8 opaque bytes |
| `0x09` | `GOAWAY` | zero | `last_stream_id` `u32` + `rpc_code` `u32` |
| `0x0A` | `WINDOW_UPDATE` | zero or non-zero | `increment` `u32` |
| `0x0B` | `PUSH` | non-zero | push envelope + body (section 19) |
| `0x0C` | `PUSH_ACK` | non-zero | `push_seq` `u64` |

The frame codec validates the header and the scope of every type. It does not parse `SETTINGS`, `GOAWAY` or `WINDOW_UPDATE` payloads; those semantics are **specified, not implemented** at this layer. The client implements `REQUEST`/`ONE_WAY` writing, `RESPONSE` decoding, `PING`/`PONG` echo, `GOAWAY`, and (in the single-shard subset of section 19) `PUSH` delivery with `PUSH_ACK` for reliable messages. `PUSH_ACK` received by a client is ignored because this slice only pushes from the server.

## 5. Frame flags <a id="s5"></a>

| Bit | Mask | Name | Meaning |
|---|---|---|---|
| 0 | `0x01` | `END_STREAM` | last frame of this stream |
| 1 | `0x02` | `COMPRESSED` | payload compressed; algorithm in bits 5–7 |
| 2 | `0x04` | `CRC32C` | `ext` carries CRC32C of the payload |
| 3 | `0x08` | `ORDERED` | 16-byte lane extension follows the header |
| 4 | `0x10` | `UNRELIABLE` | best-effort delivery; no retransmission |
| 5–7 | `0xE0` | `ALGO` | 0 none, 1 lz4, 2 zstd, 3 gzip, 4–7 reserved |

`COMPRESSED = 0` requires `ALGO = 0`; `COMPRESSED = 1` requires a known non-zero `ALGO`; values above 3 are rejected; `ONE_WAY` requires `UNRELIABLE`. The flags are defined, but no lz4/zstd/gzip codec exists in the tree, so compression is **specified, not implemented**.

## 6. Ordered extension <a id="s6"></a>

When `ORDERED` is set, exactly 16 bytes follow the fixed header:

| Offset after header | Width | Field | Type | Endianness |
|---|---|---|---|---|
| 0 | 8 | `lane_id` | `u64` | LE |
| 8 | 8 | `lane_seq` | `u64` | LE |

`payload_len` does not include these bytes. If `ORDERED` is clear, no extension exists; a decoder that assumes one is non-conformant. `lane_seq` is allocated once per lane at enqueue time and is monotonic; it is never compared across lanes.

## 7. Payload length and stream scope <a id="s7"></a>

- `payload_len` is checked against `max_frame` before any allocation. Default 16 MiB, hard cap 64 MiB; a larger constructor argument is clamped.
- `stream_id` bit 31 is reserved and must be zero; `0` is connection-level.
- `REQUEST`, `RESPONSE`, `DATA`, `ONE_WAY`, `PUSH`, `RST_STREAM` and `PUSH_ACK` require a non-zero id.
- `SETTINGS`, `SETTINGS_ACK`, `PING`, `PONG` and `GOAWAY` require id 0.
- `WINDOW_UPDATE` is valid for both a stream and the connection.
- An id is never reused on a connection. A response for an unknown or terminal id is dropped, never delivered to a recycled slot.

## 8. Call envelope <a id="s8"></a>

`REQUEST` and `ONE_WAY` start their payload with the call envelope:

| Offset | Width | Field | Type | Endianness | Semantics |
|---|---|---|---|---|---|
| 0 | 8 | `service_id` | `u64` | LE | registered service number |
| 8 | 8 | `method_id` | `u64` | LE | registered method number |
| 16 | 1 | `call_flags` | `u8` | — | bit 0 `has_extensions`; bits 1–7 zero |
| 17 | 2 | `codec` | `u16` | LE | 0 binary, 1 json, 2 xml, 3 ini, 4 toml, 5 protobuf, 6 flatbuffers |
| 19 | 1 | `ext_flags` | `u8` | — | only when bit 0 is set; see §9 |
| … | … | extension fields | … | … | in the order of §9 |
| … | rest | `body` | bytes | — | application payload |

The minimum envelope is 19 bytes. In the current code a `RESPONSE` starts with the status byte of §10 and carries no envelope; the design's success-path envelope is **specified, not implemented**. The design §7.6 also shows 32-bit ids, a `remaining_ns` varint, meta TLVs and version fields; the code uses 64-bit ids and the extension block. The code is the current wire format; matching the design exactly would be a wire break requiring a version bump.

`call_flags` has exactly one defined bit; an unknown bit is a protocol error on decode and an invalid argument on encode. `codec` must be one of the seven known values.

## 9. Trace, deadline and idempotency <a id="s9"></a>

When `call_flags` bit 0 is set, an extension block follows the codec field. Its `ext_flags` controls which fields exist, in this fixed order:

| Bit | Name | Field | Width |
|---|---|---|---|
| 0 | `has_trace_id` | `trace_id` | 16 bytes |
| 1 | `has_span_id` | `span_id` | 8 bytes |
| 2 | `has_parent_span_id` | `parent_span_id` | 8 bytes |
| 3 | `has_deadline` | `deadline_ns` | `u64` LE |
| 4 | `has_idempotency_key` | `key_len` `u8` + key | 1 + 0…128 bytes |

Bits 5–7 are reserved and must be zero. `span_id` requires `trace_id`; `parent_span_id` requires `span_id`. An idempotency key is at most 128 bytes; the current client writes the 32-byte R6 triple `(client_id[16], client_epoch u64, seq u64)`. If `call_flags` bit 0 is clear, `ext_flags` must be zero and no key may be present. If a presence bit is clear, its bytes are absent; a non-empty key without the bit is rejected.

**Deadline semantics.** `deadline_ns` is `remaining_ns`: the time left at the moment the frame is encoded, never an absolute instant. Steady-clock epochs differ per process and host, so an absolute value would be meaningless on the peer (P12, design §8.5). The client computes `slot.deadline - now` through its injected monotonic clock and sends `0` when the deadline has already passed; the server reconstructs its local deadline as `steady_clock::now() + deadline_ns`. Transit time is therefore not charged to the peer, and a zero remaining value must be rejected as `deadline_exceeded` without executing the handler.

## 10. Response and error payload <a id="s10"></a>

A `RESPONSE` payload starts with a status byte: 0 means success and the remaining bytes are the response body; 1 means error and the error envelope follows. Any other value is a protocol error.

| Error field | Encoding |
|---|---|
| `code` | varint |
| `retryable` | `u8`, 0 or 1 |
| `retry_after_ms` | varint |
| `message_len` | `u16` LE |
| `message` | `message_len` bytes, UTF-8 |
| `meta_count` | `u16` LE |
| repeated entries | `key_len u16`, key, `value_len u16`, value |

The encoder clamps `message_len`, `key_len`, `value_len` and `meta_count` to `uint16_t`; the design's 512-byte message and 1 KiB meta limits are target policy, not enforced by this codec. An unknown `rpc_code` larger than `application` (21) is decoded as `internal` with the original value in `metadata["crpc.raw_code"]`. `RST_STREAM` payload is `rpc_code` as `u32` LE; the client sends it on cancellation and ignores a late response for the same stream.

## 11. Varint and zigzag <a id="s11"></a>

Varint is base-128, least-significant group first, bit 7 is continuation. A `u64` uses at most 10 bytes. The reader rejects an overlong encoding: at shift 63 the final payload group may only be 0 or 1, and an eleventh continuation byte is rejected.

```
encode: (u64(value) << 1) ^ u64(value >> 63)
decode: (u >> 1) ^ (~(u & 1) + 1)
```

All arithmetic is unsigned to avoid signed-overflow UB. Floats are bit-cast to `u32`/`u64`, then written little-endian.

## 12. Length limits <a id="s12"></a>

| Item | Limit | Enforcement |
|---|---|---|
| frame header / ordered extension | 16 / 16 bytes | fixed |
| frame payload | 16 MiB default, 64 MiB hard cap | `max_frame`, before allocation |
| stream id | `0x7FFFFFFF` | bit 31 reserved |
| idempotency key | 128 bytes | length prefix + decode check |
| call envelope | 19 bytes minimum | truncation is `protocol_error` |
| decoder buffer | `max_frame + 32`, at least 64 MiB | `frame_decoder` |
| `buffer` ceiling | 64 MiB default | `buffer::default_max_capacity` |
| crpc-binary depth | 32 | `binary_codec` |
| HTTP/1.1 line / headers / body | 8 KiB / 64 KiB + 100 fields / 8 MiB | `parser_limits` |
| multipart | 32 parts, 8 KiB part headers, 64 MiB part, 256 MiB total, 70-byte boundary | `multipart_limits` |
| h2 frame / header list | 16 KiB default (16 MiB − 1 max) / 16 KiB | `http2` settings |

Limits are inclusive and checked before allocation or pointer arithmetic.

## 13. Fail-closed rules <a id="s13"></a>

The decoders return `rpc_code::protocol_error` for every condition below. `rpc::client` treats a decoder error as fatal: it stops reading, marks itself closed and fails all in-flight calls with `protocol_error`. The design's `GOAWAY(protocol_error)` emission is **specified, not implemented** in the client.

| Condition | Result |
|---|---|
| unknown `type` | `protocol_error` |
| non-zero `reserved` | `protocol_error` |
| `stream_id` bit 31 set | `protocol_error` |
| stream scope violation (§7) | `protocol_error` |
| `payload_len > max_frame` | `protocol_error`, before allocation |
| `COMPRESSED`/`ALGO` mismatch | `protocol_error` |
| unknown compression algorithm | `protocol_error` |
| non-zero `ext` without `CRC32C` | `protocol_error` |
| CRC32C mismatch | `protocol_error` |
| decoded length differs from `payload_len` | `protocol_error` |
| envelope truncation | `protocol_error` |
| unknown `call_flags` bit | `protocol_error` |
| unknown `codec` | `protocol_error` |
| unknown `ext_flags` bit | `protocol_error` |
| span without trace, parent without span | `protocol_error` |
| idempotency key > 128 bytes | `protocol_error` |
| extension fields without `has_extensions` | `protocol_error` |
| response status other than 0/1 | `protocol_error` |
| `ONE_WAY` without `UNRELIABLE` | `protocol_error` |

A response for an unknown or terminal stream is not a protocol error; it is dropped.

## 14. Golden bytes <a id="s14"></a>

`REQUEST`, `payload_len = 16`, `stream_id = 1`, no flags:

```
10 00 00 00 01 00 00 00 01 00 00 00 00 00 00 00
```

The same request with `ORDERED`, `lane_id = 7`, `lane_seq = 9`:

```
10 00 00 00 01 08 00 00 01 00 00 00 00 00 00 00
07 00 00 00 00 00 00 00 09 00 00 00 00 00 00 00
```

`PING` with payload `01 02 03 04 05 06 07 08`:

```
08 00 00 00 07 00 00 00 00 00 00 00 00 00 00 00
01 02 03 04 05 06 07 08
```

A `REQUEST` for `service_id = 1`, `method_id = 2`, codec `binary`, no extensions, body `61 62` (`"ab"`). The envelope is 19 bytes, so the payload is 21 bytes (`0x15`):

```
15 00 00 00 01 00 00 00 01 00 00 00 00 00 00 00
01 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00
00 00 00 61 62
```

The header and `PING` vectors are asserted by `tests/test_frame.cpp`; envelope field order is asserted by that file's `call_envelope_round_trip_all_fields` case.

## 15. Incremental decoding <a id="s15"></a>

`frame_decoder` accepts arbitrarily split chunks, including one byte at a time and several frames in one chunk. `feed()` copies input into an internal buffer; the caller's span need not outlive the call. `next()` returns `need_more`, `frame_ready` or `error`. In borrowing mode a payload points into the decoder buffer and is invalidated by the next `feed()`/`next()`; in copying mode it is copied into the caller's buffer and invalidated by the next `next()`, and an oversized payload is `resource_exhausted`.

The decoder never allocates for a payload whose header has not passed validation. It compacts only when the dead prefix is at least 4 KiB and at least half the buffer, to avoid quadratic copying. `decode_frame` requires the input size to equal the encoded size exactly; trailing bytes are a length mismatch. `decode_frame_header` validates only the header and leaves CRC to the payload-aware decoders. `decode_call_envelope` and `decode_response` borrow the body (and idempotency key) from the input; the views are valid only while the input is.

## 16. Session handshake <a id="s16"></a>

**Specified, not implemented.** The design defines a 12-byte client hello (`"CRPC"`, `u16` version, `u16` flags, `u16` tlv_len, `u16` reserved) followed by TLVs for `max_frame`, `initial_window`, `codec`, `compress_bitmap`, `compress_min`, `keepalive_ms`, `max_streams`, `identity`, `resume_token` and `hmac`. A server hello has the same prefix with a status byte. The limit is 4 KiB, the timeout 3 s, and unknown TLV/codec/capability, non-zero reserved or a version mismatch is fail-closed.

`protocol_version` is `1`, but no handshake is written or parsed by the current code. The frame header carries no version field, so version negotiation does not exist yet; peers must be built from compatible sources.

## 17. HTTP/2 mapping <a id="s17"></a>

HTTP/2 is a separate protocol layer. Its fixed header is 9 bytes and big-endian:

| Offset | Width | Field | Semantics |
|---|---|---|---|
| 0 | 3 | `length` | payload length, 24-bit big-endian |
| 3 | 1 | `type` | frame type |
| 4 | 1 | `flags` | type-specific flags |
| 5 | 4 | `R` + `stream_id` | 1 reserved bit + 31-bit id, big-endian |

Implemented types are `DATA` (0x0), `HEADERS` (0x1), `PRIORITY` (0x2), `RST_STREAM` (0x3), `SETTINGS` (0x4), `PUSH_PROMISE` (0x5), `PING` (0x6), `GOAWAY` (0x7), `WINDOW_UPDATE` (0x8) and `CONTINUATION` (0x9). HPACK implements the 61-entry static table, the dynamic table with size updates, Huffman coding and the integer/string primitives. The connection state machine implements settings/ACK, stream states, CONTINUATION sequencing, connection and stream windows, parked sends, GOAWAY and RST_STREAM. Defaults: 16 KiB frame, 100 streams, 16 KiB header list, 65535 initial window.

The intended CRPC mapping (design §12.4):

| CRPC concept | HTTP/2 representation |
|---|---|
| request | `HEADERS` with `:method POST`, `:path`, `content-type: application/crpc`, then `DATA` carrying the CRPC `REQUEST` payload |
| response | `HEADERS` with `:status`, then `DATA` carrying the CRPC `RESPONSE` payload |
| cancel | `RST_STREAM(CANCEL)`; the adapter claims the call slot `canceled` |
| shutdown | `GOAWAY`; existing streams drain or are cancelled |
| trace/timeout/idempotency/lane | `x-crpc-trace-id`, `x-crpc-timeout-ms`, `x-crpc-idempotency-key`, `x-crpc-attempt`, `x-crpc-lane-id`, `x-crpc-lane-seq` |

The two stream-id spaces are independent; the adapter must keep an `h2_stream_id -> call_slot*` map. The h2 and CRPC flow-control layers are also independent and not shared.

The HTTP server facade (`http/server.hpp`) implements the HTTP/1.1, h2c and h2 transport itself: a cleartext connection whose first bytes match the HTTP/2 connection preface (`PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`) is served as HTTP/2, any other cleartext connection as HTTP/1.1, and a TLS connection is routed by the ALPN result (`h2` or `http/1.1`). Requests are dispatched through the MVC router rather than the CRPC call-slot adapter, so the `h2_stream_id -> call_slot*` map above belongs to the CRPC-on-h2 mapping, which remains **specified, not implemented**. The HTTP server uses the same frame, HPACK and connection layers and the same flow-control rules described in this section.

## 18. Versioning and verification <a id="s18"></a>

The wire protocol has its own version, `protocol_version = 1`, independent of the library version. Frame types, flags, codec ids and `rpc_code` values are fixed forever; new values are appended, and an unknown value is rejected rather than mapped to a default. A wire change requires a protocol version bump and a migration note; the ABI policy is separate ([ABI policy](abi.md)).

Verification is in `tests/test_frame.cpp` (header and ordered/ping golden bytes, CRC32C, fail-closed rules, envelope round trip), `tests/test_wire.cpp` (varint/zigzag boundaries), `tests/test_http2.cpp` (h2 framing, HPACK RFC vectors, stream state, flow control) and `tests/test_rpc_core.cpp` (request, response, cancel and timeout). A reader that cannot decode a field exactly as specified must reject the frame; there is no "best effort" decoding path.

## 19. Push delivery (implemented subset) <a id="s19"></a>

This section is normative for the implemented single-shard, in-process subset.
PUSH travels on the same connection as REQUEST/RESPONSE. Cross-shard routing,
ordered/gap handling and `stream_transport` integration are outside this subset;
HTTP SSE has its own subset in section 20.

### 19.1 PUSH envelope

`PUSH` (type `0x0B`) requires a non-zero `stream_id`; one push stream is
allocated per subscription. The payload is:

| Offset | Width | Field | Type | Semantics |
|---|---|---|---|---|
| 0 | 1 | `version` | `u8` | must be `1` |
| 1 | 1 | `mode` | `u8` | 0 reliable, 1 unreliable |
| 2 | 2 | `topic_id` | `u16` | server-assigned, non-zero |
| 4 | 8 | `publisher_id` | `u64` | publisher identity |
| 12 | 8 | `epoch` | `u64` | publisher epoch |
| 20 | 8 | `seq` | `u64` | per-subscription reliable sequence; 0 when unreliable |
| 28 | 8 | `topic_seq` | `u64` | per-topic monotonic sequence |
| 36 | 4 | `body_len` | `u32` | must equal the remaining payload length |
| 40 | `body_len` | `body` | bytes | application payload |

An unknown version, an unknown mode, `topic_id = 0`, a reliable envelope with
`seq = 0`, a truncated fixed field, or a `body_len` that differs from the
remaining bytes is a protocol error and is rejected before delivery.

### 19.2 Subscription control and stream mapping

Push control uses the ordinary REQUEST path, not a new frame type, with the
reserved `service_id = 0xFFFFFFFFFFFFFFFF` and `method_id = 1` (subscribe):

```
request:  u64 subscriber_id, u8 mode, u8 ordered, u16 topic_len, topic bytes
reply:    u32 stream_id, u16 topic_id, u8 mode, u8 ordered, u16 topic_len, topic
```

The reply is the `stream_id`/`topic_id` mapping. Every subscription owns one
non-zero push `stream_id`, which is what lets `PUSH_ACK` stay exactly as frozen
in section 4: type `0x0C`, the subscription's `stream_id`, payload `push_seq`
`u64` (cumulative). The server maps `stream_id` to `(subscriber_id, topic, mode,
ordered)` and applies the acknowledgement with `hub.ack(subscriber_id, topic,
seq)`.

### 19.3 Client behaviour

On a well-formed PUSH the client invokes the registered push sink. For reliable
messages it keeps the highest accepted `seq` per push stream and drops a frame
whose `seq` is not greater than it (at-least-once replay becomes at-most-once
delivery at the sink). A reliable frame is acknowledged after the sink returns;
an unreliable frame (`seq = 0`) is not acknowledged. A malformed envelope is a
protocol failure.

### 19.4 Server pump and egress bound

Each connection stages PUSH frames in the existing send queue. The staged PUSH
bytes (in-flight plus queued) are bounded by
`server::options::push_egress_max_bytes` (default 1 MiB), independently of the
response in-flight bound. At the bound the pump stops calling `try_take`; a
message already taken is parked on the connection rather than dropped. The
hub's per-subscription `max_messages` / `max_bytes` and `overflow_policy`
therefore make every drop decision, and `publish()` never blocks on a slow
subscriber.

Closing a connection detaches it from the hub (`hub.detach`) before the
connection and its push token are destroyed, so a late `PUSH_ACK` cannot target
a dead connection. Subscriptions survive detach so a reconnect with the same
`subscriber_id` resumes with the unacknowledged reliable messages replayed.

`hub::tick` slow-subscriber detection is not wired in this subset: the
`slow_subscriber` event is not emitted, and memory is bounded by the hub's
per-subscription queues and their overflow policy instead.

## 20. HTTP/1.1 Server-Sent Events (implemented subset) <a id="s20"></a>

This section is normative for the implemented SSE subset: HTTP/1.1, a single
shard, in process. Long polling, HTTP/2 SSE, cross-shard publish, authentication
and `content_coding` are outside this subset. The framing helpers live in
`include/crpc/http/sse.hpp` / `src/http/sse.cpp`; the server wiring lives in
`src/http/server.cpp`.

### 20.1 Request

`GET <sse-path>`; `Accept: text/event-stream` is optional. Query parameters
override the registered route: `topic` (must be non-empty), `mode`
(`reliable` or `unreliable`), `ordered` (`0`/`1` or `false`/`true`) and
`subscriber` (a non-zero u64; a fresh id is allocated when absent). An optional
`Last-Event-ID: <epoch>:<seq>` resumes a subscription. Routes are registered
with `http_server::builder::add_sse_route()`. SSE requires
`http_server_options::shards == 1`; `start()` fails with `failed_precondition`
otherwise.

### 20.2 Response and events

The response is `200` with `Content-Type: text/event-stream; charset=utf-8`,
`Cache-Control: no-cache`, `Connection: keep-alive`, `X-Accel-Buffering: no` and
`Transfer-Encoding: chunked` (there is no `Content-Length`; the response uses a
hand-written head because `http::serialize` always appends the terminating
chunk). Event frames are `id: <epoch>:<seq>\ndata: <payload>\n\n`. A payload
that is not valid UTF-8 text is base64 encoded and marked `data: b64:<...>`;
only binary events carry the `b64:` prefix. A comment-only heartbeat `:hb\n\n`
is emitted every `sse_options::heartbeat` (default 15 s; zero disables). A
configured `retry:` hint is sent once at the start.

### 20.3 Delivery, backpressure and close

An open SSE response owns its connection: HTTP/1.1 pipelining is not used and
the idle deadline is disabled. Liveness comes from the heartbeat write (a failed
write closes the session) and from `sse_options::slow_timeout`, which closes a
peer that stops draining while staged output is non-empty. Staged output (the
write queue plus the in-flight write) is bounded by
`sse_options::egress_cap_bytes` (default
`http_server_options::egress_cap_bytes`, 1 MiB). At the bound the pump stops
calling `try_take` and parks the already-taken message instead of dropping it;
the hub's `subscribe_options` (`max_messages`, `max_bytes`, `overflow_policy`)
makes every drop decision, and `http_server::publish()` never blocks. Closing
detaches from the hub before the transport and its token are destroyed; the
terminating chunk is bound to the close, not emitted between events.

### 20.4 Reconnect and replay

A reconnect with the same `subscriber` reuses the existing subscription: the
server never re-subscribes. It clamps the `Last-Event-ID` sequence to the
highest sequence it has delivered for that (subscriber, topic), then calls
`hub.ack(cursor)` (cumulative) and `hub.attach()`, which replays the
unacknowledged reliable messages. A cursor whose epoch differs from the current
server epoch is not acknowledged. The event id therefore carries both epoch and
sequence, and a client must de-duplicate on the `(epoch, seq)` pair.

### 20.5 HTTP/2

SSE is not implemented over HTTP/2 in this subset. A request whose path matches
an SSE route is answered with a complete `501` response; no stream is opened.

## 21. HTTP content coding (implemented subset) <a id="s21"></a>

This section is normative for the content-coding wiring in the server facade
(HTTP/1.1, in process). The primitives live in
`include/crpc/http/content_coding.hpp` / `src/http/content_coding.cpp`; the
server wiring lives in `src/http/server.cpp`. SSE responses (section 20),
HTTP/2 responses and file downloads are never content-coded.

### 21.1 Request decoding

A request with `Content-Encoding: gzip` or `deflate` (exactly one layer) has its
body decoded before routing. The decoded body replaces the wire body and
`Content-Encoding` is removed; a `Content-Length` is rewritten to the decoded
size. An unsupported or layered coding is `415`; a malformed or truncated
stream is `400`; exceeding `http_server_options::max_decoded_body` (default
8 MiB) is `413` and closes the connection. `max_decoded_ratio` caps the
decoded/encoded ratio (0, the default, disables the check: a legitimate
request may be highly compressible). Request decoding is controlled by
`enable_request_decoding`.

### 21.2 Response compression

When `enable_response_coding` is set, the response body is negotiated from
`Accept-Encoding` (q-values; gzip preferred over deflate over identity).
Negotiation happens even for a body that is not compressed, and such a response
carries `Vary: Accept-Encoding`. A client that refuses every supported coding
(`identity;q=0` with no acceptable gzip/deflate) is answered `406`.

The body is actually encoded only when a coding was selected, the body is at
least `coding_limits::min_response_bytes` (default 1024), the media type is not
already compressed (`image/*`, `video/*`, `audio/*`, `application/zip|gzip|zstd|
br|octet-stream|...`), the response is not `204/304/206`, does not already carry
`Content-Encoding`, and the encoded form is **strictly smaller** than the
identity body. Otherwise the response stays identity and **no**
`Content-Encoding` header is sent (never inflate). `Content-Length` reflects the
on-the-wire (encoded) length; `HEAD` runs the same decision as `GET` and sends
the same headers with no body.

### 21.3 Codec

The encoder emits fixed-Huffman deflate blocks with a minimal LZ77 matcher
(3-byte hash chains, a 32 KiB history ring, greedy matching up to 258 bytes;
matches may cross `write` boundaries) wrapped in a gzip or zlib container. Its
output is valid for any conforming decoder; the decoder is independently
verified against zlib vectors and the encoder against Python `zlib`/`gzip`.

### 21.4 HTTP/2

HTTP/2 does not use these codings in this slice: a request that carries a
`Content-Encoding` needing decoding is `501`, and a response whose negotiation
would select gzip/deflate is `501` (fail closed, no silent identity downgrade).
`Accept-Encoding` that selects identity (and does not refuse it) is answered
normally; refusing identity is `406`.

## 22. HTTP file download and Range (implemented subset) <a id="s22"></a>

This section is normative for `download_route` in the server facade (HTTP/1.1,
in process). `download_source` and range parsing live in
`include/crpc/http/file_transfer.hpp` / `src/http/file_transfer.cpp`; the
session-level streaming wiring lives in `src/http/server.cpp`.

### 22.1 Routing and path safety

A route registers a request-path prefix `path` and a filesystem `root`. The
remainder is percent-decoded and resolved under `root`. Empty paths, absolute
paths, backslashes, `.`/`..` segments, a directory target, a missing/unreadable
file, and any result whose canonical (symlink-resolved) path is not under the
canonical root are answered with `404`. `GET` and `HEAD` are served; every
other method is `405` with `Allow: GET, HEAD`.

### 22.2 Range semantics

A `Range` header is applied to the file entity:

| Header value | Result |
|---|---|
| absent / unknown unit | `200`, full entity |
| `bytes=a-b` with `b >= size` | `206`, `b` clamped to `size-1` |
| `bytes=a-` | `206` |
| `bytes=-n` (`n > 0`) | `206` suffix range |
| `bytes=a-b` with `a >= size` | `416` + `Content-Range: bytes */size` |
| `bytes=-0` | `416` |
| missing `=`, missing `-`, unparseable number, `last < first` | ignored, `200` |
| multiple ranges (`bytes=0-1,3-4`) | ignored, `200` (`multipart/byteranges` not implemented) |

An empty entity with a Range is `416` (`bytes */0`). A `206` response carries
`Content-Range` and `Accept-Ranges: bytes`; a `200` response carries
`Accept-Ranges: bytes` and, when response coding is enabled, `Vary:
Accept-Encoding` (the download path itself is never content-coded). A `416` has
an empty body and `Content-Length: 0`.

`If-Range` is ignored because this slice emits no validator (`ETag` /
`Last-Modified`): when present, the `Range` is ignored and the full entity is
served (`200`), as RFC 9110 13.1.5 requires without a validator.

### 22.3 Streaming and liveness

The head is hand-written (for a `206` the length is the selected range), then
the body is read in `download_chunk_bytes` chunks (default 64 KiB) and queued
as ordinary bytes with a known `Content-Length` (not chunked). Output is driven
by write completions and bounded by `egress_cap_bytes`, so a slow peer
backpressures the file read instead of growing memory. A download keeps the
connection's idle deadline active (refreshed by write progress) and uses its
own `output_stall_timeout` (default 30 s): a peer that leaves staged output
undrained for that long is closed. The timer is independent of the SSE stall
deadline; unlike SSE, the download keeps `idle_timeout`. `HEAD` sends the same
headers without a body (including `HEAD` + `Range` -> `206` headers). After a
keep-alive download the connection can serve the next request.

### 22.4 HTTP/2

A request path that matches a download route is answered with a complete `501`
over HTTP/2 (fail closed); no file is streamed.
