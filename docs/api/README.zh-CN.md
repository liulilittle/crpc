# CRPC API 参考

[English](README.md) | **简体中文**

[`include/crpc`](../../include/crpc) 下公开头文件的地图，以及每个头文件定义的关键类型。
这是一份索引，不是教程：程序的形态见[教程](../tutorial.zh-CN.md)，线上与 ABI 规则见
[线上协议](../protocol.zh-CN.md) 与 [ABI 策略](../abi.zh-CN.md)。

头文件按子系统分组。每一项都链接到头文件本身，头文件才是权威声明。

## 核心原语

| 头文件 | 关键类型 | 说明 |
|---|---|---|
| [`config.hpp`](../../include/crpc/config.hpp) | `u8`…`u64`、`CRPC_API`、`CRPC_OS_*` | 平台/编译器探测与显式宽度整数别名。 |
| [`span.hpp`](../../include/crpc/span.hpp) | `span<T>`、`as_bytes` | 最小、constexpr、边界收敛的 span；唯一的借用区间类型。 |
| [`buffer.hpp`](../../include/crpc/buffer.hpp) | `buffer` | 可增长字节缓冲，提供 `readable()`/`writable()` span。 |
| [`endian.hpp`](../../include/crpc/endian.hpp) | `load_le*`、`store_le*` | 显式小端读写，也是线上唯一的字节序。 |
| [`wire.hpp`](../../include/crpc/wire.hpp) | `wire_reader`、`wire_writer` | 带边界检查的字节读写器：varint、zigzag、定长字段。 |
| [`frame.hpp`](../../include/crpc/frame.hpp) | `frame`、`frame_header`、`frame_type`、`frame_decoder`、`encode_frame`、`decode_frame` | 多路复用二进制帧头及其编解码。 |
| [`frame_compression.hpp`](../../include/crpc/frame_compression.hpp) | `encode_frame_compressed`、`frame_compression_info` | LZ4 block 帧压缩（唯一受支持的算法）。 |
| [`delivery_mode.hpp`](../../include/crpc/delivery_mode.hpp) | `delivery_mode` | `reliable` / `unreliable`，RPC 调用与推送共用。 |
| [`id.hpp`](../../include/crpc/id.hpp) | `service_id`、`method_id` | 线上 `u64` id 的强类型包装，避免两者被写反。 |
| [`error.hpp`](../../include/crpc/error.hpp) | `rpc_code`、`rpc_error`、`result<T>`、`status`、`metadata_list` | 值或错误模型：每个失败都是值，绝不抛出。 |
| [`runtime.hpp`](../../include/crpc/runtime.hpp) | `runtime`、`runtime_options` | 单个分片：一线程、一协程运行时、一定时器轮、有界收件箱与 handler 池。 |

## RPC —— 调用与服务器门面

| 头文件 | 关键类型 | 说明 |
|---|---|---|
| [`rpc/call.hpp`](../../include/crpc/rpc/call.hpp) | `call_state`、`call_context`、`call_slot`、`call_slot_pool`、`call<T>`、`call_codec<T>` | 调用状态机、每次调用的上下文与可移动句柄。 |
| [`rpc/dispatcher.hpp`](../../include/crpc/rpc/dispatcher.hpp) | `service_registry`、`method_slot`、`method_handler`、`dispatcher`、`register_typed_method` | 服务端方法表与分发；类型化方法无需 RTTI 即可用 `call_codec`。 |
| [`rpc/client.hpp`](../../include/crpc/rpc/client.hpp) | `rpc::client`、`client_options`、`start_call<Resp>`、`set_push_sink`、`subscribe_push` | 单条连接：读端、写队列、类型化调用与客户端推送。 |
| [`server.hpp`](../../include/crpc/server.hpp) | `server`、`server::builder`、`server::options`、`publish`、`publish_result` | 服务器门面：监听、分片、handler 池、优雅停止与主题发布。 |
| [`client_facade.hpp`](../../include/crpc/client_facade.hpp) | `client`、`client::builder`、`client::stub<Resp,Req>`、`client::call<Resp>` | 高层客户端：连接池、负载均衡、熔断、重试与类型化 stub。 |
| [`push/hub.hpp`](../../include/crpc/push/hub.hpp) | `push::hub`、`push::subscribe_options`、`push::push_message` | 服务器推送 hub：主题、有界每订阅队列与发布路由。 |
| [`push/wire.hpp`](../../include/crpc/push/wire.hpp) | `push::push_envelope`、`push::push_message_view`、`push::push_subscription` | PUSH/PUSH_ACK 负载编解码与订阅控制契约。 |
| [`push/stream.hpp`](../../include/crpc/push/stream.hpp) | `stream_transport`、`client_streaming`、`server_streaming` | DATA 帧流：客户端/服务器/双向形态与流控。 |

## 传输

| 头文件 | 关键类型 | 说明 |
|---|---|---|
| [`io.hpp`](../../include/crpc/io.hpp) | `io::source`、`io::sink`、`connection`、`backend_factory`、`endpoint`、`inproc_backend` | 双接口传输契约、端点类型与进程内后端。 |
| [`io/iocp_backend.hpp`](../../include/crpc/io/iocp_backend.hpp) | `iocp_backend` | Windows IOCP TCP 后端。 |
| [`io/epoll_backend.hpp`](../../include/crpc/io/epoll_backend.hpp) | `epoll_backend` | Linux 边沿触发 epoll TCP 后端。 |
| [`io/tls.hpp`](../../include/crpc/io/tls.hpp) | `tls_decorator`、`tls_errc` | 双接口契约之上的 TLS 装饰器；需要 `CRPC_WITH_OPENSSL`。 |

## HTTP 与 MVC

