# 发布说明

[English](CHANGELOG.md) | **简体中文**

## 未发布

### Redis 分布式锁：新增 `renewal_guard` 作用域守护以自动续租

- `redis::renewal_guard` 用 RAII 包装 `distributed_lock::start_watchdog()` /
  `stop_watchdog()`：续租线程在守护存活期间运行、退出时拆除，调用方不会忘记启动
  （或忘记停下）自己启动的续租线程。这就是早前设计稿里被延期的「auto-renew」
  便捷功能（原称 `lock_options::auto_renew`），以「累积型 API」交付：新符号、
  不改 `lock_options` 布局，属 MINOR 新增。
- 两条新用例双向钉住：守护存活期间必须落地多次续租、退出后不得再发；不带守护
  获取锁不得启动续租线程（默认生命周期不变）。删掉守护析构里的停止调用会让
  第一条用例变红，即该 RAII 契约有判别者。

### C++20 构建：库在 C++20 下可编译可运行，并有 CI 任务实际执行

- 库现在与 C++17 一样能在 C++20 下构建并跑过全量测试。阻塞点是
  `std::atomic_load`/`std::atomic_store` 对 `shared_ptr` 的在 C++20 中被弃用
  （MSVC STL4029），而 `/W4 /WX` 把弃用变成硬错误；GCC 与 clang 不做此诊断。
  新增 `crpc/detail/atomic_shared_ptr.hpp` 封装 `cluster::registry` 与
  `resilience::load_balancer` 使用的换入换出操作，并在会报弃用警告的编译器上
  局部抑制该警告。
- 该封装刻意保留普通 `std::shared_ptr` 成员与自由函数 API，而不是改用
  `std::atomic<std::shared_ptr<T>>`：该特化在 MSVC x64 上是 16 字节对齐，而
  `shared_ptr` 是 8 字节，嵌入它会**仅在 C++20 构建**下改变两个导出且消费者可
  构造的类的布局——C++20 消费者配 C++17 构建的库即出现静默布局错配。实测：
  不用封装时 MSVC x64 C++20 下 `load_balancer` 从 160 增到 176 字节、`registry`
  对齐从 8 变 16；用封装后两者在 C++17 与 C++20 下完全一致。测试
  （`atomic_shared_ptr_keeps_its_owner_layout`）把封装钉在与普通 `shared_ptr`
  相同的 sizeof 与对齐。
- 构建脚本新增 `-CxxStandard`/`--cxx-standard 17|20`；标准进入构建目录名
  （`...-cxx20`），因此 C++20 配置不会静默重配 C++17 目录。新增 CI 任务
  `linux/gcc/cxx20` 运行 `test.sh --cxx-standard 20`，`tests/test_rpc_response_cpp20.cpp`
  及其四个 `co_await` 用例从此真正运行——此前没有任何任务传
  `-DCMAKE_CXX_STANDARD=20`，该文件在所有配置下都是空操作，且库在 C++20 下
  根本编译不过。
- 目标平台/标准使其什么都不执行的测试二进制现在打印 `SKIP crpc_test_*` 行而
  不是静默通过（六个空操作 main），并由 `docs_lint_noop_tests_announce_themselves`
  强制执行：平凡 `int main()` 必须在自己的函数体内声明原因。守卫会忽略注释/
  字符串内的出现——其自身文档就引用了被禁止的形状——并要求声明在 main 体内，
  而非文件任意位置（已证伪：全文件搜索会放行一个什么都不打的 main 变异）。
- 证据：Windows x64/x86 Debug 与 Release 均 192/192，shared 构建 193/193 且导出
  门禁通过，Linux gcc Debug 在 C++17 与 C++20 下均 192/192。

### IPC：命名共享内存、字节环、值队列与通道

- `fs::shared_memory` 提供命名映射，供两个本地进程在不经套接字的情况下交换缓冲区。
  两个平台差异被写进 API 而不是隐藏：16 字节头部记录创建者请求的长度，因为 Windows
  把映射向上取整到页且事后无法取回请求值；`remove_unlinks_the_name()` 报告 `remove()`
  究竟是真正把名字移出命名空间（POSIX），还是仅报告其存在（Windows 下对象在任一
  handle 打开期间一直存在）。
- `ipc::ring_buffer` 是无锁单生产者单消费者字节环，控制块只有整数，因此可放进映射。
  不设哨兵槽：满环恰好容纳 `capacity()` 字节，占用 = `write - read`，
  两条自由增长计数各由一侧写入，生产者在字节之后以 release 序发布索引。
- `ipc::spsc_queue` 是同样结构的值队列。`T` 必须可平凡拷贝，由 `static_assert` 强制
  ——持有指针的值放进对端也能看到的映射里毫无意义；控制块记录元素大小，因此把 `int`
  队列当成 `double` 队列映射会被拒绝。
- `ipc::channel` 把两个环组合成全双工通道：创建方往一个环生产、从另一个消费，加入方相反。
  就绪标志最后以 release 序发布，因此中途加入的对端拿到 `unavailable` 而不是半初始化视图。
- 异步契约：`try_send`/`try_receive` 从不等待；带超时的 `send`/`receive` 在 shard 线程上
  park（绝不阻塞，否则会与等同一 shard 的对端死锁），在普通线程上按界等待。超时为零表示不等待。
- 环索引在容量为 2 的幂时避免 64 位除法：实测 3.8–4.5 ns/op 对 5.0–6.4 ns/op，约 30%。
  刻意未加 SIMD：单元素队列操作是一次存储加两个原子索引，没有可加宽的东西；1 MiB 帧
  已经跑在 `memcpy` 的内存带宽上。
- ABI：既有类型无变更，新增头文件为增量。
- 已验证：`ctest` 188/188。未验证：本机未编译 `fs::shared_memory` 的 POSIX 分支；
  内存序论证需要弱序 CPU 才能被检验。

### ODBC：事务改用驱动的手动提交模式，状态不再是猜测

