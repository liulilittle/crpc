# CRPC examples

[简体中文](README.zh-CN.md) | **English**

Runnable consumers of the public API. They are built only when
`CRPC_BUILD_EXAMPLES=ON` and are registered in CTest, so a build proves they
compile and run.

## Quickstart

`quickstart.cpp` is one executable that is both the server and the client. It:

1. builds a `crpc::server` with one listener on an in-process endpoint,
2. registers one service (`quickstart`) and one typed method (`greet`),
3. starts the server,
4. connects a `crpc::rpc::client` and makes a typed call,
5. installs a push sink, subscribes to a topic, publishes, and handles the push,
6. tears everything down.

### Build

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

### Run

```powershell
.\.opencode\tmp\p20b\crpc_example_quickstart.exe
```

Expected output:

```text
call: Hello, CRPC!
push: subscribed to 'quickstart.events' on stream 1
push: first push (seq 1)
quickstart OK
```

The process exits non-zero on the first failure, so it is usable as a smoke
check.

## Why one process

The quickstart uses `crpc::inproc_backend`, which connects two endpoints inside
one process with no sockets and no extra thread. That is the same transport the
test suite uses, and it makes the whole API deterministic and portable: the
server and the client are objects in `main()`.

To run the two roles in separate processes, keep the server code and the client
code and replace the backend and the endpoint: `crpc::io::iocp_backend` on
Windows, `crpc::io::epoll_backend` on Linux, and
`crpc::endpoint::tcp("127.0.0.1", port)` on both sides. The wire format and the
call/push API are unchanged.

## Where to go next

- [docs/tutorial.md](../docs/tutorial.md) builds the same program step by step.
- [docs/api/README.md](../docs/api/README.md) indexes the public headers.
