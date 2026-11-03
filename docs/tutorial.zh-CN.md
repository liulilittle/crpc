# CRPC 教程

[English](tutorial.md) | **简体中文**

本教程从零构建一个小程序：一个消息类型、一个服务方法、一个服务器、一次客户端调用与一个
推送订阅。它使用与 [`examples/quickstart.cpp`](../examples/quickstart.cpp) 相同的进程内传输，
因此是确定性的，且不需要网络。下面每个 API 名称都是真实的公开 API，每一步都链接到对应头文件。

如果还没有构建 CRPC，请先看[构建与测试](building.zh-CN.md) §2 的依赖检出与 §5 的一键脚本。

## 第 0 步 —— 程序的形态

一个可执行程序同时扮演两个角色：

```text
+------------------+        进程内连接                 +------------------+
|  服务器          |  <-------------------------->   |  客户端          |
|  greeter::hello  |   REQUEST/RESPONSE（类型化调用） |  start_call      |
|  server.publish  |   PUSH/PUSH_ACK（订阅）          |  推送 sink       |
+------------------+                                   +------------------+
```

`crpc::inproc_backend` 对象拥有这条通道；服务器监听器与客户端连接都由它创建。

## 第 1 步 —— 编写消息类型

请求或响应类型可以是任何带有 `crpc::rpc::call_codec` 特化的类型。该 codec 只有两个函数：
`encode` 把值写入 [`buffer`](../include/crpc/buffer.hpp)，`decode` 从
[`span`](../include/crpc/span.hpp) 读回。内建特化覆盖算术类型、`std::string` 与
`std::vector<std::byte>`（[`rpc/call.hpp`](../include/crpc/rpc/call.hpp)）；用户类型自行添加。

```cpp
#include <crpc/buffer.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/span.hpp>

#include <string>

struct hello_request {
    std::string name;
};

struct hello_response {
    std::string message;
};

namespace crpc::rpc {

template <>
struct call_codec<hello_request, void> {
    static status encode(const hello_request& value, buffer& out) {
        return call_codec<std::string>::encode(value.name, out);
    }
    static status decode(span<const std::byte> in, hello_request& value) {
        return call_codec<std::string>::decode(in, value.name);
    }
};

template <>
struct call_codec<hello_response, void> {
    static status encode(const hello_response& value, buffer& out) {
        return call_codec<std::string>::encode(value.message, out);
    }
    static status decode(span<const std::byte> in, hello_response& value) {
        return call_codec<std::string>::decode(in, value.message);
    }
};

} // namespace crpc::rpc
```

第二个模板实参 `<void>` 是主模板的默认值；特化必须写出来。函数体委托给 `std::string`
codec，因此线上格式是原始 UTF-8 字节、没有长度前缀 —— 对教程足够；真实消息应在此处加上
版本号或长度前缀。

## 第 2 步 —— 编写 handler

handler 就是普通的成员函数。它的签名决定请求与响应类型：`Resp (Ctl::*)(const Req&)` 是
[`register_typed_method`](../include/crpc/rpc/dispatcher.hpp) 接受的形式。如果 handler 需要
截止时间或 trace 元数据，可再加一个 `crpc::rpc::call_context&` 形参。

```cpp
class greeter {
public:
    hello_response hello(const hello_request& request) {
        hello_response response;
        response.message = "Hello, " + request.name + "!";
        return response;
    }
};
```

## 第 3 步 —— 构建并启动服务器

[`server::builder`](../include/crpc/server.hpp) 收集监听器、服务与方法；`build()` 把它们
冻结成 `server`，`start()` 绑定监听器并启动分片。

```cpp
#include <crpc/io.hpp>
#include <crpc/server.hpp>

crpc::inproc_backend backend;

greeter service;

crpc::server::builder server_builder;
server_builder.add_listener(crpc::endpoint::inproc("tutorial"), backend);
server_builder.add_service(1, "tutorial");
server_builder.add_method<greeter, &greeter::hello>(
    1, 1, "hello", service);

std::unique_ptr<crpc::server> server = server_builder.build();
if (!server) { /* ... */ }
if (const crpc::status started = server->start(); !started.ok()) {
    // started.message() 说明失败原因
}
```

注册只能在启动前进行：注册表在 `start()` 中冻结，因此每个服务与方法都必须在它之前添加。
`add_service` 与 `add_method` 接受普通 `u64` id；[`service_id` / `method_id`](../include/crpc/id.hpp)
这两个新类型用于希望在写反一对 id 时由编译器报错的调用点。