- `in_transaction()` 无论连接正在做什么都硬返回 `false`，而 `begin()` 确实开启了事务。`db::pool` 在复用会话前要读这个答案，于是脏的 ODBC 连接会带着未结束的事务交给下一个借用者——下一个借用者归还时可能提交或回滚它从没执行过的工作。
- `begin()`/`commit()`/`rollback()` 改用 ODBC 自身的事务模型，而不再把 `BEGIN TRANSACTION`/`COMMIT`/`ROLLBACK` 当文本发出去：`begin()` 用 `SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_OFF)` 把连接切到手动提交，`commit()`/`rollback()` 用 `SQLEndTran` 结束事务再恢复自动提交。这是所有驱动共用的一套映射，而不是每个驱动一种 SQL 方言；也正是它让状态可观测：`in_transaction()` 从连接读回 `SQL_ATTR_AUTOCOMMIT`，答案来自驱动，而不是本库自己记的标志。
- `close()` 在 `SQLDisconnect` 之前先结束未完成的事务。驱动不允许从事务中直接断开——断开失败而服务端会话仍然存活，直到超时前一直持有锁，阻塞同一张表上的下一个调用者。被遗弃的 `begin()` 不再比开启它的对象活得更久。
- 已有事务时 `begin()` 返回 `failed_precondition`；没有事务时 `commit()` 同样。空连接上的 `rollback()` 仍是空操作：池会先问 `in_transaction()`，而这个答案被有意取得偏保守（读属性失败也算"开启"），所以回滚可能落到一个无事可回滚的连接上；防御性回滚的调用者也不该先问一遍。
- 结束事务失败时，连接被有意留在手动提交状态：事务仍活跃时把自动提交打开会**提交**它，这在回滚失败之后就等于把要求丢弃的工作持久化。保持原状则 `in_transaction()` 继续返回 true，池会丢弃该会话而不是复用。
- `SQLGetConnectAttr` 读失败按"事务已开启"处理。两个方向的错误不对称：多报一次 true 只多一次结束空事务的 `SQLEndTran`，而错报 false 会让池以为会话干净，把未提交的工作交给下一个借用者。这正是 `mysql::connection` 对陈旧状态字早已采用的法则。
- ABI：无变更。没有公共签名或数据成员移动；`run_simple` 是编译单元内部的。
- 有两条边界必须写明。其一，`in_transaction()` 只观测本类开启的事务：通过 `query()` 发出的 `BEGIN TRANSACTION` 不可见，因为它不改变驱动的提交模式。其二，当**服务器自行**中止事务（`XACT_ABORT ON`、死锁牺牲者、批量错误回滚）时，`SQLEndTran` 在根本没有事务可结束时同样返回成功，于是 `commit()` 报告成功，而工作已经没了。因此 `commit()` 成功只表示驱动结束了它手上的事务，**不表示数据还在**；中止由那条失败的语句本身报告，在事务里忽略语句错误再指望 `commit()` 兜底的调用方会静默丢掉此前的工作。
- 已在桥接可能遇到的两族驱动上都验证过。SQL Server LocalDB + `ODBC Driver 17`（x64，`-DCRPC_WITH_ODBC=ON`）：13 条用例、76 项断言，5 条事务用例全绿 —— 那 13 个失败全部来自三条按 Jet 方言写的既有用例，它们在 SQL Server 上本来就不可能通过。真实 32 位 Access 驱动（x86 构建 + Jet 4 `.mdb`）：13 条用例、97 项断言、0 失败，Jet 方言那三条在那里也通过。Access 那一轮最关键：Jet/ACE 正是这个桥接存在的理由，只在 SQL Server 上成立的事务模型会是错误的修法。

### MySQL：连接池归还连接时复位会话（`autocommit`）

- `autocommit=0` 不再跨租约泄漏。池在把连接放回空闲集之前已经会回滚未结束的事务，但 `autocommit` 是 `ROLLBACK` 不会复位的会话变量：下一个借用者的语句会加入一个无人提交的隐式事务，并在它归还时被回滚——静默丢数据。`db::connection` 新增 `reset_session()`；池在回滚成功后调用它，`mysql::connection` 在会话尚未处于默认值时会发送 `SET autocommit=1`。
- 代价是每次归还**可能**多一次往返，且仅当会话最近一条完成命令没有报告 `autocommit=1` 时才发；常见情形仍是零往返复位。无法复位会话的连接会被关闭而不会放回空闲集，因为清理不了的会话不该交给下一个借用者。
- ABI：`db::connection` 新增虚函数 = vtable 变更 = MAJOR，所有消费者必须重编。默认实现返回 `status{}`，其它后端无需改动。

### MySQL：事务状态以服务端为准，不再依赖本地猜测

- `in_transaction()` 现在报告最近一条已完成命令的服务端 `SERVER_STATUS_IN_TRANS` 位，而不再使用只由 `begin()`/`commit()`/`rollback()` 维护的本地标志。对 `autocommit = 0` 的会话：连接池会回滚那个隐式事务，而不是把它交给下一个借用者；`commit()` 会提交它，而不是返回 `failed_precondition`；触发隐式提交的 DDL 也不再留下幻影事务。两个标志把两个问题分开：`explicit_txn_`（是否由我们开启）服务于 `begin()` 的闸，`server_txn_`（服务端是否这么说）用于回答池。`ERR` 包不携带状态字，因此语句失败后沿用上一个状态；在 `autocommit = 0` 下事务在错误后仍然存在，连接因此被视为"有未提交工作"，池会回滚它而不是交给下一个借用者。

### MySQL：删除 private 且无人调用的 `set_autocommit`

- 删除了 private 且无人调用的 `mysql::connection::set_autocommit`，由三个私有辅助（`apply_status`、`apply_error`、`has_uncommitted_work`）承担其职责。类上的 `__declspec(dllexport)` 会连同 private 成员一起导出（inline 定义也不例外），因此共享库构建下净效果是少一个、多三个私有符号。无公开 API 变更：它们都无法被消费者命名。

### 测试时间断言：保留能失败的断言，删掉不能失败的

- `client_cancel_during_backoff`（`tests/test_server_client.cpp`）现在在等待线程内部
  测量唤醒：`t_done` 在 `get()` 返回后立即取值，`join()` 的调度延迟不再计入区间。
  重试退避由 100 ms 提高到 1000 ms，上界为 250 ms，使立即唤醒（微秒级）与完整退避
  （一秒）分别落在中点两侧约四倍处。区间同时断言非负：负值说明测量本身坏了，而只查
  上界会静默放过它。该用例注明只能在 plain 线程运行：在协程之外的 shard 线程上调用
  `get()` 会直接以 `failed_precondition` 失败，用例根本到不了取消路径；而 shard 等待
  被切成 5 ms 的片，唤醒延迟由切片而非条件变量通知决定。
- `iocp_timer_fires_and_cancels`（`tests/test_io_iocp.cpp`）不再断言
  `elapsed < 20 ms`。1803 之前的回退定时器约 15.6 ms 的 tick 能通过该上界，而高分辨率
  路径在有负载时的中位数（23.7-25.8 ms）反而失败，方向是反的；5 s 的 `wait_for` 保留为
  挂死守护。替代它的是新能力 `crpc::iocp_backend::high_resolution_timer()`：在只能跑
  回退定时器的主机上用例跳过（绝不伪装通过）。ABI：新增一个 MINOR 符号，仅限 Windows，
  类中不增加数据成员（标志位于 out-of-line `impl`），`sizeof(iocp_backend)` 不变。
- `graceful_shutdown_drains_and_rejects`（`tests/test_server_client.cpp`）曾断言
  `elapsed < 2 s`，而 `cfg.drain` 就是 2 s：上界恰好等于合法路径，用满预算的 drain
  必然是红的。现改为 `cfg.drain * 4`。
- `net_accept_cancelled_by_close`（`tests/test_net_transport_edge.cpp`）曾等待 4000 ms
  判唤醒，却又要求 `elapsed < 3 s`，于是慢而正确的唤醒被判红。现在这个等待是 4000 ms
  的纯挂死守护（实测唤醒在 200 ms 以内），冗余的墙钟断言已删除；返回的 `shutting_down`
  码才是语义的证明。
- `perf_budget`（`tests/test_frame_compression.cpp`）中的绝对 LZ4 微秒预算移入
  `bench/bench_compression.cpp` 作为软性 PASS/WARN 阈值，CRC32C 的相对比较
  一并移入。CI 保留的是尺寸缩放断言：32 KiB 输入是 256 字节的 128 倍，线性工作量远低
  于 `128 * 4` 的上界，只有超线性回归才会触发它；自指测量（同一 codec、同一进程、两种
  尺寸）使插桩对两侧对称，因此在各优化级别与 sanitizer 下都稳定。CRC32C 的相对比较留在
  bench，因为 x64 上 `crc32c` 走的是 `_mm_crc32_u64` 内建指令，插桩对它与纯软件 LZ4
  编码器的影响差别很大 —— 实测裕度只有 2.4-2.9 倍，低于行为断言所需的 3 倍。用例同时
  保留正确性断言：可压缩的 32 KiB 输入必须缩小，两种尺寸都必须逐字节往返。
