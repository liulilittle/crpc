# CRPC 迁移指南

[English](migration.md) | **简体中文**

版本 2.0 / 2026-10-24

本指南记录最后一个兼容版本（1.0.0，`bd006b2`）与 2.0.0 之间的二进制与行为变更。ABI 策略是规范性的，见 [abi.md](abi.zh-CN.md)。下文中 MAJOR 变更要求消费方用同一份头文件重建；仅源码级变更会显式标注。

## 1. 1.0.0 -> 2.0.0：破坏点

| 变更 | 类别 | 受影响类型 / 符号 | 1.0.0 | 2.0.0 |
|---|---|---|---|---|
| `http_statistics` 增长 | MAJOR（布局） | `crpc::http::http_statistics` | 72 字节，9 字段 | 224 字节，28 字段 |
| `server::options` 增长 | MAJOR（布局） | `crpc::server::options` | 304 字节（x64）、256 字节（x86） | 320 字节（x64）、264 字节（x86） |
| `server::builder` 增长 | MAJOR（随 options） | `crpc::server::builder` | 376 字节（x64）、296 字节（x86） | 392 字节（x64）、304 字节（x86） |

1.0.0 发布的 9 个字段在 `http_statistics` 中偏移不变（`active_connections` 为 0 … `timed_out_connections` 为 64），`server::options` 的每个既有成员偏移也不变（`send_goaway_on_stop` 在 x64 为 249 / x86 为 225，`push_subscribe` 为 264 / 232）。只有 `sizeof` 增长，因为新字段追加在末尾。这并不使变更兼容：两侧对按值传递的对象尺寸认知不一致，编译器生成的拷贝、新字段之后任何成员的 `offsetof`、以及消费方分配该结构体的栈帧，都按消费方自己的头文件确定尺寸。

### 重建要求

每个消费方必须用 2.0.0 的头文件重建，并链接 2.0.0 的库。把 1.0.0 的头文件与 2.0.0 的二进制混用不受支持，也不向前兼容，因为该变更属于不同的 MAJOR 版本（[abi.md](abi.zh-CN.md) 第 7 节）。静态库场景下重新编译消费方翻译单元即可；共享库必须与消费方一起重新部署。

## 2. 符号新增（MINOR，2.x 内兼容）

以下均为增补，本身不会破坏已重建的消费方：

- 流控：`crpc::rpc::client::set_flow_control`、`crpc::rpc::client::can_admit`、
  `crpc::rpc::client::flow_snapshot`、`crpc::rpc::rpc_client_size_probe`、
  `crpc::client::set_flow_control`，以及 `crpc::rpc::flow_controller` /
  `flow_control_options` / `flow_state` 类型。`rpc::client` 本身增长了一个
  指针大小的成员，这属于 MAJOR 布局变化：必须重编所有消费方（见 `abi.md` 第 1 节）。
- `crpc::client::resilience_statistics` 与 `crpc::client::resilience_stats()`。
- `crpc::iocp_backend::high_resolution_timer()`（仅 Windows）：一个
  `const noexcept` 访问器，报告后端创建定时器时用的是
  `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`，还是回退到了 Windows 10 1803
  之前的毫秒级定时器。它只增加一个导出符号，不给类增加数据成员（标志位于
  out-of-line `impl`），因此 `sizeof(crpc::iocp_backend)` 不变。
- `crpc::resilience::fallback_statistics` 与 `crpc::client::fallback_stats()`。
- `crpc::client::call<Resp>::degraded()`（header-only 访问器）。
- `crpc::client::set_fallback`、`crpc::client::run_fallback` 与
  `crpc::client::record_fallbacks`。
- `crpc::server::builder::clock(resilience::clock_source)`。
- 易用性深度改进（方案 C）：`crpc::client::set_fallback_owned` 与
  `crpc::fallback_install`（RAII fallback 安装器）、
  `crpc::rpc::call_context::set_deadline` / `set_timeout` / `set_idempotent`
  与 `with_deadline` / `with_timeout` / `with_idempotent` 构造器、
  `crpc::client::call<Resp>::then`、共享的 `crpc::delivery_mode`、
  `crpc::service_id` / `crpc::method_id`、`crpc::result<T>::error_or` /
  `value_or`、`crpc::publish_result`，以及两参数的
  `CRPC_HTTP_GET("/path", lambda)` MVC 形式。客户端构造器新增
  `default_timeout` 与 `per_connection_inflight` 弃用别名，服务端构造器新增
  `max_inflight_per_connection`；规范名称是 `default_deadline` 与
  `max_inflight_per_connection`。

