# CRPC ABI 策略

[English](abi.md) | **简体中文**

版本 2.0 / 2026-10-24

本文档定义 CRPC 在二进制层面保证什么、不保证什么，以及破坏二进制兼容的变更如何处理。
它是规范性的：违反它的发布即为发布缺陷。

## 1. 版本语义

CRPC 使用语义化版本：

```
MAJOR.MINOR.PATCH
```

| 变更 | 版本 | ABI 影响 |
|---|---|---|
| 删除或重命名公开符号、修改函数签名、修改结构体布局、修改跨线枚举值 | MAJOR | 破坏 |
| 修改任何公开结构体的布局（含新增字段） | MAJOR | 破坏（尚无尺寸标记选项结构体，见第 4 节） |
| 新增公开符号、末尾新增枚举值 | MINOR | 兼容 |
| 无接口变更的缺陷修复 | PATCH | 兼容 |

线上协议有自己的版本（`protocol_version`，当前为 `1`），与库版本独立：
库的补丁发布永不改变线上格式；线上格式变更必须提升协议版本并附带迁移文档。

2.0.0 属于 MAJOR 变更：`http::http_statistics` 的布局增长（第 4 节），`server::options` 的布局同样增长（第 4 节）。1.0.0 布局（`bd006b2`）是最后一个兼容版本；2.0.0 与 1.0.x 二进制不向前兼容。

## 2. 允许跨越 ABI 边界的类型

导出签名中只允许出现：

- 固定宽度的基本类型（`crpc::u8` … `crpc::u64`、`crpc::i8` … `crpc::i64`、
  `float`、`double`、`bool`）；
- `std::byte`、`std::size_t`、`std::ptrdiff_t`；
- `crpc::span<T>`、`crpc::buffer`、`crpc::rpc_error`、`crpc::result<T>`、
  `crpc::status`；
- 固定周期的 `std::chrono::duration` / `time_point`；
- 指向 CRPC 类型的指针与引用；
- `std::string_view`（两字宽、可平凡拷贝的视图）。

导出签名中禁止出现：

- 标准容器（`std::string`、`std::vector`、`std::map` 等）——其布局由实现定义，
  MSVC STL、libstdc++、libc++ 之间不一致；
- `std::function`、`std::any`、含非平凡备选项的 `std::variant`；
- 跨越边界抛出的异常（本库从不抛出）；
- `long`、`unsigned long`、`wchar_t`、未指定固定宽度的 `size_t`；
- 位域、匿名联合、packed 结构体；
- 作为*消费方面向类型*的实现内部命名空间（`client_detail`、`rpc_detail`，或其它 `detail` 命名空间）下的任何类型：它们只能作为库自己产生的参数出现。

确实需要容器时，API 使用 `span`、或 `const char*` 加长度，
或返回一个被文档标注为不透明的 CRPC 自有类型。

`crpc::client_detail` 与 `crpc::rpc_detail` 命名空间下、以及其它头文件中匿名或嵌套 `detail` 命名空间下的类型是**实现内部细节**：它们不出现在*消费方面向*的导出签名中。但它们对消费方的编译器并非**全部**不可见：`client_facade.hpp:898` 在消费方自己的翻译单元内实例化 `client_detail::call_state<Resp>`（头内模板中的 `std::make_shared<client_detail::call_state<Resp>>()`），而 `call_state` 以**值**持有 `call_core`。因此 `call_core` 或 `call_state` 的布局变更**要求消费方与库用同一份头文件重建**（见第 4 节「调用方与库必须使用同一份头文件构建」）。按第 1 节表格（「改变任何公开结构体的布局，包括增加字段 | MAJOR」），这属于布局变更，是 MAJOR；本周期整体已是 MAJOR（第 4 节），本切片不新增兼容性缺口。

具体地，`client::finish_attempt(client_detail::call_core&, ...)` 与 `client::begin_attempt(client_detail::call_core&, ...)` 在导出签名中提及 `call_core`，且该引用的唯一产生者是 `client` 自身。这并不意味着布局无关：给 `call_core` 增加字段（例如记录本次调用是否由 fallback 应答的 `degraded_` 标志）会移动其后所有字段。MSVC x64 实测（`call_core` 664 -> 672 字节，`call_state<Resp>` 760 -> 768）：用旧头文件构建的消费方链接新库时，新字段会覆写旧 `inner_mutex` 的首字节，因为该字段的绝对偏移是 8 + 旧 `sizeof(call_core)`——`/MD` 下 672、`/MDd` 下 696。x86 上新字段恰好落进 `call_core` **自身尾部**的 4 字节内部 padding（`retry` 占 480..484，`sizeof(call_core)` 仍为 488），故 `sizeof` 未变、今天不触发覆写。这是**运气而非保证**：padding 只有 4 字节，`degraded_` 之后仅余 3 字节，第 4 个 1 字节字段就会把 `sizeof(call_core)` 推到 496，而单个宽度超过剩余 3 字节的字段则会立即跨过。两种情况下 x64 的失效模式都会在 x86 上重现。

