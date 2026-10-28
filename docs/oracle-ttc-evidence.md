# Oracle TNS/TTC wire evidence

Chinese: [oracle-ttc-evidence.zh-CN.md](oracle-ttc-evidence.zh-CN.md)

## 1. Status of this document

This file records the wire-level facts that `include/crpc/db/oracle/` implements, and
says where each fact came from. It is a work record, not a specification: Oracle
publishes none for TNS/TTC.

**End-to-end is not verified. There is no Oracle server on this machine** (no
Docker, no listener, no instance), so nothing here has ever been exercised against
a real database. The only authority used is Oracle's own `python-oracledb`, whose
thin mode is a complete client-side implementation of the same protocol. The code
in this repository is checked against that implementation's source and, where the
reference client can be made to emit bytes locally, against bytes it actually
produced.

The adapter in `include/crpc/db/connection.hpp` still has no Oracle backend. What
exists is a codec; the facade would have to authenticate (O5LOGON/OSESSKEY) before
it could be called an adapter, and authentication cannot be verified here. Any
claim that CRPC can connect to Oracle would be false.

## 2. Reference under test

* Repository: <https://github.com/oracle/python-oracledb>
* Clone: `git clone --depth 1 https://github.com/oracle/python-oracledb.git /tmp/podb`
* `git log -1` in that clone: `c869687e857bf40cb7b3ac29684a0da35dcbfc65 2026-10-27`
* Version file: `src/oracledb/version.py` reports `26.1.0`
* Built and installed in WSL Ubuntu 24.04 so its behaviour could be observed:

```text
apt-get install -y cython3 python3-setuptools python3-pip
pip3 install -U 'cython>=3.1' 'setuptools>=77' 'packaging>=24.2'
git -C /tmp/podb submodule update --init --depth 1        # odpi, needed by setup.py
pip3 install --no-build-isolation --break-system-packages /tmp/podb
```

The install succeeded (`Successfully installed oracledb-26.1.0`), so the **differential**
work below is not a manual reading: bytes were captured from the installed client.

Paths in the citations are relative to `/tmp/podb/src/oracledb/`.

## 3. Method

1. **Read** the Cython sources listed in section 4 and extract each constant and each
   field write, with its file and line.
2. **Differential capture**: a Python fake listener accepts a real
   `oracledb.connect(...)` from the installed client, replies with a crafted ACCEPT,
   and records every packet the client then sends. The scripts are reproduced in
   section 11. The captured bytes are the golden vectors used by
   `tests/test_db_oracle_tns.cpp`.
3. **Cross-check**: the crafted ACCEPT is not from the reference (a listener is the
   server side), but the reference client *parsed it and continued*, which is the
   green half of a red/green check on the ACCEPT layout: a wrong offset would have
   produced either an exception or no protocol message.

This is byte-level differential evidence for framing and for the connect phase. It
is **not** evidence about what a real database replies, and it says nothing about
authentication or query results.

## 4. Files read