| 头文件 | 关键类型 | 说明 |
|---|---|---|
| [`http/server.hpp`](../../include/crpc/http/server.hpp) | `http::http_server`、`http_server::builder` | 基于任意后端监听的 HTTP/1.1 + HTTP/2（h2c/h2）+ HTTPS 服务器门面。 |
| [`http/h2_client.hpp`](../../include/crpc/http/h2_client.hpp) | `http::h2_client`, `h2_client_options` | HTTP/2 多路复用客户端：同一连接上的并发流（h2c 或 TLS+ALPN h2）|
| [`http/message.hpp`](../../include/crpc/http/message.hpp) | `http::request`、`http::response` | HTTP/1.1 报文模型、增量解析器与序列化器。 |
| [`http2/frame.hpp`](../../include/crpc/http2/frame.hpp) | `h2::frame`、`h2::frame_header` | HTTP/2 帧层。 |
| [`http2/hpack.hpp`](../../include/crpc/http2/hpack.hpp) | `h2::hpack_encoder`、`h2::hpack_decoder` | HPACK 头压缩。 |
| [`http2/connection.hpp`](../../include/crpc/http2/connection.hpp) | `h2::connection` | HTTP/2 连接状态机。 |
| [`http/content_coding.hpp`](../../include/crpc/http/content_coding.hpp) | `http::content_coding` | Content-Encoding 协商与流式 gzip/deflate。 |
| [`http/multipart.hpp`](../../include/crpc/http/multipart.hpp) | `http::multipart_parser` | 流式 `multipart/form-data` 解析器。 |
| [`http/file_transfer.hpp`](../../include/crpc/http/file_transfer.hpp) | `http::upload_sink`、`http::download_source` | HTTP 上传 sink 与区间下载 source。 |
| [`http/sse.hpp`](../../include/crpc/http/sse.hpp) | `http::sse` | Server-Sent Events 分帧辅助。 |
| [`mvc/router.hpp`](../../include/crpc/mvc/router.hpp) | `mvc::router`、`mvc::route` | MVC 路由表、匹配与分发。 |
| [`mvc/binding.hpp`](../../include/crpc/mvc/binding.hpp) | `mvc::bind_request` | 请求参数绑定到 handler 形参。 |
| [`mvc/attributes.hpp`](../../include/crpc/mvc/attributes.hpp) | `CRPC_HTTP_*` 宏 | attribute 声明的路由与字段。 |

## 弹性

| 头文件 | 关键类型 | 说明 |
|---|---|---|
| [`resilience/retry.hpp`](../../include/crpc/resilience/retry.hpp) | `retry_sequence`、`retry_budget`、`retry_policy` | 单次调用重试决策与共享令牌桶预算。 |
| [`resilience/circuit_breaker.hpp`](../../include/crpc/resilience/circuit_breaker.hpp) | `breaker_config`、`breaker_permit` | 熔断器。 |
| [`resilience/rate_limiter.hpp`](../../include/crpc/resilience/rate_limiter.hpp) | `rate_limit_config` | 令牌桶限流器。 |
| [`resilience/bulkhead.hpp`](../../include/crpc/resilience/bulkhead.hpp) | `bulkhead_config`、`bulkhead_permit` | 舱壁（有界计数信号量）。 |
| [`resilience/load_balancer.hpp`](../../include/crpc/resilience/load_balancer.hpp) | `lb_strategy` | 服务实例间负载均衡。 |
| [`resilience/fallback.hpp`](../../include/crpc/resilience/fallback.hpp) | `fallback_fn`、`fallback_outcome` | 降级词汇与值入口。 |
| [`resilience/admission.hpp`](../../include/crpc/resilience/admission.hpp) | `stage_kind`、`reject_code` | 唯一的准入阶段 → `rpc_code` 映射表。 |

## 可观测性

| 头文件 | 关键类型 | 说明 |
|---|---|---|
| [`observability/context.hpp`](../../include/crpc/observability/context.hpp) | `obs::trace_context`、`obs::span` | W3C `traceparent`/baggage 链路上下文。 |
| [`observability/interceptor.hpp`](../../include/crpc/observability/interceptor.hpp) | `obs::interceptor_chain` | AOP 拦截器链。 |
| [`observability/log.hpp`](../../include/crpc/observability/log.hpp) | `obs::logger`、`log_record` | 高性能异步结构化日志。 |
| [`observability/metrics.hpp`](../../include/crpc/observability/metrics.hpp) | `obs::metrics`、`obs::counter`、`obs::gauge`、`obs::histogram` | 计数器、仪表与直方图，未配置时零成本。 |
| [`observability/tracer.hpp`](../../include/crpc/observability/tracer.hpp) | `obs::tracer` | span、头部采样与异步导出。 |

## 集群与流控

| 头文件 | 关键类型 | 说明 |
|---|---|---|
| [`cluster/registry.hpp`](../../include/crpc/cluster/registry.hpp) | `cluster::registry` | 服务目录，带不可变快照与单调递增索引。 |
| [`cluster/lease.hpp`](../../include/crpc/cluster/lease.hpp) | `cluster::lease_manager` | 租约/会话管理器。 |
| [`cluster/election.hpp`](../../include/crpc/cluster/election.hpp) | `cluster::election` | 选主原语。 |
| [`cluster/health.hpp`](../../include/crpc/cluster/health.hpp) | `cluster::health_checker` | 健康检查器。 |
| [`cluster/health_bridge.hpp`](../../include/crpc/cluster/health_bridge.hpp) | `cluster::health_bridge`、`cluster::health_instance_set` | 带去抖的“健康结果→注册表”单写者，以及只读负载均衡视图。 |
| [`cluster/registry_log.hpp`](../../include/crpc/cluster/registry_log.hpp) | `cluster::registry_log`、`cluster::file_registry_log`、`cluster::memory_registry_log` | 可持久化、可回放的记录日志，作为注册表的写前日志。 |
| [`cluster/raft_state.hpp`](../../include/crpc/cluster/raft_state.hpp) | `cluster::raft_persistent_state`、`cluster::raft_state_store` | Raft 必须在重启后保留的两个字段（term 与投票），原子写入、失败即拒绝读取。 |
| [`cluster/raft_node.hpp`](../../include/crpc/cluster/raft_node.hpp) | `cluster::raft_role`、`cluster::raft_config`、`cluster::raft_message`、`cluster::raft_node` | 单进程 Raft 节点：PreVote、投票、日志复制与提交。消息经由调用方提供的 transport 传递，因此测试可以确定性地驱动时序。 |
| [`cluster/watch.hpp`](../../include/crpc/cluster/watch.hpp) | `cluster::watch_hub` | 基于索引的目录 watch/订阅。 |
| [`flow/estimator.hpp`](../../include/crpc/flow/estimator.hpp) | `flow::rtt_estimator`、`flow::pacer`、`flow::congestion_window`、`flow::rtt_tracker` | 自适应流控原语。 |
| [`rpc/flow_control.hpp`](../../include/crpc/rpc/flow_control.hpp) | `rpc::flow_controller`、`rpc::flow_control_options`、`rpc::flow_state` | 连接级流控：字节窗口、PING/PONG 探针、每 lane 配额、pacer。 |