`call_core` 带有 `CRPC_API`（`client_facade.hpp:295`），因此它的成员会出现在共享库的
导出表里。这个装饰是**链接期要求，不是 ABI 承诺**：头内模板（`client_detail::wake_functor`、
`call<Resp>::get()`、`start_encoded`）会编译进消费方的翻译单元并调用
`call_core::signal`、`mark_degraded`、`clear_degraded`、`degraded`、`reset_for_attempt`
及其构造/析构，若共享构建把它们隐藏，任何实例化调用的消费方都会链接失败。它们出现在
导出表中**并不**使其进入第 2 节的白名单，**不**意味着其布局跨头文件变更稳定，也**不**
允许消费方命名它们（第 2 节已禁止把 `detail` 类型作为消费方面向类型）。

其它内部类型并非以同样方式跨越边界，这一区别很重要。只有 `call_state_base` 与 `call_state<Resp>` 是多态类型；`connection_slot` 与 `client_liveness` 是没有 vptr 的普通结构体，`rpc_detail::always_false` 则无状态。
- `call_state_base` 是 `call_state<Resp>` 的多态基类；消费方的翻译单元会分配派生对象，因此改动这两者中任何一个的布局，都是改动消费方分配的尺寸。
- `connection_slot` 与 `client_liveness` **只由库分配**（`src/client/client_facade.cpp:1146` 与 `:396`；任何头文件里都没有构造点），只经 `std::shared_ptr<connection_slot>` 与 `std::weak_ptr<client_liveness>` 访问。因此它们的尺寸不会作为消费方分配的对象跨越边界。但它们的**成员偏移仍被头内代码烘焙进消费方的翻译单元**：`live_owner` 读取 `client_liveness::alive`（`client_facade.hpp:399`），`client::issue` 读取 `connection_slot::mutex`、`healthy` 与 `conn`（`client_facade.hpp:1079-1081`），这两处都在消费方自己的翻译单元内编译。给这两个类型增加字段仍会破坏用旧头文件构建的消费方；对 `client_liveness::alive` 而言，失效形式是漏掉死亡检查并交出一个悬垂的 `client*`。它们被列在此处，是因为消费方仍不得*命名*或*依赖*它们（见第 2 节），也因为对它们的布局变更同样并非不可见。
- `rpc_detail::always_false`（`rpc/call.hpp`）是无状态的 trait，没有可变的布局。

`client::call<Resp>` 只持有一个成员，即 `std::shared_ptr<client_detail::call_state<Resp>>`，因此其大小恒为某个 `T` 的 `sizeof(std::shared_ptr<T>)`——x64 上 16 字节、x86 上 8 字节——无论 `call_state` 里有什么。这份稳定性只覆盖句柄，不覆盖它指向的堆对象：该对象的大小仍取决于分配它时所用的头文件，上文实测的正是这种情况。

## 3. 符号可见性

- 每个导出符号都用 `CRPC_API` 声明（见 `include/crpc/config.hpp`）。
- Windows：共享库构建只导出 `CRPC_API` 符号，其余按链接器默认行为隐藏。
- Linux/macOS：共享库构建使用 `-fvisibility=hidden -fvisibility-inlines-hidden`，
  只有 `CRPC_API` 符号为默认可见。
- 静态库按定义导出全部符号；静态链接的消费方不受可见性规则约束，
  但仍受第 2 节的布局规则约束。

由第一条可推出两条规则，两条都很容易出错：

1. **每个含 out-of-line 成员的公开类都必须自己标 `CRPC_API`。**
   导出成员函数需要在该类**自己**的声明上加装饰；基类、命名空间或兄弟声明都不会导出它。
   只有内联成员的类不需要：消费者会把它们编译进自己的目标文件。
2. **外层类上的宏不覆盖嵌套类。**
   `class CRPC_API outer { class nested { void f(); }; };` 导出了 `outer` 的成员，
   但不导出 `outer::nested::f`。每个含 out-of-line 成员的嵌套类都必须在自己的声明上加装饰，
   `server::builder`、`client::builder` 与 `http::http_server::builder` 就是这样处理的。

