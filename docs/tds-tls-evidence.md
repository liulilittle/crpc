# TDS over TLS: the wire evidence

[简体中文](tds-tls-evidence.zh-CN.md) | **English**

This file exists because the TDS adapter refused encryption for two rounds with a
written-off diagnosis ("the payload must differ", "the record framing must be
wrong"). Neither was true. The difference was the **framing of the TLS records
themselves**, and it was found by comparing full ciphertext of a working client
with ours, record by record.

## The rule TDS actually uses

| phase | what goes on the wire |
| --- | --- |
| TLS handshake (ClientHello … Finished) | each TLS record is the **body of a TDS PRELOGIN packet**: type `0x12`, status `0x01` (EOM), a normal 8-byte TDS header |
| after the handshake | **bare TLS records**: record type `0x16`/`0x14`/`0x17`/`0x15` straight to the socket, **no TDS header at all** |
| inside the TLS stream | ordinary TDS packet framing resumes (LOGIN7 `0x10`, SQL batch `0x01`, replies `0x04`) |

`0x17` is the **TLS** content type for application data. It is not a TDS packet
type, and `packet_type::ssl = 0x17` — which this repository carried until this
commit, with a comment claiming the record is carried *inside* a TDS packet —
was the wrong model and one of the reasons the failure was misdiagnosed twice. A
post-handshake TDS header in front of the record shifts every byte of the
encrypted LOGIN7 by eight and SQL Server answers
`17832, Severity 20, State 2 — The login packet used to open the connection is
structurally invalid`.

## Method

`pytds` (a known-working client) and this adapter were both pointed at a
byte-copying TCP proxy in front of SQL Server 2022 on `127.0.0.1:1433`, and the
complete bidirectional ciphertext was dumped and parsed. Only framing metadata
was compared (record types, lengths, extensions, cipher suite, boundaries); no
key material was needed because the framing is visible in the clear.

## Client → server, record by record

### pytds (working reference), 702 bytes

| # | TDS header | inner TLS records |
| --- | --- | --- |
| 1 | `12 01 00 3a 00 00 00 00` PRELOGIN, len 58, id 0 | — (plain PRELOGIN: version, encryption, thread id, MARS, MSSQLServer instopt) |
| 2 | `12 01 00 db 00 00 01 00` PRELOGIN, len 219, id 1 | `16 03 01 00 ce` handshake **ClientHello**, 206 bytes |
| 3 | `12 01 00 65 00 00 02 00` PRELOGIN, len 101, id 2 | `16 03 03 00 25` **ClientKeyExchange** 37 B, `14 03 03 00 01` **ChangeCipherSpec** 1 B, `16 03 03 00 28` **Finished** 40 B |
| 4 | *no header* | `17 03 03 00 f4` application data, 244 B (LOGIN7) |
| 5 | *no header* | `17 03 03 00 46` application data, 70 B (SQL batch) |

### the previous implementation (rejected with 17832)

The earlier code was deleted before this comparison, so it has no capture of its
own. What it did is recorded in the comment it left behind, which this commit
removes: `packet_type::ssl = 0x17` was documented as "a TLS record carried inside
a TDS packet, which is how encryption is framed after the handshake". That is
precisely the 8-byte shift described above, and it matches the server's
`State 2` rejection of a LOGIN7 that the byte comparison had already proven
correct.

### ours after the fix, 786 bytes

| # | TDS header | inner TLS records |
| --- | --- | --- |
| 1 | `12 01 00 29 00 00 01 00` PRELOGIN, len 41, id 1 | — (plain PRELOGIN) |
| 2 | `12 01 00 de 00 00 02 00` PRELOGIN, len 222, id 2 | `16 03 01 00 d1` ClientHello, 209 B |
| 3 | `12 01 00 65 00 00 03 00` PRELOGIN, len 101, id 3 | `16 03 03 00 25` CKE 37 B, `14 03 03 00 01` CCS 1 B, `16 03 03 00 28` Finished 40 B |
| 4 | *no header* | `17 03 03 00 d4` application data, 212 B (LOGIN7) |
| 5 | *no header* | `17 03 03 00 52` application data, 82 B (SELECT 42) |
| 6 | *no header* | `17 03 03 00 52` application data, 82 B (SELECT 43) |
| 7 | *no header* | `15 03 03 00 1a` alert, 26 B (close_notify) |

The one structural difference that remains is intentional and harmless: the
plain PRELOGIN is 41 bytes here versus 58 in pytds (pytds sends an INSTOPT option
we do not), and the packet ids start at 1 instead of 0. Both were already proven
irrelevant in the earlier rounds — a plaintext session with the same PRELOGIN and
the same id base logs in successfully — and the encrypted session now does too.

## Server → client, record by record

### pytds

| # | TDS header | inner TLS records |
| --- | --- | --- |
| 1 | `04 01 00 2b ...` tabular result, len 43 | — (plain PRELOGIN reply; encryption = `0x01`) |
| 2 | `12 01 04 d7 ...` PRELOGIN, len 1239, id 0 | ServerHello 65 B, Certificate 842 B, ServerKeyExchange 300 B, ServerHelloDone 4 B |
| 3 | `12 01 00 ea ...` PRELOGIN, len 234, id 0 | Finished 170 B, CCS 1 B, Finished 40 B |
| 4 | *no header* | `17 03 03 01 99` 409 B, `17 03 03 00 3d` 61 B |

### ours after the fix, 2060 bytes

| # | TDS header | inner TLS records |
| --- | --- | --- |
| 1 | `04 01 00 25 ...` tabular result, len 37 | — (plain PRELOGIN reply; encryption = `0x01`) |
| 2 | `12 01 04 d7 ...` PRELOGIN, len 1239, id 0 | same four handshake messages, same lengths (65/842/300/4) |
| 3 | `12 01 00 ea ...` PRELOGIN, len 234, id 0 | Finished 170 B, CCS 1 B, Finished 40 B |
| 4 | *no header* | `17 03 03 01 99` 409 B (login ack), `17 03 03 00 3f` 63 B, `17 03 03 00 3f` 63 B |

The 63-byte records are the two `SELECT` replies; pytds has one 61-byte record
because it sends one query.

## ClientHello comparison

Both clients produce the same 27 cipher suites in the same order
(`c02c c030 009f cca9 cca8 ccaa c02b c02f 009e c024 c028 006b c023 c027 0067
c00a c014 0039 c009 c013 0033 009d 009c 003d 003c 0035 002f`), legacy version
`03 03`, empty session id, compression method `00`, and the same extension set:

| extension | pytds | ours |
| --- | --- | --- |
| `renegotiation_info` (65281) | len 1 | len 1 |
| `ec_point_formats` (11) | `01 00` | `01 00` |
| `supported_groups` (10) | 7 groups `001d 0017 001e 0018 0019 0100 0101` | identical |
| `session_ticket` (35) | len 0 | len 0 |
| `encrypt_then_mac` (22) | len 0 | len 0 |
| `extended_master_secret` (23) | len 0 | len 0 |
| `signature_algorithms` (13) | the same 20 pairs | identical |
| `server_name` (0) | `127.0.0.1` (14 B) | `172.28.18.36` (17 B — the address actually dialled) |

No ALPN, no `supported_versions`, no compression: both are pinned to TLS 1.2,
which is what SQL Server's TDS endpoint negotiates. The extension-by-extension
equality is why the handshake never was the problem.

## Conclusion of the comparison

Three hypotheses were tested and the first two were refuted by evidence:

1. ~~The LOGIN7 payload differs~~ — refuted earlier: the plaintext is
   byte-identical to the working client's, 156 bytes, hex equal.
2. ~~The packet id base differs~~ — refuted earlier: both bases were tried.
3. **The post-handshake record framing differs** — confirmed here: the record
   must go to the socket bare, not inside a TDS packet.

## Server-side verification

The client's own `encryption_negotiated()` flag is not evidence that the server
agrees. Run on the same session over which the queries above ran:

```sql
SELECT encrypt_option, protocol_type, auth_scheme
FROM sys.dm_exec_connections WHERE session_id = @@SPID;
```

```
encrypt_option = TRUE    protocol_type = TSQL    auth_scheme = SQL
```

`encrypt_option` is SQL Server's own per-connection verdict, so encryption is
confirmed by the peer and not only by our state machine. The integration test
`tds_it_require_encryption_logs_in_and_queries` asserts all three columns, so a
regression in the framing fails the suite with the server's answer in the log.