- 证据：`tests/test_server_client.cpp`、`tests/test_io_iocp.cpp`、
  `tests/test_net_transport_edge.cpp`、`tests/test_frame_compression.cpp`、
  `bench/bench_compression.cpp`，以及一次全量 `ctest -j 4`。

### 流控接入客户端调用路径

- 自适应流控原语（`flow::rtt_estimator`、`flow::pacer`、`flow::congestion_window`、`flow::rtt_tracker`）现已通过连接级 `rpc::flow_controller`（`include/crpc/rpc/flow_control.hpp`、`src/rpc/flow_control.cpp`）接入 `rpc::client`。默认关闭；`rpc::client::set_flow_control` 安装它，门面提供同名调用并对每个池内连接生效（包括之后新建的连接）。
- 准入是以编码帧为单位的字节窗口。空窗口无条件放行一条调用；`ONE_WAY` 调用从不记账（发布后立即完成，无人能确认它）；被拒绝的调用在错误 metadata 中携带 `crpc.flow_rejected`，门面据此归类为本地闸门（不重试、不计入端点熔断）。
- 往返样本来自 PING/PONG 探针：客户端发送 payload 为发送时间戳（8 字节）的 PING，服务端以 stream 0 上的 PONG 原样回填。同一时刻最多一个探针在途，探针流量由往返时间界定。探针绕过字节窗口，但不绕过 `max_pending_write_bytes`。流控不给发送路径增加任何时钟读取。
- 拥塞控制：`theta = baseline * growth_per_mille / 1000`；超过它的样本不喂给估计器，且窗口每个基线周期最多收缩一次；停在底部的窗口连续 `slow_confirmations` 个超阈样本会被接受为新基线，使环路能离开底部。低于基线的样本需要确认，且接受窗口内**最慢**的样本，因此单个伪影无法毒化基线。
- 每 lane 字节配额，在所有终止路径（完成、取消、句柄析构、`fail_all` 兜底）上以幂等的 `(stream_id, generation)` 键释放。门面在消耗 lane 序号**之前**检查准入，因此本地拒绝不会在该 lane 留下空洞。pacer 是连接级的，其速率跟随 `window / baseline`。
- 修复：`rtt_tracker` 与 `pacer` 曾用 0 作为"未初始化"哨兵，但 0 是合法时间戳，导致从 0 起跑的时钟的第一个探针不可用、以 0 锚定的 pacer 永远拿不到信用。哨兵现改为全 1。`pacer::set_rate` 现在等比缩放已有信用，速率上调在下次发送即生效，而不是两个 refill 周期之后。
- 修复：`rpc::client::start_call` 曾用 code 与 message 重建失败的 `status`，丢弃 metadata（包括 `crpc.flow_rejected`）；现在转发完整 `rpc_error`。
- ABI：`rpc::client` 增长一个指针大小的成员（MAJOR）；`rpc::client_options` 与 `client::options` 不变。新增 MINOR 符号：`set_flow_control`、`can_admit`、`flow_snapshot`、`rpc_client_size_probe`，以及 `rpc::flow_controller` / `flow_control_options` / `flow_state` 类型。服务端零改动（PONG 是无状态回填）。证据：`tests/test_flow_wiring.cpp`、`tests/test_flow_clock_base.cpp`、`tests/test_flow_lane_quota.cpp`。

### 服务端幂等去重表

- RPC 服务端现在会对重放调用去重。信封携带非空 `call_id`（R6 三元组
  `(client_id, client_epoch, seq)`，客户端每次重试原样复用）的请求在 handler
  执行前先进入一张有界表：新键执行；仍在处理中的键把副本**挂起**（绝不丢弃），
  等 leader 发布后原样重发其编码响应，因此并发副本恰好执行一次 handler；
  已完成的键逐字节重放缓存响应，不执行 handler。这补上了需求 12 的
  「响应丢失 + 重试 = handler 执行两次」正确性事故，也让需求 16 的重试预算
  站在真实的服务端保证之上。证据：`tests/test_rpc_dedup.cpp`（13 用例 / 97 断言）。
- 新增 `rpc::dedup_table`（`include/crpc/rpc/dedup.hpp`、`src/rpc/dedup.cpp`），
  由 `rpc::dispatcher` 持有、同一 server 进程的全部分片共享。配置经
  `rpc::dedup_options`（由**头内联**的 `dispatcher(registry, dedup_options)`
  构造函数消费，记录因此不跨共享库边界——[abi.zh-CN.md](abi.zh-CN.md) §4）：
  容量 4096 条、LRU 淘汰（处理中条目被钉住；被处理中条目占满的表退化为至少一次
  bypass，绝不挂死、绝不无界增长——原稿的每客户端 FIFO 环在客户端数量上无界，
  这正是改用 LRU 的原因）；以单调时钟 5 分钟 TTL 定义去重窗口（绝不用墙钟）；
  单条缓存响应上限 16 KiB（超限响应只递交给完成时挂起的副本，不为重放保留）；
  挂起副本上限 30 秒——且永远与请求自带 wire deadline 取交集——`max_wait <= 0`
  完全禁用挂起（处理中副本立即收到可重试的显式错误）；容量 0 关闭该表。
  计数器（`claims_leader`、`claims_replay`、`claims_waiter`、`claims_bypass`、
  `waiters_woken`、`waiters_abandoned`、`waiters_expired`、`evictions`、
  `entries`）经 `dedup_table::stats()` 读取。
- 只有成功的响应会被缓存与重放。失败的完成是一枚未缓存墓碑：与其并发的副本
  收到 leader 的错误，但其后的重放会再执行——客户端重试策略因此保持有意义
  （`test_server_client.cpp` 的 `retry_idempotent_only` 依赖这一点）。
  重放的 `ONE_WAY` 副本被静默丢弃。
- 挂起的副本可被打断：服务端把连接指针作为等待键传给 `dispatch`，连接拆除
  （`on_terminal`、发送失败、连接析构）时调用 `dedup_table::abandon`，以显式
  `canceled` 错误结束该副本，而不是让分片线程滞留。
- 表的键是 `(call_id, service_id, method_id)`：设计按
  `(client_id, target_service)` 分配 `seq`，以信封 id 限定作用域可以排除跨方法的误重放。
- 已文档化的限制（[architecture.zh-CN.md](architecture.zh-CN.md) §14）：保证只覆盖
  `min(TTL, LRU 生命周期)` 窗口——淘汰或过期后的重放会再执行，因此客户端重试预算
  与服务端窗口必须配套取值；`client_epoch` 变化（客户端重启）产生新键，旧条目经
  同样的边界老化淘汰（epoch 是键的一部分，旧 epoch 条目永远无法遮蔽新 epoch 调用）；
  表是每进程的——多分片 server 经同一 dispatcher 共享，但独立进程/节点各自去重，
  把 `call_id` 路由到同一节点是负载均衡的事，本层不解决。
- **ABI：** 纯增量。新增导出类 `rpc::dedup_table`（含内嵌 `lease`）、`dispatcher`
  构造函数重载与新的六参数 `dispatch` 重载；`dispatcher` 增加了一个
  `dedup_table` 数据成员，消费方与库必须用同一套头文件重建（本 MAJOR 周期
  本来就有此要求）。无既有记录改变布局；无既有导出符号改签名。

### HTTP/2 客户端：ALPN 协商、h2c 先验知识与多路复用