命名空间级函数同理：在公开头文件中声明、在 `src/` 中定义的自由函数，
必须在其声明上标 `CRPC_API`。类内的 friend 声明会把名字引入外层命名空间，
因此当一个函数既是 friend 又是命名空间级入口时，两处声明的装饰必须一致
（否则 MSVC 报 C2375）。

`tests/test_shared_link.cpp`（仅在 `CRPC_BUILD_SHARED=ON` 时构建）是这些规则的门禁。
它是一个消费者：链接导入库并直接调用文档化 API，包括两个头内联模板
（`client_detail::wake_functor` 与 `mvc::bind_request`），它们的函数体引用了
out-of-line 成员。因此缺少装饰会在构建期失败，而不是在下游消费者那里。
参见 [building.zh-CN.md](building.zh-CN.md) 第 4 节。

## 4. 结构体布局

每个跨越 ABI 边界的公开结构体必须：

1. 是标准布局且可平凡拷贝，或被明确标注为不透明、只允许指针/引用访问；
2. 不含虚函数（虚分派仅限传输接口，且其对象从不按值跨越库边界）；
3. 不含条件成员（公开结构体内部禁止 `#if`）。

**头文件目前尚未强制第 1 条。** 公开记录与选项结构体（例如 `frame_header`、
`crpc::connect_options`、`rpc::client_options`、`server::options`）中没有任何
`static_assert(sizeof(...))` 或 `static_assert(alignof(...))`。现存的断言覆盖的是
其它契约：

- `config.hpp:96-102` 固定整数宽度与 IEEE-754 格式；
- `observability/log.hpp:327` 限定 `log_record` 为 512 字节；
- `rpc/call.hpp:559-566` 限定内联回调存储。

若干公开类型（`rpc_error`、`buffer`、`endpoint`）含有标准库拥有型成员，因此不是
可平凡拷贝；它们按引用传递、作为 CRPC 自有类型返回，或被标注为不透明，不按值跨越
C ABI。为每个结构体在头文件中增加 `sizeof`/`alignof` 断言是计划中的工作，**尚未**
实施。在它存在之前，任何公开结构体的布局变更都按 MAJOR 处理（第 7 节），而不是
MINOR 扩展。

无需改动头文件即可检查的布局不变量集中在一个内部翻译单元 `src/abi_check.cpp`：
它断言 15 个公开记录的标准布局属性、该文件中列出的 **15 个**记录的精确成员数量、
`http_statistics` / `client::statistics` / `client::resilience_statistics` /
`resilience::fallback_statistics` / `server::statistics` 各计数器的相对成员链（链条按每个成员自身的宽度推进，因此
混用 `std::size_t` 与 `std::uint64_t` 的 `server::statistics` 在 32 位与 64 位
目标上都能被检查）、`http_statistics` 的总宽度与尾字段位置，并且**仅在 x86 与
x64 上**断言该文件中列出的记录的绝对 `sizeof` 与关键 `offsetof` 数值。它随库一起
编译，因此在这些护栏覆盖的范围内，布局变更会导致构建失败。

这些护栏**并未**覆盖所有跨越 ABI 边界的类型，且「跨边界」的定义是精确给出的，
而不是开放式全称：

> **范围。** 在 `CRPC_API` 函数签名中**按值**出现的普通聚合记录，或由虚接口按值
> 传递的普通聚合记录，属于本范围。**按引用或作为出参**出现的记录，只要库在边界
> 另一侧**写入它或读取它的字段**，同样属于本范围；被调方把字段偏移编译进库的
> `const&` 参数也属于本范围。仅通过内联构造函数或 builder 方法传入的选项结构体
> 不在此范围内，因为它们从不跨越共享库边界：调用方把构造函数编译进自己的目标
> 文件，两侧对布局的认知必然一致。

通过**库内定义（out-of-line）**函数到达的选项结构体属于本范围，不是例外，因为
调用方用自己的头文件构造实参、库用自己的头文件读取它：`http_server_options` 与
`io::tls_config`（按值传入 `http_server::http_server` / `listen_tls`，且
`tls_context::config` 按值返回 `tls_config`）、`rpc::method_options`（按值传入
`method_slot` / `service_registry::register_method`）、`push::subscribe_options`
（按值传入 `hub::subscribe`）、以及 `resilience::rate_limit_config` /
`breaker_config` / `bulkhead_config` / `retry_policy`（按引用传入各自构造函数）。
真正在本范围之外的，是**完全没有** out-of-line 入口的结构体；下列清单由
`crpc_test_abi_docs` 维护，不靠手写。