| File | Lines | What it establishes |
| --- | --- | --- |
| `impl/thin/constants.pxi` | 34-59 | packet types, packet flags, data flags, marker types |
| `impl/thin/constants.pxi` | 179-201 | TTC message types |
| `impl/thin/constants.pxi` | 275-293 | execute options and execute flags |
| `impl/thin/constants.pxi` | 384-414 | protocol versions, connect flags, `TNS_PROTOCOL_CHARACTERISTICS` |
| `impl/thin/constants.pxi` | 418-450 | TTC function codes |
| `impl/thin/constants.pxi` | 475-586 | capability offsets and flags |
| `impl/thin/constants.pxi` | 606-608 | ACCEPT flag bits |
| `impl/thin/constants.pxi` | 737 | `TNS_MAX_CONNECT_DATA = 230` |
| `impl/thin/packet.pyx` | 34-35 | `PACKET_HEADER_SIZE = 8`, `DATA_PACKET_HEADER_SIZE = 10` |
| `impl/thin/packet.pyx` | 59-74 | end-of-response detection from the data flags |
| `impl/thin/packet.pyx` | 773-800 | packet header write order, small vs large layout |
| `impl/thin/packet.pyx` | 812-838 | `end_request`, `start_request`, data-flag slot |
| `impl/thin/packet.pyx` | 330-346 | control packet body |
| `impl/thin/packet.pyx` | 359-380 | which packets are kept, and the end-of-response rule |
| `impl/thin/packet.pyx` | 392-404 | first two bytes of a DATA packet are its data flags |
| `impl/thin/transport.pyx` | 334-390 | `extract_packet`: length field, type byte, flags byte |
| `impl/base/buffer.pyx` | 132-151 | data length encoding: 1-byte, or 254 + chunks |
| `impl/base/buffer.pyx` | 737-790 | universal integer encoding (`ub2`, `ub4`, `ub8`) |
| `impl/base/buffer.pyx` | 372-420 | universal integer decoding |
| `impl/base/buffer.pyx` | 263-277 | length-prefixed read and the 255 null marker |
| `impl/base/decoders.pyx` | 112-132 | big-endian integer decode |
| `impl/base/decoders.pyx` | 157-273 | Oracle NUMBER decode |
| `impl/base/connect_params.pyx` | 40-44 | default protocol, port 1521, SDU 8192 |
| `impl/base/connect_params.pyx` | 946-1012 | connect descriptor assembly (order of clauses) |
| `impl/thin/messages/connect.pyx` | 47-112 | Connect/Accept/Redirect/Refuse parsing |
| `impl/thin/messages/connect.pyx` | 114-160 | Connect body field order |
| `impl/thin/messages/base.pyx` | 286-313 | message dispatch |
| `impl/thin/messages/base.pyx` | 786-793 | message header: type, function code, sequence number |
| `impl/thin/messages/base.pyx` | 928-938 | DATA packet wrapping of every message |
| `impl/thin/messages/base.pyx` | 315-396 | column metadata layout |
| `impl/thin/messages/base.pyx` | 1282-1325 | describe-info layout |
| `impl/thin/messages/base.pyx` | 1410-1432 | message types handled by a data message |
| `impl/thin/messages/base.pyx` | 1466-1522 | row data, bit vector, row header |
| `impl/thin/messages/execute.pyx` | 63-208 | Execute body field order |
| `impl/thin/messages/execute.pyx` | 210-248 | re-execute body field order |
| `impl/thin/messages/fetch.pyx` | 36-49 | Fetch body: cursor id, array size |
| `impl/thin/messages/protocol.pyx` | 42-47 | Protocol message body |
| `impl/thin/messages/protocol.pyx` | 58-92 | Protocol reply parsing |
| `impl/thin/messages/data_types.pyx` | 696-720 | DataTypes message body |
| `base_impl.pxd` | 118-151 | `ORA_TYPE_NUM_*`, `TNS_LONG_LENGTH_INDICATOR`, `TNS_NULL_LENGTH_INDICATOR` |

## 5. TNS packet header

The header is 8 bytes. There are **two layouts**, selected by the negotiated
protocol version: below 315 the first field is a 16-bit length plus a 16-bit
checksum; from 315 (12.1) on it is a 32-bit length and the checksum word disappears.
`impl/thin/packet.pyx:775-786` writes it:

```text
if protocol_version >= TNS_VERSION_MIN_LARGE_SDU (315):
    be32 length ; u8 type ; u8 flags ; be16 0        # 4+1+1+2 = 8
else:
    be16 length ; be16 checksum ; u8 type ; u8 flags ; be16 0   # 2+2+1+1+2 = 8
```

`impl/thin/transport.pyx:361-367` reads the length with `decode_uint32be` when
`_full_packet_size` is set (which `impl/thin/messages/connect.pyx:89` sets after an
ACCEPT), otherwise with `decode_uint16be`; `transport.pyx:373-374` takes the type
byte at offset 4 and the flags byte at offset 5 — the same offsets in both layouts,
because the length/checksum swap keeps the total at 4 bytes before them.

| Offset | Small layout (< 315) | Large layout (>= 315) | Note |
| --- | --- | --- | --- |
| 0 | length hi | length byte 3 | total packet size including the header |
| 1 | length lo | length byte 2 | |
| 2 | checksum hi | length byte 1 | checksum only exists in the small layout |
| 3 | checksum lo | length byte 0 | |
| 4 | type | type | see section 6 |
| 5 | flags | flags | see section 6 |
| 6 | 0 | 0 | reserved / header checksum |
| 7 | 0 | 0 | reserved / header checksum |

Facts that matter:

* the length counts the header itself (a packet is never shorter than 8 bytes);
* the reference always writes **0** for both the checksum and the reserved word
  (`packet.pyx:787` writes `write_uint16be(0)`); it never computes or validates a
  checksum, so a peer that enables TNS checksumming is not supported by the
  reference either;
