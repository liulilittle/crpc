# Oracle TNS/TTC 线级证据

English: [oracle-ttc-evidence.md](oracle-ttc-evidence.md)

## 1. 本文档的地位

本文记录 `include/crpc/db/oracle/` 所实现的线级事实，并逐条说明出处。它是工作记录，不是规范：Oracle
从未公开 TNS/TTC 规范。

**端到端未验证。本机没有 Oracle 服务端**（无 Docker、无监听、无实例），因此这里没有任何一条经过真实数据库
检验。唯一依据是 Oracle 自己维护的 `python-oracledb`：其 thin 模式是同一协议的完整客户端实现。本仓库的代码
与它的源码逐字段核对；凡参考客户端能在本地吐出字节的地方，还与它实际产生的字节做了差分。

`include/crpc/db/connection.hpp` 里仍然没有 Oracle 后端。现在存在的是编解码器；要成为适配器必须先完成认证
（O5LOGON/OSESSKEY），而认证在这里无法验证。任何「CRPC 能连 Oracle」的说法都是假的。

## 2. 参考实现

* 仓库：<https://github.com/oracle/python-oracledb>
* 克隆：`git clone --depth 1 https://github.com/oracle/python-oracledb.git /tmp/podb`
* 该克隆 `git log -1`：`c869687e857bf40cb7b3ac29684a0da35dcbfc65 2026-10-27`
* 版本：`src/oracledb/version.py` 为 `26.1.0`
* 已在 WSL Ubuntu 24.04 中编译安装，用于观察其真实行为：

```text
apt-get install -y cython3 python3-setuptools python3-pip
pip3 install -U 'cython>=3.1' 'setuptools>=77' 'packaging>=24.2'
git -C /tmp/podb submodule update --init --depth 1        # odpi，setup.py 需要
pip3 install --no-build-isolation --break-system-packages /tmp/podb
```

安装成功（`Successfully installed oracledb-26.1.0`），所以下文的结论属于**差分**，不是人工阅读结论。
本文引用路径均相对于 `/tmp/podb/src/oracledb/`。

## 3. 方法

1. **读源码**：从第 4 节列出的 Cython 文件里逐条取出常量与字段写入顺序，记下文件与行号。
2. **差分抓包**：用一个 Python 假监听接受已安装客户端的真实 `oracledb.connect(...)`，回一个构造好的
   ACCEPT，记录客户端随后发出的每个包。脚本见第 14 节，抓到的字节就是
   `tests/test_db_oracle_tns.cpp` 使用的黄金样本。
3. **交叉验证**：构造的 ACCEPT 并非来自参考实现（监听属于服务端），但参考客户端**成功解析并继续**了，
   这构成 ACCEPT 布局的红绿检查中「绿」的那一半：偏移错一位就会抛异常或不再发协议消息。

这是分帧与连接阶段的字节级差分证据，**不是**真实数据库应答的证据，也与认证、查询结果无关。

## 4. 通读的文件