`resilience::fallback_context` 出于同样的理由属于本范围，尽管没有任何签名写出它的
名字：库构造它并用它调用消费者的 `fallback_fn`，因此两侧必须对字段偏移达成一致。
它在 `src/abi_check.cpp` 中**手工锚定**（x86 与 x64 均为 32 字节，偏移
`0/2/8/16/17/18/19/24`），因此**不**出现在下面的未守卫清单里。

范围内**没有任何编译期布局护栏**的记录如下，均使用限定名。该清单由
`crpc_test_abi_docs` 生成：测试会重新扫描 `include/crpc/**/*.hpp` 与
`src/abi_check.cpp`，若新增了跨边界记录却没有更新本清单，测试即失败。之所以使用
限定名，是因为有两个不同的记录都叫 `frame_header`（`::crpc::frame_header` 与
`::crpc::http2::frame_header`），其中只有一个受护。

- `::crpc::call_envelope`
- `::crpc::cluster::prevote_result`
- `::crpc::cluster::vote_result`
- `::crpc::compression_limits`
- `::crpc::compression_statistics`
- `::crpc::db::bulk_options`
- `::crpc::db::column_info`
- `::crpc::db::field_binding`
- `::crpc::db::insert_target`
- `::crpc::db::mapping_options`
- `::crpc::db::parameter`
- `::crpc::db::mysql::column_definition`
- `::crpc::db::mysql::eof_packet`
- `::crpc::db::mysql::err_packet`
- `::crpc::db::mysql::handshake`
- `::crpc::db::mysql::handshake_response_options`
- `::crpc::db::mysql::ok_packet`
- `::crpc::db::mysql::binary_row`
- `::crpc::db::mysql::prepare_ok`
- `::crpc::db::mysql::text_row`
- `::crpc::db::mongo::aggregate_request`
- `::crpc::db::mongo::find_request`
- `::crpc::db::mongo::message_header`
- `::crpc::db::mongo::op_msg_reply`
- `::crpc::db::mongo::replica_set_topology`
- `::crpc::db::mongo::topology_member`
- `::crpc::db::mongo::write_concern_options`
- `::crpc::db::hbase::row`
- `::crpc::db::hbase::scan_filter`
- `::crpc::db::hbase::scan_options`
- `::crpc::http::client_options`
- `::crpc::db::tds::login7`
- `::crpc::db::tds::packet_header`
- `::crpc::db::tds::prelogin`
- `::crpc::db::tds::rpc_parameter`
- `::crpc::db::tds::rpc_request`
- `::crpc::db::oracle::tns_accept`
- `::crpc::db::oracle::tns_connect`
- `::crpc::db::oracle::tns_header`
- `::crpc::db::oracle::tns_refuse`
- `::crpc::db::oracle::ttc_column`
- `::crpc::db::oracle::ttc_execute`
- `::crpc::db::oracle::ttc_fetch`
- `::crpc::db::oracle::ttc_value`
- `::crpc::flow::rtt_tracker`
- `::crpc::frame`
- `::crpc::fs::directory_entry`
- `::crpc::fs::file_info`
- `::crpc::http2::frame_header`
- `::crpc::http2::goaway_payload`
- `::crpc::http2::header_field`
- `::crpc::http2::headers_payload`
- `::crpc::http2::priority_payload`
- `::crpc::http2::push_promise_payload`
- `::crpc::http2::setting`
- `::crpc::http::coding_limits`
- `::crpc::http::multipart_limits`
- `::crpc::http::negotiation`
- `::crpc::http::range_parse_result`
- `::crpc::http::request`
- `::crpc::http::request_encoding_plan`
- `::crpc::http::response`
- `::crpc::http::forwarded_client`
- `::crpc::http::serialize_options`
- `::crpc::http::trusted_proxies`
- `::crpc::http::sse::event_id`
- `::crpc::inproc_pair`
- `::crpc::io::io_result`
- `::crpc::memcached::binary_header`
- `::crpc::memcached::connection_transport_config`
- `::crpc::memcached::text_reply`
- `::crpc::mq::amqp::frame`
- `::crpc::mq::amqp::frame_header`
- `::crpc::mq::amqp::message_properties`
- `::crpc::mvc::binding_limits`
- `::crpc::mvc::file_part`
- `::crpc::mvc::route_entry`
- `::crpc::mvc::route_params`
- `::crpc::obs::log_record`
- `::crpc::obs::trace_context`
- `::crpc::obs::traceparent_result`
- `::crpc::push::push_envelope`
- `::crpc::redis::notification`
- `::crpc::push::push_subscription`
- `::crpc::push::subscribe_request`
- `::crpc::redis::bitmap_type`
- `::crpc::redis::geo_type`
- `::crpc::redis::hash_type`
- `::crpc::redis::hyperloglog_type`
- `::crpc::redis::list_type`
- `::crpc::redis::redis_stream_entry`
- `::crpc::redis::resp_array_value`
- `::crpc::redis::resp_big_number`
- `::crpc::redis::resp_blob_error`
- `::crpc::redis::resp_boolean`
- `::crpc::redis::resp_bulk_string`
- `::crpc::redis::resp_double`
- `::crpc::redis::resp_error`
- `::crpc::redis::resp_integer`
- `::crpc::redis::resp_map_value`
- `::crpc::redis::resp_null`
- `::crpc::redis::resp_push_value`
- `::crpc::redis::resp_set_value`
- `::crpc::redis::resp_simple_string`
- `::crpc::redis::resp_verbatim_string`
- `::crpc::redis::scored_member`
- `::crpc::redis::sentinel_reply`
- `::crpc::redis::set_type`
- `::crpc::redis::stream_type`
- `::crpc::redis::string_type`
- `::crpc::redis::value`
- `::crpc::redis::zset_type`
- `::crpc::resilience::call_traits`
- `::crpc::resilience::instance`
- `::crpc::resilience::retry_policy`
- `::crpc::resilience::smart_policy`
- `::crpc::ws::client_options`
- `::crpc::ws::frame_header`