`server::publish` 现在返回 `publish_result` 而非 `bool`。到 `bool` 的转换是隐式的
（仅 `ok` 为 `true`），因此 `if (server.publish(...))` 与
`CRPC_CHECK(server.publish(...))` 仍可编译且仍表示「ok」；把结果显式写成 `bool`
变量的调用方也可编译。见 [CHANGELOG.zh-CN.md](CHANGELOG.zh-CN.md)。

没有任何导出符号被删除或改签名。共享构建现在导出全部文档化公开 API，见 [CHANGELOG.zh-CN.md](CHANGELOG.zh-CN.md)。

## 3. 不构成 ABI 破坏的行为变更

- 服务端在四个执行前准入拒绝点写出保留响应 meta 条目 `crpc-degradable: true`（服务级熔断器、deadline 仍可达时的服务级令牌桶、服务级 bulkhead 与服务级在途上限）。忽略未知 meta 键的客户端不受影响。
- HTTP MVC 错误响应在 `rpc_error::retry_after > 0` 时携带 `Retry-After`。
- `COMPRESSED` 帧不再被当作明文交付。`decode_frame` 对 `COMPRESSED` 帧返回 `protocol_error`（它没有输出缓冲），`frame_decoder` 在拷贝模式下解压、在借用模式下拒绝。手工构造压缩帧的对端现在会得到协议错误。
- 帧级压缩支持 LZ4 block 编解码器（`ALGO = 1`），其它算法 id 一律 fail-closed。

## 4. 迁移步骤

1. 用 2.0.0 的头文件重建消费方。
2. 用新头文件重新分配或值初始化每个 `http_statistics`、`server::options` 与 `server::builder`；不要反序列化 1.0.0 的数据块。
3. 链接 2.0.0 的库。
4. 重新运行测试套件：`src/abi_check.cpp` 中的布局护栏会在 x86 与 x64 上断言 `sizeof(server::options)` 与 `sizeof(http_statistics)`。

## 5. 未发布：`rpc_error` 布局与 `failure_origin` 迁移

未发布的错误诊断工作在 2.0.0 之上属于 MAJOR 变更：

- `rpc_error` 新增两个数据成员（`origin`、`attempts`），追加在 `metadata` 之后。初始化前五个成员仍可编译；按旧 `rpc_error` 分配或复制则不行。
- `failure_origin` 从 `client_detail` 迁入 `crpc`，重命名了两个导出符号（`breaker_record_for`、`result_origin`）。`client_detail::failure_origin` 别名保持源码兼容，但链接到旧符号的消费方必须重新链接。

每个消费方必须用同一份头文件重建并重新链接。旧/新 mangled 名见 [CHANGELOG.zh-CN.md](CHANGELOG.zh-CN.md)。


> 注：§6、§7 尚未翻译，见英文版 `migration.md`。

## 8. 未发布：注册中心服务（step2）运维要求

### 8.1 ACL 是 bearer-token 授权层，不是身份认证层

持有合法 token 的一方以其被签发的 principal 身份行事。没有传输层安全：RPC 层
不携带对端身份（`call_envelope`、`call_context`、`frame` 均无身份字段），也没有
TLS。线路上被观察到的 token 在过期前可被重放。需要认证对端的部署必须在服务前
终结 TLS，或运行在可信网络。token 经 DTO body 传输，绝不放进 `metadata`——客户端
的 `metadata` 根本到不了服务端。principal 由服务端从命中的 ACL 规则注入，客户端
自身的声明永不被信任。

### 8.2 token 时间窗依赖时钟同步

token 携带自己的签发时间（`registry_tag_token_issued_at`），服务端拒绝与服务端
时钟之差绝对值超过 `registry_token_window`（30 秒）的 token。比较使用
`system_clock`（Unix epoch），因为该窗口是跨进程的，而 `steady_clock` 的 epoch
是每个进程独立的。未携带签发时间的 token 被拒绝（fail closed）。

这是**限时，不是防重放**：窗口内被观察到的 token 仍可重放。它还**依赖客户端与
服务端 wall clock 一致（NTP）**；时钟偏差超过窗口的客户端会被拒绝。时钟同步性
为 `UNVERIFIED`。

### 8.3 epoch 由服务端签发，不是客户端自选

`writer_epoch` 从服务端的 epoch 存储分配，绝不采用客户端的声明值。该存储是独立
文件，**先落盘再发号**；落盘失败以 `registry.reason=epoch_not_persisted` 拒绝
写入。启动时计数器以 `max(persisted, registry.fence())` 播种，`fence()` 在
restore 之后读取。客户端传 `writer_epoch = 0` 会被 `epoch_required` 拒绝。

### 8.4 每个 state 目录只能有一个服务实例

