# CRPC 示例

[English](README.md) | **简体中文**

可直接运行的公开 API 使用示例。仅在 `CRPC_BUILD_EXAMPLES=ON` 时构建，并注册到
CTest，因此构建即证明它们可以编译并运行。

## 快速开始

`quickstart.cpp` 是一个可执行程序，同时充当服务器与客户端。它完成：

1. 用 `crpc::server` 构建一个服务器，在一个进程内端点上监听；
2. 注册一个服务（`quickstart`）与一个类型化方法（`greet`）；
3. 启动服务器；
4. 连接一个 `crpc::rpc::client`，发起一次类型化调用；
5. 安装推送 sink，订阅一个主题，发布消息，并处理推送；
6. 拆除全部资源。

### 构建

```powershell
$env:CRPC_RTTI_DIR   = 'F:\RTTI'
$env:CRPC_COROCC_DIR = 'F:\corocc'
cmake -S . -B .opencode/tmp/p20b -G Ninja `
      -DCMAKE_BUILD_TYPE=Debug `
      -DCRPC_BUILD_EXAMPLES=ON -DCRPC_BUILD_TESTS=OFF
cmake --build .opencode/tmp/p20b
```

```sh
export CRPC_RTTI_DIR=/path/to/RTTI
export CRPC_COROCC_DIR=/path/to/corocc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
      -DCRPC_BUILD_EXAMPLES=ON -DCRPC_BUILD_TESTS=OFF
cmake --build build
```

### 运行

```powershell
.\.opencode\tmp\p20b\crpc_example_quickstart.exe
```

预期输出：

```text
call: Hello, CRPC!
push: subscribed to 'quickstart.events' on stream 1
push: first push (seq 1)
quickstart OK
```

进程在首次失败时以非零码退出，因此可当作冒烟检查使用。

## 微服务

`quickstart_microservice.cpp` 在单进程内构建一个确定性的服务网格：

- 两个 provider 服务器注册到 `crpc::cluster::registry`；
- `crpc::cluster::health_monitor` 聚合一个健康探针与一个故障探针；
- 消费者以 `passing_only = true` 查询 registry，并用
  `crpc::resilience::load_balancer`（`round_robin`）在发现的实例间负载均衡；
- 每次调用都经过 `crpc::resilience::circuit_breaker` 与 `retry_policy`；
- 对不存在 provider 的调用会打开熔断器，证明失败路径生效。

关键 API：`cluster::registry`、`cluster::service_instance`、
`cluster::health_monitor`、`resilience::load_balancer`、
`resilience::circuit_breaker`、`resilience::retry_sequence`、`server::builder`、
`rpc::client::start_call`。

构建与运行（与快速开始共用构建目录；现有 `quickstart*.cpp` glob 会为每个文件生成
`crpc_example_<文件名>` 目标）：

```powershell
cmake --build .opencode/tmp/p20b
.\.opencode\tmp\p20b\crpc_example_quickstart_microservice.exe
```

预期输出：

```text
microservice: registry has 2 passing instance(s) of 'greeter'
microservice: health aggregate is passing
microservice: round-robin picked 1 2 1 2
microservice: greeter-1 -> Hello, CRPC from greeter-1
microservice: greeter-2 -> Hello, CRPC from greeter-2
microservice: breaker opened after failures
microservice OK
```

## MVC

`quickstart_mvc.cpp` 驱动 RTTI 属性 MVC 路由，并通过真实的
`crpc::http::http_server` 提供同一张路由表：

- 反射 DTO 用 `CRPC_FROM_ROUTE` 绑定路径参数，用 `CRPC_FROM_QUERY` 绑定查询参数；
- `CRPC_HTTP_GET`/`CRPC_HTTP_POST` 声明路由；
- `crpc::mvc::dispatch` 覆盖 GET/POST、路径参数、查询参数、JSON body、
  `result<T>` 错误（404）与绑定失败（400）；
- HTTP 门面在进程内端点上启动并干净停止。

关键 API：`mvc::default_router`、`mvc::dispatch`、`mvc::with_status`、
`crpc::mvc::status_body<T>`、`CRPC_ROUTE`、`CRPC_HTTP_GET`、`CRPC_HTTP_POST`、
`http::http_server::builder`。