这些记录只受第 7 节政策约束（任何公开结构体布局变更都属 MAJOR），没有构建期检查。
其中大多数都可以纳入成员数量护栏；`::crpc::endpoint` 不行，因为它的成员是私有的，
且它作为封装类（而非普通聚合）本就不在上面的范围内（虽然它由虚方法按值返回）。

`server::builder` 是第二个需要明确说明的已导出封装类。它带有 `class CRPC_API builder`
装饰，其 out-of-line 成员函数会读取消费方分配对象中的私有 `options_` 成员，因此其布局
与 ABI 相关；它不在上面的清单中，因为它不是普通聚合记录。成员数量与 `offsetof`
护栏无法作用于其私有数据成员，其宽度也不是固定数字：它等于 `sizeof(server::options)`
加上三个 `std::vector` 成员并按 builder 的对齐向上取整，而 `std::vector` 的宽度会随
标准库调试配置变化（MSVC x64：Release 392 字节，Debug 416 字节；x86：304 / 312）。
因此 `src/abi_check.cpp` 锁定的是这一结构关系而非绝对尺寸，并要求 builder 保持
options 的对齐；它能在所有目标与配置上捕获 builder 成员的增删或类型变更。三个等宽
vector 的重排与其它护栏看不见的变更一样，仍按第 7 节属 MAJOR。

这些护栏并不对所有变更类型、所有架构一视同仁，判断某个变更是否会被捕获时必须区分：

- 重排、在两个既有成员之间插入、删除：由相对成员链在所有架构上捕获，但**仅限于
  存在相对链的记录**。成员数量在此**不提供任何覆盖**：重排不会改变成员数量，因此
  对重排恒为盲。`rpc_error` 没有相对链，所以 `rpc_error` 内部的重排不会被捕获。
- 重命名：若记录存在相对链，则由该链捕获；在 `rpc_error` 中则由 `src/abi_check.cpp` 的成员类型断言捕获。
  成员数量护栏**不校验名字**——结构化绑定只是按声明顺序为每个成员引入一个本地名字。唯一没有相对链的跨 ABI 记录是 `rpc_error`（就 15
  个有成员数量护栏的记录而言；§4 中无护栏的记录则根本没有链，`status` 没有公开数据成员，改名不可能发生），因此 `rpc_error`
  中的改名不由链捕获。四个 HTTP/2 值类型 `http2::h2_error`、`http2::connection_options`、
  `http2::stream_snapshot`、`http2::window_credit` **都有** `CRPC_AFTER` 链，
  因此它们的改名会被捕获。对 `rpc_error`，该单元在 `decltype` 类型断言中逐个点名其七个成员（`code`、`message`、
   `retryable`、`retry_after`、`metadata`、`origin`、`attempts`），因此改名会使该翻译单元编译失败；其构造函数只点名 `code`、
   `message`、`retryable`，所以不会捕获 `retry_after`、`metadata`、`origin` 或 `attempts` 的改名。
