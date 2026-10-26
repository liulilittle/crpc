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
| [`observability/tracer.hpp`](../../include/crpc/observability/tracer.hpp) | `obs::tracer` | span、头部采样与异步导出。 |

## 集群与流控

| 头文件 | 关键类型 | 说明 |
|---|---|---|
| [`cluster/registry.hpp`](../../include/crpc/cluster/registry.hpp) | `cluster::registry` | 服务目录，带不可变快照与单调递增索引。 |
| [`cluster/lease.hpp`](../../include/crpc/cluster/lease.hpp) | `cluster::lease_manager` | 租约/会话管理器。 |
| [`cluster/election.hpp`](../../include/crpc/cluster/election.hpp) | `cluster::election` | 选主原语。 |
| [`cluster/health.hpp`](../../include/crpc/cluster/health.hpp) | `cluster::health_checker` | 健康检查器。 |
| [`cluster/watch.hpp`](../../include/crpc/cluster/watch.hpp) | `cluster::watch_hub` | 基于索引的目录 watch/订阅。 |
| [`flow/estimator.hpp`](../../include/crpc/flow/estimator.hpp) | `flow::estimator` | 自适应流控。 |

## 编解码与序列化

| 头文件 | 关键类型 | 说明 |
|---|---|---|
| [`codec.hpp`](../../include/crpc/codec.hpp) | `codec_registry`、`encode_into`、`decode_from` | 编解码注册表与类型化序列化接口。 |
| [`codec_id.hpp`](../../include/crpc/codec_id.hpp) | `codec_id` | 编解码标识。 |

## Redis 客户端

| 头文件 | 关键类型 | 说明 |
|---|---|---|
| [`redis/resp.hpp`](../../include/crpc/redis/resp.hpp) | `resp_value`、`resp_parser`、`resp_encoder`、`resp_protocol` | RESP2/RESP3 值模型、增量解析器与编码器。 |
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

## 另见

- [教程](../tutorial.zh-CN.md) —— 一步步构建一次调用与一个推送订阅。
- [架构概览](../architecture.zh-CN.md) —— 子系统地图与状态矩阵。
- [线上协议](../protocol.zh-CN.md) —— 帧格式与编解码。
- [Redis 客户端](../redis-client.zh-CN.md) —— Redis API 地图与配方。
- [分布式测试与基准](../distributed-testing.zh-CN.md) —— 多节点测试与基准命令。
- [ABI 策略](../abi.zh-CN.md) —— 布局与导出规则。
- [构建与测试](../building.zh-CN.md) —— CMake 选项与一键脚本。