* a DATA packet carries two more bytes after the header (`packet.pyx:34-35`,
  `packet.pyx:788-790`), so its body starts at offset 10. These two bytes are the
  16-bit data flags and are written for **every** packet of a multi-packet request,
  not only the first (`packet.pyx:773-800` runs per packet).

## 6. Packet types, flags, markers, control bodies

`impl/thin/constants.pxi:34-41`:

| Value | Name | Meaning |
| --- | --- | --- |
| 1 | `TNS_PACKET_TYPE_CONNECT` | client -> listener, first packet |
| 2 | `TNS_PACKET_TYPE_ACCEPT` | listener reply, carries protocol version and SDU |
| 4 | `TNS_PACKET_TYPE_REFUSE` | listener refuses, body is a text error |
| 5 | `TNS_PACKET_TYPE_REDIRECT` | listener redirects to another address |
| 6 | `TNS_PACKET_TYPE_DATA` | all TTC traffic after the handshake |
| 11 | `TNS_PACKET_TYPE_RESEND` | resize request |
| 12 | `TNS_PACKET_TYPE_MARKER` | break / reset / interrupt |
| 14 | `TNS_PACKET_TYPE_CONTROL` | out-of-band and in-band notification |

Packet flags (`constants.pxi:19-22`): `TNS_PACKET_FLAG_REDIRECT = 0x04`,
`TNS_PACKET_FLAG_TLS_RENEG = 0x08`.

Data flags (`constants.pxi:24-29`), read from the first two body bytes:

| Value | Name |
| --- | --- |
| 0x0040 | `TNS_DATA_FLAGS_EOF` (session shutdown) |
| 0x0800 | `TNS_DATA_FLAGS_END_OF_REQUEST` |
| 0x1000 | `TNS_DATA_FLAGS_BEGIN_PIPELINE` |
| 0x2000 | `TNS_DATA_FLAGS_END_OF_RESPONSE` |

Marker types (`constants.pxi:31-35`): `BREAK = 1`, `RESET = 2`, `INTERRUPT = 3`.
A marker body is exactly three bytes — `01 00 <marker>` — written by
`impl/thin/protocol.pyx:99-108`.

Control types (`constants.pxi:392-394`): `INBAND_NOTIFICATION = 8`,
`RESET_OOB = 9`. The body is `be16 control_type` and, for in-band notification, a
32-bit integer to skip and a `be32` error number (`packet.pyx:330-346`).

## 7. Connect packet: layout and a captured golden vector

`impl/thin/messages/connect.pyx:114-160` writes the body in this order. All
multi-byte fields are big-endian.

| Body offset | Width | Field | Source / value in the capture |
| --- | --- | --- | --- |
| 0 | 2 | protocol version desired | `TNS_VERSION_DESIRED = 319` (`constants.pxi:384`) |
| 2 | 2 | protocol version minimum | `TNS_VERSION_MINIMUM = 300` (`constants.pxi:385`) |
| 4 | 2 | service options | `0x0401` = `GSO_DONT_CARE 0x0001` + `GSO_CAN_RECV_ATTENTION 0x0400` |
| 6 | 2 | SDU | 8192 (`connect_params.pyx:44` default) |
| 8 | 2 | TDU | same value; the reference writes `sdu` twice (`connect.pyx:129-130`) |
| 10 | 2 | characteristics | `TNS_PROTOCOL_CHARACTERISTICS = 0x4f98` (`constants.pxi:414`) |
| 12 | 2 | line turnaround | 0 |
| 14 | 2 | constant | 1 |
| 16 | 2 | connect data length | 199 in the capture |
| 18 | 2 | connect data offset | **74**, hard-coded (`connect.pyx:145`) |
| 20 | 4 | max receivable data | 0 |
| 24 | 1 | NSI flags | `0x84` = `SUPPORT_SECURITY_RENEG 0x80` + `NA_DISABLED 0x04` |
| 25 | 1 | NSI flags (repeated) | `0x84` |
| 26 | 8 | obsolete | 0 |
| 34 | 8 | obsolete | 0 |
| 42 | 8 | obsolete | 0 |
| 50 | 4 | SDU (large) | 8192 |
| 54 | 4 | TDU (large) | 8192 |
| 58 | 4 | connect flags 1 | 0 |
| 62 | 4 | connect flags 2 | 1 = `TNS_CONNECT_FLAGS_CHECK_OOB` |
| 66 | 199 | connect data | ASCII `(DESCRIPTION=...))` |