- 在末尾追加字段：由成员数量在所有架构上捕获，并且在 x86 与 x64 上还会被绝对
  `sizeof` 锚额外捕获。x86 之所以必须依赖成员数量，是因为 x86 上
  `server::statistics` 为 104 字节而最后一个字段结束于 100，末尾 4 字节是填充；
  追加一个 `std::size_t` 会被这段填充吸收，`sizeof` 不变。因此**单靠绝对
  `sizeof` 断言在 x86 上抓不到尾追加**，这正是成员数量存在的理由。
- 在 x86 与 x64 之外的架构（例如 ARM64）上，不维护任何绝对 `sizeof` /
  `offsetof` 数值。尾追加仍由成员数量捕获，但如果某个变更同时保持成员数量与相对
  顺序不变、只改变绝对宽度，则在这些目标上**不会**在编译期被捕获。

该翻译单元不锁定含有标准库成员的类型的尺寸（`rpc_error`、`status`、`buffer`、
`endpoint`），因为它们的宽度属于标准库配置而非本库的属性：同一份头文件下，
`rpc_error` 与 `status` 在 x64 Release 为 80 字节、Debug 为 96 字节，在 x86 上
分别为 56 与 64 字节。这四者实际受到的守护并不相同，且只有第一个是完整的：

- `rpc_error` 有公开成员，因此由成员数量以及每个成员的声明类型断言
   （`code`、`message`、`retryable`、`retry_after`、`metadata`、`origin`、`attempts`）覆盖，二者都与
   构建类型无关。
- `status` 没有公开数据成员，成员数量与成员类型都无法断言。它只由
  `sizeof(status) == sizeof(rpc_error)` 与
  `alignof(status) == alignof(rpc_error)` 关系覆盖；这能抓到新增存储，但抓不到
  被包裹的 `rpc_error` 内部保持尺寸不变的变更。
- `buffer` 与 `endpoint` **除最小对齐外没有任何布局护栏**（`alignof` 至少为
  `alignof(void*)`）。其尺寸与成员不受保护，只受第 7 节政策约束。

四者都不受标准布局断言覆盖，也都没有绝对 `sizeof`。因此 `status` 的成员**类型**
变更不会被捕获，因为其绝对尺寸被刻意不锁定；四者中只有 `rpc_error` 的成员类型被
锁定。

这四者也没有被断言为标准布局，尽管在本次审计的工具链上（MSVC 19.44、GCC 13、
Clang 18），`rpc_error`、`status` 与 `endpoint` 事实上都是标准布局，`buffer` 在
MSVC 与 GCC 上是、在 Clang 上不是。原因并非 `offsetof` 会非法——它是合法的且能得到
预期数值——而是偏移本身属于标准库配置，与导致绝对尺寸无法锁定的依赖相同。此外，
`is_standard_layout` 断言会在 Clang 下因与本库布局变更无关的原因对 `buffer` 报错。

**当前没有 size-tag 模式。** 没有任何公开头文件声明 `struct_size` 成员；
`crpc::connect_options` 只有超时、缓冲区与 no-delay 字段
（`include/crpc/io.hpp:186-191`）。`src/runtime/runtime.cpp:101` 处的
`struct_size` 赋值作用于 corocc 依赖中局部的 `coro_rt_driver_ops`，不是 CRPC 的
ABI 机制。尺寸标记选项结构体属于计划中的方案，**尚未**实现；在它缺席期间，调用方
与库必须使用同一份头文件构建。


1.0.0 布局（`bd006b2`）是最后一个兼容版本：`http_statistics` 只有 9 个字段，
`sizeof` 为 72 字节，完全没有任何长轮询计数器。2.0.0 新增了 19 个字段：
整个长轮询块（`longpoll_waiters`、`longpoll_subscribers`、
`longpoll_replay_window`、`longpoll_events`、`longpoll_timeouts`、
`longpoll_ack_clamped`、`longpoll_conflicts`、`longpoll_restarts`、
`longpoll_overloaded`）以及 10 个 SSE/压缩/下载计数器。`sizeof` 从 72（9 字段）
增长到 224（28 字段）。1.0.0 发布的 9 个字段偏移全部不变
（`active_connections` 0 … `timed_out_connections` 64）。公开结构体新增字段属于
MAJOR 变更（第 1 节），因此 2.0.0 属 ABI breaking。