| 文件 | 行 | 确立的事实 |
| --- | --- | --- |
| `impl/thin/constants.pxi` | 34-59 | 包类型、包标志、data 标志、marker 类型 |
| `impl/thin/constants.pxi` | 179-201 | TTC 消息类型 |
| `impl/thin/constants.pxi` | 275-293 | execute 选项与 execute 标志 |
| `impl/thin/constants.pxi` | 384-414 | 协议版本、连接标志、`TNS_PROTOCOL_CHARACTERISTICS` |
| `impl/thin/constants.pxi` | 418-450 | TTC 函数码 |
| `impl/thin/constants.pxi` | 475-586 | capability 偏移与标志 |
| `impl/thin/constants.pxi` | 606-608 | ACCEPT 标志位 |
| `impl/thin/constants.pxi` | 737 | `TNS_MAX_CONNECT_DATA = 230` |
| `impl/thin/packet.pyx` | 34-35 | 包头 8 字节、DATA 头 10 字节 |
| `impl/thin/packet.pyx` | 59-74 | 由 data 标志判断响应结束 |
| `impl/thin/packet.pyx` | 773-800 | 包头写入顺序、大/小两种布局 |
| `impl/thin/packet.pyx` | 812-838 | `end_request`、`start_request`、data 标志槽位 |
| `impl/thin/packet.pyx` | 330-346 | control 包体 |
| `impl/thin/packet.pyx` | 359-380 | 保留哪些包、响应结束规则 |
| `impl/thin/packet.pyx` | 392-404 | DATA 包体前两字节是 data 标志 |
| `impl/thin/transport.pyx` | 334-390 | `extract_packet`：长度字段、类型字节、标志字节 |
| `impl/base/buffer.pyx` | 132-151 | 数据长度编码：单字节或 254 + 分块 |
| `impl/base/buffer.pyx` | 737-790 | 通用整数编码（ub2/ub4/ub8） |
| `impl/base/buffer.pyx` | 372-420 | 通用整数解码 |
| `impl/base/buffer.pyx` | 263-277 | 带长度读取与 255 空值标记 |
| `impl/base/decoders.pyx` | 112-132 | 大端整数解码 |
| `impl/base/decoders.pyx` | 157-273 | Oracle NUMBER 解码 |
| `impl/base/connect_params.pyx` | 40-44 | 默认协议、端口 1521、SDU 8192 |
| `impl/base/connect_params.pyx` | 946-1012 | 连接描述符各子句的拼装顺序 |
| `impl/thin/messages/connect.pyx` | 47-112 | Connect/Accept/Redirect/Refuse 解析 |
| `impl/thin/messages/connect.pyx` | 114-160 | Connect 包体字段顺序 |
| `impl/thin/messages/base.pyx` | 286-313 | 消息分发 |
| `impl/thin/messages/base.pyx` | 786-793 | 消息头：类型、函数码、序号 |
| `impl/thin/messages/base.pyx` | 928-938 | 每条消息装入 DATA 包的方式 |
| `impl/thin/messages/base.pyx` | 315-396 | 列元数据布局 |
| `impl/thin/messages/base.pyx` | 1282-1325 | describe-info 布局 |
| `impl/thin/messages/base.pyx` | 1410-1432 | 数据类消息处理的类型 |
| `impl/thin/messages/base.pyx` | 1466-1522 | 行数据、bit vector、row header |
| `impl/thin/messages/execute.pyx` | 63-208 | Execute 包体字段顺序 |
| `impl/thin/messages/execute.pyx` | 210-248 | re-execute 包体字段顺序 |
| `impl/thin/messages/fetch.pyx` | 36-49 | Fetch 包体：游标号、数组大小 |
| `impl/thin/messages/protocol.pyx` | 42-47 | Protocol 消息包体 |
| `impl/thin/messages/protocol.pyx` | 58-92 | Protocol 应答解析 |
| `impl/thin/messages/data_types.pyx` | 696-720 | DataTypes 消息包体 |
| `base_impl.pxd` | 118-151 | `ORA_TYPE_NUM_*`、254/255 长度标记 |

## 5. TNS 包头

包头 8 字节，有**两种布局**，由协商出的协议版本决定：低于 315 时首字段是 16 位长度加 16 位校验和；
从 315（12.1）起是 32 位长度，校验和字消失。`impl/thin/packet.pyx:775-786` 的写法：

```text
协议版本 >= TNS_VERSION_MIN_LARGE_SDU (315):
    be32 长度 ; u8 类型 ; u8 标志 ; be16 0        # 4+1+1+2 = 8
否则:
    be16 长度 ; be16 校验和 ; u8 类型 ; u8 标志 ; be16 0   # 2+2+1+1+2 = 8
```

`impl/thin/transport.pyx:361-367` 在 `_full_packet_size` 置位时（`impl/thin/messages/connect.pyx:89`
在收到 ACCEPT 后置位）用 `decode_uint32be` 读长度，否则用 `decode_uint16be`；
`transport.pyx:373-374` 取偏移 4 的类型字节和偏移 5 的标志字节 —— 两种布局下偏移相同，因为长度/校验和
的互换保持类型之前总共 4 字节。