## 编解码与序列化

| 头文件 | 关键类型 | 说明 |
|---|---|---|
| [`codec.hpp`](../../include/crpc/codec.hpp) | `codec_registry`、`encode_into`、`decode_from` | 编解码注册表与类型化序列化接口。 |
| [`codec_id.hpp`](../../include/crpc/codec_id.hpp) | `codec_id` | 编解码标识。 |

## Redis 客户端

| 头文件 | 关键类型 | 说明 |
|---|---|---|
| [`redis/resp.hpp`](../../include/crpc/redis/resp.hpp) | `resp_value`、`resp_parser`、`resp_encoder`、`resp_protocol` | RESP2/RESP3 值模型、增量解析器与编码器。 |
| [`redis/cache.hpp`](../../include/crpc/redis/cache.hpp) | `client_cache` | 有界客户端 LRU 缓存，通过 RESP3 CLIENT TRACKING 使失效推送被动刷新。 |
| [`redis/command_table.hpp`](../../include/crpc/redis/command_table.hpp) | `command_spec`、`k_command_table`、`lookup_command` | 客户端可发射的每条命令的已入库元数据（arity、键位），与真实服务器保持同步。 |
| [`redis/connection.hpp`](../../include/crpc/redis/connection.hpp) | `connection`、`connection_pool`、`connection_config`、`pool_config` | 基于 `io::source`/`io::sink` 的字节传输、重连/退避与连接池。 |
| [`redis/pipeline.hpp`](../../include/crpc/redis/pipeline.hpp) | `command`、`command_executor`、`message_channel`、`command_channel`、`pipeline` | 单条命令流、批量与面向传输的 RESP 通道。 |
| [`redis/pubsub.hpp`](../../include/crpc/redis/pubsub.hpp) | `pubsub`、`pubsub_message`、`pubsub_kind` | Pub/Sub 订阅/退订、消息解码与 `PUBLISH`/`PUBSUB`。 |
| [`redis/sentinel.hpp`](../../include/crpc/redis/sentinel.hpp) | `sentinel`、`sentinel_options`、`sentinel_status`、`sentinel_connection_executor` | 主节点发现、下线判定、选举与故障转移。 |
| [`redis/stream.hpp`](../../include/crpc/redis/stream.hpp) | `stream`、`stream_id`、`stream_entry`、`stream_read` | Redis Streams 与消费者组。 |
| [`redis/transaction.hpp`](../../include/crpc/redis/transaction.hpp) | `transaction`、`transaction_exec_result` | `WATCH`/`MULTI`/`EXEC`/`DISCARD`。 |
| [`redis/types.hpp`](../../include/crpc/redis/types.hpp) | `value`、`*_type` 构造器、`decode_*`、迭代器 | 类型化值模型、命令构造器与回复投影。 |
| [`redis/client.hpp`](../../include/crpc/redis/client.hpp) | `client`、`client::options` | 连接池 + 类型化命令门面。 |
| [`redis/cluster.hpp`](../../include/crpc/redis/cluster.hpp) | `cluster`、`cluster_topology`、`cluster_slot_executor` | 槽路由、`CLUSTER SHARDS`/`SLOTS`、`MOVED`/`ASK` 与 pin。 |
| [`redis/rtti.hpp`](../../include/crpc/redis/rtti.hpp) | `encode_to_value`、`decode_from_value`、`hash_set_command`、`decode_hash_reply` | 反射驱动的 DTO <-> value/hash 绑定。 |

API 地图、快速开始、异步模型、错误处理与已知限制见
[Redis 客户端](../redis-client.zh-CN.md)。

## C ABI

| 头文件 | 关键类型 | 说明 |
|---|---|---|
| [`c_api.h`](../../include/crpc/c_api.h) | `crpc_*` 函数 | 稳定的 C 入口。见 [ABI 策略](../abi.zh-CN.md)。 |

## 完整头文件索引

上面各节按用途挑出关键类型；这里把 `include/crpc` 下的**每一个**公开头文件按
所在目录列全，供按名字查找。条目只是链接，权威声明在各头文件里。

该清单由 `tests/test_docs_lint.cpp` 的
`docs_api_index_lists_every_public_header` 守卫：新增公开头文件而没在这里登记
会让测试失败。

### `crpc`

- [`buffer.hpp`](../../include/crpc/buffer.hpp)
- [`client_facade.hpp`](../../include/crpc/client_facade.hpp)
- [`codec.hpp`](../../include/crpc/codec.hpp)
- [`codec_id.hpp`](../../include/crpc/codec_id.hpp)
- [`config.hpp`](../../include/crpc/config.hpp)
- [`delivery_mode.hpp`](../../include/crpc/delivery_mode.hpp)
- [`diagnostics.hpp`](../../include/crpc/diagnostics.hpp)
- [`endian.hpp`](../../include/crpc/endian.hpp)
- [`error.hpp`](../../include/crpc/error.hpp)
- [`frame.hpp`](../../include/crpc/frame.hpp)
- [`frame_compression.hpp`](../../include/crpc/frame_compression.hpp)
- [`id.hpp`](../../include/crpc/id.hpp)
- [`io.hpp`](../../include/crpc/io.hpp)
- [`runtime.hpp`](../../include/crpc/runtime.hpp)
- [`server.hpp`](../../include/crpc/server.hpp)
- [`span.hpp`](../../include/crpc/span.hpp)
- [`wire.hpp`](../../include/crpc/wire.hpp)

