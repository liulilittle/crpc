# Database client

[简体中文](db-client.zh-CN.md) | **English**

An asynchronous database client: one pool, three calling styles, prepared
statements, stored procedures with OUT/INOUT parameters, bulk insert, and a
column-to-field mapping seam for an ORM.

Currently implemented for **MySQL / MariaDB**. The abstraction below the adapter
(`result_set`, `connection`, `pool`, `mapping`) is backend-neutral, so the other
adapters land behind the same surface.

## Quick start

```cpp
#include <crpc/db/client.hpp>
#include <crpc/db/mysql/connection.hpp>

crpc::inproc_backend backend;                    // or a tcp backend
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
    // rows.error() carries the server's code and message.
}
```

## Three calling styles

| Style | How | When |
| --- | --- | --- |
| coroutine | the same `query(...)` call | inside a coroutine on a shard: the call parks and the shard thread stays free |
| synchronous | the same `query(...)` call | from an ordinary thread: it waits on a condition variable |
| asynchronous | `query_async(...)` | when the caller wants to do something else; needs an `async::executor` |

There is deliberately no separate coroutine entry point: the transport already
decides between parking and waiting, and two entry points would drift apart.

```cpp
crpc::async::executor pool;
client_options.async_executor = &pool;          // required for query_async

auto future = db.query_async("SELECT id FROM users");
// ... do other work ...
auto rows = future.get();                        // the lease was released already
```

`query_async` without an executor returns an **invalid future** rather than
running the work on the caller's thread.

## Parameters

Values are always bound, never spliced into SQL. A statement with parameters goes
through the prepared-statement protocol (`COM_STMT_PREPARE` / `COM_STMT_EXECUTE`),
and the statement is closed before the connection returns to the pool.

```cpp
std::vector<crpc::db::parameter> params;
params.push_back(crpc::db::parameter::from_int64(42));
params.push_back(crpc::db::parameter::from_text("alice"));
auto rows = db.query("SELECT * FROM users WHERE id = ? OR name = ?", params);
```

A statement that expects a different number of parameters is refused before
anything is written, because a count mismatch silently shifts every value.

## Stored procedures

MySQL does not accept a bound placeholder for an argument a procedure writes, so
`call()` rewrites those placeholders to session variables, seeds INOUT values, and
reads the results back:

```cpp
std::vector<crpc::db::parameter> params;
params.push_back(crpc::db::parameter::from_int64(7));                       // IN
params.push_back(crpc::db::parameter::out("total", crpc::db::db_type::int64)); // OUT

auto rows = db.call("CALL place_order(?, ?)", params);    // rows = the proc's result set
// params[1] now holds the value the procedure wrote, and is_null is false.
```

The rewrite is a real scanner: a `?` inside a string literal (`CALL p('a?b', ?)`)
is left alone. A procedure's result set is followed by a trailing OK, which is
drained; leaving it would make the next command read it as its own answer.

## Bulk insert

```cpp
crpc::db::insert_target target;
target.table = "users";
target.columns = {"id", "name"};

crpc::db::bulk_options bulk;
bulk.max_rows_per_statement = 1000;
bulk.max_bytes_per_statement = 4u * 1024 * 1024;

std::uint64_t affected = 0;
db.bulk_insert(target, rows, bulk, affected);   // via connection::bulk_insert
```

Batches are planned from the real statement: both a row limit and a byte budget,
always at least one row per statement. Values are placeholders; table and column
names are validated instead (bare names or backtick-quoted segments), because an
identifier cannot be bound.

For a very large import, `LOAD DATA LOCAL INFILE` is available through
`connection::load_data`. **The path in the SQL is never opened**: the client
streams bytes from a `local_data_source` you supply. A client that opened whatever
path the server named would turn a hostile server into an arbitrary local file
read.

## Reading into structs

```cpp
struct order_row { std::int64_t id; std::string name; double amount; };
RTTI_DEFINE_TYPE(order_row, id, name, amount)     // at global scope

auto rows = crpc::db::read_all<order_row>(*result_set);
```