Two traps worth recording:

* **the offset field does not describe the layout.** The body ends at 66, yet the
  reference writes 74. 74 is 66 + 8, i.e. the offset is measured from the start of
  the TNS packet, header included. Nothing in the reference reads this field back,
  so a client that copies the value 74 without the same 66-byte body still works;
  a client that relies on it as a body-relative offset is wrong by 8.
* **a long connect string spills into a second packet.** `connect.pyx:156-159`
  ends the Connect packet and opens a DATA packet when
  `connect_string_len > TNS_MAX_CONNECT_DATA` (230). The Connect packet still
  declares the full 16-bit length. A capture with a 300-character service name
  produced a 74-byte CONNECT (body only up to the flags, `connect_string_len`
  `0x01ed` = 493) followed by a 503-byte DATA packet carrying the descriptor; that
  is vector `connect_long_head` in the tests.

Golden vector `connect` (273 bytes, captured, hex):

```text
0111000001000000013f012c0401200020004f980000000100c7004a000000
00848400000000000000000000000000000000000000000000000000002000
000020000000000000000001284445534352495054494f4e3d284144445245
53533d2850524f544f434f4c3d7463702928484f53543d3132372e302e302e
312928504f52543d3435343239292928434f4e4e4543545f444154413d2853
4552564943455f4e414d453d58455044423129284349443d2850524f475241
4d3d2f7573722f62696e2f707974686f6e332928484f53543d4c49554c494c
54452928555345523d6a6179292928434f4e4e454354494f4e5f49443d534a
71617a6d683050586254642f2f7850646f2b67773d3d292929
```

Its connect data (bytes 74..273 of the packet, 199 bytes) is:

```text
(DESCRIPTION=(ADDRESS=(PROTOCOL=tcp)(HOST=127.0.0.1)(PORT=45429))(CONNECT_DATA=(SERVICE_NAME=XEPDB1)(CID=(PROGRAM=/usr/bin/python3)(HOST=LIULILTE)(USER=jay))(CONNECTION_ID=SJqazmh0PXbTd//xPdo+gw==)))
```

`impl/thin/utils.pyx:80-93` and `impl/base/connect_params.pyx:946-1012` fix the
clause order: `(FAILOVER=OFF)` is omitted when failover is on (default is on, so it
is absent), then `(ADDRESS=...)` or `(ADDRESS_LIST=...)`, then `(CONNECT_DATA=...)`
with `SERVICE_NAME`, `INSTANCE_NAME` or `SID`, `SERVER`, then `CID` (program, host,
user), then `CONNECTION_ID`.

## 8. ACCEPT, REFUSE, REDIRECT

ACCEPT (`impl/thin/messages/connect.pyx:63-89`):

| Offset | Width | Field |
| --- | --- | --- |
| 0 | 2 | protocol version; below 315 is fatal (`TNS_VERSION_MIN_ACCEPTED`) |
| 2 | 2 | protocol options; bit 0x0400 = server can receive attention |
| 4 | 10 | skipped |
| 14 | 1 | NSI flags; bit 0x10 = native encryption required |
| 15 | 9 | skipped |
| 24 | 4 | SDU |
| 28 | 5 | skipped when protocol version >= 318 |
| 33 | 4 | flags2 when version >= 318; 0x02000000 = end-of-response, 0x10000000 = fast auth, 0x00000001 = check OOB |

The 37-byte ACCEPT below was fed to the installed client. It replied with the
marker and Protocol message quoted in section 9, which means every offset above was
read as intended:

```text
013f0400000000000000000000000000000000000000000000002000000000000002000001
```

REFUSE (`impl/thin/messages/connect.pyx:90-112`, `impl/thin/protocol.pyx:298-307`):
two bytes skipped, then `be16 length` and `length` bytes of text. The client looks
for `(ERR=<number>)` in that text and maps 12514 to "service not registered", 12505
to "invalid SID" (`connect.pyx:103-111`).

REDIRECT (`impl/thin/messages/connect.pyx:53-62`): `be16 redirect_data_length`, then
that many bytes of text, which may span packets. The text is
`<connect string>\0<redirect CONNECT_DATA...>`: the client splits at the first NUL
and re-parses the left part as the new address and the right part as the new
connect data (`impl/thin/connection.pyx:251-270`).

