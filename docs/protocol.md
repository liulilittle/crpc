# CRPC wire protocol

[简体中文](protocol.zh-CN.md) | **English**

Version 2.0 / 2026-10-24

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
| [23](#s23) | HTTP/1.1 long polling (implemented subset) |
| [24](#s24) | CRPC frame compression (implemented subset) |

## 1. Scope and authority <a id="s1"></a>

The CRPC binary protocol is a length-prefixed, multiplexed frame stream. A frame is a 16-byte fixed header, an optional 16-byte ordered extension, and a payload. The frame layer knows nothing about RPC methods; `REQUEST`, `RESPONSE` and `ONE_WAY` payloads carry a call envelope.

Authority: `include/crpc/frame.hpp` for field widths, numeric values, flags, limits and CRC; `src/frame/frame.cpp` for validation order and envelope encoding; `src/rpc/client.cpp` for what the client writes and accepts; `src/rpc/dispatcher.cpp` for response encoding. The design §7 is the target contract. One deviation is called out: the envelope uses 64-bit service/method ids and a different extension layout (§8, §9). The deadline is relative as the design requires (§9).

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
| 5–7 | `0xE0` | `ALGO` | 0 none, 1 lz4, 2 zstd, 3 gzip, 4 deflate, 5 lz77, 6 zip, 7 reserved |

`COMPRESSED = 0` requires `ALGO = 0`; `COMPRESSED = 1` requires a non-zero `ALGO` (`1` lz4, `3` gzip, `4` deflate, `5` lz77 and `6` zip are implemented; `2` zstd is reserved and fails closed with `protocol_error` in this build; `7` is reserved); `ONE_WAY` requires `UNRELIABLE`. Frame-level compression is normative in section 24. A decoder that cannot decompress the advertised algorithm fails closed with `protocol_error`; it never delivers the compressed bytes as a payload. Compression is defined only for `REQUEST`, `RESPONSE`, `DATA`, `ONE_WAY` and `PUSH`; a `COMPRESSED` control frame is `protocol_error`. The header validator still accepts such a header (it checks flag consistency only) so the whole-frame paths can reject it with the payload in hand.

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
- For a `COMPRESSED` frame, `payload_len` is still the on-the-wire byte count and the payload
  starts with a 4-byte `uncompressed_len` (LE) prefix; the logical payload is
  `uncompressed_len <= max_frame` bytes. Compression never enlarges a frame: a sender falls
  back to an uncompressed frame when the encoded form is not strictly smaller (section 24.4).

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

**Server admission errors.** The admission stages reuse this error envelope; no wire field is added. An optional per-connection token bucket answers `resource_exhausted` (13) with a non-zero `retry_after_ms`. A per-service token bucket answers the same, or `deadline_exceeded` (2) with `retry_after_ms = 0` when waiting would cross the request deadline. A full per-service bulkhead answers `overload` (8) with its hint; an open breaker answers `unavailable` (3) with the remaining open duration; the dispatcher's pre-execution deadline refusal stays `deadline_exceeded` with 0. HTTP mapping is 429 for `resource_exhausted`/`overload`, 503 for `unavailable` and 504 for `deadline_exceeded`. `retry_after_ms` is a floor computed by the server, never a promise that a retry will succeed. Unknown service ids remain `not_found` (9) and never create server state.

**The `crpc-degradable` marker.** A refusal that happens before the handler runs can be answered with a fallback value by a client that has one; a failure produced after the handler ran cannot. Both can carry the same `rpc_code`, so the server advertises the difference in one reserved response meta entry, `crpc-degradable: true` (key length 15, value length 4). It is written at exactly four pre-execution refusal points, and only when the request's remaining wire deadline is at least 1 ms:

| Refusal | `rpc_code` |
|---|---|
| per-service circuit breaker open | `unavailable` (3) |
| per-service rate bucket empty, deadline still reachable | `resource_exhausted` (13) |
| per-service bulkhead full | `overload` (8) |
| per-service in-flight cap reached | `overload` (8) |

Five other error paths never carry it: the per-connection flood bucket (`resource_exhausted`), the per-connection in-flight cap (`overload`), the per-service rate bucket's "cannot meet the deadline" exit (`deadline_exceeded`), the dispatcher's own pre-execution deadline refusal (`deadline_exceeded`), and every handler-produced error, including `overload` and `unavailable`. A one-way request never carries it either: it has no response frame. The marker is not derivable from the code — an `overload` returned by a handler and an `overload` returned by the bulkhead are identical apart from this entry. A client uses it as a precondition for its fallback gates: eligibility (the `(code, stage)` table), the entry floor, and the overrun check still decide whether a fallback may run. `crpc-degraded` (the companion key that reports a value actually produced by a fallback) is still written by nobody on the server, and the server still never runs a fallback itself.


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
| compressed payload prefix | 4 bytes | fixed (`uncompressed_len` u32 LE) |
| `uncompressed_len` | `1..max_frame` | `protocol_error` |
| compression ratio | 300:1 default (`max_compression_ratio`, 0 disables) | `resource_exhausted` |
| decompressed frame | `<= max_frame`; also `<=` the decoder payload buffer | `protocol_error` / `resource_exhausted` |

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
| unsupported `ALGO` (zstd, reserved id 7) | `protocol_error` |
| compressed payload shorter than 5 bytes | `protocol_error` |
| `uncompressed_len` zero or above `max_frame` | `protocol_error` |
| malformed codec stream or decoded length mismatch | `protocol_error` |
| `COMPRESSED` on a control frame type | `protocol_error` |
| decompressed size or ratio above the configured limit | `resource_exhausted` |
| `COMPRESSED` frame reaching a borrowing-mode decoder | `protocol_error` |

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

`feed_external()` is the zero-copy form of `feed()` for a caller-owned chunk. It borrows `data` and decodes complete frames straight out of it, so in borrowing mode a returned payload points into `data`; in copying mode the payload is still copied into the decoder's payload buffer. `data` must stay valid until `drain_external()` returns, and the usual shape is one `feed_external()`, a `next()` loop, then `drain_external()`, all inside the scope that owns `data`. `drain_external()` copies only the trailing bytes that do not complete a frame back into the internal buffer, so a later `feed()`/`feed_external()` completes them; the frame stream is the same `feed()` would produce, with the per-chunk copy removed, and ordering is preserved. If bytes are already buffered from an earlier partial frame, or a previous chunk was not drained, `feed_external()` falls back to the copying path of `feed()` and the frames borrow the internal buffer instead; calling `feed()` while an external chunk is active first drains it. After an error both calls are no-ops that only release the external state. The measured effect is recorded in [performance §2.4](performance.md#s2-4).

The decoder never allocates for a payload whose header has not passed validation. It compacts only when the dead prefix is at least 4 KiB and at least half the buffer, to avoid quadratic copying. `decode_frame` requires the input size to equal the encoded size exactly; trailing bytes are a length mismatch. `decode_frame_header` validates only the header and leaves CRC to the payload-aware decoders. `decode_call_envelope` and `decode_response` borrow the body (and idempotency key) from the input; the views are valid only while the input is.

## 16. Session handshake <a id="s16"></a>

**Specified, not implemented.** The design defines a 12-byte client hello (`"CRPC"`, `u16` version, `u16` flags, `u16` tlv_len, `u16` reserved) followed by TLVs for `max_frame`, `initial_window`, `codec`, `compress_bitmap`, `compress_min`, `keepalive_ms`, `max_streams`, `identity`, `resume_token` and `hmac`. A server hello has the same prefix with a status byte. The limit is 4 KiB, the timeout 3 s, and unknown TLV/codec/capability, non-zero reserved or a version mismatch is fail-closed.

`protocol_version` is `1`, but no handshake is written or parsed by the current code. The frame header carries no version field, so version negotiation does not exist yet; peers must be built from compatible sources.

Compression capability (`compress_bitmap`, `compress_min`, `max_decompressed_frame`) is not negotiated by this handshake in the current implementation; the connection-level `SETTINGS` frame carries the same fields, see section 24.5. Until a peer advertises, nothing is compressed.

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
HTTP SSE and HTTP long polling have their own subsets in sections 20 and 23.

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

## 20. Server-Sent Events (implemented subset) <a id="s20"></a>

This section is normative for the implemented SSE subset: HTTP/1.1 and HTTP/2
(h2c/h2), in process, with one or more shards. Long polling is specified in
section 23; authentication and `content_coding` are outside this subset. The
framing helpers live in
`include/crpc/http/sse.hpp` / `src/http/sse.cpp`; the server wiring lives in
`src/http/server.cpp`.

### 20.1 Request

`GET <sse-path>`; `Accept: text/event-stream` is optional. Query parameters
override the registered route: `topic` (must be non-empty), `mode`
(`reliable` or `unreliable`), `ordered` (`0`/`1` or `false`/`true`) and
`subscriber` (a non-zero u64; a fresh id is allocated when absent). An optional
`Last-Event-ID: <epoch>:<seq>` resumes a subscription. Routes are registered
with `http_server::builder::add_sse_route()`. The topic is routed to the hub of
its owning shard (`push::topic_shard(topic, 0, shards)`), so `shards > 1` is
supported: only the owning shard's hub holds the topic's subscriptions and
queue.

### 20.2 Response and events

Over HTTP/1.1 the response is `200` with
`Content-Type: text/event-stream; charset=utf-8`, `Cache-Control: no-cache`,
`Connection: keep-alive`, `X-Accel-Buffering: no` and `Transfer-Encoding:
chunked` (there is no `Content-Length`; the response uses a hand-written head
because `http::serialize` always appends the terminating chunk). Event frames
are `id: <epoch>:<seq>\ndata: <payload>\n\n`. A payload that is not valid UTF-8
text is base64 encoded and marked `data: b64:<...>`; only binary events carry
the `b64:` prefix. A comment-only heartbeat `:hb\n\n` is emitted every
`sse_options::heartbeat` (default 15 s; zero disables). A configured `retry:`
hint is sent once at the start.

Over HTTP/2 the same status and headers (minus the connection-specific
`Connection` and `Transfer-Encoding`) are sent as a HEADERS frame with
END_STREAM clear, and every event is a DATA frame on the same stream. END_STREAM
is set only on the empty DATA frame that ends the stream (server shutdown); an
event DATA frame never sets it. A header block larger than the peer's
`SETTINGS_MAX_FRAME_SIZE` is split into HEADERS plus one or more CONTINUATION
frames: only the frame that completes the block carries END_HEADERS (RFC 7540
6.2). The HEADERS frame of a split block never sets END_HEADERS.

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

Over HTTP/2 the staged bytes also include the connection's parked send queue, so
the same bound applies to a zero-window peer: `WINDOW_UPDATE` resumes delivery
and the periodic zero-window probe PING detects a dead peer (R18).

Known limitation (shared with HTTP/1.1): the delivered high-water advances as
soon as a frame is enqueued, even when only part of it is on the wire and the
stream is reset before the parked remainder is flushed. A reconnect therefore
does not replay that partially-written event. The h2 stall deadline and the RST
path both close such a stream promptly, so the window is narrow.

### 20.4 Reconnect and replay

A reconnect with the same `subscriber` reuses the existing subscription: the
server never re-subscribes. It clamps the `Last-Event-ID` sequence to the
highest sequence it has delivered for that (subscriber, topic), then calls
`hub.ack(cursor)` (cumulative) and `hub.attach()`, which replays the
unacknowledged reliable messages. A cursor whose epoch differs from the current
server epoch is not acknowledged. The event id therefore carries both epoch and
sequence, and a client must de-duplicate on the `(epoch, seq)` pair.

### 20.5 Cross-shard routing and publish

`http_server_options::shards > 1` runs one hub per shard. A topic is owned by
exactly one hub: `push::topic_shard(topic, 0, shards)`. `http_server::publish`
computes the owning shard and posts a task to that shard's runtime; the task
takes the hub mutex, publishes to the owning hub, then wakes every session. A
request on any shard (SSE or long polling) subscribes on the owning hub, so only
that hub holds the topic's subscriptions and queue: every topic keeps a single
writer. The shared hub mutex serializes a session's cross-shard `try_take` and
the publish, and the wake is posted only after the publish completes, so
ordering, backpressure and reliable replay are unchanged. The periodic long-poll
TTL sweep calls `unsubscribe_all` on every hub so a subscriber that outlived its
session on any shard is still reclaimed.

### 20.6 HTTP/2 lifecycle

A matching HTTP/2 request opens one stream and
streams events as DATA frames; the connection stays usable, so other streams
are unaffected. A `RST_STREAM` for the SSE stream detaches the hub subscription
but keeps the connection; a peer reset, a write failure or `slow_timeout` ends
the stream, and a server shutdown ends it with an empty END_STREAM DATA frame.
A session owns one SSE stream, so a second SSE request on the same connection
replaces the first and sends `RST_STREAM` (CANCEL) for the replaced stream: its
connection record and any parked bytes are released instead of lingering until
the connection closes.

A single event whose frame is larger than the sum of the available stream window
and `http_server_options::h2_max_send_queue_bytes` (default 1 MiB) can never be
parked and fails the stream with `ENHANCE_YOUR_CALM`. The already-taken reliable
event is not lost: the hub replay entry stays unacknowledged and `attach()`
replays it on the next reconnect with the same `subscriber` (at-least-once). The
fail-closed boundary is the hub's `subscribe_options::retransmit_budget`
(default 3): once a permanently-oversize event has exhausted that many replay
attempts the subscription is disconnected with `not_acknowledged`.

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

Frame-level compression on the CRPC channel is a separate mechanism with its own container,
limits and capability exchange; see section 24. Nothing in this section applies to CRPC frames.

### 21.2 Response compression

When `enable_response_coding` is set, the response body is negotiated from
`Accept-Encoding` (q-values; gzip preferred over deflate over identity).
Negotiation happens even for a body that is not compressed, and such a response
carries `Vary: Accept-Encoding`. An existing `Vary` (e.g. `Origin`) is merged
with `Accept-Encoding` (no duplicate token, never overwritten). A client that
refuses every supported coding (`identity;q=0` with no acceptable gzip/deflate)
is answered `406`, which also carries `Vary: Accept-Encoding`.

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
matches may cross `write` boundaries and every input byte enters the history
ring before the next write) wrapped in a gzip or zlib container. Hash-chain
positions are 32-bit, so after 2 GiB of streamed input matching degrades to
literals (no corruption). The maximum match distance is 32767 bytes: a distance
of exactly 32768 aliases the ring slot just written for the current position and
is therefore never selected. Its
output is valid for any conforming decoder; the decoder is independently
verified against zlib vectors and the encoder against Python `zlib`/`gzip`.

### 21.4 HTTP/2

HTTP/2 does not encode responses in this slice: a request that carries a
`Content-Encoding` needing decoding is `501`. A normal h2 client that offers
`gzip`/`deflate` is served **identity `200`** (no compression) with
`Vary: Accept-Encoding`; only refusing identity (`identity;q=0` with no
acceptable coding) is `406`, which also carries `Vary`.

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
other method is `405` with `Allow: GET, HEAD`. A `HEAD` error response (404/405/
416) has the same headers as `GET` but never a body, so keep-alive framing stays
in sync. The route's `content_type` (default and empty-string fallback
`application/octet-stream`) is sent as `Content-Type`; it never overrides the
path-safety rules. A stat/open
TOCTOU window remains (documented limitation, not hardened in this slice).

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
connection's idle deadline active and uses its own `download_stall_timeout`
(default 30 s). The stall deadline is refreshed by every progress event (a
chunk is queued, or a write completes) and cleared when the queue is empty, so
a slow peer that keeps draining is never cut off; only a peer with no progress
for the whole timeout is closed. The timer is independent of the SSE stall
deadline; unlike SSE, the download keeps `idle_timeout`. `HEAD` sends the same
headers without a body (including `HEAD` + `Range` -> `206` headers and HEAD
404/405/416 errors). After a keep-alive download the connection can serve the
next request.