Mapping rules (in `db/mapping.hpp`, usable without RTTI):

* a field matches a column by its **original name** first, alias second — binding
  on an alias breaks the moment the query is written differently;
* matching is case-insensitive;
* two columns matching one field is an ambiguity and fails rather than picking the
  leftmost;
* an unmatched field is skipped unless the caller requires every field;
* a value that does not fit the field is a decode error, never a default.

## Pool behaviour

Each `query` borrows a connection, runs the statement, and returns it before the
caller reads a row (the cursor is materialised). The pool bounds live connections
(`max_size`), idle ones (`max_idle`), idle time (`idle_timeout`) and total age
(`max_lifetime`), and pings a connection before handing it out. A connection whose
protocol is desynchronised is closed by the caller (`lease::discard()`), never
returned.

A connection handed back with an open transaction is rolled back first, and the
session is then reset to a known clean state (`autocommit=1`). The rollback covers a
transaction the caller never opened: with `autocommit=0` the server starts one
implicitly on the first statement, and returning the connection mid-transaction
would leak its locks into whichever caller borrows it next. The reset is what
restores `autocommit` itself: `ROLLBACK` ends the transaction but leaves the
session variable alone, so a rolled-back `autocommit=0` session would put the next
borrower's statements into an implicit transaction that its own release rolls
back. The clean state is deliberately not the server's global default: a server
started with `--autocommit=0` would hand the next borrower the same trap. Note that
normalisation happens on return, not on open: the very first lease of a connection
still runs under whatever default the server gave it, and only its release puts the
session into the clean state. A connection whose session cannot be reset is closed
rather than reused. Only MySQL implements the reset today; a backend with no session
state resets for free. MySQL sends one `SET autocommit=1` only when the session is
not already at that state.

## Limits

* A result set is materialised, bounded by `max_result_rows` / `max_result_bytes`;
  exceeding either fails with `resource_exhausted` instead of growing memory.
* `LOAD DATA LOCAL INFILE` sends what you give it; there is no file-path mode.
* `caching_sha2_password` over an **unencrypted** connection needs the server's RSA
  key exchange, which is not implemented: use TLS, or a `mysql_native_password`
  account. Against a real server the client reports the server's own error, so this
  is a clear failure rather than a hang.
* Not implemented yet: Oracle; TLS to the database; server-side cursors; `COM_STMT_FETCH`.

## HBase (REST gateway)

HBase has two protocols: the native one (protobuf over a custom RPC, needing the cluster's
protobuf definitions and a ZooKeeper-derived region map) and the REST gateway that ships
with it. This adapter speaks the gateway.

```cpp
#include <crpc/db/hbase/rest.hpp>

crpc::db::hbase::client_options options;
options.host = "127.0.0.1";
options.port = 8080;

auto gateway = std::make_unique<crpc::db::hbase::client>(
    options, [&backend, &options] {
        crpc::http::client_options http;
        http.host = options.host;
        http.port = options.port;
        return crpc::http::make_client(http, backend,
                                       crpc::endpoint::tcp(options.host, options.port));
    });

auto version = gateway->version();          // GET /version
auto rows = gateway->scan("people", {}, 0); // scanner, drained and released
```

One asymmetry in the gateway causes 404s that look like missing rows, and it was established
by probing a real server rather than from memory:

* the **path** carries the raw row key, percent-encoded — `GET /table/row1`;
* the **JSON body** carries base64 for row keys, column names and cell values —
  `{"Row":[{"key":"cm93MQ==",...}]}`.

`GET /table/cjE=` (base64 of "r1") answers 404 while `GET /table/r1` answers 200 with the
same row. The adapter applies each encoding where it belongs, so a caller never handles
base64.

Other behaviour worth knowing:

* `create_table` and `delete_table` wait for the transition to finish (polling
  `table_exists`, bounded by `transition_timeout_ms`). HBase creates and deletes
  asynchronously and the gateway offers no completion signal, so returning early makes the
  obvious next call — create again after delete — fail against a table that is still being
  removed.