## 9. TTC message framing

Every TTC message travels inside DATA packets. The body starts with the message
type, the function code and a sequence number (`impl/thin/messages/base.pyx:786-793`):

```text
u8 message_type ; u8 function_code ; u8 sequence_number
[be64 token]                                   # only from field version 23.1 ext 1
```

The sequence number restarts at 1 and wraps 255 -> 1, never 0
(`impl/thin/packet.pyx:903-907`).

The DataTypes message (`impl/thin/messages/data_types.pyx:696-720`) is:
`u8 type=2`, `le16 charset`, `le16 ncharset`, `u8 encoding flags`, then the compile
capabilities and runtime capabilities as length-prefixed byte strings, then a
table of `(be16 data_type, be16 conv_data_type, be16 representation, be16 0)`
entries terminated by `be16 0`.

The Protocol message (`impl/thin/messages/protocol.pyx:42-47`) is:
`u8 type=1`, `u8 6`, `u8 0`, driver name as ASCII, `u8 0`.

Captured golden vectors (both emitted by the installed client after it accepted the
ACCEPT in section 8):

```text
marker_reset     0000000b0c000000010002
data_protocol    0000001d060000000000010600707974686f6e2d6f7261636c65646200
```

Field split of `data_protocol`: `0000001d` length 29, `06` type DATA, `00` flags,
`0000` reserved, `0000` data flags, `01` message type Protocol, `06` version,
`00` array terminator, `python-oracledb`, `00`.

`marker_reset` is the 8-byte header, type 12, then `01 00 02` — the three-byte
marker body with `RESET`.

## 10. TTC function codes and message types

`impl/thin/constants.pxi:418-450`:

| Code | Name | Used by the reference for |
| --- | --- | --- |
| 4 | `TNS_FUNC_REEXECUTE` | re-execute an already parsed cursor |
| 5 | `TNS_FUNC_FETCH` | plain fetch |
| 9 | `TNS_FUNC_LOGOFF` | logoff |
| 14 | `TNS_FUNC_COMMIT` | commit |
| 15 | `TNS_FUNC_ROLLBACK` | rollback |
| 78 | `TNS_FUNC_REEXECUTE_AND_FETCH` | re-execute with prefetch |
| 94 | `TNS_FUNC_EXECUTE` | execute (parse/bind/define/execute/fetch bits) |
| 105 | `TNS_FUNC_CLOSE_CURSORS` | close cursors piggyback |
| 115 | `TNS_FUNC_AUTH_PHASE_TWO` | O5LOGON phase two |
| 118 | `TNS_FUNC_AUTH_PHASE_ONE` | O5LOGON phase one |
| 147 | `TNS_FUNC_PING` | ping |
| 163 | `TNS_FUNC_SESSION_RELEASE` | DRCP release |
| 199/200 | `TNS_FUNC_PIPELINE_BEGIN/END` | pipelining |

**The task brief assumed Execute = 0x03, Fetch = 0x11 and OALL8 = 0x06. That does not
match the reference.** `grep -rn OALL8` over the whole clone returns nothing: the
thin client has no OALL8 path at all. Execute is 94 (0x5e), Fetch is 5, and 3 is the
`ORA_TYPE_NUM_BINARY_INTEGER` data type number, not a function code
(`base_impl.pxd:122`). This document and the C++ follow the reference; the assumed
codes are wrong and are recorded here so nobody re-introduces them.

Message types (`impl/thin/constants.pxi:179-201`), the byte that starts a TTC body:

| Value | Name |
| --- | --- |
| 1 | `PROTOCOL` |
| 2 | `DATA_TYPES` |
| 3 | `FUNCTION` |
| 4 | `ERROR` |
| 6 | `ROW_HEADER` |
| 7 | `ROW_DATA` |
| 8 | `PARAMETER` |
| 9 | `STATUS` |
| 11 | `IO_VECTOR` |
| 16 | `DESCRIBE_INFO` |
| 17 | `PIGGYBACK` |
| 19 | `FLUSH_OUT_BINDS` |
| 21 | `BIT_VECTOR` |
| 27 | `IMPLICIT_RESULTSET` |
| 29 | `END_OF_RESPONSE` |

## 11. Execute and Fetch bodies