| 偏移 | 小布局（< 315） | 大布局（>= 315） | 说明 |
| --- | --- | --- | --- |
| 0 | 长度高 | 长度第 3 字节 | 含包头在内的整包长度 |
| 1 | 长度低 | 长度第 2 字节 | |
| 2 | 校验和高 | 长度第 1 字节 | 校验和只在小布局存在 |
| 3 | 校验和低 | 长度第 0 字节 | |
| 4 | 类型 | 类型 | 见第 6 节 |
| 5 | 标志 | 标志 | 见第 6 节 |
| 6 | 0 | 0 | 保留 / 头校验 |
| 7 | 0 | 0 | 保留 / 头校验 |

要点：

* 长度**包含包头自身**，任何包都不短于 8 字节；
* 参考实现对校验和与保留字一律写 **0**（`packet.pyx:787` 写 `write_uint16be(0)`），它从不计算也不校验
  校验和，所以开启 TNS 校验和的对端参考实现自己也不支持；
* DATA 包体在包头后多两字节（`packet.pyx:34-35`、`packet.pyx:788-790`），因此包体从偏移 10 开始。
  这两字节是 16 位 data 标志，多包请求的**每一个**包都会写（`packet.pyx:773-800` 是逐包执行的）。

## 6. 包类型、标志、marker、control

`impl/thin/constants.pxi:34-41`：

| 值 | 名称 | 含义 |
| --- | --- | --- |
| 1 | CONNECT | 客户端 -> 监听的首包 |
| 2 | ACCEPT | 监听应答，带协议版本与 SDU |
| 4 | REFUSE | 监听拒绝，包体是文本错误 |
| 5 | REDIRECT | 监听重定向 |
| 6 | DATA | 握手之后的全部 TTC 流量 |
| 11 | RESEND | 请求调整大小 |
| 12 | MARKER | break / reset / interrupt |
| 14 | CONTROL | 带外与带内通知 |

包标志（`constants.pxi:19-22`）：`REDIRECT = 0x04`、`TLS_RENEG = 0x08`。

data 标志（`constants.pxi:24-29`），取自包体前两字节：

| 值 | 名称 |
| --- | --- |
| 0x0040 | `EOF`（会话关闭） |
| 0x0800 | `END_OF_REQUEST` |
| 0x1000 | `BEGIN_PIPELINE` |
| 0x2000 | `END_OF_RESPONSE` |

marker 类型（`constants.pxi:31-35`）：`BREAK = 1`、`RESET = 2`、`INTERRUPT = 3`。marker 包体恰好三字节
`01 00 <marker>`（`impl/thin/protocol.pyx:99-108`）。

control 类型（`constants.pxi:392-394`）：`INBAND_NOTIFICATION = 8`、`RESET_OOB = 9`。包体是
`be16 control_type`，带内通知时还有要跳过的 32 位整数与 `be32` 错误码（`packet.pyx:330-346`）。

## 7. Connect 包：布局与黄金样本

`impl/thin/messages/connect.pyx:114-160` 按以下顺序写包体，多字节字段一律大端。

| 包体偏移 | 宽度 | 字段 | 出处 / 抓包中的值 |
| --- | --- | --- | --- |
| 0 | 2 | 期望协议版本 | `319`（`constants.pxi:384`） |
| 2 | 2 | 最低协议版本 | `300`（`constants.pxi:385`） |
| 4 | 2 | service options | `0x0401` = `DONT_CARE 0x0001` + `CAN_RECV_ATTENTION 0x0400` |
| 6 | 2 | SDU | 8192（`connect_params.pyx:44` 默认） |
| 8 | 2 | TDU | 同 SDU；参考实现把 sdu 写两遍（`connect.pyx:129-130`） |
| 10 | 2 | 特征字 | `0x4f98`（`constants.pxi:414`） |
| 12 | 2 | line turnaround | 0 |
| 14 | 2 | 常量 | 1 |
| 16 | 2 | connect data 长度 | 抓包为 199 |
| 18 | 2 | connect data 偏移 | **74**，硬编码（`connect.pyx:145`） |
| 20 | 4 | 最大可接收数据 | 0 |
| 24 | 1 | NSI 标志 | `0x84` = `SUPPORT_SECURITY_RENEG 0x80` + `NA_DISABLED 0x04` |
| 25 | 1 | NSI 标志（重复） | `0x84` |
| 26 | 8 | 废弃 | 0 |
| 34 | 8 | 废弃 | 0 |
| 42 | 8 | 废弃 | 0 |
| 50 | 4 | SDU（大） | 8192 |
| 54 | 4 | TDU（大） | 8192 |
| 58 | 4 | connect flags 1 | 0 |
| 62 | 4 | connect flags 2 | 1 = `CHECK_OOB` |
| 66 | 199 | connect data | ASCII `(DESCRIPTION=...))` |