## 第 4 步 —— 连接客户端并发起类型化调用

底层 [`rpc::client`](../include/crpc/rpc/client.hpp) 拥有一条连接。`start_call<Resp>`
接受已编码的请求体并返回可移动句柄；`get()` 等待响应并解码。

```cpp
#include <crpc/rpc/client.hpp>

std::unique_ptr<crpc::connection> connection =
    backend.connect(crpc::endpoint::inproc("tutorial"), crpc::connect_options{});
if (!connection) { /* ... */ }

crpc::rpc::client_options client_options;
client_options.slot_capacity = 16;
crpc::rpc::client client(std::move(connection), client_options);

hello_request request;
request.name = "CRPC";

crpc::buffer body;
if (const crpc::status encoded = crpc::rpc::call_codec<hello_request>::encode(
        request, body);
    !encoded.ok()) {
    // encoded.error() 说明失败原因
}

auto started = client.start_call<hello_response>(1, 1, body.readable());
if (!started.ok()) {
    // started.error() 说明失败原因（池耗尽、已关闭……）
} else {
    auto call = std::move(started).value();
    auto result = call.get();
    if (result.ok()) {
        std::cout << result.value().message << "\n";
    } else {
        // result.error().code / .message
    }
}
```

高层 [`client` 门面](../include/crpc/client_facade.hpp) 在同样的调用之上增加了连接池、
负载均衡、熔断与重试：

```cpp
auto stub = facade.service<hello_response, hello_request>(1, 1);
auto started = stub.call(request);
```

真实服务里应使用门面；推送 API 位于底层客户端上，因此本教程保留一个客户端对象，同时用于两者。

## 第 5 步 —— 推送订阅

推送是服务器到客户端的方向。服务器用 `server::publish` 发布到主题；客户端用
`rpc::client::subscribe_push` 订阅，并通过 sink 接收消息。

```cpp
#include <crpc/push/wire.hpp>

struct push_state {
    std::string body;
    crpc::u64 seq = 0;
};

// 在交付帧的线程上运行；把 body 拷贝出来。
void on_push(void* ctx, const crpc::push::push_message_view& message) noexcept {
    auto* state = static_cast<push_state*>(ctx);
    state->body.assign(reinterpret_cast<const char*>(message.body.data()),
                       message.body.size());
    state->seq = message.seq;
}

push_state state;
client.set_push_sink(&on_push, &state);

auto subscription = client.subscribe_push(
    "tutorial.events", 1, crpc::push::delivery_mode::reliable, false);
if (!subscription.ok()) {
    // subscription.error() 说明失败原因
}

const std::string payload = "first push";
if (const crpc::publish_result published = server->publish(
        "tutorial.events",
        crpc::span<const std::byte>(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size()),
        crpc::push::delivery_mode::reliable);
    !crpc::is_ok(published)) {
    // crpc::to_string(published) 说明拒绝原因
}
```

可靠消息会在 sink 返回后自动确认；用同一 subscriber id 重连时会重放未确认的尾部。sink
不得阻塞，也不得销毁客户端。

## 第 6 步 —— 拆除

```cpp
client.close();   // 取消在途调用并关闭连接
server->stop();   // 停止接收、排空在途调用、关闭分片
```

`server::stop()` 幂等，且以 `drain_timeout` 为界。未调用 `stop()` 就销毁 `server` 不受支持；
builder 借用的 `backend_factory` 必须比每一次 `stop()` → `start()` 周期活得更久。

## 运行

把程序放进 `main()` 并设置 `CRPC_BUILD_EXAMPLES=ON`：

```powershell
cmake -S . -B build -DCRPC_BUILD_EXAMPLES=ON
cmake --build build
.\build\crpc_example_quickstart.exe
```

## 说明

- **错误即值。** 上面的代码在调用中不抛出异常；`status`、`result<T>` 与 `rpc_error`
  承载失败。见 [`error.hpp`](../include/crpc/error.hpp)。
- **两个进程。** 把 `crpc::inproc_backend` 换成 `crpc::iocp_backend`（Windows）或
  `crpc::io::epoll_backend`（Linux），端点换成
  `crpc::endpoint::tcp("127.0.0.1", port)`；调用与推送 API 不变。见
  [examples/README.zh-CN.md](../examples/README.zh-CN.md)。
- **完整 API 地图** 见 [docs/api/README.zh-CN.md](api/README.zh-CN.md)。