- `http::client` 现在会在连接协商出 HTTP/2 时说 HTTP/2。TLS 下由 ALPN 结果决定：默认提供
  `{"h2", "http/1.1"}`，对端选择 h2 即走 h2（`response::version` 为
  `http_version::http_2`）；选择 http/1.1 —— 或完全没有 ALPN —— 则保持 HTTP/1.1 客户端。
  明文下通过新增的 `client_options::h2c` 使用 h2c 先验知识（RFC 9113 3.4），默认关闭：
  发出连接前导后不存在回退路径。
- **ABI 破坏：** `http::client_options::tls_alpn` 从 `std::string` 改为
  `std::vector<std::string>`（偏好列表），`client_options` 新增 `bool h2c`。两者都是布局变更
  （[abi.md](abi.md) §1），消费者必须重编；本未发布周期已是 MAJOR。
- 新增 `http/h2_client.hpp`：`http::h2_client` 保持一条连接并多路复用流 —— `begin`
  把整个请求写上线而不等待，`finish` 泵帧直到该流响应完成（其他流的响应与失败按流
  暂存、由各自的 `finish` 取回），`send`/`send_text` 覆盖一次性场景。伪头先发
  （`:method`/`:scheme`/`:path`/`:authority`），字段名小写，连接专用字段
  （`Connection`、`Keep-Alive`、`Proxy-Connection`、`Transfer-Encoding`、`Upgrade`）、`Host`
  以及非 `trailers` 的 `TE` 一律丢弃。`make_h2_client` 基于 TCP（h2c 先验知识）或 TLS
  （要求协商出 "h2"；其他结果以 `failed_precondition` 上报，由调用方回退到
  `http::make_client`）。响应侧：`:status` 决定状态码，trailer 进入 `response::trailers`，
  `content-length` 与实际正文不符时以 `protocol_error` 明确报错，绝不静默截断。
- **未实现：** 服务端推送（客户端通告 `SETTINGS_ENABLE_PUSH=0`，对端仍推送则连接失败）
  与 CONNECT。二者在此记录，不做静默。
- `http::http_version` 追加 `http_2`（按 [abi.md](abi.md) §1 追加在末尾）。
- `db::stream_channel::wait_handshake` 有界等待 TLS 握手完成；ALPN 结果在握手后才存在，
  工厂据此在所选协议的首个字节上线前完成决策。
- 两处 HTTP/2 连接层缺陷修复，由客户端角色对本库自身服务端的实测暴露
  （`src/http2/connection.cpp`）：
  - client 角色连接不再把对端 `SETTINGS_ENABLE_PUSH=0` 当协议错误（RFC 7540 6.5.2 只约束
    取值、不约束方向；两种角色均记录该值）；
  - 已关闭流上收到的 `WINDOW_UPDATE` 按规范忽略，不再以 `STREAM_CLOSED` 回敬
    （RFC 7540 5.1 closed 态）。

### ABI 破坏：ADO 式参数方向与 ORM 列元数据

- `db::parameter` 新增 `std::string name` 与 `parameter_direction direction`
  （`input`、`output`、`input_output`、`return_value`），以及 `is_output()`、
  `sends_value()` 与 `parameter::out()` 工厂。存储过程通过 OUT/INOUT 写回结果，
  而 MySQL 用会话变量绑定它们，因此方向必须是参数的一部分，不能按调用点猜。
- `db::column_info` 新增 `schema`、`table`、`original_name` 与 `flags`。ORM 要把
  结构体字段绑定到列，必须知道原始列名（用别名会绑错字段）与所属表，而不只是别名
  和抽象类型。
- 两者都是公开聚合布局，因此属 MAJOR（[abi.md](abi.md) §1）：消费者必须重新编译。
  按位置初始化前几个字段仍可编译；新成员追加在末尾。

### ABI 破坏：Redis 传输层迁移到 `crpc::net`

- 字节级的 `connection`、`connection_config`、`connection_pool` 与
  `pool_config` 本来就不含任何协议知识，因此从
  `include/crpc/redis/connection.hpp` / `src/redis/connection.cpp` 迁移到
  `include/crpc/net/connection.hpp` / `src/net/connection.cpp`。memcached、
  数据库适配器与消息队列客户端共用同一套传输与连接池，而不再各自复制一份。
- `crpc::redis::connection` 等旧名字以 `using` 别名保留在
  `<crpc/redis/connection.hpp>` 中，按旧名书写的源码无需修改即可编译。
- 链接符号发生变化，按 [abi.md](abi.md) §1 属 MAJOR：
  - `ABI 破坏：crpc::redis::connection::connection(crpc::connection_config, crpc::backend_factory&, crpc::runtime*)` 由 `??0connection@redis@crpc@@QEAA@Uconnection_config@12@AEAVbackend_factory@2@PEAVruntime@2@@Z` 变为 `??0connection@net@crpc@@QEAA@Uconnection_config@12@AEAVbackend_factory@2@PEAVruntime@2@@Z`。
  - `ABI 破坏：crpc::redis::connection::write(crpc::span<std::byte const>)` 由 `?write@connection@redis@crpc@@QEAA?AVstatus@3@V?$span@$$CBW4byte@std@@@3@@Z` 变为 `?write@connection@net@crpc@@QEAA?AVstatus@3@V?$span@$$CBW4byte@std@@@3@@Z`。
  - 该命名空间变更同样适用于 `connection`、`connection_pool` 的全部成员以及
    `crpc::redis::pool_config`。消费者必须用新头文件重新编译。
- 两处前向声明改为包含头文件：`redis::pipeline.hpp` 与 `redis::sentinel.hpp`
  现在包含别名头文件，因为别名无法前向声明。
- 影响：ABI 为 MAJOR；消费者源码无需改动。验证：静态 x64 Debug 全量 74/74；
  `CRPC_BUILD_SHARED=ON` 构建下共享库消费者门禁
  （`crpc_test_shared_link`）通过。

### 可靠性：可靠流水线路径不再死锁

- `rpc::client` 在 `completion_mutex_` 与 `map_mutex_` 上存在两套互逆的锁序：完成路径先 completion 后 map，而 `release_slot_under_lock`（句柄析构路径）先 map 后 completion。inproc 后端上并发 > 1、batch > 1 的流水线负载约三分之一概率命中。现在全文件只有一种顺序：`completion_mutex_` 外层、`map_mutex_` 内层。
- `poll()` 与 `fail_all()` 不再快照裸 `call_slot*`。`then` 回调在完成锁内运行，且合法地可以销毁**兄弟**句柄——那会把兄弟的槽位还回池；预先收集的快照因此可能去 finish 一个已回收的槽位，把伪造的 `timed_out` 写进去（若该槽位已被重新获取，则写进一个全新的、从未发送的调用）。两者现在每轮只取一个仍存活的 victim 并立刻 finish。
- `rpc::client::start_call` 新增尾部参数 `bool flush = true`。默认值保持文档化的「start_call 会发送」语义；`client_facade` 传 `false`，改在新 `flush_pending()` 里于连接槽位锁之外 flush，这正是让同步 inproc drain 安全的关键。源码兼容；模板在消费者侧实例化，库符号未变。
- 影响：MINOR。新增回归 `tests/test_poll_recycle.cpp`（2 个 case）覆盖两个回收窗口；对旧形状 200/200 check 失败。

### 错误诊断