### `crpc/algorithm`

- [`algorithm/astar.hpp`](../../include/crpc/algorithm/astar.hpp)
- [`algorithm/heuristic.hpp`](../../include/crpc/algorithm/heuristic.hpp)
- [`algorithm/pathfind.hpp`](../../include/crpc/algorithm/pathfind.hpp)

### `crpc/async`

- [`async/executor.hpp`](../../include/crpc/async/executor.hpp)

### `crpc/concurrency`

- [`concurrency/volatile.hpp`](../../include/crpc/concurrency/volatile.hpp) — `volatile_read`、`volatile_write`、`full_fence`、`signal_fence`：对普通 1/2/4/8 字节值的 acquire/release 访问。

### `crpc/crypto`

- [`crypto/base64.hpp`](../../include/crpc/crypto/base64.hpp)
- [`crypto/hash.hpp`](../../include/crpc/crypto/hash.hpp)
- [`crypto/rsa.hpp`](../../include/crpc/crypto/rsa.hpp)

### `crpc/db`

- [`db/bulk.hpp`](../../include/crpc/db/bulk.hpp)
- [`db/channel.hpp`](../../include/crpc/db/channel.hpp)
- [`db/client.hpp`](../../include/crpc/db/client.hpp)
- [`db/cluster.hpp`](../../include/crpc/db/cluster.hpp)
- [`db/connection.hpp`](../../include/crpc/db/connection.hpp)
- [`db/hbase/rest.hpp`](../../include/crpc/db/hbase/rest.hpp)
- [`db/mapping.hpp`](../../include/crpc/db/mapping.hpp)
- [`db/memory_result_set.hpp`](../../include/crpc/db/memory_result_set.hpp)
- [`db/mongo/auth.hpp`](../../include/crpc/db/mongo/auth.hpp)
- [`db/mongo/bson.hpp`](../../include/crpc/db/mongo/bson.hpp)
- [`db/mongo/connection.hpp`](../../include/crpc/db/mongo/connection.hpp)
- [`db/mongo/protocol.hpp`](../../include/crpc/db/mongo/protocol.hpp)
- [`db/mysql/auth.hpp`](../../include/crpc/db/mysql/auth.hpp)
- [`db/mysql/channel.hpp`](../../include/crpc/db/mysql/channel.hpp)
- [`db/mysql/cluster.hpp`](../../include/crpc/db/mysql/cluster.hpp)
- [`db/mysql/connection.hpp`](../../include/crpc/db/mysql/connection.hpp)
- [`db/mysql/protocol.hpp`](../../include/crpc/db/mysql/protocol.hpp)
- [`db/odbc/connection.hpp`](../../include/crpc/db/odbc/connection.hpp)
- [`db/oracle/tns.hpp`](../../include/crpc/db/oracle/tns.hpp)
- [`db/oracle/ttc.hpp`](../../include/crpc/db/oracle/ttc.hpp)
- [`db/result_set_list.hpp`](../../include/crpc/db/result_set_list.hpp)
- [`db/rtti_mapping.hpp`](../../include/crpc/db/rtti_mapping.hpp)
- [`db/stream_channel.hpp`](../../include/crpc/db/stream_channel.hpp)
- [`db/tds/cluster.hpp`](../../include/crpc/db/tds/cluster.hpp)
- [`db/tds/connection.hpp`](../../include/crpc/db/tds/connection.hpp)
- [`db/tds/protocol.hpp`](../../include/crpc/db/tds/protocol.hpp)
- [`db/tidb/connection.hpp`](../../include/crpc/db/tidb/connection.hpp)
- [`db/tidb/retry.hpp`](../../include/crpc/db/tidb/retry.hpp)
- [`db/types.hpp`](../../include/crpc/db/types.hpp)

### `crpc/detail`

- [`detail/atomic_shared_ptr.hpp`](../../include/crpc/detail/atomic_shared_ptr.hpp)

### `crpc/fs`

- [`fs/directory.hpp`](../../include/crpc/fs/directory.hpp)
- [`fs/file.hpp`](../../include/crpc/fs/file.hpp)
- [`fs/fs.hpp`](../../include/crpc/fs/fs.hpp)
- [`fs/memory.hpp`](../../include/crpc/fs/memory.hpp)
- [`fs/mmap.hpp`](../../include/crpc/fs/mmap.hpp)
- [`fs/path.hpp`](../../include/crpc/fs/path.hpp)
- [`fs/shared_memory.hpp`](../../include/crpc/fs/shared_memory.hpp)
- [`fs/temp.hpp`](../../include/crpc/fs/temp.hpp)

### `crpc/game`

- [`game/ai/adaboost.hpp`](../../include/crpc/game/ai/adaboost.hpp)
- [`game/ai/behavior_tree.hpp`](../../include/crpc/game/ai/behavior_tree.hpp)
- [`game/ai/decision_tree.hpp`](../../include/crpc/game/ai/decision_tree.hpp)
- [`game/aoi/aoi.hpp`](../../include/crpc/game/aoi/aoi.hpp)
- [`game/aoi/grid.hpp`](../../include/crpc/game/aoi/grid.hpp)
- [`game/aoi/world.hpp`](../../include/crpc/game/aoi/world.hpp)
- [`game/geom/attack.hpp`](../../include/crpc/game/geom/attack.hpp)
- [`game/geom/collide2d.hpp`](../../include/crpc/game/geom/collide2d.hpp)
- [`game/geom/collide3d.hpp`](../../include/crpc/game/geom/collide3d.hpp)
- [`game/geom/hitbox.hpp`](../../include/crpc/game/geom/hitbox.hpp)
- [`game/geom/shapes2d.hpp`](../../include/crpc/game/geom/shapes2d.hpp)
- [`game/geom/shapes3d.hpp`](../../include/crpc/game/geom/shapes3d.hpp)
- [`game/geom/transform.hpp`](../../include/crpc/game/geom/transform.hpp)
- [`game/geom/vec.hpp`](../../include/crpc/game/geom/vec.hpp)