### 22.4 HTTP/2

A request path that matches a download route is answered with a complete `501`
over HTTP/2 (fail closed); no file is streamed.

## 23. HTTP/1.1 long polling (implemented subset) <a id="s23"></a>

This section is normative for the implemented long-poll subset: HTTP/1.1, in
process, with one or more shards. Routes are registered with
`http_server::builder::add_longpoll_route()`; the session state machine lives in
`src/http/server.cpp`. The topic is routed to the hub of its owning shard
(`push::topic_shard(topic, 0, shards)`), so a request on a multi-shard server is
served by the owning shard's hub rather than answered `503`. Long-poll paths,
SSE paths and download paths are mutually exclusive (`start()` rejects a
conflict). SSE behaviour is unchanged.

### 23.1 Request and subscription identity

`GET <longpoll-path>`. `HEAD` and every other method are `405`; HTTP/1.0 is
`400`. Query parameters override the registered route: `topic` (must be
non-empty), `mode` (`reliable` or `unreliable`), `ordered` (`0`/`1` or
`false`/`true`), `subscriber` and `timeout`. `timeout` is milliseconds, defaults
to `longpoll_options::timeout` (25 s) and is clamped to
`[0, longpoll_options::max_timeout]` (30 s); `0` performs one non-blocking take
and answers `204` when the queue is empty.

