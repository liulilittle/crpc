# 进程间通信

[English](ipc.md) | **简体中文**

本页说明同一台机器上两个进程之间搬数据的三个基元：命名共享内存、字节环、值队列，
以及由它们组成的全双工通道。除映射本身属于 `crpc::fs::shared_memory` 外，其余都在
`crpc::ipc`。

本库底层是异步的，上层才提供同步形式。这决定了这里每个 API 的形状，所以阻塞规则先讲。

## 阻塞规则

| 层次 | 行为 |
|---|---|
| `try_send` / `try_receive`（字节环、队列） | 任何线程上都**不等待**；这是基元 |
| 带超时的 `send` / `receive`（通道） | shard 线程的协程内：`runtime::park()`，由 `runtime::notify()` 唤醒；普通线程：按给定超时有界等待 |
| 超时为零 | 表示"不等待"，两者都返回非阻塞结果 |

关键约束：**同步形式绝不能阻塞 shard 线程**。在 shard 上阻塞会停掉该 shard 上所有
其他任务；若对端恰好在等这个 shard，就会死锁。通道的做法是 park，用例
`ipc_async_shard_thread_parks_without_blocking_the_shard` 把生产者与消费者作为同一 shard
上的两个任务运行——如果等待是阻塞而非 park，该用例会失败。

## 命名共享内存 —— `fs::shared_memory`

```cpp
#include <crpc/fs/shared_memory.hpp>

// 创建方：
auto owner = crpc::fs::shared_memory::open("metrics.v1", 4096,
                                           crpc::fs::shared_memory_mode::create);
// 对端：
auto peer = crpc::fs::shared_memory::open("metrics.v1", 0,
                                          crpc::fs::shared_memory_mode::open_existing);
peer.value().bytes()[0] = std::byte{42};
// `owner` 通过自己的 `bytes()` 现在能读到 42。
```

`bytes()` 从本类写入的 16 字节头部之后开始，因此 `size()` 在两个平台上都是**创建者请求的
长度**。这个头部存在的理由：Windows 把映射向上取整到页，且没有受支持的办法事后取回请求
长度；POSIX 的 `ftruncate` 长度精确。没有它，有一个平台会返回页对齐的数字。

`remove_unlinks_the_name()` 如实报告平台差异，而不是掩盖：

| | POSIX | Windows |
|---|---|---|
| `remove(name)` | `shm_unlink`；之后再 `open` 会 `not_found` | 报告名字是否存在；对象在任一 handle 打开期间存在 |
| `remove_unlinks_the_name()` | `true` | `false` |
| 删除后已存在的视图 | 仍然有效 | 仍然有效 |

依赖"我删掉之后别人就连不上"的代码必须检查该标志。

名字规则取两个命名空间的交集，因此通过的名字可移植：1–96 字节，
字母/数字/`.`/`_`/`-`，不以点开头，不含分隔符。

## 字节环 —— `ipc::ring_buffer`

单生产者单消费者的字节环，无锁，布局只有整数，因此可放进映射。

```cpp
crpc::ipc::ring_control control{};
crpc::ipc::ring_view::initialise(&control, capacity);
auto producer = crpc::ipc::ring_view::attach_producer(&control, payload);
auto consumer = crpc::ipc::ring_view::attach_consumer(&control, payload);

producer.try_write(frame);                        // 满则 false，且不写入
const std::size_t n = consumer.try_read(buffer);  // 空则 0
```

两个不显然的设计点：

- **不用哨兵槽**。满环恰好容纳 `capacity()` 字节。占用 = `write - read`，
  两条自由增长的 64 位计数分别只由一侧写入。
- **内存序**。生产者先写字节、再以 `release` 存写索引；消费者以 `acquire` 读索引、
  再读字节。反方向刻意用 relaxed：陈旧的读索引只会让生产者以为更满，损失吞吐，
  不可能损坏数据。

一个 `ring_view` 要么生产者、要么消费者，不能兼具，因此环不会意外出现两个生产者。

## 值队列 —— `ipc::spsc_queue`

同样的环，但装的是值而不是字节。

```cpp
crpc::ipc::spsc_queue<std::uint64_t> q(1024);
q.push(7);            // 满则 false
std::uint64_t out;
q.pop(out);           // 空则 false，且不改动 out
```

`T` 必须可平凡拷贝且可默认构造，由 `static_assert` 强制——一个持有指针的值放进对端也能
看到的映射里毫无意义。控制块记录元素大小，因此把 `int` 队列当成 `double` 队列映射会被
拒绝，而不是读出一堆看似合理的垃圾。

跨进程使用时请用 view 形式配合映射；`spsc_queue` 自己持有存储，用于进程内 per-shard 工作队列。

## 全双工通道 —— `ipc::channel`