### `crpc/http`

- [`http/client.hpp`](../../include/crpc/http/client.hpp)
- [`http/content_coding.hpp`](../../include/crpc/http/content_coding.hpp)
- [`http/file_transfer.hpp`](../../include/crpc/http/file_transfer.hpp)
- [`http/forwarded.hpp`](../../include/crpc/http/forwarded.hpp)
- [`http/h2_client.hpp`](../../include/crpc/http/h2_client.hpp)
- [`http/message.hpp`](../../include/crpc/http/message.hpp)
- [`http/multipart.hpp`](../../include/crpc/http/multipart.hpp)
- [`http/server.hpp`](../../include/crpc/http/server.hpp)
- [`http/sse.hpp`](../../include/crpc/http/sse.hpp)

### `crpc/http2`

- [`http2/connection.hpp`](../../include/crpc/http2/connection.hpp)
- [`http2/frame.hpp`](../../include/crpc/http2/frame.hpp)
- [`http2/hpack.hpp`](../../include/crpc/http2/hpack.hpp)

### `crpc/io`

- [`io/epoll_backend.hpp`](../../include/crpc/io/epoll_backend.hpp)
- [`io/iocp_backend.hpp`](../../include/crpc/io/iocp_backend.hpp)
- [`io/tls.hpp`](../../include/crpc/io/tls.hpp)

### `crpc/ipc`

- [`ipc/channel.hpp`](../../include/crpc/ipc/channel.hpp)
- [`ipc/ring_buffer.hpp`](../../include/crpc/ipc/ring_buffer.hpp)
- [`ipc/spsc_queue.hpp`](../../include/crpc/ipc/spsc_queue.hpp)

### `crpc/lock`

- [`lock/local_lock.hpp`](../../include/crpc/lock/local_lock.hpp)

### `crpc/memcached`

- [`memcached/client.hpp`](../../include/crpc/memcached/client.hpp)
- [`memcached/hash.hpp`](../../include/crpc/memcached/hash.hpp)
- [`memcached/protocol.hpp`](../../include/crpc/memcached/protocol.hpp)
- [`memcached/router.hpp`](../../include/crpc/memcached/router.hpp)

### `crpc/memory`

- [`memory/arena.hpp`](../../include/crpc/memory/arena.hpp)
- [`memory/object_pool.hpp`](../../include/crpc/memory/object_pool.hpp)
- [`memory/pool_allocator.hpp`](../../include/crpc/memory/pool_allocator.hpp)

### `crpc/mq`

- [`mq/amqp/connection.hpp`](../../include/crpc/mq/amqp/connection.hpp)
- [`mq/amqp/protocol.hpp`](../../include/crpc/mq/amqp/protocol.hpp)
- [`mq/amqp/types.hpp`](../../include/crpc/mq/amqp/types.hpp)
- [`mq/msmq/queue.hpp`](../../include/crpc/mq/msmq/queue.hpp)
- [`mq/msmq/transaction.hpp`](../../include/crpc/mq/msmq/transaction.hpp)
- [`mq/msmq/types.hpp`](../../include/crpc/mq/msmq/types.hpp)

### `crpc/mvc`

- [`mvc/attributes.hpp`](../../include/crpc/mvc/attributes.hpp)
- [`mvc/binding.hpp`](../../include/crpc/mvc/binding.hpp)
- [`mvc/router.hpp`](../../include/crpc/mvc/router.hpp)

### `crpc/net`

- [`net/address.hpp`](../../include/crpc/net/address.hpp)
- [`net/backoff.hpp`](../../include/crpc/net/backoff.hpp)
- [`net/channel.hpp`](../../include/crpc/net/channel.hpp)
- [`net/connection.hpp`](../../include/crpc/net/connection.hpp)
- [`net/connector.hpp`](../../include/crpc/net/connector.hpp)
- [`net/icmp.hpp`](../../include/crpc/net/icmp.hpp)
- [`net/ip.hpp`](../../include/crpc/net/ip.hpp)
- [`net/listener.hpp`](../../include/crpc/net/listener.hpp)
- [`net/ping.hpp`](../../include/crpc/net/ping.hpp)
- [`net/udp.hpp`](../../include/crpc/net/udp.hpp)

### `crpc/observability`

- [`observability/context.hpp`](../../include/crpc/observability/context.hpp)
- [`observability/interceptor.hpp`](../../include/crpc/observability/interceptor.hpp)
- [`observability/log.hpp`](../../include/crpc/observability/log.hpp)
- [`observability/metrics.hpp`](../../include/crpc/observability/metrics.hpp)
- [`observability/tracer.hpp`](../../include/crpc/observability/tracer.hpp)

### `crpc/orm`

- [`orm/annotate.hpp`](../../include/crpc/orm/annotate.hpp)
- [`orm/fwd.hpp`](../../include/crpc/orm/fwd.hpp)
- [`orm/metadata.hpp`](../../include/crpc/orm/metadata.hpp)
- [`orm/orm.hpp`](../../include/crpc/orm/orm.hpp)
- [`orm/session.hpp`](../../include/crpc/orm/session.hpp)
- [`orm/sql.hpp`](../../include/crpc/orm/sql.hpp)
- [`orm/value.hpp`](../../include/crpc/orm/value.hpp)