运行：

```powershell
.\.opencode\tmp\p20b\crpc_example_quickstart_mvc.exe
```

预期输出：

```text
mvc: GET /users/42 -> 200 { "id": 42, "name": "alice" }
mvc: GET /users?page=2&limit=5 -> 200 { "id": 10, "name": "page" }
mvc: POST /users -> 201 { "id": 1, "name": "carol" }
mvc: GET /users/-1/maybe -> 404
mvc: GET /users/abc -> 400
mvc: GET /nope -> 404
mvc: http_server listening on inproc:mvc-example
mvc OK
```

## 游戏网关

`quickstart_game.cpp` 在单进程内实现两层游戏后端：

- 网关服务器接受客户端连接，把 `join`/`action` 调用经跨节点 `rpc::client`
  调用转发给逻辑层；
- 逻辑服务器持有权威、有状态的玩家会话；
- 逻辑层应答后，网关向 `game.events` 发布 reliable 事件，客户端的 push sink
  收到它。

关键 API：`server::builder::add_method`、`rpc::client::start_call`、
`rpc::client::set_push_sink`、`rpc::client::subscribe_push`、
`server::publish`、`push::delivery_mode::reliable`、
`push::push_message_view`。

运行：

```powershell
.\.opencode\tmp\p20b\crpc_example_quickstart_game.exe
```

预期输出：

```text
game: subscribed to 'game.events' on stream 1
game: join -> session alice
game: push: player alice joined at (0,0)
game: action -> (3,4)
game: push: player moved to (3,4)
game OK
```

网关在转发调用返回后发出事件。逻辑层回调网关会在同步进程内传输上死锁，因为网关
分片线程正阻塞在转发调用里；在 TCP（独立进程/线程）下这种回调形状可行。

## 共享内存（IPC）

`quickstart_ipc.cpp` 是唯一一个不经过 socket、真正跨进程的示例。两个独立进程通过命名共享内存
通道各发一条消息：

- 父进程创建通道（`crpc::ipc::channel::create`）、先排入一条请求，然后用 `peer <name>`
  参数启动**它自己**；
- 子进程按名字加入（`crpc::ipc::channel::open`）并回复；
- 两端结束时都删除名字；父进程还会校验回复内容必须**恰好**等于由请求推导出的答案，
  所以"送到了但字节不对"不会通过。

它故意自启：同一个进程里放两个 `channel` 对象，即使映射其实是私有的也照样"成功"，
所以必须用 `CreateProcessA`（Windows）/ `fork`（POSIX）才让共享内存有意义。

运行（`quickstart*.cpp` 的 glob 已经把它构建出来了）：

```powershell
.\.opencode\tmp\p20b\crpc_example_quickstart_ipc.exe
```

期望输出（`peer:` 行可能出现在 `parent: created` 之前或之后——子进程在映射建立后立即启动，
两个进程在 stdout 上没有同步）：

```text
peer: got "render frame 42", answering
parent: created channel "crpc_example_ipc" (4096 bytes each way)
parent: got "worker: handled 15 bytes"
ok: two processes exchanged a message over shared memory
```

退出码 0 表示两个方向都送出了期望的字节。通道、环形缓冲与 SPSC 队列的契约见
[ipc.zh-CN.md](../docs/ipc.zh-CN.md)。

## 跨进程部署检查

上面的 quickstart 全部走 `crpc::inproc_backend`——同一个进程内连两个端点，没有 socket。
这是有意的（让示例确定可复现），但也意味着它们从不证明网络路径可用。

`deploy_check.cpp` 是例外，也是"这个构建能不能上生产"之前该跑的那一个。它把**自己**
作为子进程拉起，于是服务端与客户端是两个操作系统进程，中间只有 loopback TCP：

```powershell
.\.opencode\tmp\p20b\crpc_example_deploy_check.exe
```

预期输出：

```text
server: READY 19562
call: /health -> 200 ok
call: /echo/deploy -> 200 deploy
server: STATS accepted=2 requests=2 responses=2 bad=0
```