* `scan` opens a scanner, drains it, and always releases it: an abandoned scanner holds
  server resources until it times out. A `limit` stops reading early and still releases.
* A missing row or table is `not_found`; the gateway answers plain HTTP codes rather than a
  body-level error object.
* A column is `family` or `family:qualifier`; cell values are opaque bytes, so a value that
  is not text survives the round trip.

Not implemented: the native protobuf protocol, filters beyond a start row, and
multi-version reads.

## Access (ODBC bridge)

Access is the one engine here without a usable public wire protocol: Jet/ACE has no
published frame format, and reverse-engineering its page layout would be enormous and
version-fragile. Every maintained client goes through a driver, so this adapter drives
ODBC and says so rather than pretending to speak a protocol.

```cpp
#include <crpc/db/odbc/connection.hpp>

crpc::db::odbc::connection_options options;
options.connection_string =
    "Driver={Microsoft Access Driver (*.mdb)};Dbq=C:\\data\\app.mdb;";

auto session = std::make_unique<crpc::db::odbc::connection>(options);
if (!session->is_open()) {
    // The driver's own message and SQLSTATE, not a generic failure.
    std::printf("%s (%s)\n", session->connect_error().message.c_str(),
                std::string(session->last_sqlstate()).c_str());
}
```

It is a build option (`-DCRPC_WITH_ODBC=ON`, Windows only, links `odbc32`); without it the
declarations still compile and every call reports `failed_precondition`, so a caller never
has to know which platform it was built for.

What the bridge does:

* binds every parameter through `SQLBindParameter` — a value never appears in the SQL text,
  so a string containing a quote is stored as text rather than breaking the statement;
* uses the wide (`W`) entry points throughout, because a table name or value outside the
  machine's ANSI code page would otherwise be corrupted before it reached the driver;
* reports the driver's SQLSTATE alongside the library code. `map_sqlstate` classifies by
  class: 23xxx is an integrity constraint, 42xxx a syntax or access error, 08xxx a lost
  connection, 40xxx a deadlock, HYT00/HYT01 a timeout;
* materialises each result set under `max_result_rows` / `max_result_bytes`, and reads every
  column as text so the driver's own conversion is the only one applied;
* drives transactions through ODBC's own model rather than through SQL text: `begin()`
  switches the connection to manual commit (`SQL_ATTR_AUTOCOMMIT` off), `commit()` and
  `rollback()` end the transaction with `SQLEndTran` and restore autocommit, and
  `in_transaction()` reads the commit mode back from the driver. Sending
  `BEGIN TRANSACTION` as a statement would work on some drivers and not others, and it
  would leave the connection's state a local guess — which matters because the pool asks
  `in_transaction()` before handing a session to the next borrower. `close()` rolls an open
  transaction back before disconnecting: a driver refuses to disconnect out of an
  outstanding transaction, and the session that survives holds its locks until it times
  out.

Two driver facts found by running against a real 32-bit Access driver:

* **Jet has no 64-bit integer**, and binding a value as `SQL_BIGINT` is refused with HYC00
  "optional feature not implemented". The bridge binds the narrowest type that holds the
  value — `SQL_INTEGER` for anything that fits in 32 bits — and only uses `SQL_BIGINT` when
  it must.
* **`note` and `memo` are reserved words** in Jet: `CREATE TABLE (… note VARCHAR(50))` fails
  with "Syntax error in field definition". Nothing here rewrites SQL, so that is the
  driver's dialect to live with.

`list_drivers()` reports what the machine offers, because "the Access driver is not
installed" explains a connection failure better than any error message. Note that the
Access driver is 32-bit only: a 64-bit process cannot use it, which is why the integration
case must run against a 32-bit build.

## MongoDB

The adapter speaks BSON, OP_MSG, and SCRAM-SHA-256 authentication, and is verified against
a real MongoDB 7.0.