Execute (`impl/thin/messages/execute.pyx:120-208`), after
`type=3, function_code=94, sequence_number`:

| Order | Width | Field | Note |
| --- | --- | --- | --- |
| 1 | ub4 | execute options | see the option table below |
| 2 | ub4 | cursor id | 0 on the first execution |
| 3 | u8 + ub4 | cursor-id pointer | `1` and the SQL length when the cursor id is 0 |
| 4 | u8 + ub4 | vector pointer | `1`, then `13` (the al8i4 array length) |
| 5 | u8 + u8 | pointer, pointer | al8o4, al8o4l, both 0 |
| 6 | ub4 | prefetch buffer size | 0 |
| 7 | ub4 | prefetch rows | prefetchrows on the first execution |
| 8 | ub4 | maximum long size | `TNS_MAX_LONG_LENGTH = 0x7fffffff` |
| 9 | u8 + ub4 | binds pointer, count | `0,0` when there are no binds |
| 10 | u8 x4 | al8app, al8txn, al8txl, al8kv | 0 |
| 11 | u8 + ub4 | defines pointer, count | `1` and the number of defines when defining |
| 12 | ub4 | registration id low | 0 |
| 13 | u8 x3 | al8objlist, al8objlen, al8blv | `0,1,0` |
| 14 | ub4 | al8blvl | 0 |
| 15 | u8 + ub4 | al8dnam pointer, length | 0 |
| 16 | ub4 | registration id high | 0 |
| 17 | u8 + ub4 + u8 | DML row-count pointers | 0,0,0 |
| 18 | u8 + ub4 + u8 + ub4 + u8 + ub4 | SQL signature / SQL id pointers | all 0 when field version >= 12.2 |
| 19 | u8 + ub4 | chunk-id pointer, count | when field version >= 12.2 ext 1 |
| 20 | u8 + bytes | SQL text, length-prefixed | short/long length rules of section 12 |
| 21 | ub4 | al8i4[0] parse | 1 when the cursor is new |
| 22 | ub4 | al8i4[1] execution count | 0 for a query's first execution |
| 23 | ub4 x4 | al8i4[2..5] | 0 |
| 24 | ub4 x2 | al8i4[5..6] SCN | 0 |
| 25 | ub4 | al8i4[7] is-query | 1 for a query |
| 26 | ub4 | al8i4[8] | 0 |
| 27 | ub4 | al8i4[9] execute flags | see below |
| 28 | ub4 | al8i4[10] fetch orientation | 0 |
| 29 | ub4 | al8i4[11] fetch position | 0 |
| 30 | ub4 | al8i4[12] | 0 |

Execute options (`constants.pxi:275-286`): `PARSE 0x01`, `BIND 0x08`,
`DEFINE 0x10`, `EXECUTE 0x20`, `FETCH 0x40`, `COMMIT 0x100`, `PLSQL_BIND 0x400`,
`NOT_PLSQL 0x8000`, `DESCRIBE 0x20000`, `BATCH_ERRORS 0x80000`.
Execute flags (`constants.pxi:288-293`): `DML_ROWCOUNTS 0x4000`,
`IMPLICIT_RESULTSET 0x8000`, `NO_CANCEL_ON_EOF 0x80`, `SCROLLABLE 0x02`.

Fetch (`impl/thin/messages/fetch.pyx:42-49`) is short: `type=3, function_code=5,
sequence_number, ub4 cursor_id, ub4 fetch_array_size`.

Re-execute (`execute.pyx:239-247`): `function_code, ub4 cursor_id, ub4 num_iters,
ub4 options_1, ub4 options_2`, then for each iteration a `TNS_MSG_TYPE_ROW_DATA`
byte and the bind row.

## 12. Data-type numbers and value encoding

`base_impl.pxd:118-142`:

| Number | Name | Number | Name |
| --- | --- | --- | --- |
| 1 | `VARCHAR` | 23 | `RAW` |
| 2 | `NUMBER` | 24 | `LONG_RAW` |
| 3 | `BINARY_INTEGER` | 96 | `CHAR` |
| 8 | `LONG` | 100 | `BINARY_FLOAT` |
| 11 | `ROWID` | 101 | `BINARY_DOUBLE` |
| 12 | `DATE` | 112/113 | `CLOB`/`BLOB` |
| 102 | `CURSOR` | 180/181 | `TIMESTAMP`/`TIMESTAMP_TZ` |
| 208 | `UROWID` | 252 | `BOOLEAN` |