A cursor is either `Last-Event-ID: <epoch>:<seq>` or `?cursor=<epoch>:<seq>`.
`Last-Event-ID` has priority; if both are present and differ the request is
`400`. Parsing is the strict SSE parser. Long-poll subscriber ids live in
`[2^32, 2^33)` (SSE allocates from 1); a client-supplied id outside that range
is `400`. When `subscriber` is absent the server allocates an id. Every `200`
and `204` carries `X-CRPC-Subscriber`, and the client must persist it. A new
subscription created after an overflow/TTL loss returns a fresh server id, so
the client resets its de-duplication state; the namespace is a
collision-avoidance convention, not a security boundary.

### 23.2 Responses

| Case | Status | Body | Key headers |
|---|---|---|---|
| Event | `200` | exactly one raw message | `Content-Type` (default `application/octet-stream`), `Content-Length`, `Cache-Control: no-store, no-cache`, `X-CRPC-Event-ID: <epoch>:<seq>`, `X-CRPC-Subscriber`, `Connection: keep-alive|close` |
| Timeout | `204` | empty | `Cache-Control: no-store, no-cache`, `X-CRPC-Subscriber`, `Connection: keep-alive|close`; no `Content-Length`, no `X-CRPC-Event-ID` |
| Zero-length event | `200` | empty (`Content-Length: 0`) | same as an event, including `X-CRPC-Event-ID` |
| Invalid parameter/cursor | `400` | problem/plain | - |
| Unknown/unowned topic | `404` | plain | - |
| Same subscriber already active | `409` | plain | - |
| Replay window gap / retransmission budget | `409` | plain (`Restart`) | - |
| Waiting-poll capacity | `503` | plain | `Retry-After: 1` |
| HTTP/2 | `501` | plain | complete response, no stream |