服务对其 `state_dir` 持有单实例租约：进程内注册表 + 跨进程命名互斥体（Windows
互斥体对持有线程可重入，单靠内核对象拦不住同进程的第二个实例）。接受
`WAIT_ABANDONED`——拒绝它会让崩溃的部署永久无法重启——接管**不重复实现**撕裂
尾部截断：`file_registry_log::open()` 已扫描文件、采纳完整前缀并截断撕裂尾部，
损坏记录直接使其失败。日志落后于已恢复目录时以 `stale_log` 拒绝。

### 8.5 幂等映射有容量边界

服务端按 `client_request_id` 缓存最后一次成功写入，映射受
`max_idempotency_entries`（默认 4096）与 `idempotency_ttl`（默认 300000 毫秒）
约束。先按 TTL 逐条淘汰，再逐个淘汰最旧的一条；**绝不整表清空**，那会一次性
丢掉所有请求的保护。

**映射之外的请求没有保护。** 被淘汰请求的重放会走正常路径，并可能被判为过期
incarnation 或过期 anchor。部署须使 `max_idempotency_entries` 乘以
`idempotency_ttl` 不小于客户端的最大重试窗口。只缓存成功写入；失败的首次尝试
仍可重试。

### 8.6 `permission_denied` 无法往返

响应编码器对高于 `application`（21）的码 fail-closed 为 `internal`，原始码在
`crpc.raw_code` 元数据项里。服务端仍按 reason 表发 `permission_denied`（22）；
需要区分 ACL 拒绝的客户端应读 `crpc.raw_code`。

## 9. 未发布：KV 数据面是复制状态机

此前 KV 写入只改内存中的 `kv_store`，并向 Raft 提交一个**空**条目。单节点上
看不出问题；多节点时，每个 follower 存下一个不携带任何命令的索引，永远学不到
该值，因此第一次 KV 写就会让多节点集群发散。

### 破坏点

- `registry_service_config::raft` 对 KV 现在是必需项。给了非空 `raft` 的服务会把
  KV 写入与 ACL 变更经它提交并应用已提交日志；给了空 `raft` 的服务仍直接应用，
  但**不会记录任何日志，重启后丢失全部 KV 值与运行期 ACL 变更**。`registryd`
  因此总是构建 `<state-dir>/raft.log` 上的单节点 `raft_node`。
- ACL 规则不再每次检查都读 `config.acl`。`config.acl` 是种子；活动表是
  `acl_rules`，只由已提交的 `acl_upsert` / `acl_remove` 命令变更。
  `set_acl_rule` / `remove_acl_rule` 是运行期修改的唯一支持方式，二者都在变更
  提交后才返回。
- `registry_service::prepare` 现在在注册 handler 之前回放已提交日志。无法应用的
  日志是启动失败，不是部分启动。

### 迁移方法

1. 给每个拥有 KV store 的服务配一个 `raft_node`。单节点集群是合法的：节点即
   自己的 quorum，`raft_node::start` 会把最后日志索引采纳为已提交。
2. raft 日志必须与 catalog 日志分文件（`registry_service` 在
   `<state-dir>/registry.log` 记 catalog 事件）。共用一个文件不受支持：两层各自
   独立编号。
3. 若原本只依赖 `config.acl`，它仍可作为种子使用。启动后修改规则必须调用
   `set_acl_rule` / `remove_acl_rule`；在运行中的服务上改 `config.acl` 无效。

### 验证

`tests/test_raft_apply.cpp` 驱动三节点集群，断言经 leader 写入一次后每个节点的
`kv_store` 与 `acl_rules` 都相同。`tests/test_registryd_composition.cpp` 杀掉整个
daemon 并在同一状态目录上重建，然后读回该值。

## 10. 未发布：`registryd` 提供 Consul HTTP API

`--http-port <n>` 在原生二进制通道之外增加 Consul 兼容的 HTTP 门面。两者都经
`registry_service::dispatch` 到达同一批 handler，因此 ACL 检查、anchor 水位与
日志追加不可能漂移。

### 运维注意

- 该门面需要自己的 `backend_factory`。同一个 `iocp_backend` 上两个 listener 会
  以 "listener could not bind endpoint" 失败（端口不同也一样），因此 `registryd`
  为 HTTP 通道单独创建第二个 backend。
- Consul 注册的 `ID` 会成为注册实例的 node id，而注册中心的权限检查要求它与
  ACL principal 相等。请用 token 签发给的身份注册，否则写入被拒（409 与
  "write authority does not belong to this node"）。
- `registryd` 自带的 ACL 规则把 `node_id` 留空（"任意节点"）。绑定到某个 node id
  的规则会让每次 KV 写都以 403 被拒，因为 KV handler 传给 `check_acl` 的是空
  node id。