`longpoll_replay_window` 的移动只发生在 2.0.0 内部：它最初放在 offset 168
（在 6 个计数器之前），后来被移到末尾（offset 216）。原先排在它后面的 6 个计数器
各前移一个 `std::size_t`（审计所用 64 位构建上为 8 字节）：

| 成员 | 移动前（`4adf359`） | 新偏移 |
|---|---|---|
| `longpoll_events` | 176 | 168 |
| `longpoll_timeouts` | 184 | 176 |
| `longpoll_ack_clamped` | 192 | 184 |
| `longpoll_conflicts` | 200 | 192 |
| `longpoll_restarts` | 208 | 200 |
| `longpoll_overloaded` | 216 | 208 |

该移动前后 `sizeof` 都是 224，原因是本次没有增删字段：`4adf359` 中 6 个计数器
紧跟 `longpoll_replay_window`（结束于 offset 176），正好填满 224 字节，移动前的
尾部填充为 0，移动后仍为 0。没有任何已发布头文件使用过 offset 168 的布局
（无 tag、无 1.0.1），所以该内部移动不构成对已发布消费者的破坏；真正破坏兼容的是
72 → 224 的增长。在尺寸标记选项结构体可用之前，任何公开结构体布局变更都按
MAJOR 处理（见第 8 节）。


增补类型不改变既有布局：`client::resilience_statistics`（标准布局、可平凡拷贝的值类型，见第 4 节）与 `client::resilience_stats()` 是 MINOR 新增符号，只增加导出；`client::statistics` 的字段顺序、大小与含义保持不变。同样，`resilience::fallback_statistics`（标准布局、可平凡拷贝的七计数器值类型）与 `client::fallback_stats()` 也是 MINOR 新增符号。`crpc/resilience/fallback.hpp` 中的 fallback 原语是 header-only，不新增导出符号；按值返回的统计类型是唯一新增符号。

`server::options` 尾部新增了一个 `resilience::clock_source clock_source` 字段。既有成员
偏移全部不变（`send_goaway_on_stop` 仍是 x64 249 / x86 225，`push_subscribe` 仍是
264 / 232），因此只读这些成员的消费方在**同一套头文件内**仍可工作；但
`sizeof(server::options)` 从 304 增长到 **320**（x64）、从 256 增长到 **264**（x86），
`sizeof(server::builder)` 也随之增长（x64 376 → 392，x86 296 → 304），因为 builder 按值
持有 options。options 结构体按值进入导出构造函数，因此按第 1 节这属于 **MAJOR** 变更，
不是 MINOR 扩展：消费方必须用同一份头文件重建。该字段同时是服务端时钟的唯一注入点：
每连接 rate 桶、每服务 rate 桶、每服务熔断器与 dispatcher 的 deadline 判定都读它，
部分注入会让同一个请求的 deadline 被两套「现在」解释。

`client::set_fallback`、`client::run_fallback` 与 `client::record_fallbacks` 是新增导出
符号，因此属于 MINOR 增补，不改变任何布局。`set_fallback` 接收 `void*` 加一对
`noexcept` 的 `retain`/`release`，而不是标准库拥有型类型，因此它保活的引用由调用方
自己的代码创建与销毁，没有消费方的 control block 跨越边界。

`resilience::min_remaining_ns`（1'000'000 ns）是服务端准入地板的**单一权威**：
dispatcher 的执行前 deadline 拒绝与服务端四处 `crpc-degradable` 判定都读它。客户端
**没有**对应地板——它的 deadline 检查只问"本地是否已经过期"，这是**有意的不对称**，
不是缺陷。将来若给客户端加"提前放弃"地板，**必须复用**该常量；新增第二个
`1'000'000` 或 `1ms` 字面量等于造出第二套 deadline 政策。
`fallback_policy::min_remaining` 是另一个**调用者配置**的地板（"降级值是否还来得及"）；
`set_fallback` 的文档要求调用者传该常量，签名本身不带默认实参，因此字面量无处可藏。

## 5. 枚举值

- 写入线上的枚举值永久固定，永不重新编号或复用。
- 新值追加在末尾。
- 解码器收到未知值必须 fail-closed（`protocol_error`），禁止映射为默认值。

## 6. C ABI 层

C++ API 是主接口。为语言绑定与插件作者提供 C ABI（`include/crpc/c_api.h`）：