一个映射里两个环：创建方往一个环生产、从另一个环消费，加入方相反。这也是为什么通道
不是"一个环加锁"——共享的环会有两个生产者。

```cpp
auto creator = crpc::ipc::channel::create("worker.1", 64 * 1024);
auto joiner  = crpc::ipc::channel::join("worker.1");

creator.value().try_send(frame);   // 创建方 -> 加入方
std::byte buf[256];
joiner.value().try_receive(buf);   // 读创建方发来的数据
```

映射布局，全部是相对基址的固定偏移：

```
channel_header    magic、capacity、就绪标志
ring_control      出向（创建方 -> 加入方）
ring_control      入向（加入方 -> 创建方）
payload           出向，capacity 字节
payload           入向，capacity 字节
```

就绪标志是必要的：创建通道是多次写入，中途加入的对端会看到半初始化区域。创建方最后以
release 序发布 `ready`；加入方 acquire 读到未设置时返回 `unavailable`——"仍在创建，请重试"，
而不是一个悄悄出错的视图。

## 跨进程唤醒

一个进程看不到另一个进程的内存，因此写方无法向读方发信号。`try_send` 会敲响通过
`bind_runtime()` 绑定的 runtime，唤醒**同一 runtime** 上 park 的读者；跨进程时读方必须
靠自己的事件循环轮询，`poll_ready()` 就是为此存在。

## 实测开销

Release 构建，8 字节帧。下表是 Windows（MSVC）数据；**Linux 上同一份代码测出来的数字不同**，
两个平台的差异在正文里单独说明——只给一组数字会让"某台机器"看起来像"整个库"。

| 操作 | 开销 |
|---|---|
| `spsc_queue` push + pop，单线程 | 4.0 ns/op |
| `spsc_queue` push + pop，双线程 | 9.3–13 ns/op（调度噪声） |
| `ring_buffer`，64 字节帧 | 9.0–10.9 ns/op |
| `ring_buffer`，1 MiB 帧 | 42–56 µs（约 40 GB/s，内存带宽） |
| 环索引，2 的幂容量 | 3.0–4.5 ns/op |
| 环索引，非 2 的幂容量 | 3.5–6.4 ns/op |
| **`std::mutex` + `std::deque`，同样模式** | **15.6 ns/op** |

mutex 那一行正是无锁形式存在的理由，但**收益大小取决于工具链**，这里必须精确：
Windows 上 mutex 路径是 15.6 ns/op、SPSC 是 4.0（**3.9 倍**）；同样代码在 Linux 上是
9.8 对 4.0（**2.4 倍**）。两者都是**无竞争**情形。方向一致，但只引 Windows 的倍数会
夸大 Linux 构建能拿到的收益。

容量为 2 的幂时索引归约用掩码，否则用取余——仅索引一项相差约 30%。单元素队列操作就是
一次存储加两个原子索引，SIMD 无从下手；1 MiB 帧已经跑在 `memcpy` 的内存带宽上，而
`memcpy` 本身使用可用的最宽 load/store。

### 这个队列**没有**被用在哪里，以及为什么

`runtime::submit` 与 `push::hub` 都是**多线程入队**：服务器连接由拥有它的那个线程提交，
hub 由每个发布者喂入。把这些换成 SPSC 队列不是优化，而是正确性缺陷——两个生产者会破坏
整个设计赖以成立的那对索引。所以实测到的 3.9 倍是"在单生产者路径上应当优先选它"的理由，
而不是"在所有地方强推它"的理由。

## 测试

每个基元的用例都是针对"它为什么存在"设计的，而不只是校验一个值：

- 共享内存：写回必须通过**第二个**句柄验证——私有页拷贝在同一句柄上读回也会通过；
- 字节环：1 到 capacity 的每个帧长都穿过 9 字节的环，几乎每个长度都跨边界；
- 值队列：满容量不丢槽、跨边界保持 FIFO、消费者视图不能 push；
- 通道：两个方向各自独立、移动后不得交换两端；
- 异步：shard 线程上的等待必须是 park 而非阻塞。

有两个用例跨的是**边界**而不是分支：

- 共享内存通过**第二个进程**读回：测试二进制把**自己**再启动一份当对端，
  因此进程私有的映射会让用例失败，而不是通过；
- 两个平台都跑整套。Linux（Ubuntu 24.04、g++ 13.3、CI 同款 flags）上 POSIX 的
  `shm_open`/`ftruncate`/`shm_unlink` 分支被编译并执行，其用例报 **46** 项断言、
  Windows 是 **43** ——多出的三项正是 `remove_unlinks_the_name() == true` 那条只有
  POSIX 才会走的路径。

仍未覆盖：内存序论证需要弱序 CPU 才能被检验；真实读者靠 `poll_ready()` 被事件循环唤醒的
路径在测试里没有外部 poller 可接，因此测试改用轮询 `try_receive`。
