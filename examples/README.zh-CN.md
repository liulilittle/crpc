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