- 不透明句柄（`crpc_client*`、`crpc_server*`）；
- 签名中不出现 C++ 类型；
- 所有字符串为 `(const char*, size_t)` 对；
- 状态以 `int` 返回，枚举有文档；
- 回调携带 `void* user` 上下文，永不抛出。

导出修饰在编译期选择，与 `CRPC_API` 一致：

| 构建形态 | 宏 | Windows | GCC/Clang |
|---|---|---|---|
| 静态库（默认） | 均无 | 无修饰 | 无修饰 |
| 动态库，构建中 | `CRPC_SHARED` + `CRPC_BUILDING` | `dllexport` | 默认可见性 |
| 动态库，消费方 | `CRPC_SHARED` | `dllimport` | 无修饰 |

C ABI 独立版本化（`CRPC_C_ABI_VERSION`），遵循相同的 MAJOR/MINOR 规则。

全部函数在 `src/c_api.cpp` 中基于 C++ 门面实现，不存在只有声明没有定义的符号。
`tests/test_c_api.c` 以 C 翻译单元编译（CMake 目标设置 `LINKER_LANGUAGE CXX`），
因此导出修饰写错或实现缺失会在构建期失败，而不是拖到下游消费者。

## 7. 兼容矩阵

针对版本 X 构建的二进制在版本 Y 上必须能链接并运行，当且仅当：

| X → Y | 保证 |
|---|---|
| 同一 MAJOR，Y.MINOR ≥ X.MINOR | 是（向前兼容） |
| 同一 MAJOR，Y.MINOR < X.MINOR | 否（新构建可能使用新符号） |
| 不同 MAJOR | 否 |

本表每一行仅在消费方与库由**同一份头文件**构建时成立。向前兼容的组合并不允许把旧头文件与新二进制混用：第 2 节已说明，头内模板会在消费方翻译单元内实例化 `client_detail` 类型，因此这些类型的布局变更即使在同一个 MAJOR 内也会破坏这种混编。

编译器与标准库：CRPC 不承诺跨标准库（MSVC STL / libstdc++ / libc++）
或跨不同 `_ITERATOR_DEBUG_LEVEL` / `_GLIBCXX_DEBUG` 设置的 ABI 兼容。
共享库必须与消费方使用同一工具链族与调试设置。这一点只写进构建文档：
没有 CMake 配置期的检查，增加该检查属于**尚未**实现的工作。

## 8. 变更流程

1. 违反第 2–5 节的变更必须提升 MAJOR 版本。
2. 同一个提交必须更新本文档与变更日志（[CHANGELOG.zh-CN.md](CHANGELOG.zh-CN.md)），并在 MAJOR 提升属于布局或签名变更时把破坏点记录进迁移指南（[migration.zh-CN.md](migration.zh-CN.md)）；可行时增加兼容垫片。
3. 变更日志必须明确写出："ABI breaking: <符号> 从 … 变为 …"。
4. 导出门禁 `scripts/export_gate.py` 由 `scripts/test.ps1` 与 `scripts/test.sh` 在 `CRPC_BUILD_SHARED=ON` 时运行。它静态扫描 `include/crpc/**` 并要求：(A) 每个含 out-of-line 成员的非模板类都带 `CRPC_API`；(B) 每个带装饰的类确实导出了符号，以 `dumpbin /exports` 或 `nm -D -C` 校验；(C) 每个无函数体的命名空间级声明都带 `CRPC_API`；(D) 类模板不得带 `CRPC_API`；(E) 导出类不得派生自未装饰的非模板基类。经评审的实现内部例外记录在 `scripts/export_baseline.txt`；缺失或过期的条目都会使门禁失败。内部翻译单元 `src/abi_check.cpp` 在编译期守护公开记录的布局。两者都不把导出表与上一发布版本对比：该比对仍是人工审阅与上一版本的 diff。

## 9. 不保证的内容

- 模板实例化：`result<T>`、`span<T>` 与拦截器链是仅头文件的，会内联进消费方；
  它们的 ABI 属于消费方。
- 头文件中的内联函数：使用不同编译选项的消费方可能内联到不同版本；
  因此任何影响正确性的行为都不会只存在于内联函数里。
- `result<T>` 内部 `std::variant` 的布局（当 `T` 是用户类型时）：
  对给定工具链与 `T` 稳定，跨工具链不稳定。
- hook 与 handler 边界上的异常行为不属于 ABI 保证；契约见[架构 §15](architecture.zh-CN.md#s15)。