两个必须记下的坑：

* **偏移字段并不描述实际布局。** 包体到 66 结束，参考实现却写 74。74 = 66 + 8，即该偏移是从 TNS 包
  起始（含包头）算的。参考实现从不回读该字段，所以照抄 74 而包体同为 66 字节的客户端仍能工作；把它当成
  包体相对偏移的客户端会差 8 字节。
* **过长的连接串会溢出到第二个包。** 当 `connect_string_len > TNS_MAX_CONNECT_DATA`（230）时
  `connect.pyx:156-159` 结束 Connect 包并另起 DATA 包。Connect 包仍声明完整的 16 位长度。用 300 字符
  service name 抓到的结果是：74 字节 CONNECT（`connect_string_len` 为 `0x01ed` = 493，包体只到 flags），
  紧跟一个 503 字节的 DATA 包携带描述符；测试中的 `connect_long_head` 向量即此。

黄金样本 `connect`（273 字节，抓包，十六进制）：

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

其 connect data（整包第 74..273 字节，共 199 字节）为：

```text
(DESCRIPTION=(ADDRESS=(PROTOCOL=tcp)(HOST=127.0.0.1)(PORT=45429))(CONNECT_DATA=(SERVICE_NAME=XEPDB1)(CID=(PROGRAM=/usr/bin/python3)(HOST=LIULILTE)(USER=jay))(CONNECTION_ID=SJqazmh0PXbTd//xPdo+gw==)))
```

`impl/thin/utils.pyx:80-93` 与 `impl/base/connect_params.pyx:946-1012` 固定了子句顺序：failover 为真时
省略 `(FAILOVER=OFF)`（默认即为真，故样本中没有），随后 `(ADDRESS=...)` 或 `(ADDRESS_LIST=...)`，
再 `(CONNECT_DATA=...)`，内含 `SERVICE_NAME`、`INSTANCE_NAME` 或 `SID`、`SERVER`，然后是
`CID`（program、host、user），最后 `CONNECTION_ID`。

## 8. ACCEPT、REFUSE、REDIRECT

ACCEPT（`impl/thin/messages/connect.pyx:63-89`）：

| 偏移 | 宽度 | 字段 |
| --- | --- | --- |
| 0 | 2 | 协议版本；低于 315 直接判失败 |
| 2 | 2 | 协议选项；0x0400 表示服务端可接收 attention |
| 4 | 10 | 跳过 |
| 14 | 1 | NSI 标志；0x10 表示要求原生加密 |
| 15 | 9 | 跳过 |
| 24 | 4 | SDU |
| 28 | 5 | 协议版本 >= 318 时跳过 |
| 33 | 4 | 版本 >= 318 时的 flags2；0x02000000 响应结束、0x10000000 快速认证、0x00000001 检查带外 |

下面 37 字节的 ACCEPT 已喂给已安装客户端，它随后发出了第 9 节引用的 marker 与 Protocol 消息，说明上述
每个偏移都被按预期读到了：

```text
013f0400000000000000000000000000000000000000000000002000000000000002000001
```

REFUSE（`connect.pyx:90-112`、`impl/thin/protocol.pyx:298-307`）：先跳两字节，再 `be16 长度` 与
该长度的文本。客户端在文本中找 `(ERR=<数字>)`，12514 映射为「service 未注册」、12505 映射为「SID 无效」
（`connect.pyx:103-111`）。

REDIRECT（`connect.pyx:53-62`）：`be16 长度` 后跟该长度的文本，可以跨包。文本形如
`<连接串>\0<重定向 CONNECT_DATA>`：客户端在第一个 NUL 处切开，左半重新解析为地址，右半作为新的
connect data（`impl/thin/connection.pyx:251-270`）。