```cpp
#include <crpc/db/mongo/protocol.hpp>

// BSON: ordered documents, every element type.
crpc::db::mongo::bson_document doc;
doc.append("name", crpc::db::mongo::bson_value::make_text("widget"));
doc.append("qty", crpc::db::mongo::bson_value::make_int32(3));

// OP_MSG: header, flags, one kind-0 section carrying the command document.
auto message = crpc::db::mongo::encode_op_msg(doc, 1);
```

What is implemented:

* BSON encode/decode for every element type this client sends, with the two length
  conventions that cause most decode bugs handled explicitly: a document's int32 length
  includes its own terminator, and a string's int32 length includes *its* NUL. A document
  with bytes after its terminator, a length that disagrees with the contents, or a
  truncated value is refused — never decoded partially.
* Field order is preserved in both directions, because a command's first field is its
  name and some commands give the first field a meaning of its own.
* OP_MSG framing (opcode 2013): header, flag bits, section kind 0, and decoding that
  refuses an unknown section kind rather than skipping it (its length is unknowable).
  `moreToCome` is reported, since a client that ignores it waits for a reply that never
  arrives.
* Commands: `hello`, `find`, `insert`, `delete`, `getMore`, `killCursors`, plus reply
  status (`ok`, `errmsg`) and error-code mapping (duplicate key, auth failure, namespace
  not found, deadline, bad value).

Connection layer:

```cpp
crpc::db::mongo::connection_options options;
options.user = "crpc";
options.password = "...";
options.auth_database = "admin";
options.database = "app";

auto factory = std::make_unique<crpc::db::mongo::connection_factory>(options, backend, endpoint);
auto session = factory->open();

crpc::db::mongo::find_request request;
request.database = "app";
request.collection = "people";
request.filter.append("age", crpc::db::mongo::bson_value::make_int32(41));
auto found = session->find(request);       // first batch plus the cursor id
auto inserted = session->insert("people", docs);
```

Wire facts that cost a round trip each to find, all now encoded and tested:

* **The client metadata document has a fixed shape.** `driver` and `os` must be
  *documents*; sending `driver` as a string is refused with "The 'driver' field is
  required to be a BSON document in the client metadata document", and omitting `os`
  gives "Missing required sub-document 'os'".
* **`conversationId` must be echoed with the exact type the server used.** Converting it
  to int64 is refused: "is the wrong type 'long', expected type 'int'".
* **`$db` is required on every OP_MSG request.** The connection adds it if a command
  builder did not, so a builder cannot produce a request the server rejects.
* **A write command reports per-document failures in `writeErrors` while `ok` stays 1.**
  Checking only `ok` reports a duplicate key as a successful insert — silent data loss.
  `insert()` now inspects `writeErrors` and `writeConcernError` and returns the server's
  own code and message.
* **A reply's `responseTo` must match the request id.** A reply that answers something
  else cannot happen without multiplexing, so accepting it would attribute another
  command's result to this one.

Not implemented: TLS, compression, sessions and transactions (`begin()` says so rather
than pretending), and aggregation. `killCursors` exists because an abandoned cursor holds
server resources until it times out; a cursor-aware client must send it.

## SQL Server (TDS)

```cpp
#include <crpc/db/tds/connection.hpp>

crpc::db::tds::connection_options options;
options.user = "sa";
options.password = "...";
options.database = "app";

auto factory = std::make_unique<crpc::db::tds::connection_factory>(
    options, backend, endpoint);
auto session = factory->open();
```

What the adapter does:

* PRELOGIN, LOGIN7, and SQL batches (`packet_type::sql_batch`); the login reply is
  decoded token by token, so the server's own message and number survive as
  `connect_error()`.
* Messages larger than the negotiated packet size are split and reassembled in both
  directions, and a reply is read until the terminal DONE token rather than until
  the end of a packet — a result taller than one packet is ordinary.
* The row count arrives in DONE, so `affected_rows()` is known after the rows, not
  before.
* `BEGIN` / `COMMIT` / `ROLLBACK TRANSACTION` are sent as batches, and the open
  transaction is also tracked locally for the pool.