- `rpc_error` 现在携带终结失败的来源与逻辑调用尝试次数：`failure_origin origin`（默认 `local_lifecycle`）与 `std::uint32_t attempts`（默认 `0`），追加在 `metadata` 之后（`include/crpc/error.hpp:179-183`）。按名称初始化前五个成员的聚合/位置初始化保持可编译。
- `crpc::to_string(const rpc_error&)` 把码、消息、来源与尝试次数渲染为一行。七个分类谓词（`is_local`、`is_network`、`is_timeout`、`is_circuit_breaker`、`is_rate_limiter`、`is_fallback`、`is_server`）把 `origin` 映射到失败的子系统，调用方询问它关心的问题，而不必在单个枚举上 switch。
- 进程级完成钩子（`diagnostics::set_completion_hook` / `on_completion`）恰好一次地观察每个终结的 `response<Resp>::get()` 结果，且不持任何库锁。当未安装用户钩子时，它在 `after_call` 运行进程级 `obs::interceptor_chain`（`obs::set_default_interceptor_chain`），使已安装的拦截器看到终结错误；未安装观察者时该路径为无锁、无分配的 no-op。
- `response<Resp>::get()` 在调用携带有效 trace 上下文时，把调用的 W3C trace id 挂到终结错误的 metadata 的 `trace_id` 键下，使失败可与产生它的 trace 关联，无需重新推导上下文。
- 影响：行为为 MINOR；下方 ABI 部分为 MAJOR。

### ABI 破坏：`rpc_error` 布局与 `failure_origin` 迁移

- `rpc_error` 由五个数据成员增至七个（`origin`、`attempts`）。公开结构体布局变更属于 MAJOR（[abi.zh-CN.md](abi.zh-CN.md) §1）。两个字段追加在末尾且有默认值，因此*初始化*前五个成员的消费者保持可编译，但按旧 `rpc_error` 分配或复制的消费者必须用同一份头文件重建。
- `failure_origin` 从 `client_detail` 迁入 `crpc`（核心错误头），使 `rpc_error` 无需循环依赖即可携带它。这重命名了两个导出符号：枚举类型现在 mangle 为 `crpc::failure_origin`，而非 `crpc::client_detail::failure_origin`。
  - `ABI 破坏：crpc::client_detail::breaker_record_for(crpc::client_detail::failure_origin, crpc::rpc_code)` 由 `?breaker_record_for@client_detail@crpc@@YA?AW4breaker_record@12@W4failure_origin@12@W4rpc_code@2@@Z` 改为 `?breaker_record_for@client_detail@crpc@@YA?AW4breaker_record@12@W4failure_origin@2@W4rpc_code@2@@Z`。
  - `ABI 破坏：crpc::client_detail::result_origin(const crpc::rpc_error&)` 由 `?result_origin@client_detail@crpc@@YA?AW4failure_origin@12@AEBUrpc_error@2@@Z` 改为 `?result_origin@client_detail@crpc@@YA?AW4failure_origin@2@AEBUrpc_error@2@@Z`。
  `client_detail::failure_origin` 仍是别名，既有拼写仍可编译，但链接的符号已改变。
- 影响：MAJOR。每个消费者必须用同一份头文件重建并重新链接。见 [abi.zh-CN.md](abi.zh-CN.md) §1、§4 与 §8.3，以及 [migration.zh-CN.md](migration.zh-CN.md)。

## 2.0.0 - 2026-10-24

### 变更（MINOR，仅行为）

- 服务端现在会在四个执行前准入拒绝点写出保留的响应 meta 条目 `crpc-degradable: true`：服务级熔断器（`unavailable`）、deadline 仍可达时的服务级令牌桶（`resource_exhausted`）、服务级 bulkhead（`overload`）与服务级在途上限（`overload`）。只有当请求剩余的线上 deadline 至少 1 ms 时才写（`resilience::min_remaining_ns`，现已与 dispatcher 自身的执行前 deadline 拒绝共用，不再有第二个字面量）。它让客户端能区分「handler 执行前被拒绝，降级值有意义」与「handler 执行后以同一个码失败」：handler 返回的 `overload` 与 bulkhead 返回的 `overload` 在线上除该条目外完全相同。五条错误路径永不携带它（连接级泛洪令牌桶、连接级在途上限、令牌桶的 deadline 不满足出口、dispatcher 的 deadline 拒绝、以及所有 handler 产生的错误），one-way 请求因没有响应帧也无法携带。`crpc-degraded` 在服务端仍无人写入，服务端仍从不调用 `fallback_execute`。
- 影响：MINOR，且属于**行为变更，不是 ABI 变更**。不新增、不移除、不改签名任何导出符号；没有公开记录改变布局或大小；不新增 `server::options` 字段，因此消费者在栈上分配 `server::builder` 不受影响。只有四个既有错误响应的负载各增长 23 字节（两字节 `meta_count` 由 `0x0000` 变为 `0x0001`，并追加一个 TLV 条目）。忽略未知 meta 键的客户端不受影响。见 [protocol.zh-CN.md](protocol.zh-CN.md) §10 与 [abi.zh-CN.md](abi.zh-CN.md) §4。

### 修复（MINOR，仅新增符号）

- `CRPC_BUILD_SHARED=ON` 构建现在导出全部文档化公开 API。此前只有 27 个类带
  `CRPC_API`，因此按 README 用法链接 `crpc.dll` / `crpc.so` 的消费者会在链接期
  因未解析外部符号失败：builder、进程内后端、全部韧性原语、push hub、MVC router、
  logger 与 tracer、HTTP 报文/编码/文件层、流控估计器、codec registry 以及 `obs`
  的 span 类型。现在 46 个公开类（43 个顶层类，加上嵌套的 `server::builder`、
  `client::builder`、`http::http_server::builder`）与 51 个含 out-of-line 定义的
  命名空间级函数，都在自己的声明上带上了装饰。另有 7 个跨 DLL 接口类
  （`io::completion`、`io::source`、`io::sink`、`io::connection`、`io::accept_sink`、
  `io::listener`、`io::backend_factory`）一并导出：消费者会继承它们并把对象传回库内，因此其析构
  vtable 槽与 RTTI 必须导出。
- 影响：MINOR，且是**纯符号变化**。没有符号被删除或改签名，没有公开记录改变布局
  或大小，也没有行为变化。Windows x64 上共享构建的导出表从 586 个名字增长到 1258
  个；静态构建不受影响，因为静态库按定义导出全部符号。
- 新增构建期门禁 `tests/test_shared_link.cpp`，仅在 `CRPC_BUILD_SHARED=ON` 时构建
  并运行。它是一个消费者：链接导入库并直接调用文档化 API，包括函数体引用
  out-of-line 成员的头内联模板，因此缺少装饰会在构建期失败，而不是在下游消费者
  那里。见 [abi.zh-CN.md](abi.zh-CN.md) 第 3 节与
  [building.zh-CN.md](building.zh-CN.md) 第 4 节。

### 新增（MINOR，仅新增符号）

- `client::resilience_statistics` 与 `client::resilience_stats()`：客户端韧性流水线的增补可观测性（`rate_limited`、`bulkhead_rejected`、`local_gate_rejected`、`endpoint_open`、`lb_unavailable`、`pre_send_retries`、`probe_admitted`、`retries_delayed_by_hint`、`bulkhead_in_use`）。不改变既有结构：`client::statistics` 的字段顺序、大小与含义保持不变，C ABI 入口不变。见 [abi.zh-CN.md](abi.zh-CN.md) §4 与 §8。
- `resilience::fallback_statistics` 与 `client::fallback_stats()`：`crpc/resilience/fallback.hpp` 中 fallback 原语的七计数器快照（`ineligible`、`skipped_deadline`、`attempted`、`failed`、`canceled`、`overrun`、`applied`），以及 `client::call<Resp>::degraded()`——它报告最终结果是否来自 fallback 函数（本切片恒为 false）。fallback 的实际接线见下文「fallback 接线与服务端时钟注入」，因此计数不再恒为 0。`client::fallback_stats()` 是该切片唯一新增的导出符号；按值返回的类型与 `degraded()` 访问器都是 header-only。见 [abi.zh-CN.md](abi.zh-CN.md) §4。