### `crpc/pattern`

- [`pattern/constraint.hpp`](../../include/crpc/pattern/constraint.hpp)
- [`pattern/memento.hpp`](../../include/crpc/pattern/memento.hpp)
- [`pattern/version.hpp`](../../include/crpc/pattern/version.hpp)

### `crpc/push`

- [`push/bridge.hpp`](../../include/crpc/push/bridge.hpp)
- [`push/hub.hpp`](../../include/crpc/push/hub.hpp)
- [`push/stream.hpp`](../../include/crpc/push/stream.hpp)
- [`push/wire.hpp`](../../include/crpc/push/wire.hpp)

### `crpc/redis`

- [`redis/cache.hpp`](../../include/crpc/redis/cache.hpp)
- [`redis/client.hpp`](../../include/crpc/redis/client.hpp)
- [`redis/cluster.hpp`](../../include/crpc/redis/cluster.hpp)
- [`redis/command_table.hpp`](../../include/crpc/redis/command_table.hpp)
- [`redis/connection.hpp`](../../include/crpc/redis/connection.hpp)
- [`redis/distributed_lock.hpp`](../../include/crpc/redis/distributed_lock.hpp)
- [`redis/keyspace.hpp`](../../include/crpc/redis/keyspace.hpp)
- [`redis/pipeline.hpp`](../../include/crpc/redis/pipeline.hpp)
- [`redis/pubsub.hpp`](../../include/crpc/redis/pubsub.hpp)
- [`redis/resp.hpp`](../../include/crpc/redis/resp.hpp)
- [`redis/rtti.hpp`](../../include/crpc/redis/rtti.hpp)
- [`redis/scan.hpp`](../../include/crpc/redis/scan.hpp)
- [`redis/scripting.hpp`](../../include/crpc/redis/scripting.hpp)
- [`redis/sentinel.hpp`](../../include/crpc/redis/sentinel.hpp)
- [`redis/stream.hpp`](../../include/crpc/redis/stream.hpp)
- [`redis/transaction.hpp`](../../include/crpc/redis/transaction.hpp)
- [`redis/types.hpp`](../../include/crpc/redis/types.hpp)

### `crpc/resilience`

- [`resilience/admission.hpp`](../../include/crpc/resilience/admission.hpp)
- [`resilience/bulkhead.hpp`](../../include/crpc/resilience/bulkhead.hpp)
- [`resilience/circuit_breaker.hpp`](../../include/crpc/resilience/circuit_breaker.hpp)
- [`resilience/fallback.hpp`](../../include/crpc/resilience/fallback.hpp)
- [`resilience/load_balancer.hpp`](../../include/crpc/resilience/load_balancer.hpp)
- [`resilience/rate_limiter.hpp`](../../include/crpc/resilience/rate_limiter.hpp)
- [`resilience/retry.hpp`](../../include/crpc/resilience/retry.hpp)
- [`resilience/smart_policy.hpp`](../../include/crpc/resilience/smart_policy.hpp)

### `crpc/rpc`

- [`rpc/call.hpp`](../../include/crpc/rpc/call.hpp)
- [`rpc/client.hpp`](../../include/crpc/rpc/client.hpp)
- [`rpc/dedup.hpp`](../../include/crpc/rpc/dedup.hpp)
- [`rpc/dispatcher.hpp`](../../include/crpc/rpc/dispatcher.hpp)
- [`rpc/flow_control.hpp`](../../include/crpc/rpc/flow_control.hpp)
- [`rpc/lane_queue.hpp`](../../include/crpc/rpc/lane_queue.hpp)
- [`rpc/method_macro.hpp`](../../include/crpc/rpc/method_macro.hpp)
- [`rpc/method_meta.hpp`](../../include/crpc/rpc/method_meta.hpp)

### `crpc/text`

- [`text/case.hpp`](../../include/crpc/text/case.hpp)
- [`text/number.hpp`](../../include/crpc/text/number.hpp)
- [`text/string_ops.hpp`](../../include/crpc/text/string_ops.hpp)
- [`text/utf.hpp`](../../include/crpc/text/utf.hpp)

### `crpc/ws`

- [`ws/client.hpp`](../../include/crpc/ws/client.hpp)
- [`ws/frame.hpp`](../../include/crpc/ws/frame.hpp)
- [`ws/handshake.hpp`](../../include/crpc/ws/handshake.hpp)
- [`ws/server.hpp`](../../include/crpc/ws/server.hpp)
- [`ws/stream.hpp`](../../include/crpc/ws/stream.hpp)

### `crpc/actor`

- [`actor/actor.hpp`](../../include/crpc/actor/actor.hpp) — `actor`：类型化投递与 stop 生命周期，有界邮箱背压。
- [`actor/mailbox.hpp`](../../include/crpc/actor/mailbox.hpp) — `mailbox<Message>`：无锁 SPSC，满时返回 `resource_exhausted`。

### `crpc/aspect`

- [`aspect/aspect.hpp`](../../include/crpc/aspect/aspect.hpp) — `advice`、`pointcut`、`aspect_chain`：before/after/around/异常四类通知，热路径零分配。

### `crpc/cache_line`

- [`cache_line.hpp`](../../include/crpc/cache_line.hpp) — `cache_line_size`、`CRPC_CACHE_LINE_ALIGN`：内部热状态的 64 字节对齐。

### `crpc/crypto` 新增

- [`crypto/md5.hpp`](../../include/crpc/crypto/md5.hpp) — `md5`、`md5_digest`：RFC 1321（仅 UUID v3 用途，非安全用途）。
- [`crypto/uuid.hpp`](../../include/crpc/crypto/uuid.hpp) — `uuid`、`uuid_v1`…`uuid_v7`、`parse_uuid`、`to_string`、`uuid_hash`。

### `crpc/db/sqlite`

- [`db/sqlite/connection.hpp`](../../include/crpc/db/sqlite/connection.hpp) — `sqlite::connection`、`connection_factory`：基于 sqlite3 C API 的嵌入式 SQL 后端。