* The first result set is returned; any further sets in the same reply are decoded
  and discarded so the stream stays aligned. Column metadata is mapped to
  `db::column_info`, so the ORM layer works unchanged.

Verified end to end against a real SQL Server 2022 (16.0.4305.5): login, framing
across packets, column metadata, rows of every type listed below, error numbers,
DDL/DML, affected rows, and a rolled-back transaction. `tests/test_db_tds_it.cpp`
runs that suite when the server details are in the environment:

```
CRPC_TDS_IT=1 CRPC_TDS_HOST=127.0.0.1 CRPC_TDS_PORT=1433 \
CRPC_TDS_USER=sa CRPC_TDS_PASSWORD=... ctest -R crpc_test_db_tds_it
```

That run is what produced the wire captures now kept as regression tests: token
lengths, column counts, flags and row value lengths are **little-endian** (only the
8-byte packet header is big-endian), `DATE` carries no type info, the date/time
types carry only a scale, the decimal sign byte is 1 for positive, and the
transaction descriptor in `ENVCHANGE(begin_transaction)` is length-prefixed.

Limits, stated rather than hidden:

* Parameters are bound by RPC, never spliced into the text. `query(sql, params)` sends
  `sp_executesql` with the `?` markers rewritten to `@P1..@Pn` (the scanner respects
  string literals, quoted identifiers and comments), and `call(name, params)` invokes a
  procedure by name. OUT/INOUT values come back through `RETURNVALUE` and the return
  code through `RETURNSTATUS` (`last_return_status()`). A placeholder/parameter count
  mismatch is refused before anything is sent.
* A NULL input parameter needs a type: `parameter::null()` has `db_type::unknown`, so
  it is refused with `invalid_argument` rather than being declared as some guess.
* **No TLS yet.** `require_encryption` and `encrypt_login_only` are refused with
  `failed_precondition` before a byte is written, and a server that demands encryption
  is refused too. A TLS channel exists in the source tree and completes a handshake
  against SQL Server 2022, but its post-handshake framing does not match what the
  server expects, so it is not wired in: shipping encryption that is unproven is worse
  than refusing it. `tests/test_db_tds_it.cpp` pins the refusal.
* Cancellation uses `ATTENTION`: `cancel()` is safe to call from another thread while a
  query is blocked, and the blocked query then returns `rpc_code::canceled`. The
  acknowledgement arrives as its own message, so the reply loop waits for the DONE that
  carries the attention bit — a reader that stopped at the first final DONE would leave
  that acknowledgement behind and the *next* query would read it as its own reply.
  Verified against a real server: a `WAITFOR DELAY '00:00:03'` returns in about 160 ms
  and the session stays usable.
* No MARS, and no bulk-load protocol: `bulk_insert` sends batched `INSERT ... VALUES`
  with every value bound. Two server limits decide the batch size — 2100 parameters per
  request (less the two `sp_executesql` arguments, so 2098 for values) and 1000 row-value
  expressions per statement. A 2-column table therefore gets 1049 rows per statement, not
  1050, and the off-by-two is the difference between working and "too many parameters".
  `affected` reports the sum over the batches that succeeded, and a shape mismatch is
  refused before anything is sent.
* Types decoded today: INT1/2/4/8, BIT, FLT4/8, INTN/BITN/FLTN, DECIMAL/NUMERIC,
  MONEY/SMALLMONEY (and their nullable MONEYN form), DATE, TIME, DATETIME2,
  DATETIMEOFFSET, the legacy DATETIME/SMALLDATETIME pair (and DATETIMEN),
  VARCHAR/NVARCHAR/CHAR/NCHAR, VARBINARY/BINARY, GUID. Any other wire type fails the
  read with `no_such_method` naming the type byte, instead of misreading bytes.
* A nullable column arrives as the `N` variant of its type (`money` is 0x6E, not
  0x3C), so both forms are decoded. `money` is two 32-bit halves with the high half
  first; the legacy DATETIME pair counts days from 1900-01-01, not from 0001-01-01;
  `datetimeoffset` carries UTC plus an offset and is printed as local time.