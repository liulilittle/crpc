# 数据库客户端

[English](db-client.md) | **简体中文**

异步数据库客户端：一个连接池、三种调用形态、预备语句、带 OUT/INOUT 参数的存储过程、
批量插入，以及供 ORM 使用的列→字段映射接缝。

当前已实现 **MySQL / MariaDB**。适配器之下的抽象（`result_set`、`connection`、`pool`、
`mapping`）与后端无关，因此其余适配器会在同一套接口下落地。

## 快速开始

```cpp
#include <crpc/db/client.hpp>
#include <crpc/db/mysql/connection.hpp>

crpc::inproc_backend backend;                    // 也可换成 tcp 后端
crpc::endpoint ep = crpc::endpoint::tcp("127.0.0.1", 3306);

crpc::db::mysql::connection_options options;
options.user = "app";
options.password = "secret";
options.database = "shop";
crpc::db::mysql::connection_factory factory(options, backend, ep);

crpc::db::client_options client_options;
client_options.pool.max_size = 8;
crpc::db::client db(factory, client_options);

const auto rows = db.query("SELECT id, name FROM users WHERE id = ?", params);
if (!rows) {
    // rows.error() 携带服务端的错误码与原文。
}
```

## 三种调用形态

| 形态 | 方式 | 场景 |
| --- | --- | --- |
| 协程 | 同一个 `query(...)` | 在 shard 上的协程内：调用挂起，shard 线程不被占用 |
| 同步 | 同一个 `query(...)` | 在普通线程上：等待条件变量 |
| 异步 | `query_async(...)` | 调用方要同时做别的事；需要 `async::executor` |

**故意不提供单独的协程入口**：传输层已经在「挂起」与「阻塞」之间做了选择，两个入口只会彼此漂移。

```cpp
crpc::async::executor pool;
client_options.async_executor = &pool;          // query_async 需要

auto future = db.query_async("SELECT id FROM users");
// ... 做别的事 ...
auto rows = future.get();                        // 租约此时已归还
```

没有 executor 时 `query_async` 返回**无效 future**，绝不会在调用线程上悄悄执行“异步”工作。

## 参数

值一律绑定，绝不拼进 SQL。带参数的语句走预备语句协议（`COM_STMT_PREPARE` /
`COM_STMT_EXECUTE`），并在连接归还池之前关闭语句。

```cpp
std::vector<crpc::db::parameter> params;
params.push_back(crpc::db::parameter::from_int64(42));
params.push_back(crpc::db::parameter::from_text("alice"));
auto rows = db.query("SELECT * FROM users WHERE id = ? OR name = ?", params);
```

参数个数不匹配会在写出任何字节之前被拒绝——否则每个值都会静默错位。

## 存储过程

MySQL 不接受过程写回参数的占位符，因此 `call()` 会把这些占位符改写成会话变量、为 INOUT
播种、再读回结果：

```cpp
std::vector<crpc::db::parameter> params;
params.push_back(crpc::db::parameter::from_int64(7));                       // IN
params.push_back(crpc::db::parameter::out("total", crpc::db::db_type::int64)); // OUT

auto rows = db.call("CALL place_order(?, ?)", params);    // rows 为过程返回的结果集
// params[1] 现在持有过程写入的值，is_null 为 false。
```

改写是真正的扫描器：字符串字面量里的 `?`（`CALL p('a?b', ?)`）保持原样。过程的回复在结果集之后
还有一个尾随 OK，会被排空；否则下一条命令会把它当作自己的应答。

## 批量插入

```cpp
crpc::db::insert_target target;
target.table = "users";
target.columns = {"id", "name"};

crpc::db::bulk_options bulk;
bulk.max_rows_per_statement = 1000;
bulk.max_bytes_per_statement = 4u * 1024 * 1024;

std::uint64_t affected = 0;
db.bulk_insert(target, rows, bulk, affected);   // connection::bulk_insert
```

分批从真实语句推导：同时受行数与字节预算约束，且每批至少一行。值是占位符；表名和列名改为**校验**
（裸名或反引号段），因为标识符无法绑定。

超大导入可用 `connection::load_data` 走 `LOAD DATA LOCAL INFILE`。**SQL 里的路径永不打开**：
客户端只流式发送你提供的 `local_data_source` 字节。若按服务端给的名字打开文件，等于把恶意服务端
变成任意本地文件读取。

## 读入结构体

```cpp
struct order_row { std::int64_t id; std::string name; double amount; };
RTTI_DEFINE_TYPE(order_row, id, name, amount)     // 必须位于全局作用域

auto rows = crpc::db::read_all<order_row>(*result_set);
```

映射规则（在 `db/mapping.hpp` 中，不依赖 RTTI）：

* 字段优先匹配**原始列名**，其次别名——按别名绑定，查询一改就失效；
* 大小写不敏感；
* 两个列匹配同一字段属歧义，直接失败，不取最左；
* 未匹配的字段默认跳过，除非调用方要求全部匹配；
* 值放不进字段是解码错误，绝不给默认值。

## 连接池行为

每次 `query` 借出一个连接、执行语句，并在调用方读取行之前归还（游标已物化）。池限制同时存活连接
（`max_size`）、空闲连接（`max_idle`）、空闲时长（`idle_timeout`）与总年龄（`max_lifetime`），
借出前会 PING。协议已错位的连接由调用方 `lease::discard()` 关闭，绝不归还。

## 限制

* 结果集是物化的，受 `max_result_rows` / `max_result_bytes` 约束；超限以 `resource_exhausted`
  失败，而不是继续涨内存。
* `LOAD DATA LOCAL INFILE` 只发送你给的字节，没有“按路径读取”模式。
* `caching_sha2_password` 在**未加密**连接上需要服务端 RSA 密钥交换，本库尚未实现：请用 TLS，
  或改用 `mysql_native_password` 账号。面对真实服务器时客户端会原样返回服务端错误，是明确失败而非挂起。
* 尚未实现：TDS（SQL Server）、Oracle、Access、MongoDB、HBase；到数据库的 TLS；服务端游标；
  `COM_STMT_FETCH`。