### 易用性深度改进（MINOR，仅符号）

易用性差距分析中的九个缺口被补齐。每一项都是增补式的：底层形式保留其签名，新名称是规范名称。

- `client::set_fallback_owned` 与 `crpc::fallback_install`：RAII fallback 安装器。`fallback_install` 用 `std::shared_ptr` 持有 `self`，并在销毁或 `reset` 时卸载该策略，因此五参数的 `set_fallback(function, self, min_remaining, retain, release)` retain/release 契约不再是安装策略的唯一方式。
- `rpc::call_context::set_deadline` / `set_timeout` / `set_idempotent` 与 `with_deadline` / `with_timeout` / `with_idempotent` 构造器：设置 deadline 时同时设置 `has_deadline`，因此只赋值 `deadline` 而不置标志位不再静默地表示「无超时」。
- `client::call<Resp>::then`：门面层的完成回调，与 `rpc::call<Resp>::then` 对应。调用终止时恰好触发一次，包括本地观测到的终止。
- `crpc::delivery_mode`：由 `crpc::rpc` 与 `crpc::push` 共享的一个枚举；两个命名空间别名是同一类型，因此 RPC 模式与推送模式无需转换即可互通。
- 构造器命名：`max_inflight_per_connection` 是客户端与服务端构造器上每连接在途上限的规范名称；服务端的 `per_connection_inflight` 是弃用别名。`default_deadline` 是客户端构造器的规范名称，`default_timeout` 是弃用别名。
- `crpc::service_id` 与 `crpc::method_id`：两个 id 的强类型 newtype。`service_id` 可从 `u64` 字面量隐式转换，但绝不从 `method_id` 转换，因此参数顺序颠倒会成为编译错误。
- `result<T>::error_or` 与 `value_or` 针对左值与 const 左值结果的重载；`call<Resp>::get()` 对无效句柄返回 `invalid_argument`，与协程外等待的 `failed_precondition` 区分开。
- `CRPC_HTTP_GET("/path", lambda)`：无需控制器类型的两参数 MVC 形式；路由序号与对象名来自 `__COUNTER__`，因此同一源行上的两次注册不会在 `__LINE__` 上冲突。
- `crpc::publish_result` 与 `server::publish`：`publish` 返回可区分的结果（`ok`、`empty_topic`、`not_running`、`unsupported_configuration`、`no_push_hub`、`internal_error`）。到 `bool` 的转换是隐式的，因此既有的 `if (server.publish(...))` 仍可编译且仍表示「ok」。
- 影响：MINOR，仅符号与行为。没有导出符号被删除或改签名，没有公开记录改变布局或大小（弃用的构造器别名是方法而非字段），没有线路值变化。不使用新名称的消费者不受影响。

### 文档与示例

- `examples/` 现在是已接入的构建目标。`examples/CMakeLists.txt` 在
  `CRPC_BUILD_EXAMPLES=ON` 下构建 `crpc_example_quickstart`——一个进程内程序，
  启动服务器、注册一个类型化方法、发起一次调用并处理一个推送订阅——并把
  它注册到 CTest；`examples/README.md` 说明如何构建与运行。此前该目录没有
  `CMakeLists.txt`，因此 `CRPC_BUILD_EXAMPLES` 必须保持关闭。
- `docs/api/README.md` 索引公开头文件与关键类型；`docs/tutorial.md` 从消息类型
  到一次调用与一个推送订阅逐步构建。两者均有中英版本，且 `README.md`、
  `README.zh-CN.md`、`docs/README.md`、`docs/README.zh-CN.md` 都链接了它们。
- 影响：仅文档与示例。没有公开头文件、符号、布局或行为变化；示例位于
  `src/` 与库目标之外。

### fallback 接线与服务端时钟注入

- 客户端现在在调用的最后一次尝试被终结、且其熔断结果被记录之后运行已安装的 fallback，
  落点是 `call<Resp>::get()` 的两个终态失败出口。`client::set_fallback(function, self,
  min_remaining, retain, release)` 安装策略，`client::run_fallback` 与
  `client::record_fallbacks` 是库内入口。保活 `self` 的引用在每次安装策略时取一次，
  在策略被替换或客户端销毁时释放一次；`self` 非空却缺少 `noexcept` 的
  `retain`/`release` 对会被拒绝，且什么都不安装。服务端四处执行前拒绝写下的
  `crpc-degradable` 是远端拒绝可降级的闸门；**已发送且非幂等**的调用永不降级，因为
  替换它的错误可能掩盖已经发生的副作用。超过 deadline 才返回的降级值报
  `deadline_exceeded`，不是降级成功。`crpc-degraded` 仍然没有线上写入者：可观测载体是
  `call<Resp>::degraded()` 与 `fallback_stats().applied`。
- 影响：新增符号属 MINOR，**下面的布局变更属 MAJOR**。既有成员偏移不变，但消费方必须
  用同一份头文件重建。
- `server::options` 尾部新增 `resilience::clock_source clock_source`（默认无效，库内替换
  为 `resilience::monotonic_clock()`），并新增 `server::builder::clock()` 设置它。同一个
  时钟现在同时供给每连接 rate 桶、每服务 rate 桶、每服务熔断器与 dispatcher 的 deadline
  判定，因此同一个请求不会再被两套「现在」判定。`sizeof(server::options)` 从 304 增长到
  **320**（x64）、从 256 增长到 **264**（x86）；`sizeof(server::builder)` 随之增长。按
  [abi.zh-CN.md](abi.zh-CN.md) 第 1 节，给公开结构体加字段属 MAJOR，因此本周期的 MAJOR
  记录也覆盖它。默认值完全保留旧行为：字段保持无效的 options 读 `monotonic_clock()`，
  与注入前服务端使用的 `std::chrono::steady_clock` 相同。
- ABI breaking：`server::options` 从 304 增至 320 字节（x64）、从 256 增至 264 字节
  （x86）；`server::builder` 随之增长（376 -> 392 / 296 -> 304）。没有导出符号改签名；
  新增三个（`client::set_fallback`、`client::run_fallback`、
  `client::record_fallbacks`）。

### 编译期布局护栏

- 新增内部翻译单元 `src/abi_check.cpp`（不进入任何头文件，也不导出符号），对跨越 ABI 边界的一部分公开记录做编译期断言：15 个记录的标准布局属性；15 个记录的精确成员数量（`CRPC_MEMBER_COUNT`，可捕获追加、中间插入或删除）；可捕获重排或改名的相对成员链（`CRPC_STEP`、`CRPC_STEP_SAME`、`CRPC_STEP_MIXED`、`CRPC_AFTER`）；`http_statistics` 的总宽度与尾字段位置；以及仅在 x86 与 x64 上的绝对 `sizeof`/`offsetof` 锚（`CRPC_ABS_SIZE`、`CRPC_ABS_OFF`）。成员数量正是能看到「尾追加被 x86 尾部填充吸收」这一情况的护栏。
- [abi.zh-CN.md](abi.zh-CN.md) §4 现在给出「跨越 ABI 边界」的范围谓词，并列出 36 个仍无编译期护栏的范围内记录（`crpc_test_abi_docs` 扫描确认的确切数量）。`crpc_test_abi_docs` 会重新扫描 `include/crpc/**/*.hpp` 与 `src/abi_check.cpp`：新增跨边界记录若不加入护栏，也不更新该清单，测试即失败。
- 已写入文档的盲区：成员数量护栏不校验名字（若记录存在相对链，则由相对链捕获；在 `rpc_error` 中则由 `src/abi_check.cpp` 的成员类型断言捕获）；在 15 个有成员数量护栏的记录中，`rpc_error` 是唯一没有相对链的，因此其内部的重排不会被该单元捕获。其构造函数只点名 `code`、`message`、`retryable`，所以不会捕获 `retry_after` 或 `metadata` 的改名；该单元的七条 `decltype` 断言点名了全部七个成员，因此其中任何一个改名都会在那里失败。`rpc_error` 与 `status` 的绝对 `sizeof` 被刻意不锁定，因为它取决于标准库配置。见 [abi.zh-CN.md](abi.zh-CN.md) §4。
- 影响：MINOR（仅构建期校验）。不破坏 ABI：不改变任何公开结构体布局，也不新增导出符号。运行时行为不变；构建约束收紧——护栏覆盖的布局变更现在会直接编译失败。见 [abi.zh-CN.md](abi.zh-CN.md) §4 与 §8。