### `crpc/domain`（DDD/CQRS）

- [`domain/entity.hpp`](../../include/crpc/domain/entity.hpp) — `entity<Id, Derived>`：按标识相等。
- [`domain/value_object.hpp`](../../include/crpc/domain/value_object.hpp) — `value_object<Derived>`：按值相等。
- [`domain/aggregate.hpp`](../../include/crpc/domain/aggregate.hpp) — `aggregate_root<Id, Derived>`：一致性边界与未提交事件。
- [`domain/domain_event.hpp`](../../include/crpc/domain/domain_event.hpp) — `domain_event`：名称/负载/单调序号。
- [`domain/repository.hpp`](../../include/crpc/domain/repository.hpp) — `repository<T, Id>`、`in_memory_repository`。
- [`domain/unit_of_work.hpp`](../../include/crpc/domain/unit_of_work.hpp) — `unit_of_work`：原子提交、逆序回滚。
- [`domain/cqrs.hpp`](../../include/crpc/domain/cqrs.hpp) — `command`、`query`、`handler_registry`、`projection`。

### `crpc/ftp`

- [`ftp/client.hpp`](../../include/crpc/ftp/client.hpp) — `ftp::client`：RFC 959/2228/4217 客户端，PASV/EPSV、断点续传、显式/隐式 FTPS。
- [`ftp/command.hpp`](../../include/crpc/ftp/command.hpp) — 命令解析与序列化。
- [`ftp/reply.hpp`](../../include/crpc/ftp/reply.hpp) — `ftp_reply`：单行与多行回复解析。
- [`ftp/server.hpp`](../../include/crpc/ftp/server.hpp) — `ftp::server`：路径受控的 FTP/FTPS 服务端。

### `crpc/i18n`

- [`i18n/catalog.hpp`](../../include/crpc/i18n/catalog.hpp) — `message_catalog`：区域、回退链、复数规则、参数插值。
- [`i18n/format.hpp`](../../include/crpc/i18n/format.hpp) — `format_number`、`parse_number`：区域感知的数字格式化。

### `crpc/numeric`

- [`numeric/bigint.hpp`](../../include/crpc/numeric/bigint.hpp) — `uint_big<Bits>` 模板：像内置整型一样的回绕算术。
- [`numeric/uint128.hpp`](../../include/crpc/numeric/uint128.hpp) — `uint128`。
- [`numeric/uint192.hpp`](../../include/crpc/numeric/uint192.hpp) — `uint192`。
- [`numeric/uint256.hpp`](../../include/crpc/numeric/uint256.hpp) — `uint256`。
- [`numeric/uint384.hpp`](../../include/crpc/numeric/uint384.hpp) — `uint384`。
- [`numeric/uint512.hpp`](../../include/crpc/numeric/uint512.hpp) — `uint512`。

### `crpc/proactor`

- [`proactor/proactor.hpp`](../../include/crpc/proactor/proactor.hpp) — `proactor<Backend>`：完成事件驱动的 I/O，直接适配 `io::` 契约，每次提交恰好一次完成。

### `crpc/reactor`

- [`reactor/reactor.hpp`](../../include/crpc/reactor/reactor.hpp) — `reactor<Demux>`：就绪事件驱动的分发与解复用适配。

### `crpc/system`

- [`system/cpu.hpp`](../../include/crpc/system/cpu.hpp) — `logical_processor_count`、`physical_processor_count`、`get/set_current_thread_affinity`、`get/set_current_thread_priority`：查询并控制调用线程的处理器放置与调度优先级。
- [`system/process.hpp`](../../include/crpc/system/process.hpp) — `process::run/spawn`、`list_processes`、`query_process`：带 stdin/stdout/stderr 重定向的子进程。
- [`system/sysinfo.hpp`](../../include/crpc/system/sysinfo.hpp) — `cpu()`、`memory()`、`disks()`、`hostname()`、`uptime_seconds()`。

### `crpc/time`

- [`time/duration.hpp`](../../include/crpc/time/duration.hpp) — `timestamp`、`duration`：纳秒基准，秒/毫秒/微秒辅助。
- [`time/datetime.hpp`](../../include/crpc/time/datetime.hpp) — `date_time`：公历日历运算、闰年规则、RFC 3339。
- [`time/format.hpp`](../../include/crpc/time/format.hpp) — `%Y %m %d %H %M %S %f %z` 格式化。
- [`time/timezone.hpp`](../../include/crpc/time/timezone.hpp) — `timezone_at`、`local_timezone`、`list_timezones`：IANA tzdata，固定偏移回退。
- [`time/clock.hpp`](../../include/crpc/time/clock.hpp) — `now_unix`、`utc_now`、`gmt_now`、`system_tick`、`process_tick`、`steady_now_ticks`、`steady_frequency`：墙钟与单调 tick 读数。

### `crpc/text` 新增

- [`text/encoding.hpp`](../../include/crpc/text/encoding.hpp) — base32/base32hex/base58/base85/ASCII85/uuencode/quoted-printable/punycode/UTF-7/Latin-1/GBK。
- [`text/pinyin.hpp`](../../include/crpc/text/pinyin.hpp) — `to_pinyin`、`to_pinyin_initials`、`pinyin_candidates`：全 GBK 拼音与词组消歧。
- [`text/regex.hpp`](../../include/crpc/text/regex.hpp) — `regex`：回溯引擎，带防灾难回溯的硬性步数上限。
- [`text/string_order.hpp`](../../include/crpc/text/string_order.hpp) — `string_order`、`string_comparison`：.NET StringComparer 风格的 ordinal/invariant 比较。
- [`text/unicode.hpp`](../../include/crpc/text/unicode.hpp) — `normalize`（NFC/NFD/NFKC/NFKD）、`to_case`、类别查询、字素簇。

### `crpc/algorithm` 新增