Universal integers (`impl/base/buffer.pyx:737-790`): one length byte, then the
magnitude big-endian, with the length chosen from the value:

| Value range | First byte | Bytes that follow |
| --- | --- | --- |
| 0 | 0 | none |
| 1..255 | 1 | 1 |
| 256..65535 | 2 | 2 |
| 65536..2^32-1 | 4 | 4 |
| above | 8 | 8 |

Signed values set bit 0x80 of the length byte (`write_sb4`, `buffer.pyx:666-684`).
Decoding uses the same length byte and `decode_integer` (`decoders.pyx:112-132`).

Length-prefixed byte strings (`impl/base/buffer.pyx:132-151`, `263-277`):

| Length byte | Meaning |
| --- | --- |
| 0 | empty / null |
| 1..252 | that many bytes follow |
| 254 | chunked: repeated `ub4 chunk_length` + chunk, terminated by `ub4 0` |
| 255 | null indicator |

Oracle NUMBER (`impl/base/decoders.pyx:157-273`): the first byte is the exponent
with a 193 bias; bit 0x80 of it is the sign (negative values are stored as the
one's complement of the exponent and as `101 - digit` for each mantissa byte); each
mantissa byte is a base-100 digit; the decimal point sits at `2 * exponent + 2`
digits into the mantissa. A single-byte value `0x80` is the number zero.

Column metadata (`impl/thin/messages/base.pyx:315-396`) is, in order: `u8 type`,
`u8 flags`, `sb1 precision`, `sb1 scale`, `ub4 buffer size`, `ub4 max array
elements`, `ub8 cont flags`, OID as length-prefixed bytes, `ub2 version`,
`ub2 charset id`, `u8 charset form`, `ub4 max size`, `ub4 oaccolid` (field version
>= 12.2), `u8 nulls allowed`, `u8 v7 name length`, name / schema / type name as
length-prefixed strings, `ub2 column position`, `ub4 UDS flags`, and from 23.1 the
domain schema/name and annotation blocks.

Describe info (`base.pyx:1282-1325`) starts with `ub4 max row size`,
`ub4 column count`, `u8 0` when the count is non-zero, then that many metadata
blocks, then a length-prefixed current date, four `ub4` dcb fields and a
length-prefixed `dcbqcky`.

A row (`base.pyx:1466-1522`) is a sequence of values in column order; a NULL value
is the single length byte 255 (or 0). `ROW_HEADER` (message type 6) carries
`u8 flags`, `ub2 num requests`, `ub4 iteration`, `ub4 num iters`, `ub2 buffer
length`, `ub4 bit-vector length` + bit vector, then a length-prefixed rowid. The
bit vector marks columns whose value repeats the previous row: a **clear** bit
means "duplicate of the previous row" (`base.pyx:1097-1106`).

## 13. What is not verified

* **Everything past the connect handshake.** No Oracle server exists here, so no
  ACCEPT from a real listener, no O5LOGON/OSESSKEY exchange, no Execute reply, no
  row data from a database. The Execute/Fetch encoders are built from the reference
  source and are not compared against bytes from a server.
* **Authentication**, deliberately out of scope (needs a server round trip).
* **Checksummed TNS traffic.** The reference writes zeros; this codec rejects a
  nonzero checksum rather than pretending to verify it.
* **TCPS, redirect round trip, OOB break.** Redirect parsing is implemented from
  source; no capture of a real redirect exists here.
* **Character sets other than UTF-8/ASCII**, LOBs, arrays, pipelining, DRCP.
* **Anything about a real database's replies**, including whether it accepts the
  Execute skeleton: the field order matches the reference, but the reference also
  negotiates capabilities in DataTypes that this codec does not send.

## 14. Reproducing the captures

The scripts run inside WSL and need the installed reference client.

```text
# 1. connect packet + the two packets the client sends after a crafted ACCEPT
python3 /tmp/ttc/capture_connect.py
python3 /tmp/ttc/capture_after_accept2.py

# 2. long connect string spilling into a DATA packet
python3 /tmp/ttc/capture_long.py

# 3. REFUSE handling
python3 /tmp/ttc/refuse_test2.py
```

`capture_connect.py` binds a listener on port 0, records the first packet and
closes; `capture_after_accept2.py` replies with the 37-byte ACCEPT and records the
next two packets. Both print `name length hex`.