### 客户端韧性流水线

- 每端点 rate limiter 与 bulkhead 准入，通过 `builder::per_endpoint_rate()` / `builder::per_endpoint_bulkhead()` 配置，可用 `builder::resilience_enabled(false)` 整体关闭。
- 实际执行序为 `select + allow -> probe 判定 -> rate -> bulkhead -> register -> pool/slot -> retry -> deadline`；半开探针跳过 rate 与 bulkhead，连接池与在途上限仍生效。
- 本地 gate 拒绝会忽略 breaker permit、携带 limiter/bulkhead 的 `retry_after`，且**不自动重试**。
- 远端 `retry_after` 是重试延迟下界（`max(backoff, hint)`），由调用 deadline 封顶；retry 预算在该判定之后才扣减。`retries_delayed_by_hint` 仅统计延迟确实由 hint 决定的那些重试。
- 选择循环用每端点位图跟踪已试候选，超过 512 个端点时自动扩容，不再静默忽略高位索引。
- `call::cancel()` 立即中断重试等待（plain-thread 用条件变量，分片用 5 ms 分片），并与完成回调注册串行化对低层调用句柄的访问。

### 准入阶段与 HTTP Retry-After

- 新增头文件 `crpc/resilience/admission.hpp`（header-only），把准入（admission）词汇集中到一处：`resilience::stage_kind`（10 个阶段）、`resilience::admission_decision`（`admit`、`reject`、`probe_bypass`；`probe_bypass` 为保留值，尚无阶段消费）、`reject_code(stage_kind, bool deadline_met = true)`——唯一的「阶段到 `rpc_code`」表，包含 rate 阶段的两种结果（deadline 仍可达时为 `resource_exhausted`，否则为 `deadline_exceeded`）——以及 `fallback_eligible(rpc_code, stage_kind, bool one_way)`；它只回答某个 (code, stage) 对是否可降级，fallback 的实际执行尚未实现。
- 本切片转换的 breaker、服务/端点 rate 与 bulkhead、服务容量、deadline 与预发送 dispatch 拒绝分支现已从该表取线上码；每连接洪泛桶、每连接在途上限、客户端连接池容量与「cannot track endpoint candidates」分支仍保留字面量。对已转换分支，码值不变，消息、`retryable`、`retry_after` 与各阶段计数器也不变：这一部分是零行为变化的重构。
- HTTP MVC 错误响应在 `rpc_error::retry_after > 0` 时新增 `Retry-After`：毫秒向上取整为整秒、下限为 1，hint 为 0 时不写该头。状态码映射不变。
- 影响：MINOR（新增头文件与内联 API；不新增导出符号）。不破坏 ABI：header-only，`stage_kind`/`admission_decision` 不是跨 ABI 类型，既有记录布局不变。行为变化仅限新增的 HTTP `Retry-After` 响应头；RPC 线上码与消息不变。

### HTTP 长轮询

- 服务器自有 housekeeping 定时器在之后没有 poll 到达时也会清理空闲订阅；由
  `start()` 装配、`stop()` 在拆 hub 之前取消，并由回调自行重新装配。
- `start()` 把前端重放上限收敛到队列上限（`max_replay < max_messages` 会被抬高，
  反之被拒绝）。条数上限与每订阅字节预算相互独立；游标缺口超过任一上限时返回
  409 Restart，而不是静默丢弃未确认数据。`http_statistics::longpoll_replay_window`
  报告生效上限，服务器未运行（含 `start()` 失败）时为 0。
- `stop()` 保留监听配置，之后 `start()` 可重新绑定同样的端点。保存的
  `backend_factory` 指针是借用的：backend 必须活过整个 server 生命周期，包括每次
  `stop()` -> `start()`（已写在 `http_server::builder::add_listener()` 与
  `server::builder::add_listener()` 上）。同时修复 `rpc::server`：其第二次 `start()`
  过去会返回 ok 却不绑定任何监听。

### 异常与编译器特性契约