## 9. TTC 消息分帧

所有 TTC 消息都装在 DATA 包里。包体以消息类型、函数码、序号开头（`impl/thin/messages/base.pyx:786-793`）：

```text
u8 消息类型 ; u8 函数码 ; u8 序号
[be64 token]                                   # 仅字段版本 23.1 ext 1 起
```

序号从 1 开始，255 之后回到 1，永不取 0（`impl/thin/packet.pyx:903-907`）。

DataTypes 消息（`impl/thin/messages/data_types.pyx:696-720`）为：`u8 类型=2`、`le16 字符集`、
`le16 国家字符集`、`u8 编码标志`，随后是带长度前缀的 compile capabilities 与 runtime capabilities，
再是 `(be16 data_type, be16 conv_data_type, be16 representation, be16 0)` 表，末尾 `be16 0` 终止。

Protocol 消息（`impl/thin/messages/protocol.pyx:42-47`）为：`u8 类型=1`、`u8 6`、`u8 0`、驱动名 ASCII、
`u8 0`。

抓包向量（均为已安装客户端在第 8 节 ACCEPT 之后实际发出）：

```text
marker_reset     0000000b0c000000010002
data_protocol    0000001d060000000000010600707974686f6e2d6f7261636c65646200
```

`data_protocol` 字段切分：`0000001d` 长度 29、`06` 类型 DATA、`00` 标志、`0000` 保留、
`0000` data 标志、`01` 消息类型 Protocol、`06` 版本、`00` 数组终止符、`python-oracledb`、`00`。

`marker_reset` 是 8 字节包头、类型 12，随后 `01 00 02` —— 三字节包体，marker 为 `RESET`。

## 10. TTC 函数码与消息类型

`impl/thin/constants.pxi:418-450`：

| 码 | 名称 | 参考实现中的用途 |
| --- | --- | --- |
| 4 | `REEXECUTE` | 重执行已解析游标 |
| 5 | `FETCH` | 普通取数 |
| 9 | `LOGOFF` | 登出 |
| 14 | `COMMIT` | 提交 |
| 15 | `ROLLBACK` | 回滚 |
| 78 | `REEXECUTE_AND_FETCH` | 带预取的重执行 |
| 94 | `EXECUTE` | 执行（parse/bind/define/execute/fetch 位） |
| 105 | `CLOSE_CURSORS` | 关闭游标 piggyback |
| 115 | `AUTH_PHASE_TWO` | O5LOGON 第二阶段 |
| 118 | `AUTH_PHASE_ONE` | O5LOGON 第一阶段 |
| 147 | `PING` | ping |
| 163 | `SESSION_RELEASE` | DRCP 释放 |
| 199/200 | `PIPELINE_BEGIN/END` | 流水线 |

**任务书假设 Execute = 0x03、Fetch = 0x11、OALL8 = 0x06，与参考实现不符。** 在整个克隆里
`grep -rn OALL8` 没有任何结果：thin 客户端根本没有 OALL8 这条路径。Execute 是 94（0x5e），Fetch 是 5，
而 3 是 `ORA_TYPE_NUM_BINARY_INTEGER` 的数据类型号，不是函数码（`base_impl.pxd:122`）。本文档与 C++
以参考实现为准；被推翻的假设记录在此，防止有人再把它加回来。

消息类型（`impl/thin/constants.pxi:179-201`），即 TTC 包体的首字节：

| 值 | 名称 |
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

## 11. Execute 与 Fetch 包体

Execute（`impl/thin/messages/execute.pyx:120-208`），在 `类型=3, 函数码=94, 序号` 之后：

