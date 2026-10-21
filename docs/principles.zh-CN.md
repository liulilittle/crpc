# CRPC 工程原则

[English](principles.md) | **简体中文**

版本 1.0 / 2026-10-19

本文列出实现必须遵守的规则、每条规则禁止什么、代码如何强制执行，以及如何验证。
执行机制缺失的规则会被明确标注，而不会描述成已完成。

## 目录

| # | 章节 |
|---|---|
| [1](#s1) | 三条底线 |
| [2](#s2) | 分布式定律 |
| [3](#s3) | AP 优先于 C |
| [4](#s4) | 不停摆（no-stall） |
| [5](#s5) | 因果优先，而非墙钟 |
| [6](#s6) | 恰好一次 |
| [7](#s7) | 控制论 |
| [8](#s8) | 显式优于隐式 |
| [9](#s9) | 执行与验证 |

## 1. 三条底线 <a id="s1"></a>

这三条不可协商，优先于任何性能或进度目标。

| 底线 | 禁止什么 | 代码如何执行 | 如何验证 |
|---|---|---|---|
| **绝不死锁** | 持锁时执行 I/O、用户回调、协程挂起、`sem_post`、日志 sink 或阻塞分配；跨分片写；每条连接多于一个读者/写者；lane 等待自身 | 文档化顺序：`lease_manager` 互斥量先于 `registry` 互斥量；registry sink 在写互斥量释放后运行；连接回调串行；`call_slot` 状态用 CAS；熔断器原子化；`io` 一读一写 | 关闭竞争、重入 push、四路状态竞争测试；设计中的锁序断言与 TSan 门禁在 CI 中属于**已设计，未实现** |
| **绝不崩溃** | 未校验的外部输入；UB（非对齐访问、溢出、悬垂、数据竞争）；异常跨越协程/ABI 边界；把 `assert` 当运行时检查；无界递归 | `byte_reader`/`byte_writer` 边界检查；帧、HTTP、multipart、信封解码器 fail-closed；用 `result<T>`/`status` 代替异常；`buffer` 使用 `new (std::nothrow)`；编解码深度 32；generation 检查 | 畸形帧/信封测试；HTTP 与 HTTP/2 永不崩溃 fuzz 用例；UBSan/ASan 脚本；独立 fuzz 目标属于**已设计，未实现** |
| **绝不泄露** | 获取后没有恰好一次释放；关闭后仍投递；迟到完成触碰已回收对象；`shared_ptr` 环；无界池 | 固定 `call_slot_pool`，回收时递增 generation；舱壁/租约使用 RAII 许可；`close`/`cancel`/`reset` 幂等；`file_part` 删除临时文件；关闭时 inproc/epoll/IOCP 恰好完成一次在途写 | 槽回收与无泄漏测试；关闭竞争测试；ASan/LSan 运行；要求所有计数归零的 `drain()` 断言属于**已设计，未实现** |

三条底线由不同机制执行，不是单一检查。无法证明无环的路径必须使用带超时的 try-lock
与显式恢复路径；当前代码中没有标记此类路径，因此这条规则是评审义务。

## 2. 分布式定律 <a id="s2"></a>

| # | 定律 | 禁止什么 | CRPC 中的执行 | 验证 |
|---|---|---|---|---|
| 1 | 没有实时全局状态 | 依赖到达顺序；终态取决于哪条消息先到 | 单调 registry `index`；generation 检查的调用槽；watch 快照 + 增量；终态 CAS | 重排注入；四路终态竞争；watch 两序测试 |
| 2 | 半开是常态 | 假设操作要么完成要么未发生 | 幂等键；已写出后的超时对非幂等调用报告 `unknown_side_effect` | `client_timeout_is_terminal_deadline_exceeded`、`client_idempotent_timeout_has_no_unknown_side_effect` |
| 3 | 对等写入必然分叉 | 两个写者无权威地修改同一状态 | registry 只接受属主 `node_id` 或管理 token；incarnation 与 fence 检查；客户端单写者 | `registry_rejects_non_owner_write`、`registry_rejects_stale_incarnation`、`registry_index_and_fence_are_monotonic` |
| 4 | 外部副作用不可撤销 | 回滚已完成的副作用；超时后声称“未执行” | 没有回滚路径；只有幂等时才重试；`unknown_side_effect` 显式存在 | 重试测试；超时语义测试 |
| 5 | 共识是最后手段 | 因多数派不可达而拒绝写入 | registry 是单一权威；保持 AP；明确没有 Raft | `registry_concurrent_snapshots`、权威测试 |
| 6 | 同步共振 | 无界重试或同步重试风暴 | 重试预算（默认流量的 10%）、最多 3 次、抖动、指数上限 | `retry_budget_exhaustion_stops_retries`、`retry_jitter_within_bounds` |
| 7 | 单调锚点 | 用随机 UUID 或时钟做去重/排序 | `call_id` = `(client_id, epoch, seq)`；registry `index`；`lane_seq`；push `seq`/`topic_seq` | `call_id` 编解码测试；registry 与 push 序列测试 |
| 8 | 规则堆积 | 对未知状态进行猜测 | 解码器 fail-closed；调用、流、h2、watch 都有显式状态机 | 畸形帧/信封测试；非法转换测试 |
| 9 | 非对称安全 | 信任不可信方向 | 每个外部字节都校验；未知类型/codec/标志/版本被拒绝；HTTP 走私防御 | 帧/HTTP/HTTP2 fail-closed 测试 |
| 10 | 事件先于权威数据 | 在快照之前或缺少缺口检查时应用增量 | watch 状态 index `S`：`E <= S` 丢弃，`E == S+1` 应用，`E > S+1` 重新同步 | `watch_gap_triggers_resync`、`watch_drops_duplicate_and_old_index` |
| 11 | NULL 不能穿透 | 用“不存在”表达删除；过期注册复活墓碑 | 墓碑；墓碑拒绝所有不是严格更大的 incarnation | `registry_tombstone_blocks_old_incarnation`、watch 墓碑测试 |
| 12 | 因果性 | 用墙钟排序跨节点事件 | 所有控制决策使用单调时钟；trace 父子来自发送/接收对；registry index | `tracer_parent_child_relation`；流控时钟测试；截止时间缺陷见 §5 |
| 13 | 端到端论证 | 把中间层 ACK 当作正确性 | 幂等键到达端点；TCP 帧只是传输，不是业务保证 | 信封携带键；服务端去重属于**已设计，未实现** |
| 14 | 观测会干扰 | 在转发路径上因日志或指标阻塞 | 日志进入每线程环；追踪使用有界队列；钩子冻结，空链开销低 | 日志/追踪分配与溢出测试；AOP 尚未接到调用路径 |

## 3. AP 优先于 C <a id="s3"></a>

**禁止：** 因多数派不可达而拒绝写入；让数据面依赖 Raft；权威消失时丢失最后可读快照。

**执行：** registry 只通过单一权威路径接受写入，但该路径是本地的，不等待多数派。
读者加载最后一个不可变快照（fail-static），写入则 fail-closed。冲突由
incarnation 与 fence 解决，而不是到达顺序。数据路径上没有 Raft/PreVote
代码；`cluster::election` 是独立原语。墓碑不能被更老的 incarnation
复活。

**验证：** `registry_concurrent_snapshots`、非属主/过期
incarnation 测试、租约到期测试。分区多节点部署尚无法测试，因为网络 registry
服务属于**已设计，未实现**。

## 4. 不停摆（no-stall） <a id="s4"></a>

**禁止：** 连接或流悄悄停止投递；在零窗口、耗尽预算或空池上无限等待；丢弃字节却不计数；只依赖
TCP FIN 作为关闭信号。

**执行：** 进程内后端对放不下的 push 尾部延迟完成，在排空腾出空间后完成；
绝不因背压失败或丢弃。epoll 写循环在字节预算耗尽时自投递（R4）。IOCP
对短写重新投递剩余部分，并把取消当作一次完成。客户端对超过
`max_pending_write_bytes` 的帧返回 `resource_exhausted`，
而不是阻塞。推送队列有字节/消息上限，每次溢出都计数；慢订阅者被断开并产生可观察事件。HTTP/2
`send_data` 停车字节并暴露 `parked_streams()`，使属主可以设置零窗口探测。

**缺口：** 周期性零窗口探测、keepalive/停滞检测器，
以及全局“连接没有进展”定时器属于**已设计，未实现**。h2 层文档要求属主设置探测；目前没有属主。
因此 no-stall 在队列与写路径层面成立，尚未端到端成立。

**验证：** `inproc_bounded_queue_defers_completion`、
`epoll_one_mib_echo_small_queue_bounded_wakes`、
`iocp_large_transfer_small_queue`、
`no_stall_writer_queue_rejects_bounded`、
`hub_publisher_never_blocks`、
`hub_slow_subscriber_disconnected`。

## 5. 因果优先，而非墙钟 <a id="s5"></a>

**禁止：** 用 `system_clock` 排序跨节点事件；从 `time.Now()` 派生
fence；使用墙钟 RTT；把时间戳当作 happens-before 边。

**执行：** registry fence 来自单调 index；租约、健康、熔断、
限流与流控使用注入的单调时钟；`flow::rtt_tracker` 拒绝倒退时间；trace
父子来自发送/接收对，而不是时间戳；`obs::deadline` 是相对时长。

**已知缺陷：** `rpc::client` 把绝对的 `steady_clock` 值写入
`deadline_ns`，而设计要求相对 `remaining_ns`。这是跨进程调用的因果性违规，
记录在[协议文档](protocol.zh-CN.md#s9)。在修复之前，
不得声称支持多进程截止时间传播。

**验证：** `deadline_is_relative`（可观测性）、
`tracker_rejects_backwards_time`、
`tracer_parent_child_relation`，以及 registry
index/fence 测试。整条调用路径的时钟回拨测试属于**已设计，未实现**。

## 6. 恰好一次 <a id="s6"></a>

恰好一次定义为**至少一次投递加上端点处的单调去重锚点**。中间层 ACK 是优化，绝不是正确性边界。

**禁止：** 用随机 UUID 作为幂等键；用墙钟驱逐去重表；没有键却重试非幂等调用；
超时后声称“未执行”。

**当前执行：** 客户端分配 R6 三元组
`(client_id[16], client_epoch, seq)`，重试时复用，并写入调用信封。
`call_context` 跟踪 `idempotent` 与
`has_idempotency_key`。dispatcher 把键解码进上下文。
已写出且非幂等的调用超时后标记 `unknown_side_effect`。`retry`
在调用非幂等时拒绝 `unknown_side_effect`。

**缺口：** 服务端去重表（fresh/in-progress/done、每客户端环、FIFO
驱逐）属于**已设计，未实现**。因此恰好一次尚不是 CRPC 部署的性质；目前只有键传输与超时语义。

**验证：**
`client_idempotent_timeout_has_no_unknown_side_effect`、`retry_unknown_side_effect_requires_idempotency`，
以及 `call_id` 编解码测试。执行一次 handler 的重放测试属于**已设计，未实现**，
因为没有去重表可测。

## 7. 控制论 <a id="s7"></a>

流控设计遵循五条来自反馈控制的规则，而不是教科书模型。

1. **不追求“知道”，追求“逼近”。** RTT 基线以小步、带门控的方式移动：向下立即，向上几何且有界。
   由 `flow::rtt_estimator` 执行；由
   `estimator_downward_is_immediate`、
   `estimator_upward_is_bounded_and_capped_at_observation` 验证。
2. **过冲购买信息，无法消除，只能设界。** 探测是强制的，但代价由预算约束。由 pacer
   的有界突发与拥塞窗口的自时钟增长执行；由
   `pacer_take_is_bounded_by_credit`、
   `window_slow_start_grows_per_ack` 验证。
3. **滞后是原罪。** 每个样本描述的是过去的路径。tracker 记录观测延迟并拒绝过期样本，
   而不是假装补偿。由 `rtt_tracker` 执行；由
   `tracker_rejects_stale_token` 验证。
4. **不存在最优步长。** 选择“足够好”的默认值（向上 12.2%、2 ms 突发、拥塞后保留 70%
   窗口）并接受权衡。由 `flow::config` 执行；由钳制测试验证。
5. **不要发明“检测路径已畅通”的机制。** 设计移除了这种机制，因为它总是检测到畅通并振荡。
   估计器没有畅通预测器；
   路径变化只从持续的方向性证据推断（`estimator_sustained_shift_is_detected_as_path_change`）。

两条推论已实现：一切归零后直接恢复历史高水位，
而不是从冷值重新爬升（`restore_high_water`、
`window_high_water_survives_a_decrease`）；宽度与波动分阶段测量，
而不是同时学习两者。不承诺公平性；唯一保证是没有任何订阅者被悄悄饿死，因此溢出总会计数，
慢订阅者会被报告。

## 8. 显式优于隐式 <a id="s8"></a>

**禁止：** 宿主字节序的线上字段；公开签名中的 `long`/`unsigned long`；
结构体 `memcpy`；位域；非对齐 `reinterpret_cast`；隐藏所有权；跨边界异常；
静默猜测的默认值。

**执行：** 固定宽度 `u8`…`u64`、显式小端读写、带边界检查的读写器、借用视图
`span`、仅移动句柄、回调 `noexcept`、用 `status`/`result<T>`
表达失败，以及对平台不变量（8 位字节、精确整数宽度、IEEE-754）的
`static_assert`。ABI 策略禁止导出签名中出现标准容器与非平凡类型。
缺少编解码特化是编译错误，而不是空负载。

无 C++ 异常的构建中该条款没有 catch 兜底；异常契约见[架构 §15](architecture.zh-CN.md#s15)。

**验证：** `test_endian.cpp`、`test_wire.cpp`、
`test_frame.cpp`、UBSan 对齐检查，以及 ABI 布局断言。
“影响正确性的行为不只存在于内联函数”这条规则写在 ABI 策略中并由评审检查；没有提交自动化 ABI
检查器。

## 9. 执行与验证 <a id="s9"></a>

一条原则只有在失败测试或构建门禁能抓住违反时才算真实。当前门禁：

- **构建：** 零警告（`/W4 /WX`、
  `-Wall -Wextra -Wpedantic -Werror`），线上格式与 ABI 假设的
  `static_assert` 覆盖。
- **单元/功能：** [架构文档](architecture.zh-CN.md#s20)列出的 16
  个套件。
- **消毒器：** Linux/Clang 上的 ASan/LSan 与 UBSan；TSan
  面向尚不存在的分片模型。MSVC 没有 TSan。
- **故障注入：** inproc 丢弃/延迟/重排/写错误/读错误、关闭竞争、四路终态竞争。
- **缺失门禁：** 独立 fuzz 目标、锁序断言、整调用时钟回拨测试、去重重放测试、
  drain/泄漏断言、多节点分区测试。每个缺失门禁都对应本文档其他位置标记为**已设计，
  未实现**的特性。

评审规则很简单：无法说出对应测试、消毒器结果或已记录测量的声明，是设计意图，不是保证。