- 在受支持的无异常消费方配置下（预编译的异常启用库、`CRPC_HAVE_RTTI=0`），52 个公共头可在关闭异常时编译（`-fno-exceptions`、MSVC `/EHs-c-`）；带反射依赖（`CRPC_RTTI_DIR`）且关闭异常时，头阶段仍会失败（见 [building.zh-CN.md](building.zh-CN.md) §3）。以无异常方式重建库仍不受支持：`src/` 的 46 个翻译单元中，Linux GCC 有 11 个失败、MinGW GCC 12 个、MSVC 13 个。
- 异常契约按边界分别说明。头内联边界（`obs::interceptor_chain::run`、`mvc::invoke_http`）跟随消费方的异常选项；启用异常时抛出的异常会被捕获并转为 `rpc_code::internal` / HTTP 500 `internal error`。库翻译单元边界（`mvc::router::handle`、`rpc::dispatcher::dispatch`）跟随库的选项；以异常构建的库会捕获抛出的 handler 并转为 500/`internal`。无异常支持时，抛出不在本库契约内，实践中会 terminate。见 [architecture.zh-CN.md](architecture.zh-CN.md#s15)。
- 新增宏 `CRPC_CAN_USE_TYPEID`：由编译器派生（`__cpp_rtti`；非 clang-cl 的 MSVC 恒为 `1`），外部定义即触发硬 `#error`。所有 `typeid` 使用都在该宏之后；编译器 RTTI 不可用时，type-erased codec 路径返回 `failed_precondition`（HTTP 409），而不是构造 `std::type_info`。`-fno-rtti` 支持仍然受限：启用反射依赖（`CRPC_RTTI_DIR`）时构建会死在依赖内部，该兜底分支目前在任何已文档化配置下都不可达；保留它是为了将来的工具链。见 [building.zh-CN.md](building.zh-CN.md) §3。
- 影响：MINOR（新增宏与新的消费方配置：无异常、`CRPC_HAVE_RTTI=0`）。不破坏 ABI：无布局或符号变更；该宏只在编译期选择内联分支。行为变化：消费方现在可以用无异常方式编译自己的翻译单元；在头内联边界（`run`、`invoke_http`）上，抛出的 hook 或 handler 会离开调用点而不是被捕获；库翻译单元边界仍跟随库的选项。

### ABI breaking：`http_statistics` 布局

- 相对已发布的 1.0.0（`bd006b2`），`http_statistics` 在本未发布周期新增了 19 个
  字段：整个长轮询块（`longpoll_waiters`、`longpoll_subscribers`、
  `longpoll_replay_window`、`longpoll_events`、`longpoll_timeouts`、
  `longpoll_ack_clamped`、`longpoll_conflicts`、`longpoll_restarts`、
  `longpoll_overloaded`）以及 10 个 SSE/压缩/下载计数器。`sizeof` 从 72（9 字段）
  增长到 224（28 字段）；1.0.0 发布的 9 个字段偏移保持不变（0 … 64）。公开
  结构体新增字段属于 MAJOR 变更（[abi.zh-CN.md](abi.zh-CN.md) §1/§4）。
- 在该周期内部，`longpoll_replay_window` 最初放在 offset 168，后来被移到末尾
  （offset 216）；原先排在它后面的 6 个计数器各前移 8 字节
  （`longpoll_events` 176→168 … `longpoll_overloaded` 216→208）。移动前后
  `sizeof` 都是 224，因为移动前结构体尾部同样没有填充。没有任何已发布头文件
  使用过 offset 168 的布局（无 tag、无 1.0.1），因此该内部移动不构成对已发布
  消费者的破坏；真正破坏兼容的是 72 → 224 的增长。

### 行为变更：COMPRESSED 帧不再被当作明文交付

帧级压缩现已实现 LZ4 block 编解码（`ALGO = 1`），其余情况一律 fail-closed。此前行为不安全：
`encode_frame` 接受 `COMPRESSED` 标志却写出未压缩 payload；`decode_frame` 与 `frame_decoder`
把压缩字节当作 payload 返回；任何算法 id 都被接受。`COMPRESSED` 的 8 字节 `PING` 甚至会被
原样回送为明文 `PONG`。

变更内容：

- `encode_frame` 对 `COMPRESSED` 标志或非零 `ALGO` 返回 `invalid_argument`；压缩帧只能由
  `encode_frame_compressed` 产生。
- `encode_frame_compressed` 在写出任何字节之前执行与 `encode_frame` 相同的帧头校验
  （已知类型、stream 作用域、`stream_id` bit 31、`ONE_WAY`/`UNRELIABLE`、`ext`/`CRC32C`、
  `payload_len <= max_frame`），并新增 `max_frame` 形参；违规时在压缩分支与 identity 分支
  都返回 `invalid_argument` 且不写入任何字节。未设 `CRC32C` 时的非零 `ext` 现在被拒绝，
  而不是被静默置 0；设了 `CRC32C` 时该字段按设计存放 payload 校验和（明文与压缩形式都一样）。
- `decode_frame` 对 `COMPRESSED` 帧返回 `protocol_error`（它没有输出缓冲）；`frame_decoder`
  在拷贝模式下解压、借用模式下拒绝。解码后的帧头被规范化：清 `COMPRESSED`/`ALGO`，
  `payload_len` 与 `CRC32C` 改写为逻辑 payload 的对应值。
- zstd（2）与 gzip（3）在本构建中是被识别但 fail-closed 的 id；`4`-`7` 直接拒绝。
- `COMPRESSED` 只允许出现在 `REQUEST`、`RESPONSE`、`DATA`、`ONE_WAY`、`PUSH` 上；
  控制帧（含 `PUSH_ACK`）带 `COMPRESSED` 是 `protocol_error`。
- `ONE_WAY` 帧上的畸形压缩 payload 属于显式丢弃：解码器返回 `result::frame_dropped`，
  `error()` 保持 `ok`，原因经 `frame_compression_info::drop_reason` 报告。判据是帧类型而非
  `UNRELIABLE` 标志，因此对端无法把 `REQUEST` 上的致命违规降级为静默丢帧。
- `frame_decoder::result` 末尾追加 `frame_dropped`（既有值编号不变）。对 `result` 做无 `default`
  的 `switch` 的代码必须补上该分支：GCC/Clang 会报 `-Werror=switch`，MSVC `/W4` 不告警。

兼容性：生产路径不写 `COMPRESSED` 帧，两处生产解码器都是借用模式，因此既有对端行为不变。
手工构造压缩帧的对端现在会得到协议错误，而不是被静默误交付——这正是有意的收紧。详见
`docs/protocol.md` §24。

## 1.0.0 - 2026-10-19

CRPC 首个正式版本。本版本覆盖核心原语、CRPC 调用/服务器门面，以及
HTTP/1.1、HTTP/2 与 HTTPS 前门。这不表示所有设计目标均已达成；实测差距见“已知限制”。

### 主要能力

- **传输后端**：进程内、边缘触发 epoll 与 IOCP 三种后端，统一实现
  `source`/`sink` 两接口契约，提供基于额度的背压与恰好一次的完成回调。
- **协程与分片**：`runtime` 分片原语（corocc LOOP 运行时、有界跨分片 inbox、
  有界 handler 就绪队列、64 个 handler 协程池、单调时钟定时器轮），以及
  `client_facade` 的协程调用写法；corocc 缺失时提供 nofiber 回退。
- **协议**：多路复用 CRPC 二进制帧、crpc-binary 编解码器、类型化派发器与服务注册表。
- **HTTP/1.1 与 HTTP/2**：消息解析/序列化、分块传输、HPACK、h2c 与 h2、
  HTTP 服务器门面、keep-alive 与请求上限。
- **HTTPS**：基于 OpenSSL 的 TLS 装饰器与 ALPN，由 `CRPC_WITH_OPENSSL` 启用。
- **MVC**：attribute 声明的路由、DTO 绑定与响应映射。
- **序列化**：crpc-binary，以及 RTTI 驱动的 JSON、XML、INI、TOML 与 schema 适配器。
- **推送原语**：主题、订阅、有界溢出策略、可靠重放与流式收发形状。
- **容灾原语**：熔断、限流、舱壁、负载均衡与重试预算；其中负载均衡、重试与熔断
  已接入客户端门面。
- **可观测性**：W3C 追踪上下文、拦截器原语、结构化异步日志与有界 tracer；
  AOP 尚未接入调用路径。
- **文件与压缩**：multipart 解析、上传落盘与 Range 下载，以及 HTTP gzip/deflate
  流式内容编码原语（尚未接入 HTTP 服务器路径）。

### 本轮修复摘要

- **借用 source 的生命周期契约**：借用的 source 由 `bound_guard` 跟踪，
  连接不会向正在销毁的 source 投递数据。
- **server/client 解绑**：`server` 与 `client_facade` 在销毁借用的 source 前
  解绑并等待在途投递结束。
- **定时器 keep-alive**：定时器回调持有 keep-alive 引用，避免弹出/停机竞态中
  回调访问已释放的上下文。

### 验证状态

- MSVC x64 Debug 与 x86 Debug：CTest 26/26 套件通过，零告警。
- TSan：26 套件，0 竞争（nofiber/Linux 配置）。
- ASan/LSan：26 套件，0 消毒器报告（nofiber/Linux 配置）。
- 独立双审：在审计基线上会签通过，无未关闭的 P0/P1 缺陷。
- 性能证据与实测差距见 [performance.zh-CN.md](performance.zh-CN.md)。

### 已知限制

- 5M RPC/s 聚合吞吐目标未达：最佳实测 0.275M RPC/s
  （进程内、MSVC、流水线深度 64）。
- IOCP 回环 p99 为 170.2 us，高于 100 us 目标。
- call slot 为 440 字节，高于 128 字节预算。
- IOCP 仅支持 Windows，未做 Linux 运行时验证。
- 跨平台与跨字节序验证（ARM64、大端、macOS）尚未完成。
- 尚未端到端完成：`PUSH`/`PUSH_ACK` 与 HTTP SSE/长轮询、HTTP 压缩与下载接入
  服务器路径、服务端幂等去重表、注册中心服务化。