| 顺序 | 宽度 | 字段 | 说明 |
| --- | --- | --- | --- |
| 1 | ub4 | execute 选项 | 见下表 |
| 2 | ub4 | 游标号 | 首次执行为 0 |
| 3 | u8 + ub4 | 游标号指针 | 游标号为 0 时写 `1` 与 SQL 长度 |
| 4 | u8 + ub4 | vector 指针 | `1`，随后 `13`（al8i4 数组长度） |
| 5 | u8 + u8 | 两个指针 | al8o4、al8o4l，均为 0 |
| 6 | ub4 | 预取缓冲大小 | 0 |
| 7 | ub4 | 预取行数 | 首次执行为 prefetchrows |
| 8 | ub4 | 最大 long 长度 | `0x7fffffff` |
| 9 | u8 + ub4 | binds 指针与数量 | 无绑定时 `0,0` |
| 10 | u8 x4 | al8app、al8txn、al8txl、al8kv | 0 |
| 11 | u8 + ub4 | defines 指针与数量 | 需要 define 时 `1` 与 define 数 |
| 12 | ub4 | registration id 低位 | 0 |
| 13 | u8 x3 | al8objlist、al8objlen、al8blv | `0,1,0` |
| 14 | ub4 | al8blvl | 0 |
| 15 | u8 + ub4 | al8dnam 指针与长度 | 0 |
| 16 | ub4 | registration id 高位 | 0 |
| 17 | u8 + ub4 + u8 | DML 行数相关指针 | 0,0,0 |
| 18 | u8 + ub4 + u8 + ub4 + u8 + ub4 | SQL 签名 / SQL id 指针 | 字段版本 >= 12.2 时全 0 |
| 19 | u8 + ub4 | chunk-id 指针与数量 | 字段版本 >= 12.2 ext 1 |
| 20 | u8 + bytes | SQL 文本，带长度前缀 | 长度规则见第 12 节 |
| 21 | ub4 | al8i4[0] parse | 新游标为 1 |
| 22 | ub4 | al8i4[1] 执行次数 | 查询首次执行为 0 |
| 23 | ub4 x4 | al8i4[2..5] | 0 |
| 24 | ub4 x2 | al8i4[5..6] SCN | 0 |
| 25 | ub4 | al8i4[7] 是否查询 | 查询为 1 |
| 26 | ub4 | al8i4[8] | 0 |
| 27 | ub4 | al8i4[9] execute 标志 | 见下表 |
| 28 | ub4 | al8i4[10] 取数方向 | 0 |
| 29 | ub4 | al8i4[11] 取数位置 | 0 |
| 30 | ub4 | al8i4[12] | 0 |

execute 选项（`constants.pxi:275-286`）：`PARSE 0x01`、`BIND 0x08`、`DEFINE 0x10`、`EXECUTE 0x20`、
`FETCH 0x40`、`COMMIT 0x100`、`PLSQL_BIND 0x400`、`NOT_PLSQL 0x8000`、`DESCRIBE 0x20000`、
`BATCH_ERRORS 0x80000`。execute 标志（`constants.pxi:288-293`）：`DML_ROWCOUNTS 0x4000`、
`IMPLICIT_RESULTSET 0x8000`、`NO_CANCEL_ON_EOF 0x80`、`SCROLLABLE 0x02`。

Fetch（`impl/thin/messages/fetch.pyx:42-49`）很短：`类型=3, 函数码=5, 序号, ub4 游标号,
ub4 取数数组大小`。

Re-execute（`execute.pyx:239-247`）：`函数码, ub4 游标号, ub4 迭代次数, ub4 选项 1, ub4 选项 2`，
随后每次迭代一个 `ROW_DATA` 字节与一行绑定值。

## 12. 数据类型号与取值编码

`base_impl.pxd:118-142`：

| 号 | 名称 | 号 | 名称 |
| --- | --- | --- | --- |
| 1 | `VARCHAR` | 23 | `RAW` |
| 2 | `NUMBER` | 24 | `LONG_RAW` |
| 3 | `BINARY_INTEGER` | 96 | `CHAR` |
| 8 | `LONG` | 100 | `BINARY_FLOAT` |
| 11 | `ROWID` | 101 | `BINARY_DOUBLE` |
| 12 | `DATE` | 112/113 | `CLOB`/`BLOB` |
| 102 | `CURSOR` | 180/181 | `TIMESTAMP`/`TIMESTAMP_TZ` |
| 208 | `UROWID` | 252 | `BOOLEAN` |

通用整数（`impl/base/buffer.pyx:737-790`）：一个长度字节，后跟大端数值，长度由取值决定：