它验证三件进程内示例验证不了的事：

1. 真实的 `iocp_backend`（Windows）或 `epoll_backend`（Linux）监听器能接住来自另一个
   进程的连接；
2. 响应被**精确**校验——状态行加 body，逐字节比较。这个文件的第一版用的是子串匹配，
   在服务端把 `/health` 改成回 `not-ok` 时它依然通过，因为 `"ok"` 就在 `"not-ok"` 里面。
   会接受错误 body 的门禁只证明了端口是开的；
3. 服务端自己的计数自洽（`accepted == requests == responses`、`bad == 0`），且进程干净退出。

两个模式也能单独用，这正是让第三方客户端有意义的方式：

```powershell
# 终端 1
.\crpc_example_deploy_check.exe serve 19555
# 终端 2 —— 无关的 HTTP 客户端，证明线上是普通 HTTP/1.1
curl -sS -D - http://127.0.0.1:19555/health
curl -sS http://127.0.0.1:19555/echo/curl-real
```

`curl` 拿到 `HTTP/1.1 200` 和预期 body，是任何进程内测试都给不出的证据：
线上的字节是合法的 HTTP，而不是只有本库能解析的东西。

### 持续负载

一次往返只证明线上能通，不证明进程扛得住一万次。`soak <port> <次数>` 做的就是这件事：
一个客户端后端、`count` 次"连接/发送/接收/关闭"，并比较前后的句柄数。

```powershell
.\crpc_example_deploy_check.exe serve 19555 600   # 一个终端
.\crpc_example_deploy_check.exe soak  19555 2000  # 另一个终端
```

```text
soak: handle counter verified (96 -> 110 for 16 deliberate leaks)
soak: requests=2000 failures=0 connect_failures=0 worst_connect_ms=0 handles 93 -> 93 per-request-ms p50=0.42 p99=0.73 max=0.75 [connect-avg=0.21 transfer-avg=0.22] [connect-ms p50=0.20 p90=0.29 p99=0.44 max=0.46]
```

两个细节比数字本身更重要：

* **计数器先自证**。测量之前，soak 故意泄漏 16 个 socket，并要求句柄数看见它们。
  一个恒返回常量的计数器会让所有泄漏检查"天然通过"——那是最糟的绿——所以它会直接
  中止，而不是报一个没有意义的通过。
* **报分布而不是均值**。整体请求与单独 connect 各自给 `p50/p99/max`，因为尾部问题
  在均值里看不见。两者分开报，避免混为一谈。上面的数字来自 connect 路径正确的机器；
  本文件早先的版本写的是 `connect p90 = 14.71 ms`，因为 `select` 之后一次多余的
  `getsockopt(SO_ERROR)` 让约三分之一的回环连接要等一个完整 Windows 时钟片。该调用
  已删除，同一分布现在报 ~0.3 ms。
* **失败的连接会说明原因**。`connect_failures` 与 `worst_connect_ms` 独立于
  `failures` 报出，因为"被拒绝"和"很慢"含义不同：拒绝是立即的，而耗满整个
  `connect_timeout` 意味着 accept 侧停止取用。前 5 次失败还会打印各自的毫秒耗时。

门禁判据是句柄**增长**，不是绝对值：绝对值随平台与系统版本不同，而泄漏总是随工作量增长。

## 为什么在一个进程里

快速开始使用 `crpc::inproc_backend`：它在同一个进程内连接两个端点，没有套接字、
没有额外线程。这与测试套件使用的传输相同，使整个 API 确定且可移植：服务器与客户端
都是 `main()` 里的对象。

若要把两个角色放到不同进程，保留服务器代码与客户端代码，替换后端与端点即可：
Windows 上用 `crpc::iocp_backend`，Linux 上用 `crpc::io::epoll_backend`，
两端都用 `crpc::endpoint::tcp("127.0.0.1", port)`。线上格式与调用/推送 API 不变。

## 下一步

- [docs/tutorial.md](../docs/tutorial.zh-CN.md) 会一步一步构建同一个程序。
- [docs/api/README.zh-CN.md](../docs/api/README.zh-CN.md) 索引公开头文件。
