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

## 为什么在一个进程里

快速开始使用 `crpc::inproc_backend`：它在同一个进程内连接两个端点，没有套接字、
没有额外线程。这与测试套件使用的传输相同，使整个 API 确定且可移植：服务器与客户端
都是 `main()` 里的对象。

若要把两个角色放到不同进程，保留服务器代码与客户端代码，替换后端与端点即可：
Windows 上用 `crpc::io::iocp_backend`，Linux 上用 `crpc::io::epoll_backend`，
两端都用 `crpc::endpoint::tcp("127.0.0.1", port)`。线上格式与调用/推送 API 不变。

## 下一步

- [docs/tutorial.md](../docs/tutorial.zh-CN.md) 会一步一步构建同一个程序。
- [docs/api/README.zh-CN.md](../docs/api/README.zh-CN.md) 索引公开头文件。