| 取值范围 | 首字节 | 后续字节数 |
| --- | --- | --- |
| 0 | 0 | 无 |
| 1..255 | 1 | 1 |
| 256..65535 | 2 | 2 |
| 65536..2^32-1 | 4 | 4 |
| 更大 | 8 | 8 |

有符号值时长度字节置 0x80（`write_sb4`，`buffer.pyx:666-684`）。解码用同一长度字节与
`decode_integer`（`decoders.pyx:112-132`）。

带长度字节串（`buffer.pyx:132-151`、`263-277`）：

| 长度字节 | 含义 |
| --- | --- |
| 0 | 空 / 空值 |
| 1..252 | 后跟该字节数 |
| 254 | 分块：重复 `ub4 块长` + 块，`ub4 0` 终止 |
| 255 | 空值标记 |

Oracle NUMBER（`decoders.pyx:157-273`）：首字节是指数，偏置 193；其 0x80 位是符号（负数把指数取反，
每个尾数字节存 `101 - digit`）；每个尾数字节是两位十进制；小数点位于尾数第 `2 * 指数 + 2` 位。

列元数据（`impl/thin/messages/base.pyx:315-396`）依次为：`u8 类型`、`u8 标志`、`sb1 精度`、
`sb1 标度`、`ub4 缓冲大小`、`ub4 最大数组元素`、`ub8 cont flags`、带长度前缀的 OID、`ub2 版本`、
`ub2 字符集 id`、`u8 字符集形式`、`ub4 最大长度`、`ub4 oaccolid`（字段版本 >= 12.2）、
`u8 允许空值`、`u8 v7 名字长度`、带长度前缀的名字/模式/类型名、`ub2 列位置`、`ub4 UDS 标志`，
23.1 起还有 domain schema/name 与注解块。

describe-info（`base.pyx:1282-1325`）以 `ub4 最大行长`、`ub4 列数` 开头，列数非零时再一个 `u8 0`，
随后同样数量的元数据块，然后带长度前缀的当前日期、四个 `ub4` dcb 字段与带长度前缀的 `dcbqcky`。

一行（`base.pyx:1466-1522`）是按列顺序排列的取值序列；空值是单个长度字节 255（或 0）。
`ROW_HEADER`（消息类型 6）带 `u8 标志`、`ub2 请求数`、`ub4 迭代号`、`ub4 迭代数`、
`ub2 缓冲长度`、`ub4 bit vector 长度` 加 bit vector，再是带长度前缀的 rowid。bit vector 标记与上一行
重复的列：**位为 0** 表示「复制上一行」（`base.pyx:1097-1106`）。

## 13. 未验证的部分

* **握手指令之后的一切。** 本机没有 Oracle 服务端，因此没有真实监听的 ACCEPT、没有 O5LOGON/OSESSKEY
  往返、没有 Execute 应答、没有来自数据库的行数据。Execute/Fetch 编码器依据参考源码写成，未与服务端字节
  比对。
* **认证**，刻意不在范围内（需要服务端往返）。
* **带校验和的 TNS 流量。** 参考实现写 0；本编解码器对非零校验和直接报错，而不假装校验。
* **TCPS、重定向往返、带外 break。** 重定向解析依源码实现，本地没有真实重定向抓包。
* **UTF-8/ASCII 之外的字符集**、LOB、数组、流水线、DRCP。
* **真实数据库应答的一切**，包括它是否接受该 Execute 骨架：字段顺序与参考一致，但参考实现还会在
  DataTypes 里协商 capabilities，本编解码器不发送。

## 14. 复现抓包

脚本在 WSL 内运行，需要已安装的参考客户端。

```text
# 1. Connect 包 + 构造 ACCEPT 后客户端发出的两个包
python3 /tmp/ttc/capture_connect.py
python3 /tmp/ttc/capture_after_accept2.py

# 2. 连接串过长溢出到 DATA 包
python3 /tmp/ttc/capture_long.py

# 3. REFUSE 处理
python3 /tmp/ttc/refuse_test2.py
```

`capture_connect.py` 在 0 号端口监听，记录第一个包后关闭；`capture_after_accept2.py` 回以那 37 字节
ACCEPT 并记录随后两个包。两者都打印「名称 长度 十六进制」。