The reliable-mode event id uses the per-subscription `seq`; unreliable mode
uses `topic_seq` and has no acknowledgement. Responses are never
content-coded and carry no `Vary` (the special route runs before response
coding). A zero-length reliable event is a `200` with `Content-Length: 0` and an
event id; it is distinguishable from the bodyless `204` timeout.

### 23.3 Acknowledgement and at-least-once delivery

A `200` does not acknowledge the message. The next request's cursor is the
cumulative ACK: the server clamps it to the highest sequence it has delivered
for that `(subscriber, topic)` and calls `hub.ack(cursor)`. An over-large cursor
is clamped (`longpoll_ack_clamped`); a cursor whose epoch differs from
`longpoll_options::epoch` is not acknowledged. The de-duplication key is
`(subscriber, epoch, seq)`.

A normal `200` or `204` never detaches: the token, subscription and queue are
kept across polls. Session end (peer close, stop, stall) uses the hub's
keep-queue `release(token)`: it invalidates the token but leaves queued
messages `in_queue` and does not count a retransmission. If the client's cursor
is behind the delivered high water, the server calls `retransmit_front`, which
inserts the already-taken but unacknowledged reliable messages at the FRONT of
the queue in sequence order before taking one. `retransmit_budget` counts only
messages that were actually transmitted. If the replay window no longer
contains the first unacknowledged sequence (it was trimmed by `max_replay`) or
the budget is exhausted, the current poll is `409 Restart` instead of silently
skipping data; the client restarts the subscription. `retransmit_front` relies
on `subscribe_options::max_messages >= subscribe_options::max_replay`. A replay
bound larger than the queue bound is rejected by the long-poll `start()` with
`invalid_argument` (the queue's memory bound is never silently enlarged); a
smaller replay bound is raised to the queue bound. Both bounds together are
necessary but not sufficient for the front replay to always fit: it must also
fit the byte budget (`subscribe_options::max_bytes`), which `start()` neither
checks nor rewrites. The two bounds are independent, so a window that satisfies
`max_messages >= max_replay` can still exceed `max_bytes`; `retransmit_front`
then returns `resource_exhausted` and the poll answers `409 Restart` rather
than silently dropping the unacknowledged messages. With the documented
defaults (`max_messages = max_replay = 1024`, `max_bytes = 1 MiB`) three
600 KiB publishes leave only the newest message queued; after it is delivered
the front replay must requeue all three (1.8 MiB), so a cursor-gap poll is a
`409 Restart` (measured, not inferred). This requirement is specific to front
replay: tail replay (SSE) has no cross-bound requirement, and
`hub::subscribe` neither rejects nor rewrites the caller's bounds. A direct hub
caller that uses `retransmit_front` while `max_messages < max_replay` gets
`resource_exhausted` (a safe restart), never a silent skip.

### 23.4 Waiting, liveness and limits

The take and the wait registration happen under the same hub lock as `publish`,
so a publish can never fall between "queue empty" and "wait registered". While
a poll waits, the connection's idle and request deadlines are disabled and a
dedicated poll deadline is armed. A publish wake posts shard work; the shard
takes one message and answers immediately. When the deadline fires, the server
performs one more take (a wake may have been dropped by a full shard inbox)
before answering `204`, then re-arms the idle deadline. The connection stays
keep-alive; HTTP/1.1 pipelining is not parsed until the poll's response has been
queued.

A subscriber has at most one active transport: a second concurrent poll from
another connection is `409` (the first token is not retargeted). Total waiting
polls are bounded by `longpoll_options::max_waiters`; beyond it the request is
`503` with `Retry-After: 1`. `http_server_options::max_connections` remains the
accept-level socket bound. A peer that stops draining a polled body is closed
by `longpoll_options::slow_timeout`; the taken message stays unacknowledged and
is replayed by the next poll's cursor gap. A subscriber that sends no poll for
`longpoll_options::subscriber_ttl` (5 min) is unsubscribed (`unsubscribe_all`)
and its high-water/owner bookkeeping is removed by a shard housekeeping timer
(period `min(subscriber_ttl, 30 s)`); cleanup does not wait for a later poll.
On `stop()` a waiting poll is
woken and the session is closed; the keep-queue release runs before the hub is
destroyed.

### 23.5 Differences from SSE (section 20)

| Dimension | SSE | Long polling |
|---|---|---|
| Framing | chunked `text/event-stream` events (HTTP/1.1); DATA frames (HTTP/2) | one `Content-Length` raw body per `200` |
| Timeout | heartbeat comments; no 204 | `204`, no body |
| ACK | reconnect `Last-Event-ID` | next poll cursor (cumulative) |
| Connection ownership | dedicated stream, idle disabled | keep-alive, idle re-armed after each poll |
| Concurrency | reconnect retargets the token | same subscriber active transport -> `409` |
| Resource cleanup | existing subset has no TTL (D5) | TTL + `unsubscribe_all` |

## 24. CRPC frame compression (implemented subset) <a id="s24"></a>

Frame-level compression is independent of HTTP content coding (section 21). It applies to a single
frame payload, never across frames, and is available on the frame types that carry application data.

### 24.1 Container

A frame with `COMPRESSED` set carries:

| Offset | Width | Field | Notes |
|---|---|---|---|
| 0 | 4 | `uncompressed_len` | `u32` LE, `1..max_frame`; logical payload size |
| 4 | N | `codec_stream` | the codec named by `ALGO` (LZ4 block, gzip member, raw DEFLATE, LZ77 block, ZIP entry) |

`payload_len = 4 + N <= max_frame`. `CRC32C`, when set, covers the whole payload including the
prefix, so a receiver verifies integrity before spending CPU on decompression. A payload shorter
than 5 bytes, a zero `uncompressed_len`, or `uncompressed_len > max_frame` is `protocol_error`.

The 4-byte prefix is the same size-prefix convention the reference LZ4 block API uses with
`store_size=True`, so a third-party implementation can feed `payload + 4` and the declared length
straight to `LZ4_decompress_safe`.

### 24.2 Algorithms

| `ALGO` | Codec | Status |
|---|---|---|
| 0 | none | required |
| 1 | LZ4 **block** format (no frame magic, no frame checksums; the container above supplies both) | implemented |
| 2 | zstd | reserved, **not implemented**: always `protocol_error` |
| 3 | gzip member (RFC 1952: raw DEFLATE + CRC32 + ISIZE) | implemented |
| 4 | raw DEFLATE (RFC 1951; stored / fixed / dynamic blocks) | implemented |
| 5 | LZ77 block (CRPC-specific container: `LZ77` magic, sizes, CRC32, token stream) | implemented |
| 6 | ZIP local file entry (`PK\x03\x04` + raw DEFLATE + CRC32/sizes in the header) | implemented |
| 7 | reserved | `protocol_error` |

`ALGO = 1` is the LZ4 block format, not the LZ4 frame format. Golden vectors in
`tests/test_frame_compression.cpp` pin the byte layout for third-party implementers; they were
produced by the reference LZ4 v1.10.0 encoder and are byte-identical to this implementation's
output for the same inputs.

`ALGO = 3, 4, 6` share one DEFLATE core (`src/compression/deflate_core.cpp`), so a gzip
member, a raw DEFLATE stream and a ZIP entry are byte-compatible with a third-party DEFLATE
implementation: the interop test decodes a gzip member produced by a reference DEFLATE
encoder, and the same encoder's output is decodable by a reference gzip reader. `ALGO = 5`
is CRPC-specific: it is not interoperable with any external tool, and its 20-byte header
carries the CRC32 that makes the stream self-verifying.

A gzip member is `1f 8b`, `CM = 8`, `FLG = 0`, `MTIME = 0`, `XFL = 0`, `OS = 255`, then the raw
DEFLATE stream, then the little-endian CRC32 and ISIZE. A ZIP entry is the 30-byte local file
header (method 8, no data descriptor) followed by the raw DEFLATE stream. An LZ77 stream is
`4c 5a 37 37`, version 1, the uncompressed size, the compressed size and the CRC32 (all `u32`
LE), then literal-run and match tokens terminated by `0x00`.

#### 24.2.1 Choosing an algorithm

The container is independent of the codec, so the choice is a policy decision, not a wire
decision. `ALGO` always names the codec that actually produced `codec_stream`; a sender never
substitutes a different codec for the requested one.

| Choice | When it fits | Trade-off |
|---|---|---|
| `none` | Payload below `min_payload`, already compressed data, or no peer capability | No CPU; the frame is not flagged `COMPRESSED` |
| `lz4` | Latency-sensitive RPC/push, the default when the peer advertises it | Lowest CPU of the compressing choices; block format needs the 4-byte length prefix |
| `gzip` | Interop with an external gzip reader, or an HTTP-style payload that is already gzip-shaped | RFC 1952 header/trailer (18 bytes) and a CRC32; slower than LZ4 |
| `deflate` | Interop with a raw RFC 1951 stream; smallest container overhead | No magic or checksum of its own; the frame `CRC32C` (when set) is the integrity check |
| `zip` | Payload that must also be readable as a ZIP local entry | 30-byte local header; no central directory is produced |
| `lz77` | CRPC-only links that want a self-verifying stream without an external format | CRPC-specific; not interoperable with any external tool |
| `zstd` | Reserved | Not implemented; always `protocol_error`, never a fallback |

The relationship between the choices is:

- `gzip`, `deflate` and `zip` share the DEFLATE core, so they produce different containers around
  the same compressed payload. They are the interoperable family.
- `lz4` is a different algorithm (LZ4 block), chosen for speed. `zstd` is a different algorithm
  that is reserved but not available in this implementation.
- `lz77` is a CRPC-specific container around an LZ77 token stream; it is self-verifying but not
  an interchange format.
- Compression here is frame compression (this section), not HTTP content coding (section 21).
  A frame can be compressed regardless of whether the same bytes would be gzip/deflate-encoded
  by the HTTP layer; the two layers do not share a stream or a dictionary.

`encode_frame_compressed` also applies the never-inflate rule: if the chosen codec's output
including the 4-byte prefix is not strictly smaller, the frame is sent uncompressed and
`chosen` reports `none`. A caller that needs a specific algorithm on the wire must therefore
check `chosen`, not just the request. Measured Debug ratios and throughput for every choice are
recorded in [performance §2.5](performance.md#s2-5).

Compression is only defined for frames that carry application data: `REQUEST`, `RESPONSE`,
`DATA`, `ONE_WAY` and `PUSH`. A `COMPRESSED` bit on a control frame (`RST_STREAM`, `SETTINGS`,
`SETTINGS_ACK`, `PING`, `PONG`, `GOAWAY`, `WINDOW_UPDATE`, `PUSH_ACK`) is `protocol_error`: those
payloads are fixed-size control fields where compression can only inflate, and `PUSH_ACK` is
additionally size-checked by the server. `decode_frame_header` still accepts such a header (it
validates flag consistency only); the rejection happens on all three whole-frame entry points:
`decode_frame`, `decode_frame_compressed` and `frame_decoder::next`. The gate is checked before
the codec runs, so a control frame carrying a perfectly valid LZ4 stream is still refused; on
`frame_decoder::next` it is a latched `protocol_error` (never `frame_dropped`), including for
`PUSH_ACK`, so a peer cannot turn the violation into a silent drop.

### 24.3 Fail-closed rules

| Condition | Result |
|---|---|
| unsupported `ALGO` (zstd, reserved id 7) | `protocol_error` |
| `payload_len < 5`, `uncompressed_len == 0` | `protocol_error` |
| `uncompressed_len > max_frame` | `protocol_error` |
| malformed or truncated codec stream | `protocol_error` (reliable) / explicit drop (unreliable `ONE_WAY`) |
| decoded byte count differs from `uncompressed_len` | `protocol_error` (reliable) / explicit drop (unreliable `ONE_WAY`) |
| `CRC32C` mismatch | `protocol_error` (reliable) / explicit drop (unreliable `ONE_WAY`) |
| `uncompressed_len` exceeds `min(max_frame, limits.max_payload, payload buffer)` | `resource_exhausted` (before decompression) |
| `COMPRESSED` on a control frame type | `protocol_error` |
| compressed/plain ratio above `max_compression_ratio` (default 300:1) | `resource_exhausted` |
| `COMPRESSED` frame on a decoder in borrowing mode (no output buffer) | `protocol_error` |

The ratio row is a CPU guard, not the memory bound. The memory bound is the declared length: the
decoder checks uncompressed_len <= payload_out.size() before decompressing and then hands the
codec the view payload_out.first(uncompressed_len), so every literal and match is checked against
the declared length before it is written and the stream must produce exactly that many bytes. A
4 MiB stream carrying a 100-byte declaration therefore fails without writing past byte 100, even if
the caller's buffer is 16 MiB.

Reliable frames treat every row as fatal for the connection, exactly like the other rows of section
13. A `ONE_WAY` frame additionally allows a *dropped frame* outcome for the frame-local rows marked
above: the frame is discarded explicitly, the connection continues, and the drop is counted. The
drop criterion is the frame **type**, not the `UNRELIABLE` flag: a peer can set that flag on a
`REQUEST`, and it must not be able to turn a fatal protocol violation into a silently lost request.
Algorithm and limit rows stay fatal on every frame type: they are connection-level threats, not
single-frame corruption.

A decoder never returns the compressed bytes as a payload: it either delivers the decompressed
payload with `COMPRESSED`/`ALGO` cleared and `payload_len` (and `CRC32C`, when present) rewritten for
the logical payload, drops the frame (a `ONE_WAY` frame only, explicitly counted), or fails.

### 24.4 Sender policy

A sender compresses a payload only when the peer advertised the algorithm (section 24.5), the
payload is at least `max(local.compress_min, peer.compress_min)` bytes (default 512), and the
encoded form **including the 4-byte prefix** is strictly smaller than the original payload.
Otherwise the frame is sent uncompressed and `COMPRESSED` stays clear: a sender never inflates a
frame, so enabling compression can never break a frame-size budget.

The algorithm is a runtime choice passed to `encode_frame_compressed` (`none` requests identity).
An unsupported choice (zstd, reserved ids) is `invalid_argument`, never a silent fallback to a
different codec: "configured zstd, sent lz4" would be the same flag-versus-payload mismatch this
section removes.

`encode_frame_compressed` validates the header with exactly the same rules as `encode_frame`
(known type, stream scope, `stream_id` bit 31 clear, `ONE_WAY` requires `UNRELIABLE`, non-zero
`ext` requires `CRC32C`, `payload_len <= max_frame`) before it writes a byte, on both the
compressing and the identity branch. A violation is `invalid_argument` with nothing written, so
the compressed entry point can never emit a frame that this implementation's own
`decode_frame_header` refuses. A caller-supplied `ext` is validated, not silently normalised to
zero when `CRC32C` is absent.

### 24.5 Capability exchange

Compression is negotiated with the connection-level `SETTINGS` frame (`0x05`, `stream_id = 0`),
which predates this section and was previously unparsed:

| Offset | Width | Field |
|---|---|---|
| 0 | 1 | `version` = 1 |
| 1 | 1 | `flags` = 0 (reserved) |
| 2 | 2 | `reserved` = 0 |
| 4 | 4 | `compress_bitmap`: bit0 lz4, bit1 zstd, bit2 gzip, bit3 deflate, bit4 lz77, bit5 zip; bits 6-31 must be 0 |
| 8 | 4 | `compress_min`: payload bytes below this are never compressed; 0 = 512 |
| 12 | 4 | `max_decompressed_frame`: accepted logical frame size; 0 = 16 MiB |

Both sides send one `SETTINGS` immediately after the transport is established and before any
request. Until the peer's `SETTINGS` arrives, nothing is compressed. The effective send set is the
intersection of both bitmaps. `SETTINGS` is a full snapshot: a later one replaces an earlier one and
only affects subsequent frames; no acknowledgement is required. `SETTINGS` payload parsing is not
implemented in this slice; the frame is ignored by both current peers, which is exactly why the
"never compress before the peer advertises" default is safe.

A receiver accepts any algorithm it can decode, whether or not it was advertised; only an
undecodable algorithm is a protocol error. This keeps mixed deployments working: a peer built
before this section ignores `SETTINGS`, never advertises, and therefore never receives a compressed
frame.

### 24.6 Interaction with other flags

- `ORDERED` is orthogonal: the lane extension is outside the payload and is never compressed.
- `END_STREAM` marks the last fragment; every fragment is compressed independently, and fragments
  may mix compressed and uncompressed forms.
- `UNRELIABLE` frames may be compressed. A frame-local decompression failure discards that frame
  explicitly and keeps the connection; the drop is reported as `rpc_code::decode_error` (through the
  per-frame report, not through the decoder's latched error state). `CRC32C` is recommended on
  compressed unreliable frames so bit rot is caught before decompression.
- Compression is per frame: there is no cross-frame dictionary or history, so a dropped or
  reordered frame can never corrupt a later one.

### 24.7 Verification

`tests/test_frame_compression.cpp` covers the container, every codec, the fail-closed table, the
unreliable drop path, the ratio and size limits, the never-inflate policy, golden vectors and a
mutation fuzz loop. The LZ4 block codec is additionally checked for byte-identical output against
the reference implementation on the golden vectors; the gzip and raw DEFLATE codecs are checked
against a reference DEFLATE encoder (a gzip member and its raw DEFLATE payload decode to the
input), and the encoder's gzip output is decodable by a reference gzip reader. The gzip, ZIP and
LZ77 containers are checked for their magic bytes, CRC32 and size fields, and a corrupted byte is
refused. The suite reports 61 cases / 95 229 checks / 0 failures in the recorded run.

`bench/bench_compression.cpp` measures the same frame path for `none`, `lz4`, `deflate`, `lz77`,
`zip` and `gzip` over text and incompressible binary payloads at 4 KiB and 64 KiB. It reports the
chosen algorithm, wire/plain ratio, and encode/decode throughput; see
[performance §2.5](performance.md#s2-5) and
[distributed testing §6.4](distributed-testing.md#s6).