- [`algorithm/sort.hpp`](../../include/crpc/algorithm/sort.hpp) — `intro_sort`、`quick_sort`、`heap_sort`、`radix_sort`。
- [`algorithm/heap.hpp`](../../include/crpc/algorithm/heap.hpp) — `binary_heap`、`make_heap`、`push_heap`、`pop_heap`。
- [`algorithm/tree_traversal.hpp`](../../include/crpc/algorithm/tree_traversal.hpp) — 非递归前/中/后/层序遍历。

### `crpc/cluster`

- [`cluster/election.hpp`](../../include/crpc/cluster/election.hpp)
- [`cluster/health.hpp`](../../include/crpc/cluster/health.hpp)
- [`cluster/health_bridge.hpp`](../../include/crpc/cluster/health_bridge.hpp)
- [`cluster/lease.hpp`](../../include/crpc/cluster/lease.hpp)
- [`cluster/registry.hpp`](../../include/crpc/cluster/registry.hpp)
- [`cluster/registry_log.hpp`](../../include/crpc/cluster/registry_log.hpp)
- [`cluster/raft_state.hpp`](../../include/crpc/cluster/raft_state.hpp)
- [`cluster/raft_node.hpp`](../../include/crpc/cluster/raft_node.hpp)
- [`cluster/raft_fence_authority.hpp`](../../include/crpc/cluster/raft_fence_authority.hpp)
- [`cluster/registry_acl.hpp`](../../include/crpc/cluster/registry_acl.hpp)
- [`cluster/fence_authority.hpp`](../../include/crpc/cluster/fence_authority.hpp)
- [`cluster/kv_store.hpp`](../../include/crpc/cluster/kv_store.hpp)
- [`cluster/consul_http.hpp`](../../include/crpc/cluster/consul_http.hpp)
- [`cluster/raft_command.hpp`](../../include/crpc/cluster/raft_command.hpp)
- [`cluster/registry_wire.hpp`](../../include/crpc/cluster/registry_wire.hpp)
- [`cluster/registry_service.hpp`](../../include/crpc/cluster/registry_service.hpp)
- [`cluster/registry_client.hpp`](../../include/crpc/cluster/registry_client.hpp)
- [`cluster/watch.hpp`](../../include/crpc/cluster/watch.hpp)

### `crpc/flow`

- [`flow/estimator.hpp`](../../include/crpc/flow/estimator.hpp)


## 另见

- [教程](../tutorial.zh-CN.md) —— 一步步构建一次调用与一个推送订阅。
- [架构](../architecture.zh-CN.md) —— 子系统地图与状态矩阵。
- [线上协议](../protocol.zh-CN.md) —— 帧格式与编解码。
- [Redis 客户端](../redis-client.zh-CN.md) —— Redis API 地图与配方。
- [分布式测试与基准](../distributed-testing.zh-CN.md) —— 多节点测试与基准命令。
- [ABI 策略](../abi.zh-CN.md) —— 布局与导出规则。
- [构建与测试](../building.zh-CN.md) —— CMake 选项与一键脚本。


### 注册中心线路 tag（step2）

注册中心服务帧为 `u16 dto_version`、`u32 total_len`，其后是零个或多个
`tag { u8 id, u32 len, u8[len] value }`。tag 1-13 由 step2 设计固定；14-17 是因
设计自身的要求（客户端幂等键、token 签发时间）在原有 tag 中没有预留位置而新增的。
tag 14 携带响应体的 `index`——它**不是** `query` 的可选 `node_id`，后者复用 tag 4。

| Tag | 名称 | 方向 | 语义 |
|---|---|---|---|
| 1 | `registry_tag_token` | 请求 | bearer token（明文；信任边界见上文） |
| ~~2~~ | ~~`registry_tag_dto_version`~~ | — | **已废弃。** DTO 版本是固定帧头的字段，不是 tag。携带 tag 2 的帧按 `dto_unknown_tag` 拒绝。 |
| 3 | `registry_tag_service` | 请求 | 服务名；全目录查询时为空 |
| 4 | `registry_tag_node_id` | 请求 | 节点 id；也承载 `query` 的可选过滤条件 |
| 5 | `registry_tag_host` | 请求 | endpoint host |
| 6 | `registry_tag_port` | 请求 | endpoint port |
| 7 | `registry_tag_incarnation` | 请求 | 期望的 incarnation |
| 8 | `registry_tag_fence` | 请求 | 来自 `next_fence` 的 fencing token |
| 9 | `registry_tag_writer_epoch` | 请求 | 客户端的 epoch 声明；服务端用自己的签发值替换它 |
| 10 | `registry_tag_weight` | 请求 | 实例权重 |
| 11 | `registry_tag_state` | 请求 | `instance_state` 值 |
| 12 | `registry_tag_meta` | 请求 | 长度前缀的 metadata 映射 |
| 13 | `registry_tag_passing_only` | 请求 | 查询过滤：仅 passing 实例 |
| 14 | `registry_tag_index` | 双向 | 响应体 `index`；请求侧的客户端 anchor index |
| 15 | `registry_tag_client_request_id` | 请求 | 客户端幂等键；同一逻辑写入的重试复用该值 |
| 16 | `registry_tag_token_issued_at` | 请求 | token 签发时间，`system_clock` 毫秒 |
| 17 | `registry_tag_token_nonce` | 请求 | 保留 nonce；step2 的服务端不解释它 |
| 18 | `registry_tag_client_authority_epoch` | 请求 | 客户端上次观察到的权威 epoch，即二维 anchor 的第一维 |
| 19 | `registry_tag_authority_epoch` | 响应 | 服务端自身的权威 epoch。不再放在 tag 9（`writer_epoch`）或 tag 7（`incarnation`）里，两者含义不同 |

未知 tag id、重复 tag id、tag 长度越过帧尾、`total_len` 与 body 不一致、以及不支持
的 `dto_version`，全部被拒绝，且各自有独立的 `registry.reason`。
