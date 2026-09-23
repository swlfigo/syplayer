# 已知缺口

实现过程中审出来、**明确决定暂不修**的项。每条注明为什么现在不修、将来在哪补。

## 1. 异步交付下会发出与在途请求重叠的冗余请求 🟠 影响流量

`Scheduler::occupied_locked()` 把两个来源混用：`received_`（Scheduler 侧，在
`on_task_data` 里更新）和 `task->next_offset()`（DLTask 内部，先一步前进）。
两者之间有一个**恰好一个 chunk 宽**的窗口会被算成"洞"，于是给一段
"在途任务马上就要交付"的字节又发了一个请求。

量化证据：`chunk_size=32` 时重叠的新请求里 49 个尺寸恰好是 32；
改成 `chunk_size=64`，重叠总数从 68 掉到 4、众数尺寸变成 64。
另一类是**异步 cancel 滞后**（已 cancel 但 `on_complete` 未到的 handle），
按 handle 的 canceled 标记分类是 25 条滞后 / 43 条 chunk 窗口。

**不是正确性缺陷**：`received_` 去重仍然挡住了重复交付
（`byte delivered twice = 0`、`offset out of file = 0`）。代价是**白下一个 chunk 的流量**。

⚠️ 但这是"绝不重下已有数据"这条不变量在异步路径上的真实缺口，
而省流量是本层的卖点之一，**M2 接真实播放前应当处理**。

另注：现有的 tracing 不变量检查**只在同步泵下成立**。要接进异步用例，
得先把不变量放宽（容忍 cancel 滞后 + 一个 chunk 的交付窗口），否则会误报红。

**端到端实测（M1 步骤 7）**：顺读到尾的场景下冗余下载占比为 0%
（faststart 0%，moov-at-end 0%）。素材约 47.97 MB（50294788 字节，两份
fixture 恰好同一大小），配置为默认值（`max_concurrent_tasks=3`，
`min_segment_size=512KiB`）。验证方式见 `tests/test_probe_e2e.cpp` 的
`sequential_read_matches()`——`CHECK_EQ(downloaded_bytes, file_size)`
是恒等断言，33 次独立运行（场景 A、B 各 33 次，共 66 次）无一例外。

**这是实测观察，不是结构性保证**，措辞不能过头：本条描述的竞态在
Scheduler 内部（`received_` 与 `task->next_offset()` 不同步），与消费者
是顺序读还是随机读无关；这条场景走的是 `syp_config_init()` 的默认配置，
`max_concurrent_tasks=3` 从未被覆盖，本身就是真并发，不能靠"顺序读"
把自己摘出适用范围。33 次未触发，机制上不能排除，需持续关注。

## 2. `Scheduler::in_schedule_` 没有 RAII 守卫 🟡 影响健壮性

异常若逃出 `schedule()`（`bad_alloc`、或用户回调在 `emit()` 里抛），
`in_schedule_` 会永远停在 1，析构就**永久等待**（每 2 秒往 stderr 打一条诊断）。
工程默认开异常。加一个 scope-exit guard 即可。

## 3. `segment_size_hint` 的公共文档与实际行为不一致 🟡 影响 API 契约

公开头里 `segment_size_hint` 只写"期望分片大小，0 = 自动"，
但实现里有一条 `kNoSplitBelow = 256` 的切分粒度下限压过 hint，
**取值落在 (0, 256) 会被静默忽略**。

落地前要么在公开头补一句说明，要么把下限做成可配置。

## 4. 碎片洞仍会产生 1 字节请求 ✅ 有意保留

`kNoSplitBelow` 管的是**切分粒度**，管不到"碎片洞本身就小于下限"这一路 ——
seek 取消任务会把缓存打碎，留下 <256B 甚至 1B 的单个洞，填它只能发等大的请求
（多取会违反"绝不重下已有数据"，代价更大）。

实测：随机配置 400 轮下 1 字节请求从 182 个降到 5 个，几何衰减
（… 9 7 5 4 3 2 2 1）已被刹在 256，但没绝迹。**这是有意的取舍，不是缺陷。**

## 5. 随机化回归的网眼 🟢 技术债

`test_scheduler` 的随机化交叉验证目前 Debug 8×50 / Release 8×100。
审计时用的是 8×150。实测提到 8×100 的代价只有 Debug +0.27s、TSan +2.6s，
下次顺手提上去。

## 6. macOS 上 ASan 查不了内存泄漏 🟢 环境限制

`detect_leaks` 在本平台不支持，所以历次"ASan 零报告"只覆盖越界 / UAF / UB，
**堆泄漏是未验证的**。Apple 后端那部分用 `leaks --atExit` 兜过一次（零泄漏），
dl 层没有。要真查得上 Linux 或 valgrind。

## 7. 超时判定的归属 🟡 契约边界

`DLTask` 自己不计时，`connect_timeout_ms` / `read_timeout_ms` 只是原样填进
`syp_http_request`，**超时判定与上报是后端的责任**。
后端不守约（永不回调）时任务不会自行超时——~~这是已知边界，将来由坏任务检测兜底~~
**已由 M6c 兜底**（2026-09-22）：`Scheduler` 的健康检查（进程单例 `HealthTicker`
每 `check_interval_ms` 一次）对 `Connecting`/`Receiving` 的尝试按
`max(本次尝试起点, 最近进展) + 对应超时 + stall_grace_ms` 判挂死，cancel 并从
`next_offset` 重发，按一次 `SYP_ERR_TIMEOUT` 计错（见 `src/dl/scheduler.h`
`HealthConfig`）。剩下的边界：后端对 **cancel 也不回调**时对象仍收不回（#103）。

Apple 后端用每 Handle 一条 GCD 定时器实现，但它是"到首字节"超时而非"建连"超时：
服务端接受了 TCP 连接却迟迟不回响应头，仍会算作 connect 超时。

## 8. 缓存索引与内容文件的一致性 🟡 只挡得住截断

`CacheIndex::open()` 会 stat 内容文件、把超出实际大小的区间裁掉，
但**挡不住文件中间被打洞**（大小没变、中间读出来是零），
也挡不住 **open 之后**才被外部截断（TOCTOU）。

彻底解决要么每段存校验和，要么用 `SEEK_DATA` / `SEEK_HOLE` 探测真实数据分布。

## 9. `syp_source_stats.completed_tasks` 恒为 0 🟢 缺钩子

`SourceBridge::completed_tasks_` 只读不写。Scheduler 没有任务完成计数
回调，SourceBridge 无从知道一条 DLTask 正常结束。不是缺逻辑，是缺钩子。

将来在 Scheduler 的 `on_task_finished` 成功路径加一条完成计数，或给
`SchedulerCallbacks` 补 `on_task_done`，再由 SourceBridge 累加。

## 10. `save_index_locked()` 持 `mu_` 刷盘 🟡 影响并发

`save_index_locked()` 在持 `SourceBridge::mu_` 时做序列化 + write +
fsync（数据文件）+ fsync + rename（`cache_index.cpp` 的 `save()`）。
整个磁盘刷写期间阻塞所有并发 read / seek / cached_ranges。

现在不修：256KB 一批已经把 fsync 次数压下来了，再拆锁要处理
`index_` 与 `file_` 的生命周期和「刷盘失败时 dirty 位」的并发。
将来把序列化/fsync 移到锁外，锁内只交换待写快照。

## 11. 总长真未知时 read 会一直阻塞 🟡 Scheduler 既有缺口

**只影响本会话从未解析出总长、索引里也没有总长的条目。**

200 响应且无 Content-Length（或 206 缺 Content-Range，Apple 后端这两种
都会把 `total_length = -1` 传上来）时，`SourceBridge::total_length_`
保持 -1。read 走到真实 EOF 既不返回 `SYP_ERR_EOF` 也不报错，会一直
阻塞；调度器对残留的洞反复重开任务。

这是 Scheduler 层的既有缺口：对「无 Range、无总长、body 自然结束」
还不认 EOF。SourceBridge 打开已有条目时会把索引里的 `total_length`
抄进 `total_length_`，用它夹 `target_end` 和 EOF，所以**曾经拿到过
总长的续下会话碰不到这条**——read 能在索引总长处返回 EOF，本会话
响应即使再解析不出总长也不影响。

曾经用 `need_total_` 把门禁写成「索引有总长就必须再等本会话
`on_total`」；那会把「索引有总长、本会话解析不出」变成永久阻塞
（字节全进 `pending_`，一个都不落盘）。那条门禁已经去掉。

长度校验不是无条件的：
- 仅当本会话**某次响应解析得出总长**时才走 `on_total` →
  `validate_and_update`（同一条 DLTask 的 `on_response` 先于 `on_data`）。
- 解析不出总长的会话（无 Content-Length / 无 Content-Range）**不做任何
  长度校验**。身份门开了之后字节在零长度校验下落盘。这正是
  `resume_without_session_total_still_reads` 构造的场景。
- 只有**首个**响应的总长会被校验。Scheduler 用 `total_notified_` 把后续
  任务响应里的总长丢掉，不会再到达 SourceBridge。源在后续任务上变长/
  变短，本会话发现不了。`total_notified_` 是去重抑制器，不是保护器。

将来在 Scheduler 对「无 Range、无总长、body 自然结束」认 EOF，再
通过 `on_total_length` / 终态把真实长度回写给 SourceBridge。

**未纳入 M1 步骤 7 的验证矩阵**：构造「无 Content-Length」的场景会挂死，
纳入等于故意制造一条红。待本条修复后补入 `tests/test_probe_e2e.cpp`。

## 12. 完整缓存且不再发请求时不会主动重校验 etag 🟢 语义取舍

头文件没要求打开已完整缓存的条目时再发条件 GET。当前实现：窗口内
没有洞就不发请求，自然也看不到新 etag。源在完整缓存之后变了，会
一直命中旧字节。

这是明确的取舍，不是漏实现。将来要补的话位置在 `SourceBridge::open()`
里对已有 etag 的条目发一个条件 GET（If-None-Match）；304 继续用缓存，
200 走 `SYP_ERR_CONTENT_CHANGED` 或作废重建。

## 13. cancel-after-complete 是对后端的隐含新要求 ✅ 已解决

B3 的 pin 修法新开了一个窗口：`on_complete` 已经回调、句柄尚未
destroy 时，另一线程的 `DLTask::cancel()` 仍能拿到非空 `handle_` 并
调用 `backend_->cancel`。修前 `sink_on_complete` 无条件把 `handle_`
置空，第二条 `cancel()` 走空分支。

`syp_http.h` 只写了「cancel 可在 start 之前调用」，没说
cancel-after-complete 合法；同时又要求「cancel 之后必须回调恰好一次
on_complete」。严格的后端会把这次 cancel 当成新的取消、补发第二次
on_complete。

DL 层能容（`finished_emitted_` 吞掉第二次），但这是对后端的隐含新要求。

**已解决**：`docs/2026-09-08-apple-backend-deadlock.md` 诊断的那条自
死锁是同一片区域（cancel 与 complete 的竞争）里的另一个缺陷，修复它时
顺带把 `syp_http.h` 里生命周期重入规则一次性补全了三条（`cancel`
声明附近），其中第一条就是本条要求的那句：cancel 在 on_complete 之后、
destroy 之前必须是 no-op，不得再回调。见 `include/syplayer/syp_http.h`
`cancel`/`destroy` 声明附近的注释。

## 14. `close()` 排空窗口丢弃已收到但未落盘的在途字节 🟢 技术债

`SourceBridge::close()` 把 `closing_ = true` 立在 `s->stop()` **之前**；
`on_data()`/`persist_chunk()` 的第一道闸都是
`if (closed_ || closing_ || ...) return;`。这两行之间（以及 `stop()`
真正排空在途请求之前）到达的字节会被整个丢弃——不写盘、不入索引、
也不计入 `downloaded_bytes_`。这是**代码确证**的成因，不是靠概率反推：
`add_range` 与 `downloaded_bytes_` 在 `persist_chunk` 里是同一把锁下的
相邻两行，永远同步；真正的缺口是"字节到达"与"`closing_` 生效"之间的
竞态，不是统计口径的问题。

**一个决定严重度的细节**：`persist_chunk` 在 `write_at` **之后**还有
第二道 `closing_` 闸，所以存在「字节已经写进文件、但没能进索引」的
中间态——后果只是这次会话少缓存一块（下次重新按洞补），**不会把
已经写坏的数据当成合法字节读出去**。这正是把严重度定成 🟢 而不是
🟡/🔴 的理由。

**观测记录（如实写，既不夸大也不隐瞒）**：90 次运行中 2 次（约 2%，
95% CI 约 0.3%~8%）观测到"两趟下载字节之和略少于文件大小"的现象
（幅度 28672~442552 字节）。2 次全部出现在同一批（旧版场景 D、缓存量
约三倍）的 20 次里，其后 70 次未复现。**两批负载不同，不可直接合并
统计**；70 次无事件只能把发生率上界压到约 4%（rule of three），
**不构成对机制的否证**——机制本身已经由代码阅读证实，不确定的只是
"当初那 2 次是否确实由它造成"。

本轮文档收尾（task-10）自己的验证过程中又独立复现过一次：25 次连跑
`ctest` 全量矩阵，1 次出现 `sum=50229252` 对 `second.metrics.cached_bytes
=50294788`（=文件大小，即索引侧已经"满"，但两趟 `downloaded_bytes` 之
和欠了恰好 65536 字节，即一个典型读缓冲区大小），方向与既有观测一致。

**重压下的新证据（本轮，改断言前的诊断复测）**：`g_restart_resumes_
instead_of_redownloading` 的 `CHECK_EQ(sum, second.metrics.cached_bytes)`
在密集连跑中闪红，为查清是这条 gap 还是 #1（冗余请求）造成，做了一轮
无任何注入的重压连跑：8 核机器上以 `xargs -P` 分三档并发（8/12/16，
刻意超订阅制造调度压力）背靠背连续跑 `test_probe_e2e` 整个二进制
（含全部 9 个场景）共 460 次，`g_restart_resumes_instead_of_redownloading`
单场景闪红 8 次（约 1.74%，95% CI 约 0.8%~3.4%）。**方向 8/8 全部一致**：
`sum < cached`（`downloaded_bytes` 之和比 `cached_bytes` 少），没有一次
反方向——按"`sum > cached` 是冗余重复请求（#1）、`sum < cached` 才是字节
进了索引却没被计数（本条 #14）"的判据，8 次全部指向本条，不是 #1。
偏差幅度 8192~524288 字节（此前两批观测分别是 28672~442552 与
65536 整）——数量级一致，本轮把上界从约 44 万字节推高到约 52 万字节。
测试侧已经改成非对称容差：`sum <= cached`（严格，不吃任何容差，专门
守住 #1 那个方向）+ `sum >= cached - 1MiB`（只在本条 gap 的方向放宽，
1MiB 是实测最大偏差 524288 字节的 2 倍余量）。反向自检（`already` 参数
临时改传空 `HoleSet{}`，模拟"续下失效、每次重下全量"）证明改窄容差后
依然能抓住真正的回归：`sum` 比 `cached` 反而多出约 1700 万字节，远超
1MiB 容差，断言如实变红；已还原，`git diff src/dl/` 干净。

**修复方向**：`close()` 里先 `stop()` 把在途请求排空、等它们的
`on_data`/`on_complete` 都跑完，再置 `closing_ = true`——顺序对调。

**判定方式要写明**：继续加跑次数不会有结论；决定性做法是在
`test_source_bridge` 里用内存桩做定向用例，在 `close()` 与在途
`on_data` 之间制造确定交错，断言「落盘字节 + 索引区间」与投递总量
一致。

## 15. ~~Range 降级前会并发发出多份全量请求~~（两半均已修）🟢

契约（`syp_config.h` 的 `allow_no_range_fallback`）承诺「服务端不支持
Range 时退回单连接全量下载」≈1× 文件大小。原始实测（`faststart.mp4`，
`total=50294788`，多次连跑稳定复现）：`bytes_sent≈203370508`，约
**4.04×** 文件大小；`peak_concurrent_requests()==3`。

> **状态（2026-09-09 第二次更新）：两半都已修，本条关闭。**
> 第 2 半（判定丢弃 200 响应体时中止连接）见下面「第 2 半的修复」；
> 第 1 半（探明 Range 支持之前并发锁 1）见「第 1 半的修复」。
> `peak_concurrent_requests()` 从 **4 降到 1**，`requests` 从 5 降到 3，
> 流量串行 1.00\~1.13×。`docs/roadmap.md` 的验收标准 ④ 随之勾上。

成因分两半，**两半都要修才够**（下面这段是当时的诊断，原文保留）：

1. **首轮并发未锁 1**：`max_tasks_locked()` 在 `total_length_ < 0` 时锁
   1，但首个 200 响应头里的 `Content-Length` 一到 `on_total`，并发立刻
   放开到 `max_concurrent_tasks`（默认 3）；而 `no_range_signal` 只在
   `scheduler.cpp:400` 附近某一条任务完成/失败后才判定触发，**必须等
   某条任务把整个 body 流完才触发**——识别之前已经按并发数抢跑发出了
   3 条各自请求 `[0,total)` 的连接。
2. **判定丢弃后不中止连接**（✅ 已修，见下节）：`dl_task.cpp` 的
   `Full200 && wanted_.start > 0 && !allow_no_range_fallback` 分支只置
   `has_fatal_ = true; drop_body_ = true;`，**没有中止请求句柄**
   ——`sink_on_data` 只是把
   字节丢在地上，body 继续在网线上跑完，白付一整份文件的流量。记录里
   `sent=50294788 truncated=0` 的那类响应正是这个：完整下完、没有被
   截断也没有被取消，纯粹是白白吃满了整份文件的流量却被判定为"失败/
   丢弃"。

**修复方向要写两半，只做一半不够**：「首轮并发锁 1 直到探明 Range
支持」**加上**「`drop_body_` 置位时 `backend_->destroy(handle)` 掐断
连接」。只做第一半——即使只发 1 条探测请求，只要那 1 条判定为要丢弃、
不主动掐断连接，依然要付一整份文件的流量，只是从"3 份"降到"1 份"，
没有解决"付了不该付的钱"这个根本问题。

**两半的风险等级不一样，不能混为一谈**：

- 第 1 半（识别 no-range 前并发已放开到 3）是 `Scheduler` 的**协议时序
  改动**——碰的是并发决策的时机，风险判断成立，改动前后都要过一遍
  完整的调度器回归。
- 第 2 半（`dl_task.cpp` 在 `Full200 && !allow_no_range_fallback` 分支
  置 `drop_body_` 后补一句 `backend_->destroy(handle)`）落在一条**已经
  判定致命、响应体已决定丢弃**的路径上——这条路径本来就要终止任务，
  只是没有连带终止底层连接，爆炸半径比第 1 半小一个数量级。而且它
  正是 4.04× 里最大的那块（上面 `sent=50294788 truncated=0` 那几条，
  纯白付一整份文件）。**第 2 半可以独立修复，成本与风险都低得多，
  建议优先做**，不必等第 1 半的协议时序改动一起上。

（注：上面那句"补一句 `backend_->destroy(handle)`"**是错的**，实际
修复用的是 `cancel`——理由见下节第 1 条。原文保留，方便对照当时的判断。）

---

### 第 2 半的修复（2026-09-09）

`sink_on_response` 的那条分支上，锁内把 `handle_` 取出并 `++handle_refs_`
打 pin，锁外调 `backend_->cancel`。改动只有这一处，公开头零改动。

**实测（端到端场景 E，`e_no_range_falls_back`）**：

| | 修复前 | 修复后 |
|---|---|---|
| 流量倍数（串行） | 4.01 / 4.05 / 4.06 / 4.07 / 4.08（n=5） | 1.06\~1.15，中位 1.09（n=30） |
| `requests` | 5 | 3\~4 |
| `peak_concurrent_requests()` | 3 | **3（没变，那是第 1 半）** |

（上表的 3 是当时那份 47.97 MB fixture 上的数字；换成现在的 31 MB
fixture 重测是 4——3 条抢跑 + 1 条 fallback 重叠。两者都 > 1。）

并行连跑下的最坏值（用来给测试上界定余量）：串行 n=12 最坏 1.16、
2 路 n=24 最坏 1.26、4 路 n=48 最坏 1.25、8 路 n=96 最坏 1.55。
（8 路那批里 probe_e2e 的场景 F 本来就 ~90% 红——修复前同样 ~92% 红——
那是这套端到端用例的墙钟性质，与本条改动无关。）

**为什么是 `cancel` 不是 `destroy`**（四条安全性判断，改动落在"回调里
操作句柄"这个刚咬过本项目的形状上，见
`docs/2026-09-08-apple-backend-deadlock.md`）：

1. `syp_http.h` 对 `destroy` 写的是「调用前 dl 层保证已收到
   `on_complete`」——此刻还没有。而且 `handle_` 是 `DLTask` 自己在管的
   生命周期，在响应回调里 destroy 掉正在用的句柄会与
   `sink_on_complete` / `~DLTask` 的 `take_handle_for_destroy_locked()`
   抢释放。`cancel` 才是这个位置的正确动作。
2. **契约覆盖**：`cancel` 写的是「可在任意线程调用，可重入」，没有禁止
   在回调帧内调用；新补的三条重入规则里第 1 条讲的是"complete 之后的
   cancel"、第 2/3 条讲的是 destroy，都不与这个用法冲突。而且
   `test_apple_http_backend.cpp` 的
   `destroy_in_on_complete_after_cancel_in_on_response` 覆盖的正是
   「on_response 内 cancel」这个形状，Apple 后端的 `Handle::cancel()`
   有专门的同线程判据防重入非递归的 `emit_mu_`。**不需要扩公开头的契约。**
3. **锁序**：`backend_->cancel` 在 `mu_` 之外调用（与 `DLTask::cancel()`
   的既有做法一致），不引入新的锁序。pin 挡住 `~DLTask`（它等
   `handle_refs_ == 0`），也让后端可能同步合成的那条 `on_complete` 把
   destroy 推迟到 unpin 之后，不与本帧抢着释放句柄。
4. **恰好一次终态**：`sink_on_complete` 里 `has_fatal_` 分支在
   `user_canceled_` 之后、在按 status 分类之前，所以终态仍是
   `SYP_ERR_RANGE_UNSUPPORTED`（不是 `SYP_ERR_CANCELED`）；
   `user_canceled_` 不置位，调度器侧看到的仍是 `no_range_signal`，降级
   逻辑不变。`finish()` 的 `finished_emitted_` 闸门未动。

中止动作放在 `notify_meta_unlocked()` 之后、函数最末：cancel 之后后端
可能在本帧里就合成出 `on_complete`，那之后不该再拿 `self` 做别的事。

**回归**：`tests/test_dl_task.cpp` 新增两条用例，断的是
`StubBackend::sink_on_data_bytes()`（后端实际交给 sink 的字节，与 DLTask
丢不丢无关）——`http_200_no_fallback_aborts_transfer_at_header`（同步泵，
确定性）与 `http_200_no_fallback_abort_is_safe_from_backend_thread`
（异步 40 轮，自带看门狗，钉住"在后端 worker 线程的 on_response 栈帧里
中止"这个形状）。修复前两条都红在 `left=100000 right=0`。

场景 E 的上界从 `total*(max_concurrent_tasks+2)`（5×）收紧到
`total*(3+max_concurrent_tasks)/3`（2×），份额按上面的并行实测校准，
注释里那句"修复后应收紧到 ~1.2x"已换成真实数字。
（第 1 半修好后这条上界又重推了一次：机制换成"1 条拉完 + 至多 2 条
被 cancel 的残条"，2.0× 在 1040 次里被顶到过一次恰好等号，已改为 2.5×。）

---

### 第 1 半的修复（2026-09-09，提交 `1e6c1c1`）

**关键事实：首个请求本来就带 `Range: bytes=<start>-`。**
`DLTask::issue_request()` 永远填 `range_start` / `range_end`，各后端据此
发 Range 头（`apple_http_backend.mm:538`）。支持 Range 的服务端对
`bytes=0-` 回 **206**，忽略 Range 的回 **200**——**首个响应就分得清，
零额外往返、零性能回归**。原来的缺陷不是"缺信号"，是调度器没去看这个
已经拿到的信号：`no_range_signal` 只在任务**完成**时判定。

所以修复不是"先发一个 `start>0` 的小请求探路"（那要多付一次往返），
而是把 `range_supported_` 从二态（bool，初值 `true`）改成三态：

| 状态 | 触发 | 并发 | 发什么请求 |
|---|---|---|---|
| `Unknown` | 初值 | **1** | 照常按洞发 Range 请求 |
| `Supported` | 见过 **206** | `max_concurrent_tasks` | 同上 |
| `Unsupported` | `SYP_ERR_RANGE_UNSUPPORTED` / `start>0` 的 200 | 1 | 只发 `[0, eof)` |

**`Unknown` 不等于 `Unsupported`**：`start == 0` 的 200 只说明"这次给了
全量"，既可能是不支持 Range，也可能是服务端选择忽略 Range 头。据此就
退化成整份重拉会误伤后一类服务端，所以 `Unknown` 下仍然按洞发正常的
Range 请求，只是并发锁 1。

**最容易踩空的一处**：并发锁 1 之后，如果分片也按"并发=1"切，探测任务的
`wanted` 会覆盖整个目标窗口；`occupied_locked()` 按 `wanted.end` 记账，
`holes_in(tgt)` 从此恒空，**并发永远放不开**（实测过这个中间状态：正常源
退化成全程单连接）。所以另开了 `planned_tasks_locked()`——"切分时假定的
并发数"，与"同时几条连接"分开：探测请求只占第一个分片，其余的洞留给
206 落地后建的任务。配套地，`segment_size_locked()` 的 `remaining` 改为
只减 `cached_`/`received_`、**不减在途**，这样探测前后切出来的分片
**完全一致**——这是"零回归"的必要条件。

**实测（端到端场景 E，fixture seed=54181493，faststart.mp4 31228460 字节）**：

| | 第 2 半修复后 | 两半都修后 |
|---|---|---|
| `peak_concurrent_requests()` | 4 | **1** |
| `requests` | 5 | **3** |
| 流量倍数（串行） | 1.00×（本 fixture） | 1.00 / 1.07 / 1.13× |

16 路并行连跑（这是本仓库认可的最敏感环境，见 README「四套构建全绿
不足以证明并发正确性」）：

| | 修复前 n=400 | 修复后 n=1040 |
|---|---|---|
| `peak_concurrent` | 1/2/3/4 = 80/52/256/12 | 1/2 = 1001/38 |
| `requests` | 3/4/5 = 100/81/219 | 3 = 1039，2 = 1 |
| 流量 min/中位/max | 1.09 / 1.24 / 1.84 | 1.08 / 1.16 / 2.00 |

修复后剩下的 `peak == 2`（38/1040，3.7%）**不是客户端真开了两条连接**：
`backend_->cancel()` 是异步的，socket 关闭与下一条连接建立之间没有全序，
被 cancel 的那条在 `LoopbackServer` 这边（`RequestGuard` 未析构）还能挂
一小会儿。记录里那条重叠的永远是 `early_close=1` 的残条，真正把文件拉完
的那条是 `early_close=0`。**确定性的那一半由单测负责**：
`tests/test_scheduler.cpp` 的 `no_range_source_stays_single_connection`
在同步桩下断 `active_task_count()` 峰值恒为 1，100% 确定。

**正常源（支持 Range）零回归**，`LoopbackConfig::delay_ms=100ms` 模拟
RTT、n=8：

| | 修复前 | 修复后 |
|---|---|---|
| 首 4 MiB 耗时 | 441~571ms（中位 453） | 425~533ms（中位 447） |
| 全文件耗时 | 3022~3411ms（中位 3306） | 2889~3371ms（中位 3119） |
| `peak_concurrent` | 3 | 3 |
| 请求条数（中位） | 85.5 | 81 |

差异全部落在噪声内。首字节延迟本来就不会变：锁 1 时那条任务的起点与
放开并发后第一个分片的起点完全相同，连接 2..N 只是晚一个响应头。

## 16. `max_retries` 的公开契约与实际不符 🟡 影响 API 契约

`source_bridge.cpp` 里
`sc.max_consecutive_errors = clamp_i32_pos(cfg_.max_retries, 5)`，
而 `clamp_i32_pos` 在 `v <= 0` 时返回 fallback 5。用户写
`max_retries = 0`（意图是"我不要重试"）实际拿到的是「单个 DLTask 不
在内部重试，但 Scheduler 仍会为同一个洞另开最多 5 轮新任务重试」——
**语义相反**：调用方以为关掉了重试，实际重试预算反而从"这一条任务"
变成了"最多 5 条新任务接力"。同类于既有的 #3（公共文档与实际行为
不一致）。

这不是猜测——task-7/8/9 的场景 F 反自检时亲手踩过一次：只把
`max_retries` 砍到 0，用例依然是绿的（没有触发预期的失败），排查后
才发现是这条 Scheduler 侧的独立兜底重试在起作用。

将来要么在公开头补一句说明 `max_retries=0` 时 Scheduler 侧兜底重试
的实际行为，要么把 `max_consecutive_errors` 也纳入 `max_retries` 语义
统一管理。

**2026-09-14 补记**：DLTask 这一侧的语义已改成"连续**无进展**的重试次数"
（推进过字节即清零，见 tech-debt 已解决区 `test_probe_e2e` 那条）。
Scheduler 侧 `max_consecutive_errors = max_retries` 这层借用仍未动，本条
描述的不一致依旧成立——而且现在两层对同一个字段的解读更不一样了。

## 17. 验证矩阵的结构性盲区：真实后端 × 真实调度器 从未被交叉覆盖过 🟢 技术债（已在本阶段修复一次真实缺陷）

既有的 155 个 dl 层单测全部走内存桩后端（`stub_backend`）；20 个 Apple
后端用例的共同形状是「主线程 `wait_complete()` 之后再 `destroy`」。
**「真实后端 + 真实调度器」这个交叉点在 M1 步骤 7 之前从未被触达**，
而那里躺着一个单线程自死锁：Apple 后端的三条「取消合成收尾」路径
（`emit_data`/`emit_response`/`emit_redirect`）在调用合成的
`on_complete` **之前**就把 `in_callback_` 清成了 false，导致
`Handle::destroy()` 的 `same_thread` 逃生口没有武装——`destroy()` 在
自己的回调栈上等待自己持有的 `inflight_`，永远等不到。已在本阶段修复
（`ed1da0e`：三处收尾统一抽成 `finish_with_cancel_emit_locked()`，让
`in_callback_`/`callback_thread_` 跨过合成的 `on_complete` 保持有效），
完整诊断见
`docs/2026-09-08-apple-backend-deadlock.md`。

这条记录的不是那个死锁本身（已修），是**测试矩阵设计上的方法论教训**：
「单元测试的两条腿（桩后端 / 真实后端）各自全绿」不等于「它们的交叉
乘积全绿」，端到端验证工具第一次把这两条腿接在一起跑，就在第一个
用例（顺序读）里当场复现，复现率接近 100%。应当引入一个维度：
**destroy 的发起线程 ×（主线程 / on_complete 回调内）**，以及 **cancel
的发起时机 ×（start 前 / connecting / on_response 内 / on_data 内）**，
补进 `test_apple_http_backend.cpp` 作为契约用例（已在 `ed1da0e` 补了
三条确定性回归，覆盖 `on_data`/`on_response`/`on_redirect` 三处收尾，
但矩阵仍不完整——见诊断报告第 7 节的"顺带补矩阵"）。

## 18. Pipeline 在带 B 帧素材上永久活锁丢帧（已修复；结构性测试盲区） 🟢 技术债（已在本阶段修复一次 C-0 缺陷）

**现象**：`Pipeline::step()` 在带 B 帧的 H.264/HEVC 内容上会永久卡在
`Blocked`——不是慢，是永远推进不到 `Eof`；而现实世界的 H.264/HEVC
几乎必带 B 帧。整个 M2a（Task 1~6）的验证矩阵在这条路径上从未真正
绿过一次：所有单测/端到端场景用的素材都是零 B 帧编码，这个缺陷因此
从建立以来就没有被任何一条用例触达。

**机理**（`src/media/pipeline.cpp` 修复前的 `step()` 第 215~217 行）：

```cpp
const bool has_pending_input = !ts.packets->empty();
const bool has_pending_flush = demux_eof_ && !ts.eof_sent;
if (!has_pending_input && !has_pending_flush) continue;   // ← 缺陷所在
```

`ts.eof_sent` 在 `drive_decoder()` 发出 `send(nullptr)` 的那一刻就置位
（`pipeline.cpp` 内 `NeedInput` 分支），而 `drive_decoder()` 拿到**第一帧**
缓冲帧就返回——此时 libavcodec 内部可能还压着 `解码器重排延迟 - 1` 帧
没吐出来，`ts.decoder_eof` 仍是 `false`。下一次 `step()`：
`has_pending_input`/`has_pending_flush` 两者都假（包已经送完、flush
已经发过），这条轨被 `continue` 跳过——即使它明明还有缓冲帧要 `receive()`
才能排出来。`demux_eof_` 已经是 `true` 时 demux 分支不再跑，
`!ts.decoder_eof` 又让 Eof 判定的 `all_done` 恒为假：三条分支都走不通，
`step()` 只能一直返回 `Blocked`，状态永不再变——活锁。零 B 帧素材上
`send(nullptr)` 之后 `receive()` 直接给 `Eof`（走另一个分支置位
`ts.decoder_eof`），永远碰不到这个漏洞。丢帧量 = 解码器重排延迟 − 1
（复现用 `-bf 3` 素材丢 1 帧；B 金字塔或更大 `-bf` 会丢更多）。

**为什么整个 M2a 都没暴露**：`tools/gen-fixtures.sh` 在这次修复之前只
产出 `-preset ultrafast` 编码的素材——libx264 在 `ultrafast` 下把
`bframes` 参数强制清零，不产 B 帧。Task 1~6 的全部单测、端到端场景
（A~D）都只用这份素材，跟真实播放场景（几乎任何默认编码参数都会带
B 帧）之间存在一个结构性盲区：**测试矩阵在"有没有 B 帧"这一维度上
从未展开过**，跟 #17 记录的"桩后端 × 真实后端"从未交叉是同一类方法论
教训——两条腿各自全绿不等于覆盖了乘积空间。`tools/syp_probe/
frame_digest.cpp::decode_pipeline()` 里"Blocked 时兜底轮询排空、排空
仍一无所获就报 `pipeline blocked with nothing to drain`"这段防御代码
先于这次修复就存在，是它把一次会挂死 ctest 的硬死锁降级成了一条可
诊断的 `error_stage`——这段代码本身没有问题，缺陷在 `Pipeline`。

**已修复**（本阶段，task-6.5）：在原判断上补一个"已经 flush、但还没
排空到 `decoder_eof`"的第三态（`draining`），让这条轨在这一状态下继续
被 `drive_decoder()` 驱动，直到真正吐空：

```cpp
const bool draining = demux_eof_ && ts.eof_sent;
if (!has_pending_input && !has_pending_flush && !draining) continue;
```

完整实测数据、反向自检见 `pipeline.h` 顶部调度说明的同步更新。

**现在靠什么守住**：
- `tools/gen-fixtures.sh` 新增 `bframes.mp4`/`bframes_faststart.mp4`
  （`-preset medium -bf 3`），脚本内现场用 `ffprobe` 断言视频轨真的
  产出了 `pts != dts` 的包（数量为 0 直接 `exit`）——不能只靠"编码参数
  写了 `-bf 3` 应该会有 B 帧"这种一厢情愿，锁死"素材本身不带 B 帧，
  回归用例就是空的"这条退路。
- `tests/test_pipeline.cpp` 新增两条回归：一条断言 B 帧素材上
  `decode_pipeline()` 能推进到 `Eof` 且帧数与 FFmpeg 参照完全一致
  （不是"跑到 Blocked 就算数"）；另一条用裸 `step()` + 即时
  `pop_frame()`（不经过 `decode_pipeline()` 的 Blocked 兜底排空，是
  M2b 真实消费者最朴素的写法）跑一遍，设连续 `Blocked` 次数上限，
  命中即时 `REQUIRE` 失败，不靠 ctest `TIMEOUT` 撞看门狗。
- `tests/test_decode_e2e.cpp` 的场景 A~D 已经从 `faststart.mp4`
  （零 B 帧）切到 `bframes_faststart.mp4`（真带 B 帧）——这四个场景
  今后天然覆盖"有 B 帧"这一维度，不会再退回到只测退化场景。

**没有覆盖到的**：其它使用 `faststart.mp4`/`moovend.mp4` 的用例
（`test_demuxer`/`test_decoder`/`test_packet_digest`/`test_frame_digest`/
`test_probe_e2e`）测的是 demux/解码器单元/dl 层，跟本缺陷所在的
`Pipeline::step()` 调度逻辑无关，没有切到带 B 帧的素材——这是有意的
范围控制，不是遗漏。

**一条措辞更正（M2a task-8 审查；已在 task-8 修复轮 1/5 挪位置）**：
`material_has_b_frames()` 的检测手段（`AVCodecParameters::video_delay
> 0`）**不是**「H.264/HEVC 通用」——它是 MOV demuxer 依 ctts 表现场估算
出来的，门槛写死 `codec_id == AV_CODEC_ID_H264`，HEVC 走不到这条路径，
目前只对 H.264 素材可靠（现状无风险：素材矩阵至今只有 H.264）。完整
技术说明已经从这里挪到 `tests/test_pipeline.cpp` 里
`material_has_b_frames()` 上方——那才是这条判据的设计依据所在，离得越
近越不容易被后来人漏看。

---

## 19. `syp_probe` 对非 HTTP scheme 静默产出垃圾后挂死 🟡 影响工具健壮性

**现象**：`syp_probe --decode --stats "file:///abs/path/to/good.mp4"` 在一份
**完好**的素材上（`ffprobe` 确认 `h264/yuv420p/has_b_frames=2` +
`aac/fltp/48000/2ch`）打出几千行 FFmpeg 解码错误：

```
[h264] Failed to parse header of NALU (type 0): "Invalid data found..."
[h264] pps_id 3199971767 out of range
[h264] Invalid NAL unit size (20882604 > 13570). Error splitting the input into NAL units.
[aac ] Number of bands (20) exceeds limit (2).
```

最后**不返回**，一直挂到看门狗硬超时：`RC=70`。

**机理（未深挖，记录观察）**：`<url>` 参数走的是 dl 层（`syp_source`），
而 dl 层是 HTTP 下载器——`file://` 不在它的契约里。它没有在入口拒绝这个
scheme，而是让请求继续往下走（NSURLSession 本身接受 `file://`，但不提供
dl 层依赖的 Range / Content-Length 语义），于是调度器拼出一段乱序/错位的
字节流喂给 demuxer。

**为什么值得记**：两个失败模式叠在一起，任何一个单独出现都比现在好——

1. **该拒绝时没拒绝**：非法 scheme 应当在入口判掉、以 `RC=3`（「工具自身
   跑不下去」）退出。工具自己的退出码表就把「比对不等」和「跑不下去」分开
   了，这里恰恰是后者，却走成了前者都不是的第三种结局。
2. **该失败时没失败，而是挂死**：字节流已经明显不是合法 mp4 了，管线却没
   有走到任何一级错误判定，一路耗到 `_exit(70)`。诊断信息全是 FFmpeg 的
   解码噪声，没有一行指向真因（scheme 不支持）。

**现状影响**：不影响任何已有用例（全部走 HTTP 或本地 `--diff` 的
`file:` 协议路径），不影响 M2a 的验收结论。纯粹是工具在契约外输入上的
健壮性问题。

**没有覆盖到的**：`syp_probe` 按 spec 第 7 节的裁定**不在 ctest 里**，
所以它的入参校验至今没有任何自动化守护——这条缺陷就是这个决定的直接
后果，也是它第一次显形。修的时候应当连同「给入参校验加一条最小的
ctest 覆盖」一起考虑，否则下一条同类问题仍然只能靠人手撞出来。

---

## 20. Mac Catalyst 没有 FFmpeg VideoToolbox 硬解——已裁定的平台差异 🟢 M3a

**背景**：M2b Task 1（`tools/build-ffmpeg.sh`，commit `e6d2f71`）给
FFmpeg.xcframework 加了 `ios-arm64-maccatalyst` slice。这个 slice 的
`SLICES` 表条目末尾带了一个额外 configure 参数 `--disable-videotoolbox`，
其它三个 slice（`ios-arm64`/`ios-simulator`/`macos-arm64`）没有这个参数。

**现象与根因**：不加这个参数，Catalyst 目标编译会直接失败——
`libavcodec/videotoolbox.c:821` 在 `#if TARGET_OS_IPHONE` 分支下引用了
`kCVPixelBufferOpenGLESCompatibilityKey`，这个符号在 macCatalyst SDK 头
文件里标了 `API_UNAVAILABLE(macCatalyst)`。Catalyst 编译时
`TARGET_OS_IPHONE` 为真（走 iOS 分支），但链的是 macOS SDK 的
CoreVideo.framework 头文件（该符号在 macCatalyst 下不可用）——这是
FFmpeg 8.1.2 自身在 Catalyst 目标上的缺陷，不是本项目配置错误。改
FFmpeg 源码在本项目是禁止的（`tools/build-ffmpeg.sh` 文件头「设计取舍」
写死：一行源码都不改、不打补丁），所以只能靠 configure 关掉整个
videotoolbox 支持，而不是只关掉出问题的那个编解码路径。

**现在为什么无害**：`tools/build-ffmpeg.sh:68` 上方注释确认，当前
`FF_COMPONENTS` 里的 `--enable-videotoolbox` 只是把库链进来占位——
"M3 硬解再加 --enable-hwaccel=h264_videotoolbox 等；现在只
--enable-videotoolbox 把库编进来，避免 M2 软解路径被 hwaccel 抢走"。
M2 阶段没有任何代码真正调用 videotoolbox 硬解路径，四个 slice 在这一点
上行为一致（都不硬解），Catalyst 缺这个不影响任何已用功能。

**将来怎么被绊到，而且是悄悄的**：`build_slice()` 里 slice 专属的额外
configure 参数是在 `FF_COMPONENTS`（全局组件清单）**之后**追加的
（`cfg+=("${FF_COMPONENTS[@]}"); cfg+=("$@")`），FFmpeg 的 configure 对
同一个 feature 的多次开关按最后一次生效。也就是说：**M3 如果按计划把
`--enable-hwaccel=h264_videotoolbox` 加进全局 `FF_COMPONENTS`，Catalyst
那一行末尾的 `--disable-videotoolbox` 仍会作为最后一个参数覆盖生效**——
其它三个 slice 都拿到硬解，Catalyst 悄悄地、永久地只有软解，而且不是
编译失败，编译会成功，是运行期悄悄退化。规划 M3 的 `VTDecoder` 时必须
显式处理这个分支：要么给 Catalyst 另评估一条硬解路径（等 Apple/FFmpeg
上游修这个 API 可用性问题），要么在文档和代码里明确接受"Catalyst 只有
软解"这个平台差异，不能假设四个 slice 硬解能力对齐。

**现状靠什么守住**：**什么都没有**。没有任何 ctest 用例断言过
"Catalyst slice 到底有没有 videotoolbox 符号"——`make_framework()` 里
现有的符号自检（`_avformat_open_input` 等六个入口）跟 videotoolbox 完全
无关，四个 slice 都会照样通过。这条缺陷只有在 M3 真正接硬解、然后在
Catalyst 壳上观察到"没有硬解生效"或者干脆运行期报错时才会显形。

### M3a 裁定（2026-09-14）

用户决定不改 FFmpeg 源码。Catalyst 上选硬解 → `supports()` 恒 false → `SYP_ERR_NOT_IMPLEMENTED`。
"悄悄退化"那一半已被两道护栏堵住：`build-ffmpeg.sh` 的 `check_hw_config()` 在 configure 后
断言 Catalyst 两个 VT hwaccel 为 0、其余 slice 为 1；`vt_decode_backend.mm` 读
`syp_ffmpeg_features.h` 决定能力。Catalyst 系统本身支持 VideoToolbox，将来若要硬解，唯一不改
FFmpeg 的路是直调 `VTDecompressionSession`（spec 决策 2 的备选）。

## 21. ~~`FakeAudioSink` 隐藏了真实 `IAudioSink` 实现必须自己处理的两处架构落差~~（已落地，M2b Task 8）🟢

**背景**：`src/media/video_renderer.h` 顶部注释点名"平台实现只做上传
纹理 + 一个着色器，薄到没地方藏逻辑——因为它没有自动化覆盖"；
`tests/support/fake_audio_sink.h`（M2b Task 4）是场景 A~I（Task 7）全部
数值断言的判据来源，但它是**测试替身**，跟 Task 8 要写的真实
`AudioUnitSink` 在架构上不是同一回事。审查用变异体核实过 `played_us()`
的算术本身（Critical 1~3、Important，见 commit `9a1a4dd` 修复轮后的
`test_fakes.cpp`），但算术之外，替身还**完全隐藏**了两处真实实现必须
自己处理、而替身上永远测不出来的落差。

**落差 1：`consumed_frames_` 的语义只在替身内部自洽，真实实现要靠快照
才能复现同一语义**。`FakeAudioSink::consumed_frames_` 是"自上次 flush
以来"的计数——`flush()` 直接把它清零（见 `fake_audio_sink.cpp`），
`advance()` 在这个清零后的基准上累加。而 `AudioRing`（M2b Task 2）的
`consumed_bytes()` 是**单调计数器，reset() 不清它，永不回退**（这是
`AudioRing` 自己的不变量，理由见 Task 2 报告）。刚读完 `AudioRing` 头
文件那句"永不回退"的人，很容易把"reset 不清 consumed_bytes"误解读成
"`AudioUnitSink::played_us()` 直接拿当前 `consumed_bytes()` 用就行"。
那样算出来的"消费量"其实是**从进程启动到现在的全部历史样本**，不是
"自上次 flush 以来"的量。

**症状**：每次 seek/变速触发 `flush()` 之后，`played_us()` 会把 flush
之前的全部历史消费量也算进当前这一段的净差，表现为**时钟在 flush 之后
突然跳到远超预期的值**，而且越到后面（累计消费的历史样本越多）跳得
越夸张——这类"数值系统性偏大、偏移量随播放时长增长"的缺陷，正是
Critical 2（speed×latency 交互）那类"判据本身对它是瞎的"缺陷的同族：
不会自己崩，只会让下游的同步断言在跑得越久之后越离谱。

**这个 bug 在 `FakeAudioSink` 替身上无法复现**——替身的 `flush()` 本身
就会清零 `consumed_frames_`，替身测的是"清零之后对不对"，不是"真实
实现有没有做等价的清零"。`test_fakes.cpp` 里新补的
`fake_sink_flush_resets_base_and_counters` 钉住的是替身自己那份状态机，
对 Task 8 要写的 `AudioUnitSink` 完全没有约束力。

**正确做法**：`AudioUnitSink::flush(base_us)` 时必须**快照**
`AudioRing::consumed_bytes()` 当时的值；`played_us()` 时用
"当前 `consumed_bytes()` − 快照" 算出"自上次 flush 以来"的净消费量，
再走延迟扣除 + 倍速换算那套公式（公式本身与 `FakeAudioSink` 一致，差的
只是"消费量"这一个输入要不要经过快照相减）。Task 8 必须自己为这条写
测试——不能假设 `FakeAudioSink` 已经覆盖过。

**落差 2：`write()` 的背压契约在替身与真实 `AudioRing` 之间发生了分叉**。
`FakeAudioSink::write()` 按**帧数**计容量，背压判定是"要么整帧写入成功
（返回 `true`），要么整帧不写（返回 `false`）"——没有部分写入这个概念。
而 `AudioRing::write()` 按**字节数**计容量，且**允许部分写入**（返回
实际写入的字节数，可能小于请求量）。但 `IAudioSink::write()` 这条接口
的契约是 `bool`（见 `src/media/audio_sink.h`）——没有部分写入语义。
也就是说：`AudioUnitSink::write()` 必须自己把 `AudioRing::write()`
"允许部分写入、返回实际字节数"这套语义，折算成"要么这一帧整帧成功、
要么整帧失败"这套语义（例如：环形缓冲剩余空间不够整帧就直接判失败，
不接受部分帧）。这层折算逻辑在 `FakeAudioSink` 里根本不存在——替身的
`write()` 直接对帧数做算术比较，从来没有跨越过"字节 vs 帧"、"部分写入
vs 整帧"这两条边界，所以也测不出这层折算写错了会是什么样子。

**（Task 8 之前）现状靠什么守住**：`test_fakes.cpp` 的判据检验只覆盖
`FakeAudioSink` 自身的状态机（`flush()` 清零、背压边界、`advance()`
不超发）；`AudioRing` 自己的欠载/背压性质在 `tests/test_audio_ring.cpp`
里有覆盖；但**两者之间的折算层——也就是 `AudioUnitSink` 本身——当时
不存在，也没有任何用例覆盖"折算对不对"这件事**。

### 落地（M2b Task 8，2026-09-10）

两处落差都按上面"正确做法"一节写死了，且都拆成了不碰
`AudioComponentInstance` 的纯函数，可以在没有真实设备的环境下直接单测
（`src/platform/apple/audio_unit_sink.h` 的两个 `static` 方法）：

- **落差 1（快照相减）**→ `AudioUnitSink::compute_played_us(consumed_bytes,
  base_consumed_bytes, device_latency_us, sample_rate, bytes_per_frame,
  speed, base_us)`：内部做"`consumed_bytes − base_consumed_bytes`"再走
  延迟扣除 + 倍速换算。`AudioUnitSink::flush(base_us)` 里在
  `ring_->reset()` 之后立即把 `ring_->consumed_bytes()` 快照进
  `base_consumed_bytes_`，`played_us()` 把这两个量原样递给
  `compute_played_us()`，调用方不需要、也不应该自己先减一次。
  `tests/test_audio_unit_sink.cpp` 的
  `played_us_uses_snapshot_delta_not_raw_consumed_bytes` 直接复现本条
  点名的症状形态（历史消费 100 秒 + flush + 再消费 1 秒），断言结果是
  `base_us + 1 秒`而不是"忘了减快照"会算出的 `base_us + 101 秒`。
- **落差 2（部分写入 → 整帧成败）**→ `AudioUnitSink::commit_frame(ring,
  data, bytes)`：先查 `ring.writable() < bytes` 直接拒绝（一个字节都不
  碰），足够才真正调 `ring.write()`。`tests/test_audio_unit_sink.cpp` 的
  `commit_frame_rejects_whole_frame_when_it_does_not_fit` 用同一个场景
  对照了折算前后的行为——先证明裸 `AudioRing::write()` 在这个场景下会
  部分写入（跟 `test_audio_ring.cpp` 的
  `ring_write_returns_partial_when_full` 同一形状），再证明
  `commit_frame()` 把它折算成了整帧拒绝，环一个字节都没被动过。

`AudioUnitSink::write(const Frame&)` 本身还多做了一层这两个 known-gaps
落差之外、Task 8 自己发现的第三层折算：`swr_get_out_samples()` 预估这次
转换"最多"会产出多少样本、据此在**调用 `swr_convert()` 之前**就用
`commit_frame` 同款的 writable() 检查判断背压——顺序不能反过来，因为
`swr_convert()` 一旦真正执行就已经消费了输入样本（可能进了内部重采样
FIFO），如果转换完才发现环装不下再回退，调用方按"帧未被消费"的契约会
把同一个 `Frame` 重新递进来，等于把这段样本喂给 swr 两遍。

**这一层的覆盖走过一段曲折**：修复轮 1/5 最初判断"构造不出确定性用例"
——尝试的构造是"同速率填充帧灌满环 → 目标帧背压被拒 → flush 腾空 →
重试 → 比对提交字节数"，反复实测都是 delta=0，误判为"顺序对不对不
影响外部可观察行为"。修复轮 2/5 的复审推翻了这个判断，并查出真正原因：
`swr_convert` 因背压被拒后"消费输入但丢弃输出"这个效应只在
`SwrContext` 冷启动的头几十个样本内存在，而"同速率填充帧"这一步恰好
会用掉目标帧即将使用的那个 `SwrContext`（`ensure_swr_for()` 按"输入
格式 + speed"整体重建/复用），等真正测试目标帧被拒时上下文早已不是
冷的了——**最自然的构造路径会在触发目标效应之前先把它自己抹掉**。正确
构造改用**不同速率**的填充帧制造背压（这样建的是另一个 `SwrContext`，
不预热目标帧要用的那个），见
`tests/test_audio_unit_sink.cpp` 的
`swr_precheck_ordering_same_frame_retry_exposes_phantom_consumption`
——反向自检确认：正确实现下 `k_ref=31171 k_main=31171 delta=0`（三次
连跑稳定一致），注入"预检挪到 `swr_convert` 之后"的变异后
`k_ref=31225 k_main=31201 delta=24`（同样三次连跑稳定）。

这条用例的数值裕度耦合了 `libswresample` 内部滤波器预热瞬态的具体
样本数量级——不是 FFmpeg 文档化的契约，是当前 vendored 版本（钉死
8.1.2）的实现细节，FFmpeg 升级后需要重新验证，已记入
`docs/tech-debt.md`。判别式本身的**方向**是稳健的：只要重采样存在非零
群延迟，有缺陷的实现提交的样本数必然 ≥ 正确实现，用例特意选
`speed=1.5`（非 1:1 直通）避开可能零群延迟的直通路径。

**现在靠什么守住**：`tests/test_audio_unit_sink.cpp`（18 条用例）覆盖
`commit_frame()`/`compute_played_us()` 两段纯算术、`played_us()`/
`flush()`/`open()` 在对象级的接线正确性（含两条需要真实硬件、
`open()` 失败即 skip 的用例），以及上面这条第三层折算顺序的回归。
四套构建（Debug/Release/ASan+UBSan/TSan）下全绿。真正碰
`AudioComponentInstance` 硬件初始化路径本身、以及 `render_cb` 在真实
设备回调时序下的行为仍然没有自动化覆盖——这不是本条落差没修完，是
另一条结构性盲区，见下一条（#24），该条也记录了"哪些其实已经测到了、
不要读成整体不可测"的盘点。

## 22. ~~`TrackPlayer::step()` 在"音频 `FrameQueue` 空 ∧ 视频早到"下是纯空操作，可永久停摆~~（已修复，M2b Task 7.5）🟢

**背景**：M2b Task 7（九条端到端同步场景 A~I）实现时，场景 D/I（倍速）
的驱动方式换了三版才绕开一个反复命中的死锁——第一版审查阶段把它归因
为"这套 fake-sink 驱动方式本身的限制，不是 `TrackPlayer` 的缺陷"，
经复审用代码证明**判错了：这是 `TrackPlayer::step()` 一个真实的活锁，
不是测试驱动方式的限制**。

**根因**（`src/media/track_player.cpp`）：

- 第 1 步（音频优先，约 178~199 行）：`pending_audio_` 与 Pipeline 的
  音频 `FrameQueue` 都空时，音频分支直接 `break`，**不驱动 Pipeline**。
- 第 2 步（视频三分支判定，约 240~244 行）：`if (diff > kPresentWindowUs)
  return Waiting;`——早到分支**直接 return**，跳过第 3 步的
  `pipeline_->step()`。

当"音频 `FrameQueue` 空 ∧ 视频早到"同时成立时，`step()` 是纯空操作：
不解码、不写音频、不呈现。时钟只能靠已经写入 sink 的音频样本推进，而
没有新样本能被写进去——**闭环，永久**，直到外部干预（比如
`seek()`/`pause()`）打破这个状态。

**「真机环大所以没事」这条论证不成立**：spec 第 4 节把 `played_us()`
定义成"已消费样本数"的函数；真实设备的音频回调在环形缓冲放空之后必须
输出静音（`AudioRing::read()` 只对真实读出的字节推进读指针，见 M2b
Task 2 的 `ring_underrun_does_not_advance_consumed_bytes`），**正确实现
不会把静音计进 `consumed_bytes()`**——记了就是谎报播放位置。也就是说
真机 `played_us()` 在环放空后**同样会停摆**，环深只把"多久停摆"从
（测试里的）瞬间拉到大约 1 秒，不改变"会不会停"这件事。能不能桥过某次
"视频早到"的缺口是一个可算的不等式：M2b Task 7 用的
`wide_frame_config()`（`max_frames_per_track=64`）在 25fps 下等于 2.56
秒的视频解码领先量，而 `AudioUnitSink`（Task 8）典型的硬件环深约 1
秒——**真机的领先量/环深配比比测试里更差，不是更好**。

**触发条件不异想天开**：`set_speed()`/`seek()` 都会 `flush()` 掉环
（清空"已写入但还没消费"的那部分存量），如果这一刻 Pipeline 的音频
`FrameQueue` 恰好接近排空（冷启动、刚变速、刚 seek 完），就会落进这个
状态。M2b Task 7 场景 D/I 的第一版、第二版驱动方式撞到的就是它——
`presented=3 device_us_spent=24999955000` 两次独立跑数值完全一致，是
确定性复现，不是抖动噪声。

`track_player.h` 顶部长注释其实已经描述过这个失败形态的**表现**
（"画面静止、无错误、无降级——静默且稳定地错"），但那段注释描述的是
"音频轨解码失败"（Important 7 修的那条路径，靠轮询
`Pipeline::track_failed()` 堵住）。**「音频只是暂时饿了、sink 完全
健康、没有任何 `failed()` 会触发」这条路径没有被那次修复覆盖，因为它
根本不触发任何 `failed()` 判据**——两条路径表现相同，根因不同，
一条已经堵了，另一条还开着。

**现状靠什么守住**：M2b Task 7 的"暂停预热"规避手段（暂停分支无条件
驱动 Pipeline，见 `track_player.cpp` 第 4 步）本身是合格的、干净的
测试写法，但它只是**绕开**了这个状态，不是**修复**了它——九条端到端
场景因此全绿，但这不代表这个活锁不存在，只代表这九条场景都小心地没有
在"音频 `FrameQueue` 空 ∧ 视频早到"这个组合状态下驱动过 `TrackPlayer`。
目前没有任何用例直接断言"这个组合状态下 `step()` 会不会永久卡死"。

**这是 Task 7.5 的入场条件**：修复思路方向上至少有两种——(a) 让视频
早到分支在"确认音频侧也真的没活干"之后仍然驱动一次 Pipeline（打破
"早到分支直接 return、跳过第 3 步"这条捷径）；(b) 重新设计音频/视频
两个分支对 Pipeline 驱动权的仲裁，不再是"谁先产出谁独占这一次 step()"。
两种思路都要求先补一条能在假时钟/假 sink 组合下**确定性**复现这个
"音频饿、视频早"组合状态、并断言"`step()` 调用 N 次后仍未能推进"的
回归用例（这条本身也是当前测试矩阵里没有的缺口），再着手改
`track_player.cpp`——按本仓库的既有纪律，不能先改实现再补测试。

---

### 修复（M2b Task 7.5，2026-09-10）

**先补的回归用例**：`tests/test_track_player.cpp` 的
`step_does_not_livelock_when_audio_queue_empty_and_video_early`——用
`make_wide_fixture_with_audio_and_video()`（视频/音频各自单独编码再
`-c copy` 封装，音频比视频多留 1 秒富余，避免撞上另一个不相关的收尾
边界，见该 fixture 顶部注释）+ 暂停预热攒出解码领先量，再用真实的
音频优先分支把 Pipeline 的音频 `FrameQueue` 排空，然后直接调
`sink->flush(0)`（不经 `TrackPlayer::set_speed()`/`seek()`，只改
"sink 有没有未消费存量"这一个变量）精确复刻触发条件。反向自检实测：
在修复前的代码上，这条用例连续 2000 次 `Waiting` 判据必现（`written`/
`consumed`/`position_us()` 三个量在两千次 `step()` 调用里逐比特不变），
确认测试确实抓得住这条活锁，然后才动 `track_player.cpp`。

**修法**：跟修复轮 1/5 的 Important 1（暂停分支短路、不驱动 Pipeline）
同一个形状、同一个思路——视频早到分支不再 `return`，只是记下"这一步
该报 Waiting、带上这一帧的 track_index/pts"，跳出视频判定，仍然走到
第 4 步驱动一次 `Pipeline::step()`。音频分支"队列空就 `break`"那半句
本身不需要改：它不直接 `return`，只是让控制流落到视频判定，这次一并
确认了视频判定改完之后，两条分支合起来不会再构成新的闭环。

**反向自检当场又挖出两个只有在"视频早到分支真的会驱动 Pipeline"之后
才可能触发的新坑**（Pipeline `Blocked`/`Eof` 各一个），一并堵上：

1. `seek_runs_all_four_steps`/`seek_failure_still_flushes_and_resets`
   两条既有用例用"sink 容量归零、audio_blocked 永久为真"构造"卡住一帧
   旧视频"的前置状态，靠 `step()` 返回的 `pts_us` 识别那一帧——但
   Pipeline 自己的 `FrameQueue` 很快因为音频永久背压而堆满，之后每次
   驱动都报 `Kind::Blocked`；若这个信号在 switch 里跟 Pipeline 自己的
   `Kind::Blocked` 同一优先级（或更低），"视频早到"这条更具体的信息会
   被永久盖住。修法：`video_waiting_early`／`audio_blocked` 两个本地
   信号统一提到 Pipeline 自己的 `Kind::Blocked` 前面判——它们俩报出的
   `Blocked` 载荷本来就跟 Pipeline 自己报的完全一样，调换顺序不改变
   `audio_blocked` 分支的可观测行为，只是给 `video_waiting_early`
   让出优先级。
2. `early_frame_waits_and_is_not_presented`（单帧素材，第一次 `step()`
   就会驱动 Pipeline 恰好吃到这条轨最后一帧）：Pipeline 自己的 `Eof`
   判据只问它内部的 `PacketQueue`/`FrameQueue` 是否排空，对
   `TrackPlayer` 自己攥着、已经 `pop` 出来但还没呈现/写出去的
   `pending_video_`/`pending_audio_` 一无所知——若原样转述 `Eof`，这一
   帧就永远不会被呈现/写出去。修法：`video_waiting_early`／
   `audio_blocked` 还要排在 Pipeline 的 `Kind::Eof` 前面，不只是排在
   `Kind::Blocked` 前面。`Error` 不受影响，仍然最高优先级如实转述。

修完之后的优先级顺序、以及为什么这么排，写进了
`track_player.h` 顶部 step() 总览的第 3~5 步（照该文件顶部现有几段
长注释的风格）。

**场景 D/I 的连带调整（`tests/test_sync_e2e.cpp`）**：反向核对还挖出一
个不算 bug、但值得记录的事实——Task 7 原版的场景 D（倍速呈现速率）
实测三档 `queued`（真正写进 sink 的音频帧数）恒为 64，跟 speed 完全
无关，因为"视频早到不驱动 Pipeline"这条老缺陷让暂停预热之后 Pipeline
再没机会解出新内容，`run_budget()` 量到的其实是"把预热阶段攒好的固定
存量、用不同时钟速率消费完要几次 step()"，从未真正测过"持续解码 + 呈
现"这条链路——这也是该场景在 Task 7 审查里判"绿"却仍放过 #22 的原因，
它自己就是被这个 bug"喂养"出来的测量方式。修复接上之后 `queued` 随
speed 变化，呈现数第一次真实反映持续解码吞吐，但 Task 7 原版给的
FrameQueue 深度（64）与 sink 写入容量（默认约 1 秒）对 2x 档的持续吞吐
来说不够，2x 呈现数一度反而低于 1x（写满 sink 触发大量 `Blocked`，或
去掉预热后大量迟到丢帧）。调整为 FrameQueue 深度 512、暂停预热
20000 次 `step()`、sink 容量 480000 帧（约 10.9 秒）后，dropped/blocked
在三档下都恢复 0，呈现数干净地随 speed 单调、成比例变化，既有的
0.75×/1.5× 比例判据不需要放宽。完整推导与实测数字见
`tests/test_sync_e2e.cpp` 场景 D 顶部长注释。

**现在靠什么守住**：`step_does_not_livelock_when_audio_queue_empty_
and_video_early` 直接断言这个组合状态下 `step()` 最终能推进到
`Eof`（不是"跑到 Blocked/Waiting 就算数"），命中过久的连续 `Waiting`
会立即 `REQUIRE` 失败，不依赖 ctest `TIMEOUT` 撞看门狗。九条端到端场景
（`test_sync_e2e.cpp`）与既有 26 条 `TrackPlayer` 单测全绿；四套构建
（`build`/`build-asan`/`build-tsan`/`build-rel`）全部验证过。

## 23. ~~音频轨比视频轨短时，`TrackPlayer::step()` 会在尾部永久停摆，且永不报 `Eof`~~（已修复，M3b）🟢

**背景**：这是 Task 7.5 修复 #22 时，`make_wide_fixture_with_audio_and_video()`
第一版视频、音频请求同一个 `duration` 直接撞出来的一个**独立**边界——
第一版实现者当时靠"音频比视频多留 1 秒富余"绕开了它，只在测试 fixture
的注释和任务报告里提了一句，没有立案。审查（opus）复核时指出：**音视频
轨时长不严格相等在真实 mp4 里是常态**（不是刻意构造的极端情形），这个
边界会让真机卡死，必须立案，不能只是绕开。

**根因**：`TrackPlayer` 只持有 `pending_video_`/`pending_audio_` 各一帧
待交付缓存（见 `track_player.h` 顶部长注释"pending_audio_ /
pending_video_"一节）——`step()` 每次只从 Pipeline 的 `FrameQueue` 里
`pop` 一帧，直到手里这一帧被消费掉（呈现/丢弃/写出）才会去 `pop` 下一
帧。若音频轨的真实播放时长**短于**视频轨（音频先到 `decoder_eof` 且
`FrameQueue` 排空），音频耗尽之后 `AudioClock` 的读数永久冻结在最后一次
`consumed_bytes()`——这是设计上的**正确**行为，不是 bug：`AudioRing`
在环放空之后必须让回调只读出静音、不推进读指针（`known-gaps` 本条修复
之前的 #22 引用过的 Task 2 `ring_underrun_does_not_advance_consumed_bytes`
就是保证这件事的用例），`played_us()` 不能把静音算进"已播放"，否则就是
谎报播放位置。时钟冻结之后，视频轨剩余的尾部帧（pts 超过音频轨总时长
的那些）会永远判成"早了"——`video_waiting_early` 从此永远为真，
`pending_video_` 卡住不动；而 #22 的修复本身工作正常（视频早到分支会
继续驱动 `Pipeline::step()`），但 Pipeline 自己也已经无事可做（两条轨
都到了各自的 `decoder_eof`，只是视频侧还有已解码但没被 `TrackPlayer`
取走的尾部帧堆在 `FrameQueue` 里）——`Pipeline::step()` 因此持续报
`Blocked`（该轨 `FrameQueue` 非空，`Eof` 的排空判据不满足），
`video_waiting_early` 优先级又压过 `Blocked`，`step()` 对外只会看见
连续不断的 `Waiting`，永不报 `Eof`。

**独立于 #22，不是同一个缺陷**：#22 是"该驱动 Pipeline 时没驱动"，本条
是"Pipeline 已经无事可做、`TrackPlayer` 自己的单帧缓存设计又不允许多
`pop` 几帧去补上尾部"——#22 修复前后，本条的表现**逐比特相同**（见下方
"验证"一节），证明两者互不依赖，也证明本条不是 #22 修复引入的回归。

**可复现的实测数字**（视频、音频各自单独编码再 `-c copy` 封装：3 秒
视频 75 帧、2 秒音频 88 帧，默认 `PipelineConfig`，`FakeAudioSink`/
`FakeRenderer`，只在 `Kind::Waiting` 上推进设备时间）：

```
clock_kind=0
[PROBE] eof=0 presented=52 max_consec_waiting=2999455 blocked=0 dropped=0 pos=2020136 shown=52
```

75 帧视频只呈现了 52 帧（尾部 23 帧永远不会被呈现），`eof` 恒为 0，
`max_consec_waiting` 撞满探针给的调用预算（本仓库独立复现用的是
300 万次 `step()` 上限；审查用另一份独立探针实测撞满 39.9455 万次，
两次数量级不同只是探针各自给的循环预算不同，`presented=52`、
`pos=2020136`、`shown=52` 三个关键数字逐位相同）。

**验证过"不是 #22 引入的回归"**：把 Task 7.5 的全部改动
`git stash`（`track_player.{h,cpp}`、两个测试文件、`known-gaps.md`）
之后用同一份素材、同一个探针重跑，输出逐字节相同——本条在 #22 修复
前后表现完全一致，是预先存在的问题。

**现状靠什么守住**：什么都没有。九条端到端场景（`test_sync_e2e.cpp`）
用的固定素材音视频轨时长本就接近（或用 `wide`/`hugecfg` 系列配置把
FrameQueue 撑得足够深，尾部差异被吞掉，测不出这条边界）；
`test_track_player.cpp` 里唯一撞到过这条边界的地方（Task 7.5 第一版
`make_wide_fixture_with_audio_and_video()`）已经用"音频多留 1 秒富余"
主动绕开，没有留下任何断言这个边界"卡不卡死"的用例。

### 终审复核（M2b 收尾，2026-09-11）：数字逐位不变，Task 8/9/10 之后毫无变化

标题此前写的是「🟠 M2b Task 8 前向陷阱」。**Task 8 已经交付，这条没有
做**——继续挂着"前向陷阱"这个归属，会让人以为它已经在某个任务的射程里。
终审重新跑了一遍复现，数字跟本条立案时记录的**逐位一致**：

```
presented=52   （75 帧只呈现 52）
eof=0
pos=2020136
shown=52
max_consec_waiting=2997851
```

所以归属改成「已知且接受，留给 M3」：它是一条真实的、仍然存在的
🟠 级缺陷，不是某个已完成任务的遗留待办。修复方向（下面两段）不变。

**修复方向（原文保留，归属已改为 M3）**：`AudioUnitSink` 接真实设备之后，音视频轨
时长不等这件事只会更常见（容器元数据、编码器 priming/尾部截断都可能
让两条轨差出几帧到几十毫秒），修复思路方向上至少有两种——(a) 音频轨
耗尽后触发某种形式的"尾部呈现"路径（不再要求时钟继续走，直接按到达
顺序把剩余视频帧交付掉，行为类似 `pause()` 之后 `Pipeline` 仍推进但
换一种终止判据）；(b) `Eof` 的判定本身加一条"音频轨已经
`decoder_eof` 且 `FrameQueue` 空，即使视频轨还有内容，也应该给调用方
一个不同于普通 `Blocked` 的信号，让上层决定要不要降级到系统时钟去把
尾部播完"。两种思路都要求先想清楚 spec 第 4/5 节要不要为"音频轨提前
结束"单独定义一条降级路径（现有的 `degrade_to_system_clock()` 只在
`sink_->failed()`/`track_failed()` 时触发，音频轨"正常耗尽"不是这两种
情形之一）。

### 修复（M3b Task 3，2026-09-15）

**判据**：`played_us() ≥ 最后写入结束时刻 − 20ms` 这条 spec 原计划的判据被
证伪——延迟大于容差（实测 45ms 即停摆）时永不成立，因为 `played_us()`
本身要扣设备延迟，`output_drained()` 与设备延迟无关。落地判据是
`clock_kind_ == Audio ∧ pipeline_->track_drained(audio_track_) ∧
pending_audio_ 为空 ∧ sink_->output_drained()`——`IAudioSink` 新增纯虚
`bool output_drained() const noexcept`（自上次 flush()/open() 起
`write()` 接受的样本是否已全部被设备取走；未 open 或已 failed 均返回
true）；`AudioUnitSink` 实现为 `ring_->readable() == 0`（线程安全论证见
`audio_unit_sink.mm`）；`FakeAudioSink` 实现为
`consumed_frames_ >= written_frames_`。切钟基准 = **当时的音频时钟读数**
（不是最后写入的结束时刻——AAC priming 使读数比 pts 超前约一帧，取结束
pts 会回跳，取当前读数零跳变）。`kAudioEndSlackUs`/`last_audio_end_us_`
已删除，判据不再依赖任何容差常量。

**新增**：`TrackPlayer::clock_switch_reason()` 返回
`ClockSwitchReason::{None, AudioFailed, AudioEnded}`，区分"sink 失败降级"
（不可逆）与"音频正常播完切钟"（可逆）。

**seek 恢复**：`seek()` 之后若音频轨仍存在且 sink 未失败，自动恢复 Audio
时钟（`AudioEnded` 可逆，`AudioFailed` 不可逆）。

**回归用例**（`tests/test_track_player.cpp`）：
`audio_shorter_than_video_switches_to_system_clock_and_reaches_eof`、
`audio_shorter_than_video_with_large_device_latency_reaches_eof`（150ms
设备延迟，专门钉住"判据与延迟无关"这条）、
`video_shorter_than_audio_keeps_audio_clock_until_eof`（视频先结束行为
不变，仍按音频播完报 `Eof`）、`seek_after_audio_ended_restores_audio_clock`、
`seek_past_audio_end_after_switch_still_reaches_eof`、
`set_speed_after_audio_drained_still_reaches_eof`。判据区分力已用变异
验证：去掉 `output_drained()` 这一项后 38/47 用例变红（含两条 #23 用例
断言"声音远未播完就切钟"）。

## 24. `AudioUnitSink` / `MetalRenderer` 的硬件路径没有自动化覆盖 🟢 技术债（结构性，跟 #17 同形）

**背景**：`src/media/video_renderer.h` 顶部注释早就点名过"平台实现只做
上传纹理 + 一个着色器，薄到没地方藏逻辑——因为它没有自动化覆盖"；M2b
Task 8 把同样的判断第一次真正落到代码上——`AudioUnitSink` 是 M2b 第一个
真正碰硬件的组件。spec 第 8 节写明这是**结构性**盲区，不是"这次没顾上
补测试"：`AudioComponentInstanceNew`/`AudioUnitInitialize`/
`AudioOutputUnitStart` 这条路径依赖真实音频设备（CI 机器不一定有，
沙箱/无头环境下可能直接拿不到默认输出设备），而 render callback 一旦
接入真实 `AudioOutputUnit`，就运行在一条不受测试进程调度的实时线程上
——想在 ctest 里确定性地断言"回调在正确的时刻被正确调用"，本身就需要
先解决"如何确定性地触发一次真实硬件的 I/O 回调"这个更难的问题，价值
不足以支撑这个投入（跟 dl 层的桩后端 vs 真实后端交叉覆盖问题是同一个
方法论教训，见 #17）。

**跟 #17 的关系**：#17 是"两套已经分别被测过的东西，交叉点没人测过"；
本条是"这一整层（真实音频/视频硬件后端）压根没有能在 CI 上跑的自动化
测试"，程度更深——不是"交叉点"空了，是"其中一条腿"本身就空了。

**现状靠什么守住**：

1. **平台实现刻意做薄**——`AudioUnitSink`（`src/platform/apple/
   audio_unit_sink.{h,mm}`）把所有不碰 `AudioComponentInstance` 的算术
   （`write()` 的整帧折算、`played_us()` 的快照相减，见 #21"落地"
   一节）拆成两个 `static` 纯函数，用普通 `tests/test_audio_unit_sink.cpp`
   直接单测；真正碰硬件的部分只剩"调 Apple 的 API、把结果搬进/搬出这两
   个纯函数"，薄到审查读一遍源码就能核对——没地方藏逻辑，也就没有"藏起
   来的地方需要测试才能挖出来"这个问题。`MetalRenderer`（M2b 另一半）
   **已经走了同一个策略并落地**（原文写的是"预计走同一个策略"，Task 9/
   9.5 之后即失实，M2b 终审 m11 订正）：`is_pix_fmt_supported()` /
   `select_color_matrix()` / `pack_plane_rows()` 三个纯函数零 GPU 依赖、
   普通 `.cpp` 直接单测。

   **而且 `MetalRenderer` 这一半比 `AudioUnitSink` 走得更远，本条此前
   完全没提**：`tests/test_metal_renderer.cpp` 有 **5 条真碰 GPU 的对象级
   用例**（`MTLCreateSystemDefaultDevice()` + `present()` +
   `debug_copy_output_rgba()` 回读，**像素级断言**，跟独立手写的标量参考
   实现 `reference_yuv_to_rgb` 比对）：
   `object_level_present_and_readback_on_real_gpu`、
   `object_level_present_actually_uses_selected_matrix_for_720p_frame`、
   `object_level_present_uses_real_colorspace_over_resolution_presumption`、
   `object_level_present_uses_real_full_range_from_avframe`、
   `object_level_same_size_different_colorspace_invalidates_matrix_cache`
   （最后一条是终审 I3 补的，见 #26 的终审复核一节）。它们不需要显示器/
   窗口/`CAMetalLayer`，只需要一块 Metal 设备；按"设备不可用就 skip、不算
   失败"的纪律写，同时用 `device_available()`/`ready()` 拆分把"没有 GPU"
   与"有 GPU 但管线建不起来"分开（审查修复轮 1/5 Critical M11），后者会
   `CHECK` 记一次真实失败而不是静默 skip。
   **换句话说：`MetalRenderer` 的硬件路径并不是完全没有自动化覆盖**——本条
   标题对它的概括过于笼统，真正没有覆盖的只剩下窗口/`CAMetalLayer` 上屏
   路径与真实显示时序。`AudioUnitSink` 那一半（下面第 3 点、以及"仍然
   没有"那段）则**原样成立**：`open()` 的硬件初始化、`render_cb` 在真实
   回调驱动下的时序、析构与正在运行的回调之间的竞争，仍然是结构性盲区
   （变异 M15 存活，与本条的声明一致）。
2. **第一天就记**——不是先假装"以后补"再遗忘，本条在 Task 8 完成的
   同一个提交里立案，附带这一层结构上为什么测不了的完整论证，跟 #17 的
   写法一致（"记录方法论教训，不是等出了事才追溯"）。
3. **demo 壳的漂移读数当人工交叉点**——`AudioUnitSink::open()` 会把三个
   延迟分量（`kAudioUnitProperty_Latency` / safety offset / buffer frame
   size）分别打日志（spec 第 11 节风险第一条）；demo 壳跑起来之后，
   这三个数在真机上是否符合预期（不是异常值、不随时间漂移）就是人工
   验证这条硬件路径"没有肉眼可见地跑偏"的检查点，弥补没有自动化断言的
   缺口。Task 8 报告里记了一次本机（macOS，非 demo 壳，手工探针）的实测
   读数，可以当作"正常量级"的参照基线。

   【审查 Important 3 · 补充，M2b Task 10】demo 壳（`demo/ios`、
   `demo/mac`）已经落地，"M4 的 demo 壳"这个引用是失实的——项目里目前
   只有这一份 demo，不是 M4 的事。**实测结果**（Mac Catalyst 壳构建产物
   经一次 headless 冒烟测试驱动，原生 macOS 进程直接跑同一份
   `bridge.mm`，播放内置 12 秒样片到 EOF；`demo/ios` 的真机壳只构建到
   `CODE_SIGNING_ALLOWED=NO` 这一步，没有实机数据）：
   主时钟全程保持 `Audio`（`AudioUnitSink::open()` 在这台机器上成功）；
   40 次采样（跨约 12 秒）漂移落在约 **−25ms 到 +33ms** 之间，没有随时间
   单调增长或发散的趋势——落在 `kPresentWindowUs`（±40ms，
   `track_player.h`）允许的呈现窗口内，是设备延迟常数没有系统性偏差的
   签名（取错的表现是整条曲线整体加一个恒定偏置，而不是像这样跨 12 秒
   保持在一个带宽内游走）；`dropped_frames()`/`present_failures()`
   全程为 0。这是本条目第一次有真实设备（哪怕只是 Mac 原生环境、不是
   iOS 真机）跑出来的数字，之前只有 Task 8 报告里的手工探针读数。iOS
   真机上的读数仍然缺失，留给下一次有真机/签名条件时补。

**这条不是"整个文件不可测"的免责声明——以下部分不需要硬件，审查修复
轮 1/5（C1/C2）之后已经补上，别读成"本类整体在 ctest 里零覆盖"**：

- `played_us()` 在从未 `open()` 时返回上一次 `flush()` 的基准（从未
  `flush()` 则为 0）——连 `open()` 都不用调，是纯状态查询。
- `flush(base_us)` 在 sink 从未 `open()` 成功时仍然要能设置基准。
- `open()` 的参数校验（`sample_rate <= 0` / `channels <= 0` →
  `SYP_ERR_INVALID_ARG`）在任何硬件调用之前就返回。
- 真正需要设备、但**这台开发机上就有**的"接线"回归（`played_us()`
  是否真的把 `flush()` 快照的值用上了，而不是传字面量 0 或者干脆没
  快照）：写成`open()` 失败就 `skip`（不算失败）的条件用例，不会让
  CI 在没有音频设备的机器上变得不确定——`tests/test_audio_unit_sink.cpp`
  的 `object_level_flush_snapshot_and_underrun_signal_on_real_hardware`。
- 第三层折算（`swr_get_out_samples()` 预检必须在 `swr_convert()` 之前）
  的顺序回归——修复轮 2/5 补上，见 #21"落地"一节，
  `swr_precheck_ordering_same_frame_retry_exposes_phantom_consumption`。
  同样是"`open()` 失败就 skip"的形状，但数值裕度耦合了 libswresample
  内部预热瞬态，FFmpeg 升级时需要重新验证（`docs/tech-debt.md`）。

上一版本条遗漏的正是这份盘点——只论证了"为什么这一层大部分测不了"，
没说清"这一层里哪些其实测得了"，审查（opus）指出这种写法会被后来者
读成"这个文件整体不可测"的免责声明，M1b（`played_us()` 里传字面量 0
而非 `base_consumed_bytes_`）的存活正是这种误读的代价——那条变异体
在**纯函数层面**（`compute_played_us`）测得很仔细，却没人测过
"`played_us()` 成员函数有没有真的把这两个参数接对"这件事本身，而这件
事其实不需要新的基础设施，只是没人把它当成"应该补"的那一类。

**仍然没有、且明确不在本条待办里的**：`open()` 里的硬件初始化路径本身
（`AudioComponentInstanceNew`/`AudioUnitInitialize`/
`AudioOutputUnitStart` 会不会失败、失败时的清理是否正确）、
`render_cb` 在真实回调驱动下的时序行为、析构与正在运行的回调之间的
竞争——这三段仍然是"不是要不要修，是要不要投入去把它变得可测"的问题，
见下段。

**不是要不要修的问题，是要不要投入去把它变得可测的问题**：真要交叉
覆盖，需要类似 dl 层 `test_apple_http_backend.cpp` 那种"真实后端 + 本地
loopback"的思路，音频这边大概率要接一个虚拟音频设备（如 macOS 的
`BlackHole`/自建 `AudioServerPlugIn`）才能在无人值守的 CI 上确定性地
喂/收样本——这本身是一项独立的基础设施投入，评估值不值得做是后续任务
的事，不在本条里裁定。

**M6d 补记（Task 8，2026-09-21）**：新增的几何/音量逻辑延续同一策略——
`blit_transform()`/`display_size()`（`src/media/video_geometry.h`）与
`AudioUnitSink::apply_gain()`/`advance_gain()` 都是平台无关或不碰硬件的
纯函数，各自被 `test_video_geometry.cpp`/`test_audio_unit_sink.cpp` 直接
单测，覆盖到了"算法本身对不对"。但两处仍有本条描述的同一种盲区留在硬件
薄层里：

- **`MetalRenderer::present()` 实际传给 `encode_blit()` 的参数不可回读**
  （`test_metal_renderer.cpp` 的 `debug_blit_to_bgra()` 走的是与生产
  `present()` 共用的 `encode_blit()`，但只在测试缝里手动构造调用；挂
  `CAMetalLayer` 时生产路径实际算出的 `BlitParams` 是否与测试缝里断言的
  一致，只有 `layer_present_with_rotation_and_fill_smoke` 这一条冒烟，
  没有像素级断言）——M6d Task 3 minor (deferred) 登记，本次收尾确认仍
  成立。
- **`render_cb` 里两条变异存活**：删掉 `apply_gain()` 调用、或丢弃它的
  返回值不写回 `ioData`，两者在 M6d Task 4 的变异验证里都**未被现有用例
  杀死**（`apply_gain()` 本身的纯函数用例覆盖了"算得对不对"，但没有用例
  断言"`render_cb` 真的把算出来的样本写回了输出缓冲区"）。这与本条第一条
  的性质相同：真正需要硬件回调驱动的那一段，纯函数覆盖天然够不到。

两条都不是新盲区，是本条既有论证在 M6d 新代码上的重演，登记不修——同一份
"薄到没地方藏逻辑"的论证仍然成立，代价是这两处的正确性目前只靠审查读代码
兜底。

## 25. iOS 上 `AudioUnitSink` 的设备延迟——已接入 `AVAudioSession`，仍无真机数值验证 🟡 已落地（M3b Task 7），真机验证前风险未完全消除

**M3b Task 7 更新**：iOS/Catalyst 分支已经改用
`AVAudioSession.sharedInstance.outputLatency` + `.IOBufferDuration`
换算成 us，经 `AudioUnitSink::compose_device_latency_us()`（负值按 0、
饱和相加、钳位 `[0, 500000]`，`tests/test_audio_unit_sink.cpp` 覆盖）
合成进 `device_latency_us_`（现为 `std::atomic<int64_t>`），不再"完全
不扣"。`AVAudioSessionRouteChangeNotification` 触发时会在通知线程重新
查询并原子写回。以下是本条修复前的原始记录，改动完成之后仍然成立的
部分是**最后一段**——本机（M3b 的目标设备同样是 macOS）没有 iOS 真机/
模拟器可以验证这条路径读到的数值是否正确，`tools/check-deploy-
target.sh` 仍然只验证语法/availability，不验证数值。

**背景**：审查（opus）修复轮 1/5 C4。`AudioUnitSink::query_and_log_
device_latency()` 在 macOS 上读三个分量（`kAudioUnitProperty_Latency` +
CoreAudio HAL 的 safety offset + buffer frame size）；iOS 上后两个分量
曾经恒为 0——不是权限问题，是 `CoreAudio/CoreAudio.h` 这整条头文件路径在
iOS SDK 上根本不存在（`tools/check-deploy-target.sh` 用真实 iOS SDK
`-fsyntax-only` 验证过这一点：`#include <CoreAudio/CoreAudio.h>` 在
`arm64-apple-ios13.0` target 下直接 `fatal error: file not found`）。

**量化后果**（本条要解决的正是"只说恒为 0，没说后果"这个信息缺口）：

- iOS 真实输出延迟大致等于 `AVAudioSession.outputLatency` +
  `.ioBufferDuration` 两者之和，默认档位典型量级 **10~40ms**（随设备
  型号、当前音频会话类别/模式浮动）。
- `AudioUnitSink::played_us()` 因此会**系统性高报** 10~40ms——公式里
  `effective = consumed_frames - latency_frames`，`latency_frames`
  在 iOS 上永远比真实值小，`effective` 永远偏大，`played_us()` 的读数
  永远比真实播放位置靠前。
- 下游影响：`TrackPlayer` 拿 `played_us()` 当音频主时钟，视频侧按
  `diff = pts - now_us()` 判断早到/迟到——`now_us()` 系统性偏大意味着
  视频会被**系统性判成"迟到"**，比实际应该呈现的时刻更早被交付/丢弃。
  `kPresentWindowUs = 40000`（40ms）：10~40ms 的偏移量吃掉这个窗口
  **25%~100%**，且是恒定偏向同一侧（不是抖动噪声、不会自己抵消）。

这正是 spec 第 11 节点名的那类最危险的风险："设备延迟常数取错会让同步
稳定地错，而自动化测不到"——iOS 上现在是"完全不扣"，不是"扣得不够
精确"。

**（M3b Task 7 之前）现状靠什么守住**：什么都没有。`tools/check-deploy-
target.sh` 只验证 `TARGET_OS_IPHONE` 分支能**编译通过**，不验证也无法
验证这条路径在真实 iOS 设备上的数值是否正确——这是纯编译期语法检查，
没有链接、没有运行。本任务（M2b Task 8）的目标设备是 macOS（见
roadmap.md 的裁定），iOS 落地在这之后，还没有发生。

**M3b Task 7 之后仍然遗留**：`tools/check-deploy-target.sh`（`-fsyntax-
only`，用真实 iOS SDK 头，覆盖 `AVAudioSession` 代码路径的语法/
availability）、`cmake --build`（macOS 构建+单测）、iOS demo/Catalyst
demo 的 `xcodebuild`（编译+链接，`CODE_SIGNING_ALLOWED=NO`，不运行）
三条都通过，但没有一条真正在 iOS 设备/模拟器上**跑起来**读一次真实的
`AVAudioSession.outputLatency`/`.IOBufferDuration`——本仓库目前没有
iOS 侧的 ctest 基础设施，这条本身也要先解决。"典型 10~40ms"仍然是
来自公开文档的估计值，不是实测读数。真机验证之前，`compose_device_
latency_us()` 的合成公式（钳位、饱和相加、负值兜底）本身有单测覆盖，
但"喂给它的这两个 session 分量到底对不对"这一半仍然未经验证。

**M3b Task 8 补记**：`AVAudioSessionRouteChangeNotification` 触发时
`AudioUnitSink::open()` 里注册的路由变化 block（只捕获 `LatencyState` 的
`shared_ptr`，调静态的 `AudioUnitSink::recompose_session_latency(*state)`）在通知线程
**当场**重新读 `outputLatency`/`.IOBufferDuration` 并原子写回，隐含假设
是"通知发出的那一刻，这两个属性已经反映了新路由的真实值"。这个假设本身
未经真机验证——蓝牙设备协商延迟量级（几十到几百毫秒）显著大于有线耳机，
`AVAudioSessionRouteChangeNotification` 的官方文档没有承诺"属性一定已经
按新路由结算完毕才发通知"这件事；如果蓝牙路由切换时通知先于属性结算到达，
本次重算读到的会是旧路由（或过渡态）的延迟值，要等下一次触发（如果有）
才纠正。没有蓝牙真机/耳机可以验证这个时序，登记为待办：真机验证时应
在蓝牙路由切入/切出的瞬间连续采样 `outputLatency`，确认通知触发时刻与
属性稳定时刻的先后关系。

**fix round 1 追加记录**：

- `AVAudioSessionRouteChangeNotification` 观察者的"注册 → 重复
  open()/析构 → 注销"这一整条生命周期，本机（macOS 开发机）没有
  `AVAudioSession` 真机环境可以触发一次真实的路由变化通知，因此这条
  路径**没有任何自动化覆盖**——`tests/test_audio_unit_sink.cpp` 只能
  验证 `compose_device_latency_us()`/`seconds_to_us_clamped()` 这两个
  不碰 `AVAudioSession`/`NSNotificationCenter` 的纯函数，验证不了"注册
  的 block 到底会不会被正确调用"、"重复 open() 之后旧 block 是否真的
  只写旧状态不写新状态"这类真正需要触发通知才能观察到的行为。
  `AudioUnitSink::LatencyState` 靠 `shared_ptr` 生命周期而不是"靠人验证
  时序"来保证安全（见 `audio_unit_sink.h`/`.mm` 里 fix round 1 的注释），
  但这终究是"设计上论证安全"，不是"跑起来验证过"。
- 只观察了 `AVAudioSessionRouteChangeNotification`，没有观察
  `AVAudioSessionMediaServicesWereResetNotification`（音频服务被系统
  整体重置，比如来电打断后 daemon 崩溃重启）——这类事件之后
  `AudioUnit`/`AVAudioSession` 的属性读数、乃至 `AudioUnit` 本身可能都
  已经失效，理论上也需要重新查询延迟甚至重新 open()。本任务范围明确是
  "延迟三分量 + 路由变化"，`MediaServicesWereReset` 的处理（大概率不只是
  重算延迟，还涉及重建整个 AudioUnit）留作单独的已知缺口，不在这里
  顺带做。

**M3b 终审补记（I1）：路由变化让音频时钟在运行中阶跃**。`played_us()` 按
"已消费样本 − 设备延迟"计算，路由变化 block 把新的延迟原子写回后，下一次读数立刻
按新延迟扣——音频时钟跟着延迟差 Δ 阶跃，没有任何平滑：

- **有线 → 蓝牙（延迟变大，时钟往回跳 Δ）**：物理上是对的（耳朵真正听到的声音确实
  更晚了），但视频侧会看到时钟倒退：已经按旧时钟呈现过的位置之后的帧全部变成"早了"，
  画面冻结约 Δ（蓝牙 Δ 常见几十到上百毫秒）；追帧的迟到丢帧窗口
  （`late_drop_times_us_`，按媒体时间记）里留着倒退前的时间戳，窗口裁剪按
  `now − front > 1s` 判，时钟倒退后这些旧记录要多挂 Δ 才会被清掉。
- **蓝牙 → 扬声器（延迟变小，时钟往前跳 Δ）**：原本按时的帧瞬间变成迟到，Δ 超过
  `kDropThresholdUs`（80ms）时会连续迟到丢帧，1 秒内达到 5 次就会**误开追帧**
  （之后连续 30 帧按时才关闭）。
- **不修的理由 / 方向**：把 Δ 平滑掉（按小步长 slew 到新延迟）属于 M3c"缓冲水位与卡顿
  判定"的时钟平滑范畴，本轮不做。**不能**把 Δ 吸收进时钟基准（flush 时补偿 Δ）——那会
  留下一个永久的 A/V 偏移（时钟不再等于"耳朵听到的位置"）。已登记 `docs/tech-debt.md`。
- 同样没有真机/蓝牙设备验证 Δ 的实际量级与阶跃对画面的观感。

**M3b 终审补记（M2）：macOS 不观察默认输出设备变化**。原生 macOS 分支只在 `open()` 里读
一次 HAL 分量（`kAudioHardwarePropertyDefaultOutputDevice` 的 safety offset + buffer
frame size），没有注册 `kAudioHardwarePropertyDefaultOutputDevice` 的属性监听——播放中把
Mac 的默认输出切到 AirPods 之类的蓝牙设备，延迟分量不会更新，要等下一次 `open()`；
而 iOS/Catalyst 经 `AVAudioSessionRouteChangeNotification` 会重算。两端不对称，登记在此，
不在本轮修。（另：`AudioUnit` 本身在默认设备切换后的行为——跟随新设备还是停在旧设备——
同样未验证。）

**M3b 终审补记（deferred T7）：注册观察者前后的窗口已关闭**。初始 session 读数
（`query_and_log_device_latency()`）发生在注册路由变化观察者**之前**，两者之间发生的
路由变化此前没有人接收，延迟会停在旧值直到下一次路由变化。现在 `open()` 注册观察者之后
立刻再调一次 `recompose_session_latency(*state)` 补读、写回合成总和
（`src/platform/apple/audio_unit_sink.mm` open() 注册处）。同样没有自动化覆盖（需要真实
`AVAudioSession`）。

**M3c 补记：路由变化的时钟阶跃已缓解（M3c 斜坡），真机观感仍未验证**。上面"M3b
终审补记（I1）"记录的"路由变化让音频时钟在运行中阶跃 Δ"，M3c 已经用限速斜坡处理：
`AudioUnitSink` 保存"目标延迟"（路由变化 block 原子写入）与"已生效延迟"，
`played_us()` 读数时按单调时钟把已生效延迟以 `kLatencySlewRate`（0.1，即最多
100ms/s）向目标逼近，并加读数单调护栏（`flush()`/`open()` 时随基准一起复位）——
音频时钟不再瞬间阶跃，而是在约 Δ/0.1 秒内平滑过渡到新值，暂停期间斜坡不推进。
回归用例 `slew_latency_keeps_played_us_monotonic_across_route_change`
（`tests/test_audio_unit_sink.cpp`），配合斜坡限速/到达即停/单调性/暂停不推进等纯
函数用例。**这只是缓解了"阶跃"这个具体问题，不是解决了"没有真机验证"这条**——
斜坡数学本身有单测钉住，但"100ms/s 这个速率人耳是否真的察觉不到"、"蓝牙路由切换
的实际 Δ 量级"仍然只能靠 iOS/蓝牙真机验证，本机没有这个环境，见 #62。

## 26. ~~`MetalRenderer` 结构性拿不到真实 `colorspace`/`color_range`，"推定"分支恒为真~~（已修复，M2b Task 9.5）🟢

**背景**：task-9-brief.md（M2b Task 9）要求 `MetalRenderer` 的色彩矩阵
"从 `AVFrame` 的 `colorspace`/`color_range` 取，取不到时按分辨率推定并
记日志"。落地时发现：`src/media/frame.h`（`Frame`，M2a 已定型，Task 9
的任务边界明确禁止改动）只暴露 `width()/height()/pix_fmt()/plane(i)/
stride(i)` 四个视频访问器，**没有 `colorspace()`/`color_range()`**——
`AVFrame::colorspace`/`AVFrame::color_range` 这两个字段在 `Frame` 的公
开缝上完全不可见。`IVideoRenderer::present(const Frame&)`（缝 ②，Task 4
定的接口，同样不在本任务改动范围）只传一个 `Frame`，拿不到底层
`AVFrame*`。

**实际后果**：`MetalRenderer::present()` 结构性地只能恒以
`AVCOL_SPC_UNSPECIFIED`/`AVCOL_RANGE_UNSPECIFIED` 调用
`select_color_matrix()`——brief 原文"取不到时按分辨率推定"这条分支，在
当前接线下**永远**是唯一会被走到的分支，不是"多数情况读到真值、少数
情况推定"。

**审查（opus）修复轮 1/5 指出：这条记录最初把矩阵（`colorspace`）与
范围（`color_range`）两半的严重程度写得一样重，是低估——两者的兜底
质量、错判后果、触发概率都不对称**：

| | 矩阵（`colorspace`） | 范围（`color_range`） |
|---|---|---|
| 取不到时的兜底 | 按分辨率推定，**对典型内容大多正确**（`≥720p` 用 BT.709 这条经验规则命中率不低） | **没有任何启发式**——`select_color_matrix()` 里 `color_range` 取不到时恒定写死 limited，不看分辨率、不看任何其它信号 |
| 错判的后果 | 整体偏色（色调偏移） | 黑位抬升 / 高光削波（对比度失真），**视觉上比偏色更明显、更容易被普通观众发现** |
| 常见触发内容 | 元数据与分辨率不匹配的老素材（相对少见） | mjpeg、摄像头采集、`full_range=1` 标注的 AVC 内容——**不罕见** |

**M10 变异（审查提出、本轮已独立复现，见 task-9-report.md）实锤了
`full_range` 这一半的接线现状**：把 `metal_renderer.mm` 里送进
`ColorParamsGpu` 的 `full_range` 字段硬编码成 `0u`（完全忽略
`impl_->cached_choice.full_range` 的真实值），`tests/test_metal_
renderer.cpp` 全部 24 条用例（含真的碰 GPU、读回渲染结果的那几条）
**依然全绿**。原因很直接：当前所有测试帧、以及生产接线下的所有真实
帧，都通过恒为 `AVCOL_RANGE_UNSPECIFIED` 的路径把 `full_range` 推定成
`false`（0）——跟这条硬编码的错误实现产出完全相同的值，测试数据无法
区分"正确地推定出 limited"与"压根没看 `cached_choice`、写死了
limited"这两种实现。**`yuv420p.metal` 里 `if (params.full_range == 0u)
... else ...` 的 `else` 分支（full range 反归一化公式），从 CPU 的
`select_color_matrix()` 到 GPU 的着色器，端到端是死代码**——不是"测试
覆盖率低"，是"这条路径在当前系统里除了理论上存在、没有任何输入能激活
它，也没有任何断言在守它"。

对当前素材矩阵（M2a `syp_probe --stats` 确认过是 `yuv420p` 无特殊
colorspace/range 标注）实际影响有限；但**一旦接入的真实素材是
full-range 编码（比如很多摄像头直出的 H.264/mjpeg 流），当前实现会把
它当 limited-range 处理，画面对比度失真，且不会有任何自动化用例
发现**——这正是这次把评级从 🟡 上调到 🟠 的原因：矩阵那一半错判的后果
是"色调偏移"，范围这一半错判的后果是"看起来明显不对"，触发它的内容
类型也更常见。

**现状靠什么守住**：

1. `select_color_matrix()` 本身是不受这条接线限制影响的纯函数，接受
   任意 `(colorspace, color_range, width, height)` 输入——
   `tests/test_metal_renderer.cpp` 第 1 段用真实的 `AVCOL_SPC_BT709`/
   `AVCOL_SPC_BT470BG`/`AVCOL_SPC_SMPTE170M`/`AVCOL_RANGE_JPEG`/
   `AVCOL_RANGE_MPEG` 等值完整测过"读到真值就用真值"这条判定逻辑，逻辑
   本身是对的、也测过——缺的只是"从真实 `Frame` 读出真值再传进来"这
   一步。M10 打穿的不是这个纯函数，是"纯函数算对了、GPU 端有没有真的
   用上"这条接线，见上面的复现记录。
2. `presumed` 标志本身照常打日志（`SYP_LOG_WARN`），且这条日志在当前
   接线下会对**每一帧不同尺寸的素材**都出现一次——不是被吞掉的沉默
   推定，人工/demo 壳阶段可以看见"这条渲染路径目前是靠推定撑着的"这个
   事实。但日志只说"range=limited [presumed]"，不会告诉你"如果真实
   素材其实是 full range，这条日志之后的画面对比度是错的"——日志能
   提示"这里不确定"，不能提示"这个不确定的后果有多明显"。
3. 本条第一天就记（M2b Task 9 落地的同一个提交里立案，本轮修复轮
   1/5 按审查意见重写严重程度），不是先假装"以后补"再遗忘——跟
   #17/#24 同一条方法论。

**修复（Task 9.5，专门解禁 `src/media/frame.h`/`.cpp` 的这两个访问器
落地此条）**：

1. `Frame` 加了 `colorspace()`/`color_range()` 两个 `int32_t` 访问器
   （`src/media/frame.h`/`.cpp`），逐字照抄 `pix_fmt()` 的写法：直接
   映射 `AVFrame::colorspace`/`AVFrame::color_range`，`av_` 为空时分
   别返回 `AVCOL_SPC_UNSPECIFIED`/`AVCOL_RANGE_UNSPECIFIED`。纯追加，
   不改任何已有签名/语义/布局——`Pipeline`/`TrackPlayer`/既有测试一行
   未动；`Frame` 不在 `include/syplayer/` 公开缝里，跟 `check-abi.sh`
   无关；`frame.cpp` 已在 `check-deploy-target.sh` 扫描列表里，自动
   纳入（复跑确认过）。
2. `MetalRenderer::present()`（`src/platform/apple/metal_renderer.mm`）
   把两行硬编码的 `AVCOL_SPC_UNSPECIFIED`/`AVCOL_RANGE_UNSPECIFIED`
   换成了 `f.colorspace()`/`f.color_range()`；`select_color_matrix()`
   本身一个字没改——纯函数早就就绪，等的只是真实输入。
3. `tests/test_metal_renderer.cpp` 第 3 段补了两条接线回归用例（强制
   交付物，直接对应上面严重程度表的两行）：
   - `object_level_present_uses_real_colorspace_over_resolution_
     presumption`：SD 尺寸（640x480，分辨率推定会给 BT.601）但显式
     `AVFrame->colorspace = AVCOL_SPC_BT709`，回读像素确认实际用了
     BT.709（跟独立手写的标量参考实现 `reference_yuv_to_rgb` 比对），
     且确认没有落回分辨率推定的 BT.601。反向自检：把 `present()` 的
     接线改回硬编码 `UNSPECIFIED`，这条用例真的红了。
   - `object_level_present_uses_real_full_range_from_avframe`：显式
     `AVFrame->color_range = AVCOL_RANGE_JPEG`，回读像素确认走了
     full-range 反归一化路径（结果与 limited-range 期望值不同）。
     反向自检：复现 M10（`full_range` uniform 硬编码 `0u`），这条
     用例真的红了、且只有这一条红——证明它确实是唯一能杀掉 M10 的
     断言，不是碰巧跟着别的用例一起红。
   两次反向自检都用 `cp → 注入 → 重编（确认该 TU 真的被重新编译）→
   跑（贴实际输出）→ mv 恢复 → touch → 重编 → 跑绿 → git diff 干净`
   这套流程。

### 终审复核补记（M2b 收尾，2026-09-11）：修复只覆盖了每个分辨率的第一帧

上面「已修复 🟢」这个标题在本次终审之前**只对每个分辨率的第一帧成立**。
Task 9.5 把 `f.colorspace()`/`f.color_range()` 真接上了，但
`metal_renderer.mm` 里色彩矩阵缓存的键**没跟着改**，仍然只有
`(width, height)`——同一个 `MetalRenderer` 实例、同一个分辨率下，第 2 帧
起无论 `colorspace`/`color_range` 怎么变，都会沿用第 1 帧算出来的矩阵。

终审的像素级实测（零变异探针，同一个 renderer，全部 640x480）：

```
#1 cs=UNSPEC           → rgba = 213 19 203   （推定 BT.601 limited）
#2 cs=BT709            → rgba = 213 19 203   ← 沿用第 1 帧的矩阵
#3 cs=BT709 range=JPEG → rgba = 213 19 203   ← 同上
对照：全新 renderer 同参数 → 227 48 208 / 214 56 197
```

这条缺陷躲过 Task 9.5 那两条新用例的原因是结构性的，跟 #24 是同一种形状：
**当时没有任何一条用例在同一个 `MetalRenderer` 上改变过
colorspace/color_range**——绝大多数用例各建一个新 renderer，缓存里永远
是空的、命不中沿用路径；唯一复用同一个 renderer 的
`object_level_color_matrix_choice_is_logged_and_cached_per_size` 只测了
"尺寸变了要失效"，反方向（尺寸不变、色彩参数变）零覆盖。
真实播放里这条路径反而是常态——一次播放通常只有一个分辨率、成千上万帧。

**终审修复（本轮）**：
- 缓存键补上 `colorspace`/`color_range`（`Impl::cached_colorspace`/
  `cached_color_range`），跟宽高那一半合起来才是 `select_color_matrix()`
  的完整入参集合。
- `Impl::cached_choice` 上方那段"present() **目前恒以** UNSPECIFIED 调用
  ……结果因此只随分辨率变化"的注释是 Task 9 时点的事实、Task 9.5 之后即
  失实，已改写。
- 新增对象级回归用例
  `object_level_same_size_different_colorspace_invalidates_matrix_cache`：
  **全程一个 renderer、全程 640x480**（用例形状本身就是判据的一部分），
  四帧依次 UNSPEC → BT709 → BT709+JPEG → 回到 UNSPEC，每帧都做像素级
  回读断言，并断言"上一帧那套颜色不成立"。反向自检（把缓存键退回只有
  `(w,h)`）：三条断言当场变红。

**现在靠什么守住**：`select_color_matrix()` 的判定逻辑本身仍由第 1 段
纯函数用例覆盖；"缓存键是否完整"由上面那条同 renderer / 同尺寸的新用例
覆盖（它和 `object_level_color_matrix_choice_is_logged_and_cached_per_size`
是本文件仅有的两条复用同一个 renderer 的用例，各守一个方向：那条守
"尺寸变了要失效"，这条守"尺寸没变但色彩参数变了也要失效"）；上面两条新用例把"真实 `AVFrame` 读出的
colorspace/color_range 有没有真的传下去、有没有真的影响 GPU 输出"这条
此前完全没有代码路径可测的接线补上了自动化回归——尤其是
`color_range`/`full_range` 这一半，此前 24 条用例（含真碰 GPU 的）对
M10 全部无感，现在有且仅有新增的那一条会为它变红。四套构建（Debug/
Release/ASan+UBSan/TSan）+ `check-abi.sh`/`check-deploy-target.sh`/
`syp_dl_purity_check` 全部过；四条禁改路径（`src/dl/`、
`include/syplayer/`、`src/media/pipeline.*`、`src/media/track_player.*`）
`git diff --stat` 确认为空。

---

## 27. ~~interrupt 回调只接在 HLS 这一条路径上，`create_file()`/`create_avio()` 仍然没有~~（已修复，2026-09-14）🟢

**原状**：`Pipeline::request_abort()` 只对 `create_hls()` 建的管线有效；
`create_avio()`/`create_file()` 上它是 no-op。demo 壳的 URL 播放走
`create_avio()`，网络挂住时泵线程卡在持 `mu_` 执行的 `step()` →
`av_read_frame()` → `syp_source_read()` 里无人能打断；M2b Task 10 为修 UAF
加的 `dispatch_sync` 又把这一点扩大成"主线程上的 `-close`/`-dealloc` 冻结到
网络层超时"。`syp_probe` 与 demo 壳两个独立消费者各自撞上过这个空白。

### 修复

| 打开路径 | 怎么打断阻塞 IO | 中止之后 `step()` |
|---|---|---|
| `create_hls()` | 不变：`HlsSession`（interrupt_callback + 分片 `AvioBridge` + 播放列表抓取） | `Error(SYP_ERR_CANCELED)` |
| `create_avio()` | **新增可选参数 `io_abort`**：`request_abort()` 调它。阻塞点在调用方的 `AVIOContext` 底下，Pipeline 够不着，只能由调用方交钩子（典型 `[b]{ b->request_abort(); }`）。没交钩子时正卡着的那次读打不断，要等读自己返回 | `Error(SYP_ERR_CANCELED)` |
| `create_file()` | 无需打断（本地读不长时间阻塞） | `Error(SYP_ERR_CANCELED)` |

- `Pipeline::step()` 入口先查 `aborted_`；demux Error 分支在 `aborted_` 时报
  CANCELED（被钩子打断的读以 `AVERROR_EXIT` 冒成 Error，不是 Eof——变异实测：
  去掉这条改写，AVIO 用例报 `SYP_ERR_IO`）。
- `TrackPlayer::request_abort()`：转发 + 自己入口也挡（手里有现成帧时
  `TrackPlayer::step()` 不一定调 `Pipeline::step()`；只转发不挡时一帧到点的帧
  照样被呈现，用例实测）。明示可与 `step()` 并发调用。
- 没有给 `create_file()`/`create_avio()` 设 `interrupt_callback`：这两条路径上
  没有 FFmpeg 代码会在阻塞处轮询它，设了也打断不了任何东西。

**demo 壳**（M4 起迁至 `swift/SYPlayerKit/Internal/SYPBridge.mm`）：`close_internal()` 先 `running_=false`，
再经 `abort_mu_` 保护的 `abortable_`（`TrackPlayer*` 副本，锁序
`abort_mu_ → mu_`，泵线程从不持 `abort_mu_`）调 `request_abort()`，然后才
`dispatch_sync` 等泵线程；泵线程对"已经不在运行时收到的 CANCELED"不上报
`onError`。URL 播放给 `create_avio()` 交 `AvioBridge::request_abort` 钩子。

回归：`test_pipeline` 的 `pipeline_request_abort_on_create_file_stops_step_with_canceled`、
`pipeline_request_abort_on_create_avio_unblocks_a_hanging_read`（自造阻塞
AVIOContext）；`test_track_player` 的
`request_abort_stops_step_even_with_an_on_time_frame_queued`；`test_hls_e2e` 的
`url_playback_request_abort_unblocks_a_pump_stuck_in_a_network_read`（逐字照
demo 的形状：真实 Apple 后端 + loopback 服务端每条响应发满 1MB 后停 60 秒 +
泵线程 + 另一线程中止）——实测中止后 1ms 返回；不交钩子（修复前 demo 的写法）
8104ms 才返回、用例变红。

**仍未覆盖**：
- **打开阶段不可中止**：`create_*()` 内部的探测（`avformat_open_input` /
  `find_stream_info`）期间还没有 `Pipeline`，调用方没有中止口。demo 壳里
  `-openURLString:` 在后台队列上跑，调用期间 Swift 侧持有 bridge 强引用，
  `-dealloc` 不会与它重叠，所以**不冻结 UI**，只是那条后台线程要等到网络层
  超时。
- demo 壳的改动没有在真机/模拟器上跑过，只验证了 iOS 与 Mac Catalyst 两个
  工程编译链接通过；机制本身由上面那条 e2e 用例按 demo 的形状覆盖。

## 28. demo Xcode 目标豁免 `-Werror` 🟢 记录在案的取舍

**背景**：M2b Task 10 的两个 demo 壳（`demo/ios/syplayer-ios.xcodeproj`、
`demo/mac/syplayer-mac.xcodeproj`）编译警告开关是 `-Wall -Wextra -Wshadow
-Wconversion -Wsign-conversion`（跟 `CMakeLists.txt` 里的 `syp_warnings`
INTERFACE 一致），但**没有** `-Werror`。理由见
`demo/generate_xcodeprojects.rb` 顶部注释：
三条"构建强制"纪律（`check-abi.sh`/`check-deploy-target.sh` 按路径扫描
`src/`；`-Wconversion -Werror` 通过 CMake 的 `syp_warnings` 只挂在
`syp_dl`/`syp_media`/`syp_platform_apple`/`tests` 几个目标上）明确不覆盖
这个独立 Xcode 工程；手搭的这套工程在评审窗口内没有反复试错 `-Werror`
的余地，选择只保留警告可见性。

**审查判定：可接受**——`swift/SYPlayerKit/Internal/SYPBridge.mm`（M4 前为
`demo/shared/bridge.mm`）、以及 `demo/` 两个工程里
引用的 `src/dl/`、`src/media/`、`src/platform/apple/` 源文件，**同一份
源码在四套 CMake 构建里依然带着 `-Werror` 编译过一遍**（Xcode 只是又编
了一份，target 不同、警告开关不同，但源码本身没有变），"零警告"这条
纪律本身没有被削弱。

**风险敞口（登记为债务，不是立即要修的缺陷）**：以后如果只在 Xcode 里
改动 `demo/shared/` 下的文件（不经过任何一套 CMake 构建重新编译），警告
可能悄悄积累而不会造成构建失败——跟"四条禁改路径 + 四套 CMake 构建"
这套纪律保护的范围不重叠的那一小块面积。如果以后要收紧，落脚点是给这
两个 Xcode 工程的 `WARNING_CFLAGS` 加上 `-Werror`，逐条清完当时的警告
再打开。

---

## 29. 变速点有一次可听断点 ✅ 有意接受

**现象**：`TrackPlayer::set_speed()`（`src/media/track_player.cpp`）的四步
顺序里，第 2 步是 `IAudioSink::flush(base)`，先于第 3 步真正调
`IAudioSink::set_speed(speed)`。落到 `AudioUnitSink::flush()`
（`src/platform/apple/audio_unit_sink.mm`）：先 `AudioOutputUnitStop`，
再 `ring_->reset()` 把环里已经转换好、还没被硬件消费掉的样本**整段丢弃**，
最后按原先的暂停状态决定要不要 `AudioOutputUnitStart` 回去。这意味着每次
调用 `setSpeed()`，播放中的那一刻都会有一小段已经排好队、马上要发声的
音频被直接扔掉，而不是被平滑地时间拉伸（time-stretch）过渡到新速率——
用户耳朵听到的是变速那一刻有一次短促的断点（静音或不连续），不是无缝
渐变。

**为什么现在这样做、且判定为有意接受**：无缝变速要求的是"样本数→媒体
时长"的精确映射——变速前后，环里/`SwrContext` 内部缓冲的那批样本对应的
真实媒体时长要能被连续追踪，才能在切换采样比例的同时不丢弃、不重复任何
一段音频。这需要一张跨越 flush 边界的分段映射表，量级上是重采样器的
一次结构升级，不是这一层能顺手做的（见 `docs/tech-debt.md` 对应条目，
已排进 M3）。M2b 的验收标准只要求"暂停/恢复/倍速正确"（数值上换算准确、
不影响后续同步判定），没有要求变速本身无爆音——`AudioUnitSink` 的
`played_us()`/`compute_played_us()` 在 flush 前后的算术是正确的（见
known-gaps #21 落地一节），断点只是**听感**层面的代价，不影响
`position_us()`/同步判定的正确性。

**现状靠什么守住**：没有自动化守护（断点是听感问题，当前测试矩阵不
覆盖音频输出的实际声音内容），也不需要——这是有意的取舍，不是缺陷。
将来 M3 如果要做无缝变速，落脚点在 `docs/tech-debt.md` 记的那张分段
映射表。

---

## 30. `Frame::pts_us()` 的「AV_NOPTS_VALUE 原样透传」不变量，今天几乎没有守护 🟢 技术债

**背景**（M2b 终审 m8）：`src/media/frame.cpp` 的 `from_av()` 里立着一条
不变量，原文：

> `AV_NOPTS_VALUE` 保持原样传递，不要换算成一个看起来合法的数字 ——
> 上层需要能分辨「没有 pts」与「pts 是 0」。

M2a 定下这条时还没有任何上层。**M2b 是第一个真有上层的里程碑**，而唯一的
新消费者 `TrackPlayer` 三处用 `pts_us()`（音频 `Queued` 的返回值、
`just_sought_` 分支、视频三分支的 `diff` 计算）**全部不分辨**这两种情况。
后果是这条不变量当前基本是空立的：审查变异 M1（把 `from_av()` 改成
`AV_NOPTS_VALUE` 也透传成 0）**全绿存活**。

**这条不变量目前靠什么守**：

1. `TrackPlayer::sanitize_position()` + 它的三个调用点（`seek()` 的
   `base`、`just_sought_` 分支的 `flush()`/`set_base()`、`set_speed()` 的
   `base`）。终审 C1 第 2 步又补了一个：`just_sought_` 分支里
   `last_known_position_us_ = sanitize_position(pts)`（此前是裸 `pts`，是
   全文件唯一一处不过钳位就写进兜底源的赋值）。**这是唯一一处真的会因为
   "分辨得出没有 pts"而走不同分支的代码**——M1 变异之下它会退化成恒等，
   但至少这条路径今天存在且有注释说明。

   **钳位本身现在有守卫了（终审复审 m-1）**：此前它是零覆盖的——把
   `sanitize_position()` 整个删成 `return v;`，全量 ctest 23/23 全绿。
   现在 `tests/test_track_player.cpp` 的
   `set_speed_while_sink_failed_does_not_flush_a_sentinel_base` 与
   `seek_to_sentinel_timestamp_does_not_flush_a_sentinel_base` 各钉死一个
   调用点（`set_speed()` 的 `sanitize_position(position_us())`、`seek()` 的
   `sanitize_position(ts_us)`），同一个删除变异现在会让 6 条断言变红。
   观测出口是 `IAudioSink::flush(base)`——钳位后的值经它递给 sink，
   `FakeAudioSink::last_flush_base_us()` 把它读出来。
   仍然没有守卫的是第三个调用点（`just_sought_` 分支的
   `sanitize_position(pts)`），原因见下面"还差什么"第一条：今天造不出
   无 pts 的帧。
2. 别的什么都没有。三处 `pts_us()` 里另外两处（返回给调用方的 `pts_us`
   字段、`diff = pts - clock_->now_us()`）都直接把 `AV_NOPTS_VALUE` 当数字
   用——后者正是终审 C1 那条有符号整数溢出的另一半原料。

**还差什么**：

- **一份能产出无 pts 帧的素材**。今天不可达：本仓库 FFmpeg 只
  `--enable-demuxer=mov`，mov 恒有 pts，`from_av()` 里那条 `AV_NOPTS_VALUE`
  分支在整个测试矩阵里一次都没被执行过。要真正守住这条不变量，需要能构造
  一帧 `pts == AV_NOPTS_VALUE` 的 `Frame`——要么放开解封装器集合（会引入
  一整批无关变量），要么给 `Frame` 加一个测试专用构造口子（这个仓库对
  "为测试新增接口"一贯保守，见 `track_player.h` 顶部第 2 条理由）。
- **`diff` 那条减法的定位**。终审 C1 之后它不会再拿到失效时钟的
  `AV_NOPTS_VALUE`，但 `pending_video_->pts_us()` 侧仍然可能是哨兵值（只是
  今天不可达）。真要收口，`step()` 的视频分支需要一条"这一帧没有 pts 怎么
  办"的明确策略（当作立即呈现？丢弃并计数？），而 spec 第 5 节今天没有
  定义它。

**判定**：不阻塞 M2b 合并——触发路径今天结构性不可达（解封装器集合限死在
mov）。一旦解封装器集合扩大（M3 之后很可能），这条要跟 spec 第 5 节的
"无 pts 帧策略"一起做，不能只补一个钳位了事。

---

## 31. demo 壳的残余 UAF：回调内释放最后一个强引用会让 `~PlayerCore` 嵌套在 `pump_loop()` 栈里 🟠 调用方约束，未修

**背景**（M2b 终审 m13）：Task 10 修复轮 1/5 修掉的那条 UAF（泵线程睡在
`usleep()` 里、调用方释放 `PlayerCore`，见 roadmap「M2b 抓到的真实缺陷」
第 3 条）之外，还剩一层**结构上更深**的：如果 `close_internal()` 是从
**泵线程自己身上**被调用的——典型触发是某个 `onFrame`/`onEof`/`onError`
回调里把 `SypPlayerBridge` 的最后一个强引用释放掉——那么整条析构链会嵌套
在 `pump_loop()` 自己的调用栈里：

```
pump_loop() → step() → present() → onFrame 回调 → dealloc
  → close_internal()（检测到"我就在泵队列上"，跳过 dispatch_sync，正确）
  → 返回 → .cxx_destruct → ~PlayerCore() → delete
  → 栈继续回溯到 pump_loop() 的 while 循环，此时 `this` 已经是悬空指针
```

`close_internal()` 里跳过 `dispatch_sync` 是**对的**（对自己所在的串行队列
`dispatch_sync` 会自死锁），但它解不了这一层：问题不是"等不等泵线程"这类
队列同步问题，是**对象在自己的调用栈还活着的时候被释放**。

**为什么现在只记不修**：两个 demo 壳目前都不会这么用（`PlayerViewController`
持有 `SypPlayerBridge` 的强引用，回调里只 `dispatch_async` 回主线程更新 UI，
不释放任何强引用）。真要修，路子是让 `pump_loop()` 在每轮开始时持有一个自己
的强引用（`__strong SypPlayerBridge*` 或等价的 `shared_ptr<PlayerCore>`），
把"泵循环还在跑"这件事变成一条真实的所有权边而不是一个裸 `this`——这会改动
`PlayerCore` 的所有权模型，属于 demo 壳的一次结构调整，不该塞进终审修复轮。

**此前记在哪里**：只活在当时的 `demo/shared/bridge.mm` 的注释（`close_internal()`
那段"例外"里）与 `task-10-report.md`，**没有进 `known-gaps`/`tech-debt`**
——终审 m13 点名的正是这件事：一条已知的残余 UAF 只写在源码注释里，等于
没有立案。

**调用方约束（当前唯一的守护，靠纪律不靠代码）**：**不能在
`onFrame`/`onEof`/`onError` 回调里释放持有 `SypPlayerBridge` 的唯一一个
强引用。** 这条写在 `SYPBridge.h`（M4 前为 `bridge.h`）的接口注释与
`SYPBridge.mm:226-227` 附近的实现注释里（`SYPBridge.h` 才是调用方真正会读的
那个文件，所以两处都要有），但没有任何自动化手段强制。

**M4 补记（2026-09-16）**：`swift/SYPlayerKit/SYPlayer.swift` 这个 Swift 门面
把这条约束**结构性消除**了——桥的 `onEof`/`onError` 先 `Task { @MainActor }`
再投递给业务（delegate/`stateStream`/`@Published`），业务回调栈里已经没有
`pump_loop()`；`SYPlayer` 自己对桥持强引用，block 只捕获 `[weak self]`。
本条风险因此收窄到**唯二两类调用方**：demo 两个 App（已确认不再直接引用
`SypPlayerBridge`，grep 断言钉住）；以及任何绕过 `SYPlayer`、直接拿
`SYPlayerKit_Private` 模块写代码的假想调用方——理论上仍然存在，但仓库内
没有这样的调用方。

**现状靠什么守住**：什么都没有。`build-asan` 下现有的 demo 冒烟驱动不构造
这个场景（它在**外部**释放最后一个强引用，走的是已修的那条路径）。要覆盖
需要写一个"在 onFrame 回调里 `bridge = nil`"的驱动程序，届时应该先确认它
在未修实现上真的能在 ASan 下复现，再决定修法。

**2026-09-14 补记（代码审查指出，先于 #27 修复存在）**：`onFrame` 这条路径上
UAF 之前先撞上的是**自死锁**——`present()` 是在泵线程**持 `mu_`** 执行的
`player_->step()` 里调的，回调里释放最后一个强引用 → `-dealloc` →
`close_internal()` 在同一线程上再锁 `mu_`（`std::mutex` 不可重入）。#27 的
修复新增的 `abort_mu_` 不改变这一点（它先于 `mu_` 获取，泵线程从不持有它）。
调用方约束不变：别在 `onFrame`/`onEof`/`onError` 回调里释放持有 bridge 的
唯一强引用。

## 32. HLS：播放列表/分片按 URL 路径后缀分流，是启发式判据 🟡 M-HLS 设计取舍

`HlsSession::io_open()` 按 URL 路径部分的后缀（先剥掉 `?query` 与
`#fragment`）判定走播放列表通道（`.m3u8`/`.m3u`）还是分片通道（其余一律）
——见 `src/media/hls/url_rewrite.h` 的 `channel_for()`。HLS 规范不要求播放
列表以 `.m3u8` 结尾，后缀只是启发式，不是协议保证。

**判错的后果是可观测的，但不会自动报警**：把播放列表误判成分片 → 它会被
缓存 → 直播刷新失效（表现为直播卡在某一刻不再前进）；把分片误判成播放列表
→ 它会被整个读进内存且不缓存（表现为内存占用异常 + 重复下载）。两种后果
都要靠人观察到播放异常才会被发现，没有一条断言/日志会主动指出"分流判错了"。

**为什么不按 Content-Type 判**：那要求先发请求才能决定走哪条通道，而两条
通道的请求发法不同，是循环依赖。

**现在靠什么守**：`channel_for()` 在 `tests/test_hls_url_rewrite.cpp` 里被
穷举测过（大小写 scheme、query 里带 `.m3u8`、路径以 `.m3u8` 结尾后跟 `/`
等边界输入），保证的是"给定后缀，分流结果符合预期"；`test_hls_e2e.cpp`
的用例进一步用真实 fixture 证实播放列表确实零缓存、分片确实进缓存
（`vod_single_variant_plays_to_eof` 末尾那两条缓存断言）。

**还差什么**：没有任何机制能在"后缀判断本身就用错了"（比如真实 CDN 给了
一个不带 `.m3u8` 后缀的播放列表 URL）时发出警报——这类源会直接表现为上面
两种症状之一，需要人工诊断，没有自动化的"分流可能错了"检测。

## 33. HLS：单 scheme 假设——混合 scheme 的流会失败 🟡 M-HLS 设计取舍，方向安全

`HlsSession` 只在 `open()` 时记一份原始 master URL 的 scheme
（`real_scheme_`），`url_rewrite.h::to_real_url()` 还原时一律用这一份。
如果一条 HLS 流的播放列表是 `https://`、分片却是 `http://`（或反过来），
分片打开时会被当成与 master 同一 scheme 请求，导致连接失败。

**这是降级失败而非降级成功，方向是安全的那一侧**：把 http 资源当 https
请求会失败并报错，不会静默地明文传输；反过来把 https 资源当 http 请求
同样会失败（对方大概率不监听明文端口或直接拒绝），不会把本该加密的内容
明文发出去。

**现在靠什么守**：`to_real_url()` 本身的正确性在 `test_hls_url_rewrite.cpp`
里被穷举测过（`round_trip_is_identity_for_http_and_https` 等）；
`test_hls_e2e.cpp` 的 `https_master_is_rewritten_for_ffmpeg_and_restored_for_the_wire`
用可观测的副作用（明文 server 上零请求）钉住了"单一 https 流"这一种形状
下改写/还原确实接上了。

**还差什么**：没有任何 fixture 构造过"播放列表与分片 scheme 不同"这种
混合场景，所以"失败时的错误信息是否足够清楚地指向真因"从未被验证过——
用户看到的会是一次普通的连接失败，不会有"scheme 不匹配"这样的诊断提示。

## 34. HLS：直播长时间运行下的缓存淘汰未实测 🟡 M-HLS 未实测

分片 URL 唯一、只写不改，直播下持续产生新的分片会持续写入缓存目录，
靠已有的 `max_cache_bytes` + `min_free_space_bytes` 淘汰兜住——本里程碑
**没有新增任何淘汰机制**，也没有为它写过回归。

**现在靠什么守**：`max_cache_bytes`/`min_free_space_bytes` 是 dl 层 M1
就有的既有淘汰机制，对分片这种"只写不改、URL 永久唯一"的资源在道理上
应该适用（这正是分片被设计成走带缓存的 `syp_source` 通道的前提）。

**还差什么**：没有任何用例模拟过"直播持续运行数小时、产生远超
`max_cache_bytes` 的分片总量"这种场景，淘汰策略在这种持续增长的写入模式
下是否跟得上（会不会有一段时间窗口里淘汰速度跟不上写入速度、导致
`min_free_space_bytes` 被击穿）**没有数据**。

## 35. HLS：不支持 ABR（中途切流）🟢 M-HLS 显式排除的范围

多码率 HLS 源在打开时按 `HlsOptions::max_bandwidth_bps` 选定一档，整场
不换。真实弱网下会一直卡在选定的那档，不会像成熟播放器那样自动降档/升档。

**现在靠什么守**：这是 spec 第 1 节显式排除的范围（"ABR 中途切流"），不是
遗漏——需要带宽估计、切换点对齐、以及解码器应对分辨率中途变化，最后一条
会碰 M2b 刚落地的 `TrackPlayer`/`MetalRenderer`，独立成块更安全。

**还差什么**：整个 ABR 能力，留给未来一个独立里程碑。

## 36. HLS：`EXT-X-MEDIA` 分轨归组依赖 FFmpeg 内部行为，无独立验证 🟢 M-HLS 已知限制

分轨（音视频在不同播放列表里，本设计的目标源形状）的归组——哪条音频
播放列表属于哪个 variant 的 `AUDIO=` 组——完全由 FFmpeg 的 hls 解封装器
自己处理（`add_stream_to_programs`，`hls.c:2031-2053`），本项目不重新解析
`EXT-X-MEDIA` 标签，也没有独立于 FFmpeg 的归组校验。

**现在靠什么守**：`tests/test_hls_e2e.cpp` 的
`demuxed_source_binds_both_tracks_and_plays_both` 在 `vod_demuxed` fixture
上验证了归组的**结果**（选中的视频/音频轨确实各出一条、都出帧）——但这
只证明"FFmpeg 在这一份 fixture 上归对了"，不是"归组逻辑本身被独立验证过"。

**还差什么**：如果 FFmpeg 未来版本改变了归组行为（比如对畸形/边界情况的
处理方式），本项目不会有任何独立信号发现——只会表现为播放结果异常。

## 37. HLS：没有真机验证，只覆盖到 loopback 🟢 M-HLS 已知限制，与 M2b 同形状

自动化验证全部走 127.0.0.1 上的 `LoopbackServer`；真实 CDN 的重定向、
限速、连接复用（keepalive）、TLS 握手行为、以及混合内容/边缘案例的播放列表
形状，从未被实际跑过一次。

**现在靠什么守**：`http_persistent=0` 保证每次打开都经我们的 `io_open`
（见 `architecture.md` 第六节），所以即使真实 CDN 有连接复用行为，也不会
绕过 dl 层；`protocol_whitelist` 护栏保证"绕过不会静默发生"。这两条是
结构性保证，不依赖真机跑过。

**还差什么**：真实 CDN 常见的重定向链、限速导致的慢速下载、以及部分
CDN 在 `EXT-X-STREAM-INF`/`EXT-X-MEDIA` 上的非标准写法，都没有被验证过。
与 `docs/roadmap.md` M2b 判定里 iOS 真机从未运行过是同一类"验证到了什么
程度"的诚实记录，不是遗漏。

## 38. HLS：纯音频源的选轨回退分支与自引用纯音频 variant 覆盖不足 🟡 M-HLS Task 7

`HlsSession::select_variant()` 的规则是"候选先限定在含至少一条视频流的
program；仅当没有任何 program 含视频时才回退到全部 program 参选"（见
`architecture.md`/spec 第 6.2 节）。**"没有任何 program 含视频"这一支
（纯音频 HLS，比如播客）在当前测试矩阵里没有任何 fixture 能触发**——
`tools/gen-fixtures.sh` 生成的三套 HLS fixture（`vod_single`/`vod_multi`/
`vod_demuxed`）全部含视频轨。

另外，`vod_demuxed` 里那种"ffmpeg 给 `AUDIO=` 组音频轨额外写一条自引用
`EXT-X-STREAM-INF`"的形状（触发"必须先过滤掉不含视频的 program"这条修正
的根源）只在这一份 fixture 上验证过，没有真实 CDN 素材佐证这确实是普遍
行为而不是这一版 ffmpeg hls muxer 的偶然产物。

**现在靠什么守**：`lowest_bandwidth_fallback_never_picks_an_audio_only_variant`
（`tests/test_hls_e2e.cpp`）钉住的是"含视频的候选存在时，不会误选纯音频
program"，不是"纯音频回退分支本身走对了"。

**还差什么**：一份真正全部 program 都不含视频流的 fixture（纯音频播客
形状的 HLS 源），以及一份独立于本项目生成脚本的、真实 CDN 产出的分轨
HLS 素材。

## 39. HLS：Task 7 用例 3 的"≥3 秒"/44100 断言与 fixture 形状耦合 🟢 M-HLS 测试耦合

`tests/test_hls_e2e.cpp` 里某些用例对播放时长/采样率的断言（写死
`44100` 采样率、"至少约 3 秒"内容这类数字）是按当前 `tools/gen-fixtures.sh`
产出的具体 fixture 形状写的，不是从协议/接口契约推出来的不变量。

**现在靠什么守**：注释已在断言旁标注这是与 fixture 形状耦合的数字（见
`test_hls_e2e.cpp` 对应用例）。

**还差什么**：如果将来 `gen-fixtures.sh` 调整了生成参数（采样率、时长），
这些写死的数字需要跟着手动更新，没有从 fixture 元数据自动派生。

## 40. HLS：`on_error` 未触发时退回 `SYP_ERR_IO` 的分支无用例覆盖 🟢 M-HLS Task 9

`HlsSession::pending_segment_error()` 优先用 `precise_segment_error()`
（`syp_source_open()` 的 `on_error` 回调给的精确原因），`on_error` 没
触发过时才退回粗粒度的 `pending_segment_error_`（恒定的 `SYP_ERR_IO`）。
**"`on_error` 没触发过、退回粗粒度值"这条分支本身没有任何用例专门覆盖**
——这是既有粗粒度 gate 一直缺的覆盖，Task 9 把错误分级做得更精细之后，
这个分支反而更隐蔽了一层（外层多了一层"优先用精确值"的包装）。

**现在靠什么守**：什么都没有专门守住这条分支——现有用例（比如
`segment_404_is_reported_not_silently_treated_as_eof`）触发的都是
`on_error` 会响应的失败形状，走不到这条回退路径。

**还差什么**：一个"`on_error` 不触发但分片确实失败"的构造场景——比如
`AvioBridge::create()` 内部 OOM 这类不经过 `syp_source` 内部重试/致命判定
的失败路径。

## 41. HLS：`open_segment()` open 阶段的 `pending_segment_error_` 记录是零覆盖分支 🟢 M-HLS Task 9

`open_segment()` 在 `syp_source_open()` 同步失败时会记一次
`pending_segment_error_`，但本项目的 `syp_source_open()` 对 HTTP 资源是
**乐观开**——404 这类失败通常要到第一次 `read()` 才暴露，`open()` 本身
`rc == SYP_OK`。变异注入（把这条记录连同它守着的条件一起改成恒
`false &&`）后，全量 26/26 照样全绿，证实这条分支目前确实零覆盖。

**现在靠什么守**：代码里按既有约定标注了"当前套件无覆盖、原因是
`syp_source_open()` 对 HTTP 资源乐观开"这条注释（`hls_session.cpp` 的
`open_segment()`），不是静默的死代码。

**还差什么**：一个"`open` 就同步失败"的现场（比如 `cache_dir` 指向一个
不可写的路径，或者 `syp_source_open()` 收到畸形 URL）。留着不补是权衡：
这条分支处理的是"万一将来 `syp_source_open()` 的实现改成同步校验
URL/cache_dir"这类尚未发生的情况，纯防御，收益小于新引入一条专门覆盖它
的用例的成本。

## 42. HLS：对路由资源发 Range 请求（`routed_body` + `body_start`/`body_end`）零实测覆盖 🟢 M-HLS Task 3

`tests/support/loopback_server.cpp` 的路由机制（`set_route`）支持按路径
路由到内存里的 `routed_body`，并正确处理 Range 请求（`body_start`/
`body_end` 按 `Range` 头夹取，`loopback_server.cpp:546-559`），但**目前
没有任何 HLS 用例真正对一个路由资源发过 Range 请求**——HLS 分片走的是
`syp_source` 的整段下载，不主动发 Range 子请求。

**现在靠什么守**：路由本身的正确性（精确匹配、200/206 状态码、Range
头解析）在 `test_source_bridge.cpp` 里有覆盖（非 HLS 用例）。

**还差什么**：留给第一个真正需要 `EXT-X-BYTERANGE`（M-HLS 显式排除的
范围之一，见 spec 第 1 节）的任务——那时候分片会需要对同一个路由资源发
多个不同 Range 的请求，现在这条能力已经在测试基础设施里，只是没有 HLS
场景用上它。

## 43. HLS：逐帧内容比对未做，只到帧数等量 + 结构性请求计数 🟢 M-HLS 已知限制

`test_hls_e2e.cpp` 的判据是"帧数与直接播源素材一致"（内容**等量**）+
"该请求的 URL 确实被请求过/不该请求的确实零请求"（结构性），**不是**
逐帧比对 HLS 播出来的每一帧内容与直接播源素材是否逐字节一致。

**现在靠什么守**：`tools/gen-fixtures.sh` 用 `-c copy` remux 生成 HLS
分片（分片与 `source.mp4` 是同一份编码的字节，不是重新编码），所以帧数
相等是一条有意义的必要条件，不是纯粹巧合；这条判据在文件顶部有注释
诚实标注（`test_hls_e2e.cpp` 顶部"判据说明"一节）。

**还差什么**：逐帧内容比对——HLS 走 `Pipeline::create_hls()` 这条路，
`frame_digest.h` 现有的 `decode_pipeline()` 只吃 `create_file`/
`create_avio`，接上去要动 `syp_probe_core` 的公开接口，留给后续任务。

## 44. HLS：`vod_demuxed` 在 open/find_stream_info 阶段会把 `media_audio.m3u8` 及其分片实际读两遍 🟢 M-HLS 结构性限制

分轨 fixture（`vod_demuxed`）里，ffmpeg 给 `AUDIO=` 组的音频轨额外写了
一条自引用的 `EXT-X-STREAM-INF`（只含 `CODECS="mp4a..."`，URI 指回音频
播放列表本身，标准行为，真实 CDN 上同样有，见 `architecture.md` 第六节
"选轨"部分的背景）。这导致 `avformat_open_input()`/
`avformat_find_stream_info()` 阶段，`media_audio.m3u8` 及其分片会被
FFmpeg 实际读两遍（一遍作为分轨的一部分、一遍作为那条自引用"变体"的
探测），是 ffmpeg 自引用 `STREAM-INF` 的副作用，`-var_stream_map` 层面
没有干净的抑制开关。

**现在靠什么守**：`HlsSession::select_variant()` 的 keep-set 逻辑在选轨
**之后**会掐断那条冗余 playlist 的后续分片下载（Task 7 实测：
`seg_audio_0` 请求 2 次、`seg_audio_1` 只 1 次），所以影响面被压缩到只有
"首个分片可能被多取一次"，不是整条播放列表反复重取。

**还差什么**：这是 `tools/gen-fixtures.sh` 生成 fixture 的方式（用
`-var_stream_map` 生成分轨 `.m3u8`）与 FFmpeg hls muxer 行为共同决定的，
不是本项目代码能干净修掉的一层浪费；如果将来分片体积变大或分片数变多，
这多出来的一次首片下载在流量上会更显眼。

## 45. HLS：`seek()` 在早于流"实际起始时间戳"的位置上会失败，返回 `SYP_ERR_IO` 🟡 M-HLS Task 11 核实

**这是本轮文档收尾核实过的一条，不是转抄——之前只在 Task 8 复审时顺带
观测到、没有查清根因。**

`Pipeline::seek(ts_us)` 转发给 `Demuxer::seek()`
（`src/media/demuxer.cpp:216-219`），后者调用
`av_seek_frame(fmt_, -1, ts_us, AVSEEK_FLAG_BACKWARD)`，失败时把**任何**
负返回值压成 `SYP_ERR_IO`（与 known-gaps 里其他"压平错误粒度"的既有模式
一致）。实测（临时探针，HLS VOD 单码率 fixture，`step()` 前进约 50 次后
再 `seek`）：`seek(0)` 返回 `SYP_ERR_IO`（-10），紧接着 `seek(1000000)`
（1 秒处）返回 `SYP_OK`——**不是"HLS 不支持 seek"，是"seek 到早于流实际
起始时间戳的位置会失败"**。

根因在 FFmpeg 的 `hls.c`：`find_timestamp_in_playlist()`
（`hls.c:1926-1954`）里 `pos = c->first_timestamp == AV_NOPTS_VALUE ? 0
: c->first_timestamp`，若目标 `timestamp < pos` 直接返回失败
（`hls.c:1934-1937`）。`c->first_timestamp` 初值是 `AV_NOPTS_VALUE`
（`hls.c:2154`，`read_header` 时），**在读到第一个真实 packet 时才被
赋值为该包的 dts**（`hls.c:2563`）——如果编码器有起始延迟（比如 AAC
的 codec delay、或 edit list 偏移），这个值可能是一个小的正数而不是
恰好 0。

**因果的起点不是 `step()`，调用方根本没有机会介入。**读第一个 packet
这件事发生在 `create_hls()` **内部**——`avformat_find_stream_info()`
就会读，`first_timestamp` 在 `create_hls()` 返回给调用方之前就已经锁死。
终审实测（零次 `step()`，紧接 `create_hls()` 返回就二分查找最小可 seek
目标）：

```
[PE4] create_hls 之后（零 step）最小可 seek 目标 = 23243 µs（23242 失败）
[PE3] step 之前 seek(0)=-10   step 之后 seek(0)=-10  seek(1)=-10  seek(100000)=0
[PE3] 对照 create_file：seek(0)=0
```

所以正确的说法是：**HLS 上 `seek(t)` 对任意 `t ∈ [0, first_timestamp)`
（本素材实测 `first_timestamp` = 23243 µs）从第一次调用起就恒失败**，
与 `step()` 推进过没有推进过无关。把因果挂在 `step()` 上会让人以为
"开头别 seek 就没事"或"先 seek 再 step 能绕过"，两条都不成立，也会
低估影响面——这是 `create_hls()` 一返回就存在的性质。

这是 FFmpeg `hls.c` 的既有行为，不是本项目引入的缺陷；但
`Demuxer::seek()` 把这个"为什么失败"的信息压成了通用的 `SYP_ERR_IO`，
调用方看到的只是一次不明原因的 IO 错误。

**现在靠什么守**：没有任何东西"守"这个行为——`seek()` 只是如实转发
FFmpeg 的判断。spec 第 7.2 节写的"seek 出窗口 → FFmpeg 返回错误 → 沿用
M2a 已有的 `seek()` 失败路径"覆盖的是直播窗口外 seek 这一种已知会失败
的场景，没有覆盖"点播下 seek 到 0 也可能失败"这一种。

**还差什么**：1）没有任何 HLS 用例调用过 `seek()`（`test_hls_e2e.cpp`
全部用例只测顺序播放/中止/错误分级/优先级，不含 seek）——这条行为完全
没有回归保护；2）`Demuxer::seek()` 与既有的 `open_prepared()` 同款
"压平所有失败到 `SYP_ERR_IO`"的风格一致（见下面 tech-debt 的对应条目），
调用方拿不到"为什么失败"；3）~~没有验证这个边界是否只在 `ts=0` 触发~~
**已验证（终审）：是整个 `[0, first_timestamp)` 区间，不是只有 0。**
上面 `[PE4]` 那次二分给出的分界点是 23243 µs——23242 及以下全部失败、
23243 及以上成功，与源码读法一致。

## 46. ~~HLS：直播中途播放列表变加密 → 静默 `Eof`，报错的那半边整个丢了~~（已修复，2026-09-14）🟢

`HlsSession::open_playlist()` 在扫到 `EXT-X-KEY:METHOD=AES-128` 时会拒绝。
修复前只有 **open 阶段**是完整的（`precise_open_error_` → `create_hls()`
报 `SYP_ERR_NOT_IMPLEMENTED`）；直播刷新时播放列表新增 `EXT-X-KEY`，
`step()` 报的是 **`Kind::Eof` + `SYP_OK`**。安全那一半（`key.bin` 请求数 = 0）
一直成立，丢的是报错那一半。根因：`precise_open_error_` 唯一的读点在
`create_hls()` 的失败分支里，播放期间的那次写没有任何人读。

### 修复

与 #47 同一处改：`Pipeline::create_hls()` 在 open 成功后调
`HlsSession::mark_playback_started()`；此后 `open_playlist()` 判定变加密时
按 URL 记进 `playlist_errors_`（`mu_` 保护），`step()` 的 Eof 出口在
`aborted_`、分片错误之后查 `pending_playlist_error()`，改写成
`Error(SYP_ERR_NOT_IMPLEMENTED)`。`precise_open_error_` 改为只在 open 阶段写，
"它不必是原子的"那条论证因此继续成立。

回归：`live_playlist_turning_encrypted_mid_stream_is_reported`（同时断言
`key.bin` 仍为零请求）。

## 47. ~~HLS：直播播放列表重拉失败（404/超时/超限）→ 静默 `Eof`~~（已修复，2026-09-14）🟢

修复前 `open_playlist()` 的所有失败路径都不记账，只把负的 AVERROR 交还
`hls.c`，最终干净地返回 `AVERROR_EOF`：直播期间撤掉 `media.m3u8` 的路由，
`step()` 报 `Eof` + `SYP_OK`，与正常播完无法区分。

### 登记时"只登记不修"的理由不成立

当时的顾虑是"直播源在换段边界上短暂 404 并不罕见，一律升级成致命错误
可能比静默 EOF 更糟"。但 FFmpeg 8.1.2 `hls.c:1607-1611`（`read_data` 里的
重拉）对播放期间重拉失败**不重试**，直接 `return ret`，那一路播放列表就此
结束——**停播本身已经发生，不由我们决定**。唯一可选的是停的时候报不报，
报显然优于不报。"短暂失败要不要容忍"是一个独立的重试策略问题，另登记
为 #49。

### 修复

- 只记**播放阶段**的失败（`mark_playback_started()` 之后）。open 阶段不能记：
  `hls.c:2178-2183` 在多档 master 下对某一档拉取失败只标 `broken` 并跳过，
  其余档照常播——记下来会把一次完整播放改写成 Error。回归
  `variant_broken_at_open_does_not_poison_eof`（一档 404 + 一档加密 + 一档
  正常，必须干净 Eof；变异实测：把阶段判断改成恒真，本用例与
  `encrypted_playlist_is_rejected_with_a_clear_error` 同时变红）。
- **判据是结构性的：直播只有在最后一次取回的播放列表带 `EXT-X-ENDLIST`
  时才会正常播完。** 代码审查（Finding 1）指出只记"取回失败"不够：取回
  200 但 body 解析失败（CDN 的 HTML 错误页，`hls.c:849`）、重拉始终没有新
  分片（`m3u8_hold_counters`）同样让那一路列表结束，而取回层面全是"成功"。
  所以每次播放期取回按 URL 记最后一次结局：失败 → 那次的 status；变加密 →
  `SYP_ERR_NOT_IMPLEMENTED`；成功但无 ENDLIST → `SYP_ERR_IO`（仍是直播）；
  成功且有 ENDLIST → 删掉条目。Eof 出口有非 OK 条目即报错，具体原因优先于
  笼统的 `SYP_ERR_IO`。点播列表播放期间从不重拉，表恒空。
- ENDLIST 判定 `playlist_declares_endlist()`（`url_rewrite.cpp`）与 FFmpeg
  8.1.2 逐字一致（首行 `#EXTM3U`、行由 `\n`/`\r` 切分、只剥行尾空白、
  大小写敏感前缀匹配），两个方向都不许偏，单元测试穷举。
- **按 URL 记、后一次覆盖前一次。** `hls.c:1963`（`select_cur_seq_no`，列表
  在播放期重新变成 needed 时）会忽略重拉失败继续用旧列表，之后同 URL 取回
  成功不该留账；分轨源两路列表各自重拉，一路的结局也不能冲掉另一路的。
- 中止（`SYP_ERR_CANCELED`）不记，与分片通道同一纪律。
- Eof 出口优先级：`aborted_` → 分片错误 → 播放列表错误。

回归：`live_playlist_refetch_404_is_reported_not_silently_treated_as_eof`、
`live_playlist_refetch_returning_an_unparsable_body_is_reported`、
`live_stream_ending_with_endlist_reports_a_clean_eof`（反方向守卫）、
`abort_outranks_a_pending_playlist_error_at_the_eof_gate`。变异实测：ENDLIST
判定改成恒真 → 解析失败那条变红；改成恒假 → 正常收尾那条变红。

**仍未覆盖 / 已知不对齐**：
- 超时、超限两种取回失败走同一条记账分支，没有各自的用例；
  `select_cur_seq_no` 那一支（失败后同 URL 成功则清账）没有用例。
- `playlist_declares_endlist()` 只对齐了首行检查；首行之后的其它解析失败
  （属性畸形、ENOMEM、行超长截断）若恰好 body 里带 ENDLIST，会被当成正常
  结束。真实播放列表里罕见。
- **代码审查 Finding 2，登记不修**：`mark_playback_started()` 在
  `create_hls()` 末尾才调，而 `avformat_find_stream_info()` 在那之前读真实
  包，窗口足够短的直播可能在那期间就撞上一次重拉失败，这次不记账 ⇒ 仍是
  静默 Eof。不把标记提前到 `avformat_open_input` 之后的理由：选轨（discard）
  在 `find_stream_info` **之后**才生效，提前标记会把**未选中档**的直播重拉
  也记进表里，于是一条正常以 ENDLIST 收尾的多码率直播会被误报成错误——
  用"引入一条今天不存在的假阳性"换"修一条假阴性"，而且两条都没有夹具能
  复现（本仓库 fMP4 素材上 `find_stream_info` 在 seg0 内就结束，尝试过的
  用例前提 `media.m3u8 在 open 期间被请求 >= 2 次` 不成立）。要修得同时
  知道"哪个播放列表 URL 属于选中档"，hls.c 不暴露这层映射。

## 48. ~~HLS：`pending_segment_error_` 的粘性穿透 `seek()`，与 `seek()` 的"完全复位"契约相互矛盾~~（已改行为，2026-09-14）🟢

修复前实测：分片 404 → 补回真实字节 → `seek(1000000)` 返回 `SYP_OK` →
重新播下去，**仍然报 `Error(-21 SYP_ERR_HTTP_STATUS)`**，尽管这一轮内容
完整。对有进度条的 demo 是可见行为："拖回去重看还是报错"，唯一出路是
销毁重建。

### 裁定：成功的 seek 清掉 HLS 的错误账

登记时倾向"不改行为"，理由是"内容曾经不完整"是会话已观测到的事实。
终定改为清零，理由：Eof 出口上这条改写要回答的问题是"**这一轮**播放
到底了，内容完整吗"。seek 成功就开启了新的一轮，旧位置上漏过的那一片
与新一轮是否完整无关；如果新一轮又漏了，会重新记上。继续粘住只会产生
"内容完整却报错"的假阳性。

- 清什么：分片错误（粗粒度 + 精确）与播放列表错误，
  `HlsSession::clear_playback_errors()`。
- 何时清：`Demuxer::seek()` **返回之后**且**仅当成功**——hls.c 的 seek 会
  关掉旧分片，`on_io_close` 可能恰在那时补记一笔；seek 失败则位置没变，
  旧账仍描述当前这一轮。
- 不清什么：`aborted_`，中止不可逆（`pipeline.h` `request_abort()`）。
- 契约已写进 `pipeline.h` `seek()` 上方。非 HLS 路径 `hls_` 为空，行为不变。

回归：`successful_seek_clears_a_pending_segment_error`。

## 49. HLS：直播播放列表重拉的短暂失败没有任何重试 🟡 #47 修复时拆出

`hls.c:1607-1611` 对播放期间重拉失败不重试，`HlsSession::open_playlist()`
也只抓一次。直播源在换段边界上的短暂 404 / 超时会直接结束播放（#47 修复
之后至少会报 Error，而不是静默 Eof）。

**还差什么**：在 `open_playlist()` 播放阶段加有界重试（次数/总时长受
`target_duration` 约束，且每次重试前检查 `abort_`）。策略参数应当跟真实
直播源的行为一起定，不在 #47 的修复里拍板。

## 50. HEVC 的 VideoToolbox 内部软件回退测不出 🟡 M3a

**背景**：spec 决策 6/§10——FFmpeg 对 H.264 用 `RequireHardwareAcceleratedVideoDecoder`
（`videotoolbox.c:845-849`），不支持就直接失败；对 HEVC 只用
`EnableHardwareAcceleratedVideoDecoder`，这个 flag 只是"允许"用硬件，VT 内部判定某个具体流
（特殊 profile/level/参数集组合）不能硬解时，**允许自己悄悄退回软件解码，session 照常建立
成功、照常吐帧，只是内部实际在用 CPU**。本项目在 `vt_supports()`/`videotoolbox_decode_backend()`
里加了 `VTIsHardwareDecodeSupported()` 预检（`vt_decode_backend.h` 顶部注释），但这是一个
**per-device**（这台设备/这个 GPU 支不支持 HEVC 硬解）的查询，不是 **per-stream** 的——预检
问的是"这台设备原则上能不能硬解 HEVC"，答案跟"这一路具体的流（编码器某个冷门 profile、
超出这台设备硬件解码器支持的分辨率/level 上限等）在 VT 内部会不会被引擎自己踢回软件"是两个
不同的问题，预检通过不等于这一路流真的全程走了硬件。

**为什么测不出**：FFmpeg 的 `AVHWDeviceContext`/`AVVideotoolboxContext` 没有把 VT session 的
`kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder`（或等价属性）透传出来；
`Frame::hw_handle()` 只保证拿到的是 hwaccel 帧（`AV_PIX_FMT_VIDEOTOOLBOX`，`CVPixelBuffer`），
不代表这一帧的解码过程发生在硬件里——一个纯软解也可以把结果包进同样形状的
`CVPixelBuffer` 交出来。也就是说，即便本项目跑通了 Task 7 的"硬解 vs 软解逐比特一致"回归
（`faststart.mp4`/`bframes.mp4`/`hevc.mp4` 各 1710 帧、`mismatched=0`），那只证明"两条路径解出
来的像素相同"，**不能**反过来当成"硬解路径全程真的在用硬件"的证据——如果 HEVC 那一路在某台
设备上被 VT 悄悄退到软件，逐比特比对依然会全绿（两个都是软解，理所当然一致），本项目现有的
任何一条自动化用例都分辨不出这两种情况。登记为已知缺口，不是遗漏——spec §10 已经预先写明
"本方案测不出"。

**边界澄清（M3a 终审 C1 之后）**：VT **拒绝建 session**（超硬件上限、iOS 后台打开、session
耗尽、真不支持的 profile）现在能被发现——`FFmpegVideoDecoder` 的 get_format 闩 + 输出闸把它变成
该视频轨 `track_failed()`，不再静默软解（spec §3.4/§8）。本条剩下的只是"session 建成功、VT
内部自己走软件"那一种，仍然测不出。

**真机人工验证清单**：已拆到 #51。

## 51. M3a demo 真机/模拟器人工验证清单未执行 🟡 M3a

**清单（task-10-brief.md Step 7、spec §9.4）**：① Mac Catalyst 壳硬解分段禁用、软解出画；
② iOS 真机硬解本地文件/`.m3u8` 出画、统计标签显示"硬解"；③ iOS 真机硬解播放中切后台 5 秒再
回来、画面继续（VT session 重建）——**均未执行**。执行 M3a 的是代理（agent），不持有物理 iOS
设备，也没有条件把 Xcode 项目跑到真机/模拟器上做交互式验证，只完成了到"两个 demo 工程用
`xcodebuild ... CODE_SIGNING_ALLOWED=NO build` 编译链接通过"这一层，见 `task-10-report.md`。
终审修复轮（C1/I1/M4）同样只到这一层。`docs/roadmap.md` M3a 判定一节同步记录这一条边界。

**将来在哪补**：有真机的人按清单跑一遍，结果记回本条；③ 在 C1 修复后预期的失败形态是"视频轨
track_failed、有明确错误"，而不是"静默黑屏/只剩声音"。

## 52. ~~`nextDrawable` 可能在持 `mu_` 时阻塞泵线程最多 1 秒~~（已修复，M3b）🟢

**原始现象**（M3a）：挂了 `CAMetalLayer` 后，`MetalRenderer::present()` 在泵线程上同步调
`[CAMetalLayer nextDrawable]`；drawable 池耗尽（窗口被遮挡、显示链路卡住、drawableSize 为 0
等）时它最多阻塞 1 秒才返回 nil（`SYP_ERR_TIMEOUT`）。demo 的泵循环在这期间持着 `mu_`，
主线程上任何要拿 `mu_` 的调用（快照、`-attachVideoLayer:`、控制操作）都会跟着卡住同样时长。

### 修复（M3b Task 6，2026-09-15）

**在途上限门**（`InflightGate`，`src/platform/apple/inflight_gate.h`，独立成类、原子计数，
`try_acquire()`/`try_acquire(limit)`/`release()`/`in_flight()`，单测直接构造覆盖）：
`present()` 入口先非阻塞取名额，取不到直接返回 `SYP_ERR_BUSY`，不碰 GPU、不调
`nextDrawable`。上限按路径区分：

- **离屏（无 layer）**：`kMaxInflightFrames = 3`，命令缓冲**完成回调**里归还。
- **挂 layer**：`min(kMaxInflightFrames, layer.maximumDrawableCount − 1)`（默认 3 → 2，
  留一个前缓冲）——`CAMetalLayer.maximumDrawableCount` 只能是 2 或 3，`InflightGate::
  try_acquire(limit)` 按"构造容量与本次 limit 取小者"实现这条动态上限。名额在
  `[drawable addPresentedHandler:]` 里归还（drawable 此后才真正回到系统的 drawable 池），
  不是命令缓冲完成时——完成时归还会让门形同虚设、`nextDrawable` 照样阻塞（实测：不设门时
  60Hz 上屏窗口每次 `nextDrawable` 约 16ms；按 presentedHandler 归还、上限 2 时 ≤0.2ms）。
- **命令缓冲执行失败**（drawable 未真正上屏）时改由**完成回调**归还，两处经 exactly-once
  令牌守卫，不会重复释放。
- **presentedHandler 丢失兜底**：命令缓冲已完成、但超过"提交时刻 + `kMaxPresentDueUs`
  + 200ms（约 1.2s）"令牌仍未认领，后续 `present()` 取名额前与 `wait_until_idle()` 里强制
  认领归还；令牌未认领的槽不复用。`wait_until_idle()` 因此有界（命令缓冲由 Metal 保证完成，
  名额最迟在最晚提交后约 1.3s 回收），析构不再可能挂死。
- **离屏路径**在命令缓冲完成回调里归还（不涉及 layer/presentedHandler）。

**已知局限（spec §3.3 已记录，不是遗漏）**：真实上屏 layer 的 presentedHandler 可能
**合法地**迟到超过兜底期限（窗口被遮挡、显示器休眠等不刷新的情形）。此时兜底会把名额
提前收回，但 drawable 本身仍未回到系统池，之后的 `nextDrawable` 仍可能阻塞，最长到其
1 秒超时（超时返回 nil → `SYP_ERR_BUSY`，不再是 `SYP_ERR_TIMEOUT`）。影响有界，优于名额
永久泄漏、layer 路径永远 BUSY、析构挂死三选一。

**测试/调试缝**（`src/platform/apple/metal_renderer.h`）：`wait_until_idle()`（原计划的
`debug_wait_until_idle` 改名——析构函数本身也用它，不再只是测试专用）、
`debug_hold_gate_releases(bool)`（挂住本该归还的名额，确定性填满门测 BUSY）、
`debug_suppress_presented_release(bool)`（模拟 presentedHandler 丢失，验证兜底）、
`debug_set_permit_reclaim_after_ms(int32_t)`（缩短兜底期限，压缩测试耗时）、
`debug_reclaimed_permit_count()`（兜底回收次数，正常路径用例断言其为 0 以钉住
presentedHandler 主路径没有被兜底悄悄掩盖）、`debug_last_slot()`（纹理槽轮换验证）、
`debug_inflight_count()`（当前在途名额数）。

**回归用例**（`tests/test_metal_renderer.cpp`）：`present_never_blocks_and_returns_ok_or_busy`、
`offscreen_gate_full_returns_busy_immediately_deterministic`、`consecutive_presents_rotate_slots`、
`layer_gate_caps_at_drawable_count_minus_one_and_skips_next_drawable`、
`layer_rapid_presents_with_due_never_stall`、`destructor_releases_held_gate_and_does_not_hang`、
`layer_missing_presented_handler_permit_reclaimed_by_fallback`、
`destructor_is_bounded_when_presented_handler_never_releases`、
`layer_permit_not_released_by_completion_alone`、
`layer_permit_returns_via_presented_handler_promptly`。TSan/ASan 全跑
`metal_renderer`/`track_player`/`vt_decode`，去同步等待后完成回调在 Metal 内部线程执行是
本轮主要并发风险来源，槽轮换与在途计数全部原子操作。

**遗留**（登记为 tech-debt，不阻塞）：`has_layer` 重建时的兜底回收守卫误报窗口极窄（微秒
级），没有能区分的用例；离屏槽同样写 `reclaim_deadline` 字段（无害的记账冗余，折进
`wait_until_idle()` 的上界即可）；无窗口 layer 上 100ms presentedHandler 计时断言是墙钟
性质（3×TSan、1×ASan 下通过，存在 CI 抖动导致偶发假红的风险）。

**仍需真机验证**：本条自动化验证的是"在途上限门 + 归还时机"这套机制本身的正确性（BUSY
计数、槽轮换、exactly-once、兜底回收），不是"真实窗口上 `nextDrawable` 到底有多快"——
spec §2 决策 2 里"上限 2 时 ≤0.2ms"这个数字是**独立命令行程序**在一个真实窗口上量出来的
（不经过本仓库任何测试框架），不是 ctest 里量出来的；`test_metal_renderer.cpp` 的 layer
用例全部用无窗口/离屏 `CAMetalLayer`（`make_offscreen_metal_layer`），从未在一个真实
`UIWindow`/`NSWindow` 上验证过这个门在真实显示链路下的延迟量级。iOS 后台运行、窗口被
系统遮挡（如控制中心下拉、分屏）时 presentedHandler 的实际到达时机（是否真的如"已知
局限"一节假设的那样可能长时间不回调）同样没有真机/模拟器数据支持，只有代码阅读 + 无
窗口 layer 上的间接实测。两条都记入 `docs/roadmap.md` M3b 判定的人工验证清单。

### 终审 C1（2026-09-15）：真实窗口上 BUSY 成为常态，"BUSY 即丢帧"改为"保留重试"

**问题**：上面的门在真实上屏窗口上与播放器的提交节奏组合出了成批丢帧。`TrackPlayer` 最多
提前 `kPresentWindowUs`（40ms）提交，挂 layer 的名额在 presentedHandler（**上屏之后**）才
归还，每个名额被占 40~57ms；上限 2、60fps 时两个名额经常同时被占，`present()` 频繁返回
BUSY。当时的 `TrackPlayer` 把 BUSY 的帧当"已消费"丢掉——终审用真实 `NSWindow` +
`CAMetalLayer` 原型实测 600 帧丢 202/277/252（提前 0ms 提交也丢 102/111），每 2ms 重试则
0/600。ctest 结构性看不见：无窗口 layer 在命令缓冲完成时就回调 presentedHandler，名额几乎
立刻归还。

**修复**：`IVideoRenderer::present()` 契约改为"BUSY 时保留帧稍后重试"（`src/media/
video_renderer.h`）；`TrackPlayer` 正常视频路径、seek 后第一帧、暂停中预览帧三处 BUSY 都
不消费帧，与早到分支同形（驱动 Pipeline、报 `Waiting`），下一轮按新时钟重判，迟到超过
80ms 由正常迟到路径丢弃（计入 `dropped_frames()`、喂追帧窗口）。seek 首帧的 flush/时钟
复位每次 seek 只做一次。`render_busy_frames()` 语义改为 BUSY 重试次数（demo 标签"忙重试"）。
否决把挂 layer 上限提到 3：终审实测 `nextDrawable` 会阻塞最长 33ms。回归用例
（`tests/test_track_player.cpp`）：`render_busy_retains_frame_not_failure_not_drop`、
`render_busy_once_then_retry_presents_same_pts`、
`sustained_busy_frame_goes_too_late_and_is_dropped_by_late_path`、
`sustained_busy_late_drops_feed_catchup`、
`paused_seek_first_frame_busy_retains_just_sought_and_retries_same_frame`、
`unpaused_seek_first_frame_busy_retries_same_frame`、
`paused_preview_busy_retains_frame_and_retry_presents_it`。

**复审 I-1（修复后补）**：上面的修复最初让非暂停 seek 首帧的重试**没有迟到上限**——它不
消费帧、不 pop 后续视频，渲染器持续 BUSY（如 `drawableSize` 为 0×0，`nextDrawable` 恒 nil）
时视频 FrameQueue 满、联合背压停掉解封装、音频断粮、AudioClock 停走，帧永远"不迟"，音画
永久冻结（修复前 BUSY 即丢，至少音频还在播）。现在 seek 首帧的复位做过之后按时钟判迟到，
超过 80ms 就放弃首帧特殊路径、交给正常迟到路径丢弃；复位之前不判（落点关键帧 pts 常早于
请求值，时钟尚未纠偏）。回归用例：`unpaused_seek_first_frame_sustained_busy_gives_up_when_too_late`、
`unpaused_seek_first_frame_not_dropped_before_rebase_even_if_clock_ahead`。

**真实窗口复测工具**：`tools/present_window_harness/main.mm`（`cmake --build build --target
present_window_harness`，EXCLUDE_FROM_ALL、不进 ctest）。开一个 `NSWindow` + `CAMetalLayer`，
按 60fps 合成 NV12 帧、泵线程每 2ms 一轮驱动 `MetalRenderer`，对照"BUSY 丢弃"与"BUSY 重试"、
提前 40ms 与 0ms 四种组合，打印 presented / BUSY 返回次数 / lost / 兜底回收次数 / 单次
`present()` 最长耗时。

**本机实测（2026-09-15，M 系列 Mac，60Hz，600 帧）——环境受限，数字只作量级参考**：
本次是在无人值守的会话里跑的，窗口 `occlusionState` 始终报"不可见"（显示器可能处于休眠/
被遮挡），合成器不按显示节奏刷新，三次运行差异很大：

| 运行 | discard@40ms lost | retry@40ms lost | discard@0ms lost | retry@0ms lost | 兜底回收 |
|---|---|---|---|---|---|
| 1 | 310 | 316 | 351 | 326 | 8~10（presentedHandler 超 1.2s 未到） |
| 2 | 206 | 33 | 50 | 10 | 0 |
| 3 | 529 | 582 | 250 | 15 | 7~18 |

presentedHandler 按时回调的那一次（运行 2，兜底回收 0）：提前 40ms 提交时丢弃策略丢 206/600
（与终审原型的 202~277 一致），重试策略丢 33/600；提前 0ms 时 50 → 10。重试策略剩余的丢帧
都是重试到迟到超过 80ms 后的迟到丢弃，与窗口不可见时合成器节奏不稳一致；终审原型在可见
窗口上测得 0/600。**仍需在显示器点亮、窗口可见的会话里重跑本工具**确认重试策略接近 0 丢失，
记入 `docs/roadmap.md` M3b 人工验证清单同一项。

## 53. 按时刻上屏的实际观感未经真机/肉眼验证 🟡 M3b，人工清单

**背景**：spec §1 目标 4"按时刻上屏"——挂 `CAMetalLayer` 时用
`presentDrawable:atTime:CACurrentMediaTime() + due_in_us/1e6`，把"这一帧该在什么时候
出现在屏幕上"这个决定交给系统去对齐最近一次刷新。`test_metal_renderer.cpp` 的
`present_schedules_drawable_at_now_plus_due` 只验证了**参数正确**——
`debug_last_present_host_time()` 记录的目标时刻确实约等于调用时 `CACurrentMediaTime() +
due_in_us/1e6`（容差 5ms）——不验证、也没有办法在无窗口环境里验证系统真的按这个时刻
把帧贴上了屏幕、贴的时候是否比不传 `atTime:` 的旧路径更平滑（更少撕裂/更少掉帧）。

**现状靠什么守住**：纯参数级单测（上面那条用例）+ spec §2 决策 1/2 的设计论证（`due_in_us`
本身经过与设备时钟一致的换算）。**没有任何自动化或人工验证过肉眼可见的呈现效果**——
这需要在真实设备/显示器上播放，同时用高速摄像头或系统自带的帧率/丢帧统计工具比对
"用 `atTime:` 前后"两种路径的主观流畅度，不是本仓库现有测试基础设施能做的事。

**将来怎么补**：留在真机验证的人工清单里（见 `docs/roadmap.md` M3b 判定）；如果以后要
自动化，方向是接入 `CADisplayLink` 或系统的呈现时间戳 API（`CAMetalLayer` 的
`presentedTime`）比对"计划呈现时刻"与"实际呈现时刻"的差值分布，而不是继续停留在"参数
传对了"这一层。

## 54. `present()` 不再同步报告 GPU 执行错误 🟢 契约变化，已在头文件写明

**背景**：M3b 之前（M3a）`present()` 会在调用返回前同步知道命令缓冲是否成功执行；去掉
`waitUntilCompleted` 之后（spec §3.3），`present()` 一旦成功提交命令缓冲就返回 `SYP_OK`，
不再等待 GPU 真正执行完。命令缓冲执行失败（含设备丢失、着色器运行期错误等）只能在
**完成回调**里异步发现，`present()` 的返回值本身此后无法用来判断"这一帧真的画对了没有"。

**现状靠什么守住**：完成回调里记日志并累加 `debug_gpu_error_count()`（只读计数，测试可以
轮询），契约变化已经写进 `metal_renderer.h` `present()` 声明附近的注释与本条。
`TrackPlayer::present_failures()` 的统计口径也相应变化——它现在只统计"同步可知的失败"
（比如非法帧、渲染器拒绝了不支持的像素格式），不再包含"命令缓冲提交后才发现的 GPU
执行错误"这一类；后者只能通过 `debug_gpu_error_count()` 单独观察，`TrackPlayer` 层面
没有对应的公开计数——调用方如果想知道"最近是否发生过 GPU 执行错误"，目前只能穿透到
`MetalRenderer` 自己的调试接口，`TrackPlayer`/demo 快照都没有转发这个信号。

**为什么不算缺陷**：这是 spec 明确的设计取舍（不阻塞泵线程换来的代价），不是遗漏；
调用方如果真的需要同步知道 GPU 执行结果，唯一的办法是恢复 `waitUntilCompleted`，那样
会重新引入 #52 修复前的阻塞问题，两者不可兼得。记录本条是为了让以后读 `present_failures()`
的人不会误以为它覆盖了 GPU 执行失败这一类。


## 55. ~~NSURLSession 透明 gzip 让下载层按压缩长度记账，资源被截断~~（已修复，2026-09-15）🟢

**现象**：twimg 的 HLS 链接打开失败，`syp_status=-10`（`SYP_ERR_IO`）。`syp_probe` 显示
master 播放列表只下了 623 字节，实际 1657 字节，FFmpeg 报 `Invalid data found when
processing input`。

**原因**：NSURLSession 默认自动带 `Accept-Encoding: gzip` 并透明解压，交付的是解压后的
字节，但 `expectedContentLength`（以及 `Content-Range`）描述的是压缩表示。CDN 回了 gzip，
下载层按 623 记账，交给 FFmpeg 的是截断的播放列表。缓存按字节偏移分片拼接，编码表示上的
Range 本身也没有意义。

**修复**（`src/platform/apple/apple_http_backend.mm`，提交 `5e24b6c`）：每个请求显式
`Accept-Encoding: identity`（放在 extra_headers 之后，调用方不能覆盖）；服务端无视它仍
回 `Content-Encoding` 非 identity 时，Content-Length / Content-Range 一律按未知上报。
回环测试服务器新增 gzip 响应模式（deflate stored 块，不依赖 zlib）。回归用例
（`tests/test_apple_http_backend.cpp`）：`requests_identity_encoding_so_cdn_does_not_gzip`、
`gzip_response_reports_unknown_lengths`，两条都做过变异验证。

**残余**：服务端坚持压缩时长度未知，下载层无法按 Range 续传/分片，只能整份顺序读——
行为正确但失去缓存分片优势。真实 CDN 对 identity 基本都会遵守，未观测到。

## 56. HLS：字幕 rendition 在交给 FFmpeg 前被剥掉，播放器不显示字幕 🟢 设计取舍（2026-09-15）

**现象**：修掉 #55 之后同一条 twimg 链接仍 `-10`，FFmpeg 报 `Error when loading first
segment '...vtt'`。

**原因**：master 带 `#EXT-X-MEDIA:TYPE=SUBTITLES`（twimg 普遍带自动生成字幕）。hls.c 的
`hls_read_header` 为每个 rendition 建子 demuxer 并探测首段；本项目 FFmpeg 构建只开了
mov/hls/mpegts，没有 webvtt demuxer，字幕组探测失败会让整个 `avformat_open_input` 失败——
一个用不上的字幕组拖垮整条流。

**处理**（`src/media/hls/url_rewrite.{h,cpp}` `strip_subtitle_renditions()`，
`HlsSession::open_playlist()` 调用，提交 `be273cf`）：交给 FFmpeg 前删掉 TYPE=SUBTITLES 的
`#EXT-X-MEDIA` 整行，其余字节原样；属性按 HLS 属性列表语法切分，引号内的 `TYPE=SUBTITLES`
字样不会误删；变体行的 `SUBTITLES="组名"` 保留（hls.c 找不到组只是不挂，无害）。回归用例
（`tests/test_hls_url_rewrite.cpp`）：`strip_subtitle_renditions_removes_only_subtitle_media_lines`、
`strip_subtitle_renditions_leaves_other_playlists_untouched`。修复后该链接打开并播到 EOF，
主轨帧数与系统 ffmpeg 一致（25s × 60fps = 1500）。

**代价与后续**：播放器因此显示不了 HLS 字幕。将来要支持字幕，需要 FFmpeg 加
`--enable-demuxer=webvtt` 重编 xcframework、Pipeline 接字幕轨，并去掉这一步剥离。

**附带观察（不是本项目缺陷）**：该流开播时 FFmpeg 会打几条 `Packet corrupt` /
`Invalid NAL unit size`，出自被丢弃的其他 variant 的少量包；系统 ffmpeg 对同一 URL 输出
完全相同的警告。

## 57. 已缓冲时长取在播轨最小值：某轨在文件尾之前先结束会误判卡顿，闩锁使 `full` 恒真时不再显示卡顿 🟡 M3c

**现象**：如音频 2 秒/视频 3 秒的素材经网络播放，音频包在 2 秒处断供（该轨还
没读到真正的文件尾，只是暂时没有更多数据），`buffered_until_us` 取各在播轨
最小值会被钉在 2 秒，读到该轨真正的文件尾之前，播放器会把这段误判为卡顿并
进入卡顿缓冲。本地文件因加载线程瞬间读到尾不受影响。

**闩锁行为（Task 6 fix round 1，`stall_armed_`）**：无闩锁时这类误判会反复
进出 Stall（`full` 每次离开又立刻不满足恢复水位）；加了闩锁之后是"进一次
Stall、被强制恢复（`full` 或读到文件尾）、此后不再武装"——`leave_buffering()`
按离开时的已缓冲时长决定是否重新武装，只有已缓冲时长回到触发线以上或发生
Seek 才会重新武装（`src/media/track_player.cpp:884-903`）。**但如果此后
`full` 一直为真**（字节上限触发，或同步模式下单轨 128 包上限触发，见
`src/media/pipeline.h:146` `max_packets_per_track = 128`），闩锁就一直不重新
武装，Stall 被永久抑制——这段时间里声音可能因为 sink 欠载而静音，播放器
却不会显示任何缓冲状态（`buffering()` 为假）。这是协调方裁定接受的取舍
（ledger "Ruling R1 update"、Task 6 报告"关于原顾虑"一节），不是本任务遗漏。

**提前结束的轨对停读水位与 Seek/起播缓冲的影响（M3c 终审 Minor 1 补记）**：
上面的"某轨先结束"不只发生在网络断供——**本地文件里任何一条在播轨比文件短**
（音频比视频短、TrackPlayer 未绑定的第二条音轨比主轨短，第二条音轨同样参与
水位，见 spec §8.4 第 11 条）都一样，而且后果不止误判卡顿：

- **30 秒时长上限失效，加载线程读满 64MiB**：线程模式停读水位的时长条件取
  在播轨包队列时长的最小值（`src/media/pipeline.cpp` `water_full_locked()`）。
  那条轨读完最后一个包、包队列被解码取空后，队列时长恒为 0，min 被钉在 0，
  `max_buffer_ms` 永远不成立，只剩字节条件——加载线程一直读到
  `max_buffer_bytes`（64MiB）或文件尾才停，内存占用按字节上限而不是 30 秒算。
- **seek 越过该轨末尾后 Seek 缓冲等到 64MiB 或文件尾**：seek 清空全部包队列
  （`PacketQueue::take_all()` 把 `buffered_until_us_` 复位为 `AV_NOPTS_VALUE`），
  该轨此后不会再有包进来，但它没到 `decoder_eof`（那要等 demux 读到文件尾），
  仍参与 `buffer_stats()` 的 min，`buffered_until_us` 恒为 `AV_NOPTS_VALUE`，
  TrackPlayer 读成已缓冲 0。Seek/起播缓冲于是只能靠 `full`（64MiB）或
  `demux_eof` 离开：本地文件加载线程很快读到尾，无感；**网络流上按 5Mbps
  计要约 100 秒**（64MiB × 8 / 5Mbps），期间画面停在 seek 首帧、无声、显示
  "缓冲中"。起播缓冲同理（打开时某条在播轨一个包都没有的素材）。

本轮只登记，不重新设计：根治与上面同一个前提——需要确切知道"这条轨已无后续
包"，才能把它移出 min。

**修法方向**：需要按"该轨已无后续包"的确切证据（而非"已入队包读完"）排除
该轨参与 min，这需要容器层能区分"这条轨确实结束"和"暂时没数据"，本里程碑
没有这个信息来源。

**回归用例**：闩锁本身由
`buffering_full_exit_below_trigger_does_not_flap_until_rearmed`
（`tests/test_track_player.cpp`）钉住（变异：进入条件去掉 `stall_armed_ &&`
后该用例变红）。

## 58. 线程模式 seek 不打断阻塞中的网络读 🟡 M3c 设计取舍

**现象**：网络停滞（比如后端不回应）时，线程模式下调用 `seek()` 不会立即生效——
要等加载线程当前那次阻塞中的 `demuxer_->read()`（进而是 dl 层的网络读）自己
返回，才会处理排队的 seek 请求。最坏情况下要等到 dl 层 `read_timeout_ms` 默认值
15000ms（`include/syplayer/syp_config.h:48`、`src/dl/dl_task.h:27`）超时加上重试
用完。

**原因**：spec §2 非目标 5 明确排除"seek 打断正在阻塞的网络读"——HLS 的
`AVERROR_EXIT`（打断读回调常用的错误码）会被 hls.c 当作"这一片分片读完了"而
跳到下一段，而不是当成"被打断"，`src/media/pipeline.h:267-272`
（`request_abort()` 注释）记录了这条 FFmpeg 内部行为；打断读不可安全复用，
seek 只能等当前读自然返回。

**处理**：异步 seek 本身不受影响——请求（代号 + 目标位置）写入后立即返回给
调用方；只是加载线程要等当前读返回、检查到代号变化后才真正执行
`av_seek_frame`。回归用例
`pipeline_demux_thread_seek_while_read_blocked_applies_after_read_returns`
（`tests/test_pipeline.cpp`）覆盖"读阻塞期间 seek，读返回后 seek 才生效"这条
时序。

**后续**：真正打断需要给 dl 层一个"精确取消当前这次读、不当作 EOF"的信号，
超出本里程碑范围。

## 59. 加载线程没有回滞，`full` 在稳态下大约每帧翻转一次 🟢 M3c 设计取舍

**现象**：停读水位没有"低水位才恢复读"这道回滞——`full` 一满，泵线程每消费
一个包就唤醒加载线程读一个包，读完立刻又满，两个线程来回切换，稳态下频率
大约是每解码一帧就翻转一次（`src/media/pipeline.cpp` `water_full_locked()`
在 `wake_loader()` 之后几乎立刻重新变真；Task 4 报告"设计观察/顾虑"一节
实测记录了这一点）。

**为什么不是缺陷**：正确性不受影响——`TrackPlayer` 的缓冲状态机只在"已缓冲
时长 < 触发线"时进入卡顿，`full` 只出现在**离开**缓冲的条件里（P4 裁定），
不参与进入判定，因此这种逐帧抖动不会被误判成反复卡顿。用例
`buffering_full_flicker_with_healthy_level_never_transitions`
（`tests/test_track_player.cpp`）专门构造"`full` 每步翻转 300 步、已缓冲时长
本身健康"的场景，断言从未进入缓冲、sink 从未暂停（变异：把进入条件改成
`s.full || buffered < trigger` 后该用例变红，证明这条防线是必要的）。

**代价**：每消费一个包就有一次加锁/唤醒的往返，实际播放速率下开销可忽略；
如果以后 HLS 单次网络读的调用成本变高，才值得加一个低水位阈值，但那会改变
`full` 的抖动语义与 `TrackPlayer` 离开缓冲的判据，需要重新裁定（见
tech-debt.md"M3c 收尾登记"）。

## 60. 欠载计数按"段"计，含播放自然结束的放空 🟢 契约

**定义**：`IAudioSink::underrun_count()`（`AudioUnitSink` 实现，
`src/platform/apple/audio_unit_sink.h:84-87`）计的是"欠载段数"，不是"欠载
样本数"或"卡顿次数"——一段 = 环里写入过样本（`armed_` 置位）之后，渲染回调
从"拿得满"变成"拿不满"；持续断粮只算一段，直到再次写入样本重新武装。
`flush()`/`open()` 会解除武装（环刚被清空，接下来拿不满是预期的，不计
新的一段）。**播放自然结束（音频轨解到底，环被取空）同样会计一段**——这不是
缺陷，是"拿不满"这个判据本身覆盖不了"结束"和"卡顿"的区别。

**与 `TrackPlayer::rebuffer_count()` 的关系**：`TrackPlayer::audio_underruns()`
（`src/media/track_player.h:417-421`）原样转发这个计数，注释里写明"不等于
卡顿次数"——除了网络卡顿造成的断粮，还包含自然播完的放空，以及起播/seek/
缓冲恢复后首批样本写入之前设备先拉空的"预热"段（Task 6 报告"控制器约束
落实"第 2 条实测到的约 12 步预热窗口）。卡顿次数应看 `rebuffer_count()`。

**自动化覆盖**：纯函数判定用例
`underrun_edge_counts_each_starvation_episode_once`、
`underrun_count_is_zero_before_open`（`tests/test_audio_unit_sink.cpp`）；
真实硬件用例 `underrun_count_on_real_hardware_counts_one_episode`——`open()`
在本机失败时打印 `[SKIP]` 并返回，不构造假失败。

## 61. 水位默认值未经真机网络调参 🟡 M3c

**现象**：`startup_buffer_ms=500`/`rebuffer_trigger_ms=100`/
`rebuffer_resume_ms=2000`/`max_buffer_ms=30000`/`max_buffer_bytes=64MiB`
（`src/media/track_player.h` `BufferPolicy`、`src/media/pipeline.h`
`PipelineConfig`）全部是 spec 设计阶段给出的经验值（spec §2 非目标 1、§3
决策 4/5/6），不是在真实网络条件下测出来的。

**影响**：这组数字直接决定"多久判定一次卡顿""卡顿后攒多少才恢复""内存
上限多大"，真实弱网下卡顿触发可能过于敏感（正常抖动就进 Stall）或过于
迟钝（明显卡顿很久才进），恢复水位/起播水位也可能偏高（起播变慢）或偏低
（恢复后很快再次卡顿）。自动化测试只验证了状态机在给定水位下的行为正确，
不验证这些具体数字本身是否合适。

**后续**：需要真机、真实网络（含弱网/限速）跑量后回填，同时登记在
`docs/tech-debt.md`"M3c 收尾登记"①；相关的人工验证项见 #62。

## 62. M3c 人工验证清单未执行 🟡 M3c，人工清单

Task 8 Step 3（demo 人工冒烟）在本环境无法交互式启动 demo/接入真实网络限速
环境，未执行（见 task-8-report.md）。以下几项都还没有真机/真实网络下的人工
验证：

- **demo 冒烟**：用真实 HLS URL 或限速服务打开 demo，确认统计标签"缓冲
  中…(起播)"→"已缓冲 Xs"、网络断开时"缓冲中…(卡顿)"，且 play/pause/seek
  按钮仍即时响应，网络卡住不冻结 UI。
- **蓝牙路由变化斜坡观感**（iOS 真机）：`kLatencySlewRate`（100ms/s）逼近
  目标延迟这条平滑是否真的让人耳察觉不到跳变，自动化只验证了斜坡数学本身
  （单调、限速、到达即停），见 known-gaps #25"M3c 补记"。
- **弱网卡顿进入/恢复体验**：真实弱网下卡顿触发是否过于敏感/迟钝、恢复水位
  是否合适，见 #61。
- **卡顿恢复时的音频缺口**：每次缓冲进出停/启 AudioUnit，恢复瞬间是否听得到
  缺口/爆音（尤其蓝牙），见 #63。

若这些验证后续被执行，把结果写回本条并按实测调整级别。

## 63. 每次缓冲进出都停/启 AudioUnit，停止会丢掉设备侧已缓冲的音频 🟡 M3c，未经真机验证

**现象**：缓冲状态机进入任一缓冲（起播/Seek/卡顿）时冻结时钟调
`IAudioSink::pause()`，离开时调 `resume()`（`src/media/track_player.cpp`
`freeze_clock()`/`unfreeze_clock()`）。`AudioUnitSink` 的实现是
`AudioOutputUnitStop()`/`AudioOutputUnitStart()`（`src/platform/apple/audio_unit_sink.mm`
`pause()`/`resume()`）：

- **两个调用都是同步阻塞的**，而 demo 是在持 `mu_` 的 `step()` 里调到它们
  （`swift/SYPlayerKit/Internal/SYPBridge.mm` `pump_loop()`，M4 前为
  `demo/shared/bridge.mm`）。停/启一次通常是毫秒级，但这段时间里
  UI 线程的 play/pause/seek/snapshot 都在等 `mu_`；蓝牙等路由下停/启更慢。
- **每次 Stop 都会丢掉已经交给设备、还没真正放出来的那一截音频**（HAL IO 缓冲；
  蓝牙路由下游缓冲更深，可达百毫秒级）。时钟读数不跳变——`played_us()` 按环的
  已取走字节减设备延迟计算，暂停中不推进（裁定 P7 的单调护栏），恢复后从环里剩下
  的样本接着放；被 Stop 丢掉的那一截在记账上已算"取走"，理论上恢复后声音相对时钟
  滞后这一截，量级是 IO 缓冲（毫秒级）、蓝牙下未测。更直接的后果是**每次卡顿恢复
  都可能听到一小段音频缺口/爆音**，蓝牙上尤其明显。卡顿越频繁越明显（#57 的误判卡顿、#61 水位偏敏感都会放大它）。

**为什么这样做**：spec §4.3 让缓冲与用户暂停同形——冻结时钟最直接的办法就是停设备，
与已有的暂停路径共用一套已验证的时钟语义。换成"设备继续跑、回调喂静音"能避免停/启
阻塞与丢缓冲，但要改渲染回调与 `played_us()` 的记账（喂静音期间不能计入已播放），
超出 M3c 范围。

**后续**：真机（含 AirPods）上实测卡顿恢复是否可闻（并入 #62 人工清单）；若明显，
改为缓冲期间回调输出静音、不停设备。

## 64. `SYPlayerError.httpStatus` 的载荷恒为 0 🟢 M4 契约

**现象**：`swift/SYPlayerKit/Internal/SYPBridge.mm` 的 `onError` 与
`-errorOut:` 只透传 `syp_status`，不带 `syp_error_info`，所以
`SYP_ERR_HTTP_STATUS`（-21）经 `SYPlayerError(statusCode:)` 映射后恒为
`.httpStatus(0)`（`swift/SYPlayerKit/SYPlayerError.swift`），业务拿不到
具体是 404 还是 503。

**影响**：demo 与业务只能显示"服务器返回了错误状态码"这类无具体码的文案
（`SYPlayerError.errorDescription` 的 `.httpStatus` 分支，`code > 0` 时才
显示数字）。

**后续**：要补需要给桥加一个错误详情出参（例如 `errorInfoOut:`），属于接口
扩展，本轮不做。回归用例
`SYPlayerErrorTests.testHTTPStatusPayloadDoesNotChangeStatusCode` 钉住了
"载荷不影响 statusCode 往返"这条语义，不承诺载荷本身有意义。

## 65. `SYPlayerKit` 的 ObjC++ 桥不在 iOS 13 可用性扫描范围内 🟡 M4，既有面积

**现象**：`tools/check-deploy-target.sh` 只 `find "$P/src"`，
`swift/SYPlayerKit/Internal/SYPBridge.mm`（M4 前为 `demo/shared/bridge.mm`，
本来就在扫描外，M4 只是把它挪了个位置，未改变这条口径）不在扫描范围内；它
引用的 `src/` 侧实现都在扫描内，桥自己只用 Foundation/QuartzCore/GCD，风险
低但不为零。

**影响**：桥里任何直接使用 Apple SDK API 的代码（目前没有，未来若新增）不会
被这条 `-fsyntax-only` 检查在 iOS 13 部署目标下拦截。

**后续**：把 `swift/SYPlayerKit/Internal` 加进脚本的第二个 `find`（需要先给它
补 FFmpeg 头与 `-I$SRCROOT/generated` 两个前提，脚本目前只给 `src/` 侧配了
这些搜索路径）。

## 66. 后台/前台切换未做处理 🟡 M4 非目标

**现象**：spec §2 非目标 3、§5"后台/前台"一行明确本轮不做。进后台后
`CAMetalLayer` 拿不到 drawable、`AudioUnitSink` 的行为取决于 Audio Session
类别，demo 未配置 `AVAudioSession` 类别（`swift/SYPlayerKit/` 与
`demo/shared/` 均无相关代码）。

**影响**：进后台时画面/声音的具体表现（是否崩溃、是否静默失败、恢复前台后
能否自动续播）未验证。

**后续**：真机上实测（并入 #67 人工清单），若要支持需要新增后台播放会话策略，
属于独立课题。

## 67. 真机未验证：SwiftUI 页、旋转、tab 来回切 🟡 M4，人工清单

**现状（如实抄自 `task-7-report.md` 第 3/4 节）**：本环境未授予 `osascript`
辅助访问权限（`System Events` 报 `-1719`），无法脚本化点击/拖拽 UI，因此
Task 7 的人工冒烟**只执行到**：

- ✅ `syplayer-mac.app`（未签名）能正常启动，进程存活，无 crash report；
- ✅ 截图确认 `UITabBarController` 在 Catalyst 上渲染成标题栏分段控件，
  `UIKit | SwiftUI` 两页入口都在，UIKit 页控件齐全，构造两个 `SYPlayer`
  没有引发启动崩溃。

以下各项**均未执行**，需要人手在真机/本机交互式验证一遍（清单原文见
`task-7-report.md` 第 4 节 A–E 段，Task 8 未新增执行）：

- **A. UIKit 页基本功能**：播放内置样片落地为 `.paused`（不出声）、点 Play
  出声、漂移读数带宽、进度条拖动不被轮询回拉、倍速四档、打开本地文件/畸形
  文件的错误文案、Catalyst 上硬解段禁用、播到结尾与拖回重放。
- **B. SwiftUI 页**：渲染正常、内置样片自动起播、URL 播放、进度条边拖边
  seek、倍速 Picker、数字持续刷新（`@Published` 链路的唯一手工证据）。
- **C. Task 4–6 挂起的三项**：SwiftUI 页 attach/detach、UIKit↔SwiftUI 来回
  切两次两页画面都要回来（ruling P4 唯一的端到端验证）、旋转时
  `drawableSize` 同步（仅 iOS 真机/模拟器）、切走的页自动暂停。
- **D. 其余**：iOS 真机运行一次（目前只做到"能编译出 generic/iOS 产物"，
  没有真机运行证据）；硬解路径只能在真机上验（Catalyst 恒软解）。
- **A 段追加（Task 7 fix round 1）**：播放中制造一次卡顿，按钮仍显示 Pause
  且点它确实暂停（缓冲优先级高于暂停的回归验证；`task-7-report.md` 的
  "清单更新"一节，不是该报告第 4 节原有的 A–E 分段之一）。
- 本条同时收纳 #66（后台/前台切换）的人工验证——真机上进后台一次，观察
  画面/声音/恢复前台后的状态。

若这些验证后续被执行，把结果写回本条并按实测调整级别。

## 68. XCTest 在本机的可执行性：**跑通了**，58 条全绿 🟢 M4

**结论（不含糊）**：`xcodebuild test` 在无签名、无 host 的 Mac Catalyst
destination 上**从 Task 3 起就一次性跑通**，没有降级为
"仅 `build-for-testing` + 人工清单"。命令：

```
xcodebuild test -project demo/mac/syplayer-mac.xcodeproj -scheme SYPlayerKitTests \
  -configuration Debug -destination 'platform=macOS,variant=Mac Catalyst,arch=arm64' \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY=""
```

Task 8 复跑结果：`Executed 57 tests, with 0 failures (0 unexpected) in 7.963
(7.985) seconds`，`** TEST SUCCEEDED **`。终审修复波次后复跑：
`Executed 58 tests, with 0 failures (0 unexpected) in 8.748 (8.772) seconds`，
`** TEST SUCCEEDED **`。

套件用例数随任务推进增长：17（T3）→22（T4）→38（T5 初版）→44/46/50（T5 三轮
修复）→56（T6 初版）→57（T6 fix round 1）→58（终审修复波次，I1 的回归用例
`testTransportControlAfterCloseDoesNotResurrectState`，终值）。分布见 spec §8
落地对照的 §6.1 用例清单。

**已知局限（不是"没跑通"，是范围边界）**：测试 bundle 无 host（逻辑测试），
`UIWindow` 在这个环境里不可构造（R4，见 known-gaps #67 与 spec §8），所以
"视图真的挂进一棵窗口树"这一跳测不到，转成了人工清单第 C 段；音频设备是否
真的产生声音同样未被断言（生命周期用例用 `position` 作为播放/暂停的代理
指标，不直接查询硬件）。

## 69. SwiftUI 页的已知限制：无文件选择器、无软硬解切换开关 🟢 M4 已知限制，非缺陷

**现象**（原样对应 `task-7-report.md` 第 4 节真正的"E. SwiftUI 页的已知限制"
——上一版 #67 误把这一段的引用位置和内容都张冠李戴成了 Fix round 1 追加的卡顿
用例，此为订正）：

- **SwiftUI 页只能打开内置样片与 URL，没有"打开本地文件"入口。**
  `UIDocumentPickerViewController` 是 UIKit 的 presentation 流程，SwiftUI 页
  要接它得额外包一层 `UIViewControllerRepresentable`，对"证明 SwiftUI 接入
  成立"这个目的没有增量，本轮有意不做。任意本地文件的选取只在 UIKit 页
  （`demo/shared/PlayerViewController.swift`，`UIDocumentPickerViewController` +
  拷贝到 tmp 目录）。
- **SwiftUI 页没有软解/硬解开关**，按 `SYPlayer.isHardwareDecodeAvailable`
  自动选（Catalyst 上恒软解）。要手工对比两种解码方式，同样只能在 UIKit 页做。

**这不是 bug，也不需要人工验证**——两条都是范围收缩后的既有事实，验收时不应
当作缺陷登记；列在这里是为了不必靠读代码推断。

## 70. `open()` 落地为 `.paused` 存在残余的 pump-hop race 🟡 M4，见 spec §8.5

**现象**：`SYPlayer.open()` 在桥 `-open*` 返回成功之后、`continuation.resume`
之前调用一次 `pause()`，但 `-open*` 内部把 `pump_loop()` 异步 `dispatch_async`
到另一条串行队列；如果那次线程切换抢在 Swift 侧的 `pause()` 之前拿到 `mu_`，
起播瞬间可能已经真的写出一帧画面或一段音频，才被 `pause()` 追上。

**影响**：概率很低（Swift 侧几乎总赢这场线程调度的赛跑），但是一场真实的
竞争，不是纸面推理；结构性关严需要让核心"以暂停态启动"，属于改
`TrackPlayer`/桥的默认语义，本轮明确不做（29 个 ctest 与 ObjC 直连桥的
假想调用方都依赖 `paused_` 默认 `false`）。

**详细推演与代理测试**：见 `docs/tech-debt.md`"M4 收尾登记"#10。不在此重复。

**【终审 M9】代理用例已改成不会为这条已知缺口误报**：
`testOpenLeavesPlayerPausedWithNoPlaybackBeforePlay` 原先卡在 position ≤ 0.05，
机器一忙就会为这条**已登记接受**的竞争把套件测红。现在首个断言放宽到 0.25 秒
（"没有走出一次抢跑能解释的距离"），并支持 `SYPLAYER_SKIP_RACE_SENSITIVE_TESTS`
环境变量整条跳过；真正的回归探针——300ms 之后位置**不再前进**，容差 0.01——
保持不变，它才是能抓住"pause() 被删掉"的那一条。

## 71. 没有公开的播放 C ABI，非 Swift 宿主仍需自己实现一层 🟢 M4 非目标

**现象**：`swift/SYPlayerKit/` 的 Swift 公开 API 是 M4 唯一交付的接入层，
`include/syplayer/syp_player.h`（spec 缝①）本轮没有做；`SYPlayerKit_Private`
模块（ObjC++ 桥）也不对外公开。Flutter/RN 等非 Swift 宿主目前没有任何可用
的播放接入点。

**为什么这样做**：spec §2"非目标"明确排除，`docs/roadmap.md`"M4 交付物"
一节的非目标段同样列出；一个只有 Swift 一个消费者的 C ABI 在当前是纯成本，
接口形状要等真的接 Flutter/RN 时才知道。

**后续**：需要时另立任务设计公开 C ABI，参见 `docs/roadmap.md` M4 非目标段与
spec §2。不在此重复展开。

## 72. `.failed` 在 Swift 门面层是终态，而桥把错误当作可恢复的 🟡 M4，终审 I2

**现象**：`SYPBridge.h` 的 `onError` 契约明写"**报错后泵线程不停止**（M3c 终审
Minor 3），出现非 Error 结果（如 seek 成功后恢复播放）即解除，再次出错会再
触发"——也就是说，错误在桥这一侧是**可恢复**的。而 Swift 门面层把错误记在
`SYPlayer.lastError` 这个闩上，`refresh()` 每一拍都拿它算 `playback`
（`SYPlayerSnapshotMapping.swift` 的 `if let error = error { return .failed(error) }`
优先级最高），于是一旦进 `.failed` 就再也出不来：桥自己恢复播放时，
`state.position` 会在 `.failed` 底下继续前进，状态却纹丝不动。

**本轮做了一半**：`play()` 现在和 `seek()` 一样先清 `lastError`/`pendingError`
（`swift/SYPlayerKit/SYPlayer.swift`），所以"出错 → 点一下播放"这个最自然的
重试动作能走通了。**没做的是另一半**：`.failed` 仍然不会自己退出。

**正确的修法（属于后续里程碑，不在 M4）**：给 `SypPlayerSnapshot` 加一个错误
字段，把桥那边的错误闩如实上报，让 `refresh()` **从桥的活闩派生** `failed`，
而不是从 Swift 这一侧自己记的闩派生。那样"桥恢复了、状态跟着恢复"就是结构性
成立的，不再依赖调用方恰好调了 play/seek。改桥的快照结构要连带核对
`SYPlayerRawSnapshot` 的逐字段搬运与全部状态映射用例，是一块独立投入。

**这是第三次遇到同一个形状的问题**（前两次：EOF 闩不随 seek 重新武装、
`duration_us_` 在锁外写），都是"Swift 层断言了桥并不保证的东西"。修法一律
是修在源头，不是在上层绕。

## 73. HTTP 缓存目录不可通过 API 配置 —— **已补上** 🟢 M5 Task 8

**原现象**（M4）：`swift/SYPlayerKit/Internal/SYPBridge.mm` 的 `-openURLString:`
把缓存目录写死为 `NSTemporaryDirectory()` 下的 `syplayer-http-cache`，公开的
Swift API 上没有任何入口能改路径、设容量上限或触发清理。

**M5 Task 8 的落地**：新增 `SYPlayerCacheConfiguration`（`directory` /
`maxBytes` / `minFreeSpaceBytes` / `timeToLive`）与 `SYPlayer(cache:)`，作用域
定为**每个实例一份**；默认目录从 tmp 挪到 `Caches/syplayer-http-cache`（tmp 由
系统在空间紧张时无预警回收，预加载花带宽暖出来的字节最不该落在那里）。
`SYPlayerPreloader` 用同一个配置类型，**目录相同即共享同一份缓存**——两侧的
目录都经 `syp::dl::normalize_cache_dir()` 归一化（全仓唯一一份实现）。

**M5 Task 8 fix round 1 补的两半**（复审判出这条当初关得早了半步）：
- **归一化守错了方向**（ruling R6）。原实现只剥尾斜杠，而实测
  `URL(fileURLWithPath:"/a/b/").path == "/a/b"`——**尾斜杠公开 API 根本产不出**；
  产得出的是 `//`、`.`、`..`（`URL.path` 原样保留），而
  `NSTemporaryDirectory()` 自身以 `/` 结尾，于是最自然的
  `NSTemporaryDirectory() + "/" + name` 就产出 `…/T//name`，实测后果是播放侧
  `open_count` 读到 0、让路逻辑静默失效。现在加了
  `lexically_normal()`（**顺序在剥尾斜杠之前**，因为它自己会造出尾斜杠）。
  已知局限两条：不做 Unicode 归一化（不需要：`URL.path` 恒输出 NFD，两侧
  逐字节相同）、不解析符号链接（`..` 按字面弹）。
- **容量/TTL 那一半当初没有检验缝**（ruling R7）。目录那一半有
  `cacheDirectoryInUse` + 变异验证，而 `max_cache_bytes` /
  `min_free_space_bytes` / `cache_ttl_ms` 从 `bridged()` 之后就没人看着——把
  `prepare_cache_dir` 里那三行改成硬 0（缓存无上限、永不过期、无限涨）74 条
  用例一条都不红。现在两侧各有读回缝（预加载侧读
  `PreloadStack::provider_config_for_test()`，播放侧读
  `-lastOpenCacheConfig`，后者由**真的** `-openURLString:` 写下），三个字段
  可以一个一个单独被变异杀掉。

**M5 Task 8 fix round 2 补的两条**（复审判出 fix round 1 的缝装在支流上）：
- **容量/TTL 的检验缝装错了分支**（ruling R8）。R7 补的那条读回
  `MediaInfoProvider::cfg_`，而承担**全部预加载下载**的是 `syp::dl::Preloader`
  和它开出去的 `SourceBridge`——那一支一条断言都没有。复审在
  `PreloadStack::create` 里 provider 构造之后、`Preloader::create` 之前把三个
  字段清零，**`ctest 33/33` 与 `xcodebuild 82 tests, 0 failures` 同时全绿**。
  现在 `Preloader` 有 `dl_config_for_test()`（读 `base_config()`，也就是它派给
  每一条 `SourceBridge` 的那份配置的起点），经 `PreloadStack::
  preloader_config_for_test()` → `-preloaderCacheSettingsInUse` 到 Swift；
  另加一条**行为用例**（`preloader_capacity_actually_evicts_an_unreferenced_entry`）
  钉最终后果——预加载的源真的会按 `max_cache_bytes` 淘汰掉没人引用的旧条目。
- **C ABI 上的 `cache_dir` 仍然没有闸**（ruling R9）。`syp_source_open` /
  `syp_preloader_create` 原样吃调用方的目录串，直接用 C ABI 的调用方混用
  `…/d` 与 `…//d` 照样静默分裂成两份缓存。fix round 1 的注释把修法写成了
  **假二选一**（"要么破 dl 的 purity 闸、要么抄第二份归一化"）；第三扇门是
  开着的：purity 闸只禁 `.mm`/`.m` 源与链接接口里的平台 framework，对纯 C++
  stdlib 没有意见。现在唯一那份实现下沉进 `src/dl/cache_store.{h,cpp}`
  （紧挨 `make_key`），收口在 `SourceBridge` 与 `Preloader` 的**构造函数**里
  ——也就是 `cache_dir_` 诞生、同时喂给 `make_key` 和落盘路径的那一行。
  **刻意不放进 `make_key` 里**：实测那样 `ctest` 红 2 条用例 5 条断言，而且
  `enforce_capacity` 手工拼的那个注册表 key 会与它分叉，把还开着的条目删掉。

**仍然没有的**（下一轮再定，不在本条范围内）：
- **目录建不出来不会被上报**（fix round 2 复审 Minor 2）。`-setCacheSettings:`
  返回 `void`、`createDirectoryAtPath:` 的 `error:` 传 `nil`，实测词法父目录
  `chmod 0500` 时调用方拿到零信号，真实后果是第一次 `open` 报 `.io`。fix
  round 1 的注释声称"建不出来会在配置那一刻暴露"，那句话是假的，已订正。
  要真做到得给 `-setCacheSettings:` 一个出参或可读的 lastError，而那会动到
  今天不 `throws` 的 `SYPlayer.init(cache:)`。
- **`SYPlayerCacheConfiguration.directory` 不校验 `isFileURL`**（Minor 5，
  Task 8 原生）。实测 `URL(string:"https://example.com/cache")!.path` 是
  `/cache`（文件系统根目录），`URL(string:"~/Library/Caches/syp")!.path` 是
  字面量 `~`。两者都不报错。刻意不加 `precondition`：那是在公开初始化器上崩
  调用方的 App。
- **没有公开的"立刻清理"入口**。淘汰只在打开资源时按 `maxBytes` /
  `minFreeSpaceBytes` / `timeToLive` 被动触发，业务侧没有 `purge()`。
- **没有"当前缓存占了多少"的读数**。`SYPlayerPreloadStatistics.downloadedBytes`
  是本次会话下载量而且低报（见 §6 的注释），不是磁盘占用。
- **配置在 `open()` 之后改不生效**：`-openURLString:` 只在建 `syp_source` 时读
  一次；`SYPlayer.cacheConfiguration` 因此是 `let`，改配置要新建一个实例。

## 74. 没有音量 / 静音，也没有原始视频尺寸 —— **已修复** 🟢 M4 终审 I3，M6d 交付

**原现象**（M4）：`SYPlayer` 的公开 API 里没有 `volume`、没有 `isMuted`，
`SYPlayerState` 里也没有视频的原始宽高。前者意味着接入方只能改系统音量（或者
自己去碰 `AVAudioSession`，而框架并不管理它，见 #66）；后者意味着**画面区的
宽高比只能靠猜**——两个 demo 页都把它硬编码成 16:9（`demo/shared/
PlayerView.swift:177` 的 `multiplier: 9.0 / 16.0`、`demo/shared/
SwiftUIPlayerViewController.swift:48` 的 `.aspectRatio(16.0 / 9.0, contentMode:
.fit)`）。播一份 4:3 或竖屏素材时，`MetalRenderer` 的 aspect-fit 会把画面正确
地缩在容器里，但容器本身的形状是错的，上下/左右留黑边。

**M6d 的落地**：`SYPlayer.volume`（`0...1`，非有限值口径 NaN→0、+inf→1、
−inf/负数→0，与 #72 M5 终审 F3 给 `SYPlayerCacheConfiguration` 定的口径同源、
各写一份不互相引用）与 `SYPlayer.isMuted`，两者合并成单一增益（`muted ?
0.0 : volume`）作用在 `AudioUnitSink::render_cb` 上，15ms 线性斜坡防爆音，
不碰系统音量、不碰 `AVAudioSession`（#66 仍不受管理，本轮非目标）。
`SYPlayerState.videoSize: CGSize?` 给的是**显示尺寸**（含 SAR 拉伸与旋转规整
之后的结果，不是编码宽高），随 `open()` 写入、`hasMedia == false` 或纯音频源
时为 nil；两个 demo 页已删掉硬编码的 16:9，改用它。`SYPlayerVideoGravity` 补
齐 `.aspectFit`/`.aspectFill`/`.resize` 三态，`videoGravity` 可在播放中任意
时刻切换。持久状态（音量/静音/gravity）放在桥的 `PlayerCore` 上、跨 `open()`
保留（与原设计写的存放位置不同，属主动偏差）。

**本条关闭后新开的遗留登记**（不在本条范围内）：#94（显示矩阵的镜像分量未
处理）、#95（非 90° 倍数旋转被就近取整、静默改变方向）、#96（真机方向/听感
人工验证，与 #51/#53 同类）。

## 75. `SYPlayerKit` 里一条 Mac Catalyst 可用性警告未登记 🟢 既有问题，M4 终审复审补记

**现象**：干净编译 `SYPlayerKit`（Mac Catalyst）会出一条
`src/platform/apple/metal_renderer.mm:715:19: warning: 'addPresentedHandler:' is only available on
macCatalyst 13.4 or newer [-Wunguarded-availability-new]`。

**来历**：`ff22b93`（模拟器 SDK 无 `addPresentedHandler:` 那次修复）引入，M4 一行没碰。M4 各任务报告里
"零警告" 的说法指的是 Swift 侧与 demo 侧，`SYPlayerKit` 目标本身并非零警告——终审复审实测纠正了这一点。

**影响**：仅警告。Catalyst 的部署目标是 13.0，而该 API 要 13.4；真机/模拟器路径不受影响
（`#if !TARGET_OS_SIMULATOR` 分支已按 SDK 差异处理）。

**修法**：在那一处补 `if (@available(macCatalyst 13.4, *))`，或把 Catalyst 的部署目标提到 13.4。
两者都要重新跑三个目标的编译与 `check-deploy-target.sh`，留到下一次碰这个文件时做。

## 76. `syp_preloader_remove` 对**无关条目**的生效要等一次播放列表读超时 🟡 M5 Task 7，复审 M2

**现象**：`Preloader` 只有一条驱动线程，HLS 的播放列表抓取（`fetch_text`）
**同步**跑在它上面。`remove(u)` 在锁外打断在途抓取，但打断对象由
`fetching_owner_` 指定，只打断**属于 `u` 自己**的那一次。驱动线程正卡在
**别的**条目的播放列表 `read()` 里时，`remove(u)` 要等那次读自己返回。
实测 **6,004ms**（`kPlaylistReadTimeoutMs` 5s + 一次重试的开销）。

**不是什么**：不是无界等待，也不是正确性问题。条目最终一定会消失；
上界 ≈ `kPlaylistConnectTimeoutMs` + `kPlaylistReadTimeoutMs` ×
(1 + `kPlaylistMaxRetries`)，三个常数由
`tests/test_preloader_hls.cpp::a_stalled_playlist_fetch_gives_up_within_the_retry_bound`
钉住（门槛 10,000ms；常数退化时实测 15,021ms ⇒ 变红）。
`remove` 打断**自己那一条**是有效的：实测 0–2ms，60 次随机化时序的试验里
没找到窗口（不打断则要 5,819ms）。

**影响面**：只在"同时预加载多条 HLS，且其中一条的播放列表服务端卡住"时出现。
表现是 `syp_preloader_remove` 返回之后 `get_stats().entries` 还要几秒才减一，
以及 `syp_preloader_destroy` 跟着多等这几秒。

**修法**：把播放列表抓取挪出驱动线程——异步发起 + 一套在途任务的所有权管理。
那是重构不是修补（ruling R3 判定本轮不做），风险远大于收益。真要做时一并
处理 provider 探测（同一条线程上的同一类阻塞）。

## 77. `SYPlayerPreloader` 的析构是**异步**的 🟡 M5 Task 8

**现象**：`SYPlayerPreloader.deinit` 不在原线程销毁底层的
`syp::media::PreloadStack`，而是把最后一份强引用交给
`DispatchQueue.global(qos: .utility)`。于是 `deinit` 返回时驱动线程还活着，还
可能继续往缓存目录里写字节、还占着连接额度。

**为什么这么做**：`~PreloadStack` 要 join 驱动线程，而驱动线程可能正卡在
provider 的只读 header 探测里。本机实测（只接受连接、永不响应的 socket）：
就地析构 **5,205ms**，交后台队列 **0.023–0.046ms**。`SYPlayerPreloader` 是
`@MainActor`，正常写法下 `deinit` 就在主线程上——5 秒的主线程卡顿不可接受。
由 `SYPlayerPreloaderTests.testDeinitDoesNotBlockTheCallingThread` 钉住（门槛
200ms；改回就地析构即变红，实测 5,205ms）。

**影响面**：
- 要断言"这个 preloader 已经彻底停了"的用例必须自己等（泄漏用例就是轮询
  `weak` 引用变 nil）；
- 进程立刻退出时那条后台析构可能来不及跑完，已下字节留在缓存里——这本来就是
  `syp_preloader_destroy` 的既有语义，不是新增的不确定性；
- **真正的修法是给探测挂一条硬死线（墙钟看门狗）**，那样就地析构也只有几百
  毫秒。已登记在 Task 10 / §8.4，与 #76 是同一条驱动线程上的同一类问题。

## 78. `syp_cache_evict` / `_clear` / `_remove` 不查 `CacheStore` 🟡 M5 Task 1

**现象**：这三个全局缓存管理函数按"扫目录 + 删文件"工作，**不检查该 key 是否
正被某个 `syp_source` / `Preloader` 打开着**。删掉一个正在被读写的资源，会让活着
的 `CacheIndex` 声称有数据、`.dat` 却已经不存在。

**为什么本轮不改**：它们的语义是"调用方明确要求删"，改成"跳过被打开的 key"会
悄悄改掉 `syp_cache_clear` 的公开承诺，也会让既有用例 `cache_size_remove_clear`
的判据变模糊。`CacheStore::enforce_capacity()`（自动淘汰）**已经**跳过被引用的
key——两者的区别正是"自动 vs 显式"。

**M5 把它从单源 TOCTOU 变成了跨持有者 TOCTOU**：`CacheStore` 之前，同一个
(目录, URL) 在进程内只可能有一个 `SourceBridge`；现在预加载与播放会**共享同一份
`CacheIndex`/`CacheFile`**，所以一次 `syp_cache_remove` 能同时打到两个持有者。

**影响**：只有调用方在播放/预加载进行中主动调这三个函数时才会撞上。表现是读到
零字节或 `SYP_ERR_IO`，不会静默错播（`cache_index.h` 顶部"残留风险"第 2 条是
同一类 TOCTOU）。

**旁证**：这三个函数**不归一化** `cache_dir`，但那**不构成分裂**（Task 8
fix-round-2 复审逐个调用点查过）——它们只把 `cache_dir` 当文件系统路径用，
从不当注册表 key。

**修法**：给这三个函数加一个"跳过被引用的 key"的变体，或者在文档里把"调用前须先
关闭相关 source"写成硬约束。

## 79. `Preloader` 的驱动线程是单条，慢活会互相排队 🟡 M5 Task 4/6/7

**现象**：provider 估算（开一次连接读 header）与 HLS 展开（串行抓两次播放列表）
都跑在同一条驱动线程上，每轮各最多做一次。一次慢估算会把**另一个条目的**估算
推迟到下一轮。

**为什么这样设计**：所有阻塞动作在一条线程上就只有一条时间线，配额分配与开关源
的顺序因此是确定的，测试也能用 `wait_settled_for_test()` 拿到一个静止的观测点。
计划里已经把 `Close` 排在慢活之前——"高优先级要停低优先级"这件急事永远不会排在
一次慢估算后面，受影响的只有"多个条目同时需要估算"。

**影响**：N 个新条目同时入队时，最坏要 N 轮才全部估完。每轮的代价是一次 header
请求（几十到几百毫秒）。#76（remove 的生效延迟）与 #77（析构异步化）是同一条
线程上的同一类问题。

**修法**：给慢活开一个小线程池，或者把估算做成"先按字节起下、估出来再修正目标"。
两者都要重新想清楚"配额在估算期间算给谁"。**更便宜的那一半**是给探测挂一条墙钟
看门狗（#77 已点名），它同时缩掉 #77 的 5 秒与本条的最坏单轮时长。

## 80. 预加载条目的配额在两个非零值之间变化时不立即生效 🟢 M5 Task 4

**现象**：`0 → 非 0` 与 `非 0 → 0` 立即生效（开源 / interrupt + close），但
`3 → 2` 这种变化要等该条目下一次真正 open 才用上新值。

**为什么**：为了少一条连接就把已经建好的连接全拆掉重连，代价高于收益；而 spec
决策 4 要保证的"低优先级让路"本身就是 `非 0 → 0` 这个边界。

**影响**：优先级密集变动时，实际并发可能比理论配额多 1~2 条，短时间内。

## 81. dl 侧的 m3u8 判据与 media 侧的 `url_rewrite` 各有一份 🟡 M5 Task 7

**现象**：`src/dl/m3u8_scan.cpp` 的 `is_playlist_url()` 与加密判定，和
`src/media/hls/url_rewrite.cpp` 的 `channel_for()` / `playlist_declares_encryption()`
**判据相同、实现两份**。

**为什么不能合并**：`Preloader` 在 dl 层，而 dl 必须零 FFmpeg、零平台符号
（`syp_dl_purity_check`）；`url_rewrite.cpp` 虽然本身也零依赖，但它挂在 `syp_media`
里，dl 依赖 media 会把整条依赖方向倒过来。

**这不是理论风险，它真的漂过三次**，每次都是靠对跑真 FFmpeg 才发现的：
1. 加密判据：dl 说"没读到 METHOD ⇒ 不加密"，media 说"证明了 METHOD=NONE 才放行"
   ——16 条播放列表里 4 条分歧，后果是去暖一条 media 层根本不会打开的流；
2. BOM：dl 剥 UTF-8 BOM、media 不剥 ⇒ fuzz 里 7,876 条分歧（已按 FFmpeg
   `hls.c:849` 的字面 `strcmp` 统一成"不剥 + 首行 `#EXTM3U` 闸"）；
3. `has_endlist`：228,361 次对比里 **38,139 次**分歧（行首空白、裸 `\r` 行尾）。
   ⇒ 最后**删掉了 dl 侧那一份**（`src/` 里零消费者），不留第二份转写。

**防漂移的手段**：两侧的判据表**是同一个文件**——`tests/support/hls_cases.h`
（11 条 channel + 17 条 encryption + endlist），`test_hls_url_rewrite.cpp` 与
`test_preloader_hls.cpp` include 的是同一份，头注释互相点名。
**共享表挡得住"改了代码忘了改用例"，挡不住"两侧同时都不守某个字符"**：
`av_isspace` 的六个字符里 `\v`/`\f` 曾经两侧都没人钉，51 条表 + 920 万次 fuzz
一条都杀不掉。现在有专门的用例钉那六个字符。

**修法**：把纯文本判定那几个函数抽成一个不依赖任何层的独立小库（`src/common/`），
两边都链它。本轮没做是因为要新开一层目录与 CMake 目标。

## 82. 预加载的时间→字节估算对 VBR 素材只保证"不少于" 🟡 M5 Task 6

**现象**：`MediaInfoProvider` 用 `头部常数项 + 总字节 × 时长比例`（CBR 假设）折算，
再乘 5/4 的安全系数、夹到资源总长。对码率波动大的素材，实际暖出来的时长可能明显
多于或少于请求值——本轮只保证**不会少于**。

**已测量的偏差**：+6.6% @1s、+15.7% @3s；补上头部常数项之后 500ms 的 −7.8% 欠下
消失，但 1000ms 变成比真实需要多 **118%**——因为"探测够到的前缀长度"并不等于
真实的头部长度。**这多下的是真的网络字节**（约 462 KiB/条目），不只是多占缓存。

**已知的更好办法**（本轮没做）：`avformat_index_get_entry(st, 0)->pos` 直接给出
第一个媒体字节的偏移，**零额外 IO**，三个 fixture 上实测 49,717 / 64,797 / 62,095。
文档里曾写着"真实头部偏移拿不到"，**那句话是假的**。未建索引的容器再退回用探测
够到的长度。

**另一个已登记的坑**：探测的 2 MiB 上界数的是**本次新下的字节**（open 后的增量），
不是目录里已缓存的总量——早期版本数后者，于是一次按时长的预加载暖了 >2 MiB 之后，
**下一次会话对同一个 URL 的探测会直接 NOT_IMPLEMENTED**（实测：预热 3.1MB ⇒ 零
新网络字节就放弃；预热 1MB ⇒ 同 URL 同 ms 估出 2,229,233 vs 冷缓存 1,025,009）。
已修，由 `probe_cap_and_estimate_ignore_bytes_cached_before_the_probe` 钉住。

**影响**：VBR 素材上预加载可能多占一些缓存与带宽。相比"少下导致首帧仍要等网络"，
这是刻意选的那一侧（spec §7）。

**修法**：读 `sidx` / 索引表拿到真实的"时间 → 字节"映射，属于独立课题（M6 之后）。

## 83. `syp_preload_stats::downloaded_bytes` 是**条目源**的字节，不是本次网络总量 🟡 M5 Task 9

**现象**：这个字段**只计入 `Entry::source`** 这些条目源（含 HLS 展开出的
`EXT-X-MAP` 与前若干分片）。不计入的有两类：
1. **provider 探测源**——上界 `kProbeMaxBytes` 2 MiB/URL；实测一个 1,440,157 B 的
   faststart MP4，探测落盘 **524,288 B**；
2. **`fetch_text` 为每张播放列表另开的一次性 `SourceBridge`**（master 一次 +
   选中的 media 一次）——上界 `kMaxPlaylistBytes` 8 MiB/次；实测 120 B + 271 B。

实测一例：统计报 **545,588 B**，缓存目录同期实际落盘 **1,070,267 B**，
**低报 49.0%**。

**反方向同样要写明**：它**也不是**缓存目录的占用量——目录里还有历次会话的字节、
还有播放页写进去的字节。demo 的预加载演示页曾把"目录总量 − 统计值"印成"低报的
那一部分"，同目录连跑两次不清缓存时它声称低报 1,594,946，真值仍是 524,679
（**夸大 3.0×**，且每跑一次再涨约 1 MB，无上界）。页面已改成记一个起始基线、
只显示增量，**减法只数 `.dat`**（`.idx` 一轮 714 B，算进去就成了假低报）。

**确切措辞已写进** `include/syplayer/syp_preload.h` 该字段的注释。

**修法**：要一个真的"本次网络总量"，得让 provider 与 `fetch_text` 也把字节记回
同一个计数器——那要给 `MediaInfoProvider` 一条回调，属于接口改动。

## 84. 缓存目录建不出来时，调用方拿不到可编程的错误信号 🟡 M5 Task 8/10

**现象**：`SYPlayerCacheConfiguration` 传进来的目录，由 `SYPBridge.mm` 的
`prepare_cache_dir` 在 `-setCacheSettings:` 那一刻 `createDirectoryAtPath:` 建出来。
建不出来（父目录只读、磁盘满、路径被同名普通文件占着）时，调用方能拿到的信号是
**零**——`-setCacheSettings:` 返回 void，`SYPlayer.init(cache:)` 不 `throws`。
真实后果是第一次 open 时 dl 层报 `SYP_ERR_IO`，**伪装成一次"打开失败"**。

**M5 Task 10 补了一半**：`error:` 不再传 `nil`，失败时走
`syp::dl::log_msg(SYP_LOG_WARN, "cache", …)` 打一条带原因的日志。
**零公开 API 变更、零 source-breaking、不崩任何人**——"零信号"变成"日志里有一条"。
（上一轮把这件事拒绝成了"要么 throws、要么出参、要么 lastError，都动公开 API"
的假二选一；`log_msg` 这条现成的非致命诊断通道是**第三扇门**。）

**仍然缺的那一半**：可编程检测。要它得给 `-setCacheSettings:` 一个出参或一个
可读的 `lastError`，那会动到 `SYPlayer` 今天不 `throws` 的公开初始化器。

## 85. `cache_dir` 的**相对 vs 绝对**与**大小写**两种拼法仍然分裂 🟡 M5 Task 8/10

**现象**：`syp::dl::normalize_cache_dir()` 是纯词法的（`lexically_normal` + 剥尾
斜杠），于是：
- `"cache"` 与 `"/abs/path/cache"` 即便指向同一个目录也是**两个 key**（实测
  `"/…/n5"` vs `"n5"` → SPLIT）。Swift 门面产不出相对路径（`URL.path` 恒为绝对），
  但 **C ABI 调用方完全可能写 `cfg.cache_dir = "cache"`**。
- APFS 默认大小写不敏感（保留大小写）的卷上，`Sub` 与 `sub` 是同一个目录、两个
  key（实测 SPLIT）。

**为什么不修**：要合流得用 `fs::absolute()`，而它依赖**进程的当前工作目录**——
一个会被任何人随时改掉的全局状态，拿它当 key 的一部分比分裂更糟；大小写折叠则
要查 `statfs` 拿文件系统真值，而这个函数刻意只做词法（与"不解析符号链接"同源）。

**后果的量级**：缓存分裂（各下各的、`open_count` 让路逻辑静默失效）。key 里带着
URL 的哈希，所以两个 key 指的仍是各自正确的内容。

> **订正（全支终审 F1）**：这一段原先还写着"**不会串到别人的数据上**"。那句话
> 是**错的**，而且错得很严重。`CacheStore::enforce_capacity()` 手上只有文件名里
> 的哈希，原先它拿**调用方传进来的那个 cache_dir 串**拼注册表 key 去问"这组
> 文件还有没有人开着"；两种拼法在注册表里是两个 key，而 `scan_cache_dir()` 拼
> 出来的**文件路径是同一组文件** ⇒ 查不到那条活着的条目 ⇒ 直接 `unlink`。
> 实测五类拼法全中，`.idx` 与 `.dat` **双双 GONE 而 `open_count` 仍为 1**，
> 返回值还是 `SYP_OK`，而活着的 `CacheIndex` 仍然声称有 `[0,600000)`、盘上一个
> 字节都没有（正是 `cache_index.h`「残留风险」第 2 条那个状态）：
>
> | 拼法 | 修复前 | 修复后 |
> |---|---|---|
> | 绝对 vs 相对（`/…/ra` vs `ra`） | `idx=GONE dat=GONE`，返回 `SYP_OK` | `PRESENT`，返回 `SYP_ERR_NO_SPACE` |
> | APFS 大小写（`cachedir` vs `CACHEDIR`） | 同上 | 同上 |
> | **`/var/folders/…` vs `/private/var/folders/…`** | 同上 | 同上 |
> | 目录符号链接（`realdir` vs `linkdir`） | 同上 | 同上 |
> | Unicode NFC vs NFD（`café` 两种写法） | 同上 | 同上 |
>
> 第三类**不需要调用方犯任何错**：`/var → private/var` 是系统软链，
> `NSTemporaryDirectory()` 给 `/var/…` 而 `resolvingSymlinksInPath()` /
> `realpath()` 给 `/private/var/…`——那是 Foundation 自己对同一个目录给出的
> 两个答案。
>
> **修法不是再写一份更强的归一化**（词法归一化永远补不上：相对 vs 绝对要 cwd、
> 大小写折叠要 statfs、软链要 resolve、NFC/NFD 要知道卷的归一化策略，四件事全是
> 文件系统真值；而"同一道闸的第二份转写悄悄漂移"这个里程碑已经被咬过两次）。
> 改成：`CacheStore::Entry` 在 `acquire()` 时记下缓存目录的 **dev+ino**
> （一次 `::stat`），`enforce_capacity()` 判"活没活"时按 dev+ino 认人，key 串
> 那一道保留作为 stat 失败时的兜底。回归用例
> `enforce_capacity_spares_a_live_entry_under_another_spelling` 把五类逐一钉住。
>
> **所以本条现在只剩"缓存分裂"这一个后果**（各下各的、让路失效），那部分描述
> 依然成立；"删到别人的数据"那一半已经关上了。残留的同源尖角：`space_backoff_`
> 仍按 cache_dir **串**记账，两种拼法各记一份——后果只是退避不共享（另一种拼法
> 照样会去删），不会删到活文件。

**已经关上的那些拼法**（别以为整件事没做）：`d` / `d//` / `d/` / `d/./` / `d/x/..`
在纯 C 程序里实测**全部 SHARED**（去掉构造函数里的归一化则全部 SPLIT）。
归一化的唯一实现在 `src/dl/cache_store.{h,cpp}`，`SourceBridge` 与 `Preloader`
的**构造函数**各调一次，C ABI 的两个入口因此都被关上了。

**记录处**：`src/dl/cache_store.h` 的「四条刻意不做的事」清单（其中"相对 vs 绝对"
那条是 M5 Task 10 **补回来的**——它原本写在旧的 `make_key` 注释里，Task 8 重写
清单时弄丢了。文档退化比代码退化更难发现）。

## 86. `xcodebuild -enableThreadSanitizer YES` 今天不能当闸门：`AudioRing` 的析构与 CoreAudio 渲染回调竞争 🟠 M3 既有（全支终审 F17 登记）

**现象**：`xcodebuild test -enableThreadSanitizer YES` 在本机 **崩 6 次**。
报告点名的是 `syp::media::AudioRing::~AudioRing()`（在 `PlayerCore::close_internal()`
里，线程 T5）与 CoreAudio 渲染回调里的 `AudioRing::read()`（线程 T23）：
**sink 正在被拆的时候，渲染回调还在它里面**。

**不是 M5 的代码**：`AudioRing` 与 `AudioUnitSink` 是 M3 的产物，M5 一个字都没改
它们。M5 自己的那些新对象在 TSan 下是干净的——复审实测 24 条 M5 用例全过、
**M5 符号零竞争**，`SYPlayerPreloader.deinit` 把桥交后台队列那条路径也零告警。

**为什么这条比它看起来严重**：它让 **TSan 在 `xcodebuild` 这一侧整个用不了**。
CMake 那一侧有独立的 `build-tsan`（M5 复审跑过全量 33/33、`WARNING: ThreadSanitizer`
出现 0 次、598 秒），所以 dl/media 层仍然有 TSan 覆盖；但 **ObjC++ 桥
（`SYPBridge.mm`）与 Swift 门面根本不在 CMake 里**，它们唯一可能的 TSan 覆盖就是
`xcodebuild`，而这条既有竞争让那条路每次都崩在同一个地方。也就是说
`SYPBridge.mm` 的并发正确性今天**没有任何自动化工具在看**。

**修法**：拆 sink 之前先把渲染回调停掉并等它真的退出（`AudioOutputUnitStop` 之后
还要有一个"回调不会再进来"的同步点），或者给 `AudioRing` 的生命周期加一层
引用计数让回调持有它。属于 M3 的音频输出路径，不是本里程碑的范围。

**在那之前**：`xcodebuild` 侧不要把 TSan 写进任何门禁清单——它会恒红，而恒红的
闸门的下场就是被关掉。要给 M5 的代码跑 TSan 就跑 `build-tsan`（CMake 侧）。

## 87. 非 file URL 且 `.path` 为空时，缓存目录**静默**退回默认值 🟢 M5 Task 8（全支终审 F17 登记）

**现象**：`SYPlayerCacheConfiguration.directory` 是个公开的可变属性。传一个非
file URL 进去，`bridged()` 里的 `assert(directory.isFileURL, …)` 在 **Release 下是
完全的 no-op**；而如果那个 URL 的 `.path` 恰好是空串（例如 `URL(string: "x:")!`），
下游就当"没给目录"处理，**静默退回默认的 `Caches/syplayer-http-cache`**。

**为什么这是个坑**：那正是 `SYPlayerPreloader.swift` 的注释里明确说"不做"的那件
事——"静默换一个目录会让调用方拿到一个看起来成功了、但缓存落在别处的结果"。
Debug 下 assert 会断住，所以写代码的人当场看得见；Release 下一个人都不崩，也一个
信号都没有。

**为什么不修**：加一条运行时拒绝要么是崩（公开初始化器上崩调用方的 App，代价
比缓存落错目录大）、要么要改公开 API 的形状（`throws` / 出参 / `lastError`），
两者都超出本次修复波次的范围。已有的 `cacheDirectoryInUse` 能把真实目录读回来，
调用方要确认可以自己读一下。

## 88. `AvioBridge::Diag::max_read_end` 的名字与注释都不是它真正的含义 🟢 M5 Task 6（全支终审 F17 登记）

**现象**：`media_info_provider.cpp` 拿它当"这次解析够到的**前缀长度**"用，注释也
这么写；但它实际是"**够到过的最大偏移**"。对一个 moov 在尾部的 MP4，FFmpeg 会
seek 到文件尾读 moov，于是这个值接近文件总长，而真正读过的字节只有前后两小段。

**后果在安全的一侧**：它被当成 `header_bytes` 这个**加数**，还夹在 `kProbeMaxBytes`
里，所以偏大只会让预加载**多下一点**，不会让首帧等网络（估算的契约本来就只保证
"不少于"那一侧，见 #82）。

**为什么不改**：改名要动 `avio_bridge.h` 的公开面与所有调用点，而真正该问的问题是
"常数项到底该取什么"——取"读过的字节总数"还是"最大偏移"在 faststart 与 moov-at-end
两类素材上是两个不同的答案，需要重新量一轮。本轮只把事实记下来，不改。

## 89. `SYPlayerPreloader.add()` 文档说的 `false` 三种情形，漏了 `.url(URL(fileURLWithPath:))` 🟢 M5 Task 8（全支终审 F17 登记）

**现象**：`add` 的文档注释说返回 `false` 只有三种情形，其中一种是"`.file` 源"。
但 `.url(URL(fileURLWithPath: "/tmp/a.mp4"))` 是个 `.url` 源、`absoluteString` 非空
（`file:///tmp/a.mp4`），于是 **`add` 返回 `true`**，然后异步失败。

**为什么不修**：判它要把 dl 层的 scheme 白名单在 Swift 侧**再转写一份**，而本里
程碑已经两次被"同一道闸的第二份转写悄悄漂移"咬到（`playlist_has_endlist` 的两份
实现对 `\v`/`\f` 与 NUL 的处理已经不一致）。`add` 的语义本来就是"登记一条意图"，
真正的失败是异步的，已经有 `statistics.failed` 这个指定通道。
**要修的是文档措辞**，但它与 #83 那段口径改写是同一块，留到下一次动这段注释时一起。

## 90. 预加载 demo「分行口径」的警告文案把 6/3/3 与 12 写死了 🟢 M5 Task 9（全支终审 F17 登记）

**现象**：`PreloadViewController` 的分行口径警告写的是"第 1 行有「正在播」条目
⇒ 额度 6，第 2/3 行各 3，聚合上界 12"。那只在**样例那套优先级**（正在播 / 下一个 /
背景）下成立。三行都设成「背景」时，三个 preloader 都没有 `.playing` 条目，
`budget = total - reserve = 3` 各自成立，真实聚合上界是 **9** 而不是 12。

**为什么不修**：要说准就得把 `allocate_locked` 的 `budget` 公式在 Swift 侧算第二遍
（同一个"第二份转写"的坑），或者给 `SYPlayerPreloader` 加一个公开的"当前额度"
读回口——后者是公开 API 变更。这一格本来就标着"仅演示·别照抄"，写死的数是样例
默认摆法下的真值，方向（分行会把额度放大、让 `reservedForPlaying` 失效）是对的。

## 91. `-[SypPreloaderBridge initWithCache:]` 丢弃 `syp_status`，返回一个会静默空转的对象 🟡 M5 Task 8（全支终审 F17 登记）

**现象**：底层 `PreloadStack::create` 失败时（HTTP 后端没注册上、缓存目录建不出来、
`new` 失败），`-initWithCache:` **仍然返回一个非 nil 的对象**，只是它的 `_stack`
是 `nullptr`。之后所有方法——`add` / `setPriority` / `remove` / `removeAll` /
`statistics`——都**静默空转**：`add` 返回 `false`，统计恒为全零。

**观测口**：`add` 恒 `false` 是可区分的（它是那三种 `false` 之一，见 #89 的文档），
`statistics.entries` 恒 0 也是；但没有任何一个能告诉调用方**原因**。

**为什么不修**：ObjC 初始化器要报错就得 `initWithCache:error:` 或返回 nil，两者
都是公开面（`SYPBridge.h`）的变更，而 Swift 侧的 `SYPlayerPreloader.init` 是个
不 `throws` 的公开初始化器，返回 nil 会让它也得改形状。与 #84（缓存目录建不出来
拿不到可编程信号）是同一条链上的同一个决定。

## 92. `tools/check-demo-no-c-types.sh` 的反向自检只覆盖一个文件 🟢 M5 Task 10（全支终审 F17 登记）

**现象**：那个脚本有一段"反向自检"——往源码里塞一个 C 类型、确认脚本真的会报——
但它只对 `demo/shared/PlayerViewController.swift` 做。其余 demo 源（含
`PreloadViewController.swift`、`PreloadSampleServer.swift`）没有被反向验证过，
也就是说"脚本对这些文件的扫描真的在工作"这件事没有证据。

**为什么不修**：脚本本身是同一条 grep 对整个 `demo/` 跑的，文件列表来自 glob 而
不是硬编码，所以"漏掉某个文件"在结构上不成立；反向自检只覆盖一个文件是**证据的
缺口**，不是**行为的缺口**。补全它要在脚本里对每个文件各做一次临时改写 + 还原，
把一个 0.2 秒的闸变成几秒，收益不抵。

## 93. `CacheStore::last_round_stats_for_test()` 在三条提前返回上漏出上一轮的数字 🟡 M5 Task 3（全支终审 F17 登记）

**现象**：`enforce_capacity` 有三条提前返回的路径不更新这组计数器，于是
`last_round_stats_for_test()` 会**原样报出上一轮的数**。复审实测：round 2/3
逐字段重复 round 1 的 `examined=5 attempts=5 deleted=5`。

**为什么这条值得记**：这个计数器存在的**全部理由**就是"一轮的上界在外部不可观测"。
一个会说谎的观测点比没有观测点更糟——将来任何依赖它的回归用例都会**假绿**，
而假绿正是本里程碑反复被咬的那个形状（见 `CLAUDE.md` 的六种假结果机制）。

**为什么本轮不修**：它是测试缝，不影响发布行为，而 `src/dl/cache_store.cpp`
在本次修复波次里归另一条车道，跨车道改同一个函数会制造合并冲突。
**下一次动 `enforce_capacity` 时顺手在三条提前返回上各写一次计数器即可**。

## 94. 显示矩阵的镜像（hflip/vflip）分量完全未处理 🟡 M6d Task 2

**现象**：`av_display_rotation_get()` 读的是 FFmpeg `AV_PKT_DATA_DISPLAYMATRIX`
的 3×3 仿射矩阵，理论上既能表达纯旋转，也能表达"旋转 + 镜像"的组合——ffplay
的参考实现（`fftools/ffplay.c:2025`）用 `displaymatrix[3] > 0 ?
"cclock_flip" : "clock"` 区分了这两种情形，说明带镜像分量的矩阵在真实素材里
存在（常见于某些前置摄像头/编辑软件产出的文件）。`src/media/demuxer.cpp` 的
采集点（见 §3.1 的 `rotation_deg` 契约）只取旋转角、**把矩阵当成纯旋转处理**，
镜像分量被静默丢弃——一份带 hflip 的素材会被当成同角度的纯旋转来渲染，画面
左右（或上下）是反的，且没有任何错误或日志提示。

**为什么不修**：`video_geometry.h` 的 `BlitParams`/`blit_transform()` 目前
没有表达镜像的字段（`uv_scale` 是缩放不是翻转），要支持需要新增至少一个
bool 或把 `uv_scale` 的符号语义打开，属于新特性而不是本条修复范围；本轮没有
真实测试素材带镜像分量，无法钉住正确性。登记为已知缺口，留给下次真正需要
镜像支持时一并做。

## 95. 非 90° 倍数旋转被就近取整，静默改变画面方向 🟡 M6d 非目标，spec §6.4

**现象**：`normalize_rotation()`（`src/media/video_geometry.h`）把
`av_display_rotation_get()` 的原始角度 `round(deg / 90) * 90 mod 360`
规整到 `{0, 90, 180, 270}` 四个象限。理论上 `av_display_rotation_get()`
可以返回任意角度（虽然真实拍摄/编辑软件几乎只产生 90 的倍数），任意角旋转
需要真正的纹理采样变换（本轮的 uv 象限表做不到），spec §1 明确把它列为
非目标。**后果**：一份带 45° 这类非 90 倍数旋转元数据的素材会被就近取整到
0° 或 90°，画面方向与源标注不符，且不报错、不降级、不告警——调用方完全
看不出发生了取整。

**为什么不修**：真实需求几乎不存在（拍摄/编辑软件产生的旋转元数据本来就是
90 的倍数），真要支持任意角需要顶点/uv 的完整 2D 仿射变换，投入与收益不
成比例，spec §1 已裁定为非目标。`test_video_geometry.cpp` 的
`rotation_normalizes_to_four_quadrants` 已经把"45° 就近取整"这个行为本身
钉住（不是遗漏，是显式验证了这个已知限制），本条只是把这个限制单独登记
成缺口，方便以后有真实需求时能找到。

## 96. M6d 人工验证清单未执行 🟡 M6d，人工清单，与 #51/#53 同类

以下几项都还没有真机/真实听感下的人工验证：

- **旋转画面在真机上的方向正确性**：自动化（`test_metal_renderer.cpp` 的
  `blit_rotates_quarter_turn_clockwise_yuv420p` 等）只验证了"画面按预期的
  象限变换了"（GPU 离屏回读 + 像素断言），"在真机屏幕上看起来是正立的"
  需要人眼确认，见 spec §6.1。
- **增益斜坡的实际听感**：15ms 线性斜坡是防爆音的经验值，自动化
  （`test_audio_unit_sink.cpp` 的 `gain_ramp_approaches_target_
  monotonically_without_overshoot` 等）只验证了数值单调、不过冲，"听不出
  咔哒声"需要真实设备/真人耳朵，见 spec §6.2。
- **`sample.mp4` 没有旋转元数据**：demo 自检（`SYPLAYER_DEMO_PLAYER_
  AUTORUN=1`）与 XCTest 的像素回读用例（`SYPlayerRendererForwardingTests`）
  都用内置的 `sample.mp4`，它是 640×360、SAR 1:1、不旋转——旋转路径在
  demo 里完全没有被人工或自动化跑过，只有 ctest 侧用合成素材覆盖过。
- **【终审 M6，只登记】demo 里肉眼验不出 gravity 的效果**：UIKit 播放页的画面区
  宽高比按 `state.videoSize` 设成与视频一致，于是 Fit / Fill / Resize 三种出图
  在 demo 里几乎一模一样——人工拨 gravity 分段控件看不出区别。播放页自检
  （`SYPLAYER_DEMO_PLAYER_AUTORUN=1`）只验证控件 → `player.videoGravity` 的
  **接线**，不验证出图；出图差异只有 `test_metal_renderer.cpp` 的
  `blit_gravity_fit_fill_resize_on_wide_source` 与 XCTest
  `testGravityChangeReachesMetalRenderer`（离屏 64×64 正方形目标）覆盖。要人工
  看到效果，需要一个宽高比与视频不同的画面区（例如临时去掉宽高比约束）。
- **【终审 M3】SwiftUI 页没有音量/静音/gravity 控件**（spec §8.3 第 16 条）：
  这三项在 SwiftUI 页上既没有自动化、也无法人工验证，只有 UIKit 播放页可验。

若这些验证后续被执行，把结果写回本条并按实测调整级别。

## 97. 限速只控**平均**速率，不做瞬时整形 🟡 M6a，spec §1 非目标

`RateLimiter` 的记账窗口是"到点即回补"的令牌桶，准入判据只看余额是否越过
阈值，不管单个在途请求内部的实际吞吐——一个请求一旦被准入，它自己的读写
全速跑完，不受任何节流。**后果**：瞬时突发上限约等于"当前在途请求数 ×
1 秒额度"（每条在途请求最多能占用一整份 `max_segment_bytes()`），设的 R
越小、并发请求数越多，瞬时/平均的比值越夸张。`include/syplayer/syp_net.h`
的头注释已写明"只控平均速率"，本条是把这句话单独登记成缺口，方便按行为
而不是按文档条款查到。

**为什么不修**：spec §1 明确把"瞬时速率整形"列为非目标——做到需要在
`DLTask` 内部按字节分片限速（例如限制单次 `read` 的返回长度），那是与
`syp_http.h` 后端契约耦合的改动，且与"不阻塞任何回调线程"这条硬约束
（spec §2 决策 3）直接冲突。

## 98. HLS 播放（非预加载）时抓取的播放列表不受限（有意豁免）🟡 M6a，spec §1 非目标

`src/media/hls/playlist_fetcher.cpp:130`（`fetch_playlist()`，`HlsSession`
在实际播放/直播刷新时调用）直接 `backend->create`/`backend->start`，绕过
`SourceBridge`/`Scheduler`，因此也绕过了 `RateLimiter` 的准入判定——这条
路径根本没有 `Scheduler` 可言，不存在"给它设类别"这回事。

**订正（M6a Task 6 复核发现）：这条豁免不包括 `Preloader::fetch_text`**
（预加载扫描 m3u8 时抓播放列表，`src/dl/preloader.cpp:616`）。spec §1
原文把它与上面那条并列成"都直接调后端、绕过调度器"，但实测并非如此——
`fetch_text` 经 `SourceBridge::open()` 走的是**完整的** `Scheduler`/
`RateLimiter` 路径，只是没有传 `RateClass`，默认 `RateClass::Playing`
（M6a Task 3 报告 §12 自审："播放列表条目、`fetch_text` 的桥都没有传
类别，默认 Playing"）。它**技术上受准入判定约束**，只是因为拿到的是
最优待的 Playing 类别、且文件通常只有几 KB 到几十 KB（一次合法准入基本
就够），实际效果接近不受限——但这与"绕过调度器"不是一回事：如果播放本身
（同样是 Playing 类别）已经把余额耗到 ≤ 0，`fetch_text` 的准入会与播放
竞争同一份 Playing 预算，可能被拒绝并等唤醒。本条是 spec §1 原文表述
不准确，如实登记订正在这里，不改 spec 原文。

**同理补登（M6a 终审 I3）：预加载的元数据探测也按 `Playing` 准入。**
`src/media/media_info_provider.cpp` 的 `MediaInfoProvider::probe()`（按时长
暖的条目需要它把时长换成字节）经公开 `syp_source_open` 打开源——公开 C API
没有类别参数——落到 `SourceBridge::open` 的默认值 `RateClass::Playing`；每条目
至多 `kProbeMaxBytes`（2 MiB）。与 `fetch_text` 一样，它**受限**但拿的是最优待
的类别，与播放共用 Playing 预算。

**这两条都有意保留 `Playing`（控制方裁定，终审 I3/M1）**：它们都是
`Preloader` 驱动线程上的**同步阻塞读**。若按 `Preload` 准入，播放持续消费时
余额很难高过保留线，读几乎拿不到额度 ⇒ `fetch_text` 撞
`kPlaylistFetchDeadlineMs`（10 s）⇒ 条目 Failed、探测撞 `kProbeWallClockMs`（15 s）
⇒ 退化为按字节，且在这段时间里整个预加载器（让路、其余条目）都被按住。`fetch_text`
现在**显式**传 `RateClass::Playing` 并在调用点写明理由（`src/dl/preloader.cpp`
`Preloader::fetch_text`，终审 M1），防止被"顺手改成按条目优先级"。代价：每条
目最多 2 MiB 探测 + 几十 KB 播放列表与播放同级抢额度。

**为什么不修**：`playlist_fetcher.cpp` 那一半是 spec §1 裁定的有意豁免
——m3u8 文件本身字节量小，且抓取延迟直接影响首帧/切换分辨率的响应速度，
限速对它只有坏处没有好处，这个理由没有变。`fetch_text` 那一半不需要
"修"，因为它本来就没有被豁免，只是 spec 原文的措辞不准确，登记订正即可。

## 99. 服务端不支持 Range 时的整文件下载不受限 🟡 M6a，spec §1 非目标

`Scheduler` 的准入与分片上限只作用于"按洞切片"的正常路径；一旦探测到
服务端不支持 Range（`no_range_signal → Unsupported`），整份文件退化成
单个 `[0, eof)` 请求，这个请求本身不再经过 `admit()`/`max_segment_bytes()`
——它已经在准入时被放行过一次（用于探测），后续字节全速下载，直到完成。
`test_preloader.cpp` 的 `no_range_source_under_a_limit_still_completes`
钉住的是"降级之后仍然能在限速下完成"，不是"降级之后的字节速率受限"，
两者不要混淆。

**为什么不修**：spec §1 明确列为非目标——单连接整文件下载没有分片点，
准入只能卡住它的开始，卡不住中间；真要限速需要在 `DLTask`/后端层面做
瞬时整形（见 #97），超出本轮范围。这条同时与 M6a Task 2 报告记录的一个
实现细节相关：**分片上限覆盖了 `segment_size_locked()` 的全部出口**，
所以"总长未知时的首个探测请求"从原来的 `[read_pos, eof)` 变成了
`[pos, pos+R)`——对支持 Range 的源无害，对不支持 Range 的源多一次
200 往返（先收到整份响应体、该次响应体本身也照常记账，然后才走降级），
是 spec §3.2 原文之外、M6a Task 2 的一处主动收紧，已在 spec §8 落地
对照登记。

## 100. 真实网络下的速率准确度与保留线未经实测 🟡 M6a，spec §6.4/§6.5

`RateLimiter` 的"1 秒容量"（= R）与"保留线"（= 容量 / 2）都是 spec §2
决策 7 给的经验常量，本轮只在 127.0.0.1 回环服务器（`test_preloader.cpp`
的 `throughput_under_a_limit_tracks_the_rate` 等）上验证过：实测吞吐
落在 `[0.5·R·t, R·t + 容量 + 在途上限]` 区间内，容差本身是为回环这种
低 RTT、几乎无抖动的环境设计的。蜂窝网络或高 RTT 链路下，1 秒的令牌桶
容量是否仍然是一个合适的突发缓冲、保留线是否仍然能让播放及时抢到额度，
都没有实测数据。

同一份限制的另一面：**保留线是"播放优先"意图的直接体现，不是 bug**——
`Preload` 类别需要余额 `> 容量 / 2` 才能准入，这意味着播放码率一旦高于
`R / 2`，预加载在播放持续下载期间基本拿不到任何额度（`test_preloader.cpp`
的 `playback_outpaces_preload_under_a_shared_limit` 实测多轮 `pre == 0`）。
这是设计意图，但容易被误读成"预加载卡死了"，需要在文档里写明。

**极低限速下的两条墙钟截止同样未经实测（M6a 终审 M6 并入）**：`R` 设得很低
（例如 64 KB/s）时，预加载的两条同步读——`Preloader::fetch_text` 的
`kPlaylistFetchDeadlineMs`（10 s）与元数据探测的 `kProbeWallClockMs`（15 s）
——都按 `Playing` 准入（#98），但与播放共享同一份额度，被拒等唤醒的时间照样
算进墙钟，可能被触发。前者超时的条目**直接 Failed、不重试**（`expand_playlist`
置 `expanded`，此后不再展开）；后者超时则 `probe()` 返回空、条目退化为按字节暖
（`provider_miss` 计一次），不判 Failed。只在推理上成立，
没有用例或实测覆盖。

**为什么不修**：没有真实蜂窝/高 RTT 环境可供本轮测试；常量本身留了
调整空间（spec §2 决策 7："容量、保留线都是内部常量，不进 API…需要调时
再开旋钮"），一旦有真实网络下的反例，改常量不改接口。

## 101. fail-open 路径的已知窄窗口（(a) 已修，(b)–(d) 未修）🟡 M6a Task 4，复审登记；终审 I1 修 (a)

`RateLimiter::set_rate()` 懒启动唤醒线程失败时会 fail-open（把 `rate_`
主动置 0、记 WARN、在调用线程同步补跑一次 `fire_due()` 唤醒窗口期内已
`armed` 的订阅——见 `src/dl/rate_limiter.h`/`.cpp` 的"修复轮 2"一段）。
这条路径**只在"唤醒线程创建失败"时触发**（线程资源耗尽等罕见情形），
复审最终确认还有四条更窄的残留窗口，如实登记；其中 (a) 已在终审修复波修掉：

- **(a) ✅ 已修（M6a 终审 I1）**：调度器 `admit()` 被拒之后、`arm()` 之前，
  与另一线程的 fail-open 同步派发发生交错，`arm()` 会登记在"这一轮
  `fire_due()` 已经跑完"之后，不会被那次派发覆盖到；此后 R == 0、再无任何
  派发者——该 `Scheduler` 永久卡住（终审在临时副本里实测
  `request_count 0 / delivered 0`）。**修法**：`src/dl/scheduler.cpp`
  `schedule()` 里 `sub_->arm(rate_class_)` 之后复查一次
  `if (limiter_->admit(rate_class_)) schedule_again_ = true;`（仍在 `mu_`
  临界区内，本轮尾检据此再跑一轮）。`arm` 与 fail-open 的 `rate_ = 0` 都在
  限速器 `mu_` 下，二者必有先后：arm 在前 ⇒ 被那次同步派发叫醒；arm 在后
  ⇒ 复查读到 R == 0。**用例**：`tests/test_scheduler.cpp` 的
  `fail_open_between_rejected_admit_and_arm_does_not_stall`，经新增测试钩子
  `Scheduler::set_test_hook_before_arm`（默认空、生产无调用点）在窗口里把 R
  置 0 且此后不派发；删掉复查那一行 ⇒ 只红这一条（`request_count 1`、
  `delivered 16384`）。
- **(b)** `thread_started_` 判据（用来区分"这次 fail-open 是否该覆盖并发
  成功设置的速率"）存在顺序竞争，后果仅"限速静默关闭"，不是数据损坏。
- **(c)** fail-open 同步调用 `fire_due()` 让它多了一个潜在的第二派发者，
  与 `fire_due()` 原本"同一时刻只有一个派发线程"的前提（`in_cb` 用
  `bool` 而非计数）冲突；理论 UAF 需要多重叠加的时序才能触发。
  见 `src/dl/rate_limiter.h` 头文件注释。
- **(d)** 公开契约本身是一个约束："fail-open 时回调在调用线程同步执行"
  已写进 `include/syplayer/syp_net.h` 与 `swift/SYPlayerKit/
  SYPlayerNetwork.swift` 的文档——调用方不得在持有这些回调会取的锁时
  调用限速 setter，这条限制会一直存在，不是过渡态。

**若终审决定修其中某条，以终审修复波为准，本条相应改写。**

## 102. "为播放保留余量"在预加载**突发**时不成立 🟡 M6a 终审 I2（只登记，不改算法）

**机理**：`RateLimiter::admit()` 只看"此刻余额是否高于阈值"，而扣账要等数据
**到达**才发生（spec §2 决策 4）。一次 `schedule()` 里连续的几次 `admit()`
之间没有任何字节到达，余额不变，于是 `Preload` 类别能把
`max_concurrent_tasks` 个分片（每片至多 `max_segment_bytes()` = R 字节）
**一次性**放行；这些字节随后全速到达、把余额扣到远低于 0。保留线（容量 / 2）
只挡得住预加载**开始新请求**，挡不住已发出请求的透支——spec §1 目标 2 与
决策 2 的"**永远**为播放保留余量"在这一刻不成立。

**实测数字**（终审，假时钟、内存桩）：Preload 6 槽、满桶 R = 64 KiB/s ⇒ 一次
放行 6 片、余额降到 **−5R**，随后 `Playing` 等待者的到点时刻为 5001 ms（手算：
(0 − (−5R × 1000 mB)) / R + 1 = 5001）。生产默认 `reserved_for_playing = 3`
⇒ 非 `Playing` 至多 3 条连接；最坏在余额刚过保留线（≈ R/2）时放行 3 片 ⇒
余额 ≈ −2.5R ⇒ 播放最多约 **2.5 秒**在还预加载的账。

**典型触发**：播放缓冲满、暂停取数 → 桶 1 秒内回满 → `Next`/`Background`
条目一把占满自己的连接额度 → 播放续数据时被拒、等待。

**公开措辞已改为如实表述**（终审 I2）：`include/syplayer/syp_net.h`、
`README.md` 的 `RateLimiter` 一行、`swift/SYPlayerKit/SYPlayerNetwork.swift`
都写成"预加载只在余额高于保留线时才能开始新请求；已发出的预加载请求透支的
额度由后续时间偿还，播放在预加载突发之后可能短暂等待"。spec §1 目标 2、§2
决策 2 就地加了 `> ⚠️ 落地时发现` 标注。

**为什么不修**：真修就是 spec 决策 4 刻意回避的"预支 / 退还"复杂度（请求
取消、重定向、失败时各退多少）。**后续修法方向**：限速器记录"已准入但尚未
到达"的 `Preload` 字节（在途预加载额度），`Preload` 准入改为
"余额 − 在途预加载 > R/2"；到达时从在途里转成扣账，任务结束时把没到达的
剩余从在途里抹掉（只抹 `Preload` 自己的在途，不涉及退还已扣的账）。可与
M6c（坏任务检测，同样要在 `DLTask`/`Scheduler` 上读在途状态）一起考虑。
（M6c 已交付，**没有**顺带做这件事——M6c 只读在途任务的尝试时长与速度，不涉及
限速器记账；本条仍然成立。）

## 103. 后端对 cancel 也不回调时，被判坏的任务收不回 🟡 M6c，spec §1 非目标

`syp_http.h` 规定 cancel 之后后端必须**恰好回调一次** `on_complete`。M6c 的挂死
兜底只兜"后端不守超时约定"，不兜"连 cancel 约定也不守"：违约后端下，被判挂死
（或被慢替换）的 Slot 标 `dead`、不再占调度名额，它负责的剩余区间照常重发，
**播放不受影响**；但 `DLTask` 对象本身收不回——`~DLTask` 会等终态，而终态永远
不来。所以 `Scheduler` 把这类 Slot 留在 `slots_` / `deferred_release_` 里，直到
`~Scheduler`；`~Scheduler` 若轮到拆它，会在调用线程上永久等待。

**为什么不修**：真修要么是"放弃等待、泄漏 handle"（与后端的回调在途竞争，UAF
风险），要么是给 `syp_http.h` 加新的契约（非目标 4：不改后端契约）。`HealthTicker`
线程**不会**成为这类对象的最后持有者（`deferred_release_`，M6c Task 3 复审 I1），
所以卡住的至多是拆 `Scheduler` 的那个线程，不会是全进程唯一的 ticker 线程。

## 104. 坏任务检测的阈值没有实网数据支撑 🟡 M6c，spec §6.3

`slow_ratio = 4`、`slow_strikes = 2`、EWMA α = 1/8、替换后基线 ×½ 并冷却一个
`speed_window_ms`、`stall_grace_ms = 2000`——全部是经验起点，只在同步桩（假时钟）
与 127.0.0.1 回环上验证过。蜂窝 / 高 RTT / 突发丢包网络下的误杀率与替换收益
都没有测过。阈值放在内部 `SchedulerConfig::health`，不进 `syp_config`
（spec 决策 6：不开公开旋钮），调整只需改默认值。与 #100（限速的实网准确度）
同一类缺口。

## 105. 基线每个 `Scheduler` 一份；新开的源前一两个窗口不做慢判定 🟢 M6c，spec §6.2

慢判定的基线是**本 `Scheduler`** 健康样本的 EWMA，生命周期与之相同，不跨源、
不按主机/CDN 节点记历史（spec §1 非目标）。后果：

- 新开的源在第一个健康样本进基线（尝试满一个 `speed_window_ms` 且处于
  `Receiving`）之前，`baseline == 0` ⇒ **不做慢判定**；从一开始就慢的连接
  在这段时间里只受挂死兜底约束。
- 只有一条连接时（探测期并发锁 1、或只剩一个洞），它自己就是基线，永远不会
  "比基线慢"（spec §4 表格已写明）。
- 回环上的快连接若在一个窗口内就下完，同样采不到样本——端到端用例因此要
  给快连接节流（见 #106）。

## 106. 慢替换的端到端用例只能从服务端侧断言 🟢 M6c Task 5

`test_source_bridge` 的 `slow_loris_connection_is_replaced_end_to_end` 走
`syp_source` 公开面 + 真实 Apple 后端 + 回环服务器。`SourceBridge` 不暴露
`slow_kills()` / `stall_kills()`（spec §3.4：其余层不改），所以用例断言的是
服务端看到的现象：`start == 0` 那条请求被客户端提前断开（`early_close`）、
存在一条从已收字节处续起的替换请求（`0 < start < 探测区间终点`）、整份文件在
20s 内读完且字节正确。它证明"慢连接被换掉了"，但**区分不了**是慢替换还是
别的机制换掉的——由"关掉慢判定（`may_judge = false`）⇒ 该用例红"这条变异
补上归因。**挂死兜底没有端到端用例**（回环服务器的 `hang` 会让 Apple 后端先
按自己的超时报错，轮不到调度器兜底），只有 `test_scheduler` 的同步桩用例。
另外该用例把快连接节流到约 320 KiB/s、只在 APPLE 下编译，耗时上界是负载
敏感类（`docs/tech-debt.md`「M6c 收尾登记」第 4 条）。

## 107. 预连接的连接是否被复用取决于 HTTP 后端，库无法保证 🟡 M6b，spec §6-1

`syp_preconnect` 只负责经后端发一个 `Range: bytes=0-0` 的 GET、让 206 正常收尾；
连接留不留在池里、随后的播放请求挑不挑它，全是后端（Apple：进程内唯一
`NSURLSession`，`HTTPMaximumConnectionsPerHost = 8`）的池策略。

**已验证的范围**：`test_source_bridge` 的 `preconnect_warms_connection_reused_by_playback`
（真实 Apple 后端 + 回环服务器 keep-alive 模式 + `max_concurrent_tasks = 1`）断言
预连接之后播放读完 64 KiB 全程 `accepted_connections() == 1`。M6b 收尾时单独连跑
10 次（每次一个新进程）**10/10 为 1、0 次为 2**；`ctest -j4` 全量与 `build-tsan`
全量下各再过一次。spec §5.2 的降级条款**没有触发**。

**没验证的范围**：只有明文 HTTP/1.1 + 127.0.0.1——TLS 会话复用、HTTP/2 连接合并、
真实 CDN 的 keep-alive 超时（服务端先关空闲连接则预热白做）、蜂窝网络下系统回收
空闲连接的时机，都没有测过；其它后端（将来的 OkHttp / Cronet）要各自验证。
也没有"预热真的缩短了首个请求耗时"的数字（spec §5.2 明确不做墙钟断言，负载敏感）。

## 108. 预连接的 30 秒去重窗口与在途上限 4 是经验值 🟢 M6b，spec §6-2

`Preconnector::kDedupeWindowMs = 30000`、`kMaxInflight = 4` 没有实网数据支撑：
30 秒是"连接大概率还在池里"的猜测（URLSession 的空闲回收时间不公开），4 是
"别和播放抢后端每主机 8 条连接"的保守值。两者都是内部常量、不进 `syp_config`
（spec 决策 4/5）。另：失败的预连接同样占 30 秒去重（不重试的一部分），服务端
一时不可达时，30 秒内再调用不会补发。与 #100 / #104 同一类缺口。另：一个永远不
回调 `on_finished`（既不成功也不失败、不超时）的后端会让对应 Entry 永久停在
"在途"，占掉 `kMaxInflight = 4` 里的一个名额不会被回收——M6c 的 `HealthTicker`
只管播放/预加载的 `Scheduler` 挂死检测，不认识 `Preconnector` 的 Entry，这类
坏后端不在它的覆盖范围内。

## 109. 服务端不支持 Range 时，预连接会多收一个 chunk 才被 cancel 🟢 M6b，spec §6-3

回 200 全量时 `Preconnector` 在首个 `on_data` 里 cancel 自己，但交付粒度由后端决定
（Apple 后端一次 `didReceiveData` 可能是几十 KiB），所以最多白下一个后端 chunk；
cancel 通常还会让这条连接**不**回池（HTTP/1.1 下 body 没读完只能关连接，预热对这类源等于没做）。这类源本就少见
（spec §4 边界表），不另做 HEAD 探测（`syp_http.h` 没有 method 字段，决策 2）。
另：非 2xx 的错误响应体会被完整下载——`DLTask` 的 drop_body_ 路径（丢弃已下载
内容、不落缓存）不经 `on_data` 投递数据给上层，而 `Preconnector` 判断"该不该
cancel"用的正是 `on_data` 回调，收不到回调就没有 cancel 的时机，只能等错误响应
自然收完。对预连接的目的（暖连接、不消耗流量）是净损耗，但这类响应本就少见
（决策 2 同一段边界表）。

## 110. 预连接对调用方没有任何可观测结果 🟢 M6b，spec §1 非目标

`syp_preconnect` / `SYPlayerNetwork.preconnect` 立即返回、静默失败（未注册后端、
非 http(s)、去重命中、在途已满、网络错误都一样）。调用方无法知道预热成没成、
也拿不到统计——有意为之（YAGNI，spec §1）。demo 的"预连接"按钮因此只能显示
"已发出预连接"，无人值守自检也不新增步骤；判断它有没有用只能靠后端侧抓包或
服务端日志。将来若要做效果统计，入口应是后端的连接指标（如 URLSession 的
`URLSessionTaskMetrics.isReusedConnection`），而不是给预连接加回调。

## 111. SwiftPM 分发不支持 Intel Mac 🟡 SwiftPM 分发，spec §1 非目标 / §4-1

`SYPlayerKit.xcframework` 与 `FFmpeg.xcframework` 的原生 macOS 切片、Mac Catalyst
切片都只有 arm64（FFmpeg 这两个 slice 本来就只编了 arm64）。Intel Mac 上的接入方
工程解析包没问题，链接时找不到 x86_64 符号；原生 macOS 接入方即使只在 Apple
Silicon 上跑，也要把 `ARCHS` 设成 `arm64`，否则 Release 默认连 x86_64 一起编、
链接失败（示例工程 `examples/macOSExample` 就是这么设的）。要支持 Intel 得先给
FFmpeg 补 x86_64 的 macOS / Catalyst 切片，再让分发工程 archive 出 fat 切片。

## 112. SwiftPM 分发不支持 tvOS / visionOS / watchOS 🟢 SwiftPM 分发，spec §1 非目标

`Package.swift` 只声明了 iOS / Mac Catalyst / macOS，两个 xcframework 也没有这三个
平台的切片。在这些平台的 target 上加依赖，SwiftPM 解析阶段就会拒绝。没有对应的
FFmpeg slice 是根因。

## 113. 首次发版需要人工上传 Release；发版前远程模式解析必然失败 🟡 SwiftPM 分发，spec §4-5

`Package.swift` 默认（不设 `SYPLAYER_LOCAL_BINARIES`）指向
`https://github.com/swlfigo/syplayer/releases/download/<version>/*.xcframework.zip`。
这两个 zip 要人按 README「本地构建与发版」上传——顺序是：回填 checksum 的提交 →
在该提交上打 tag 并推送提交与 tag → 基于**已存在的** tag 建 Release 并上传（或先建
草稿、推完 tag 再发布）。反过来先建 Release，GitHub 会在回填之前的提交上创建同名
tag，接入方拿到的清单里是旧 checksum。在上传之前，任何人按 README 的
依赖写法接入都会在解析阶段报 `badResponseStatusCode(404)`（实测
`xcodebuild -resolvePackageDependencies` exit 74）。另一个人为风险：上传的 zip 必须
是算出已提交 checksum 的那两个文件本身，脚本重跑一次 zip 字节就变，checksum 对不上
SwiftPM 会拒绝——仓库里没有任何自动化能在上传前核对这一点。

**另外两个首发前提**：仓库必须是**公开**的——私有仓库的 Release 附件要带认证才能下，
SwiftPM 按 `url:` 拉 binaryTarget 时不会带，接入方同样拿到 404；仓库目前**没有
`LICENSE`**，公开之前要补（用哪种许可证由仓库所有者定，不在本轮范围）。随包的
FFmpeg 已经附了它自己的 LGPL 许可证与 NOTICE，但那只覆盖 FFmpeg，不覆盖本仓库的代码。

## 114. demo 的静态 SYPlayerKit 与分发的动态 SYPlayerKit 是两套构建配置 🟡 SwiftPM 分发，spec §4-2

demo 工程（`demo/generate_xcodeprojects.rb` 默认模式）把 SYPlayerKit 编成静态
framework、源码直编；分发（`--dist` 模式 + `tools/build-xcframework.sh`）是动态
framework、库演进模式、四个平台逐个 archive。两者共用源清单，但链接形态、build
setting、平台条件编译路径（原生 macOS 的 AppKit 视图只在分发里编）都不同，可能
漂移：demo 的 `xcodebuild test` 全绿不代表分发产物能用。兜底是 `tools/check-spm.sh`
（两个示例 App 经 SwiftPM 接入后跑测试），**但它依赖先手动重跑
`tools/build-xcframework.sh`**——产物是旧的，门禁验证的就是旧代码。门禁不比对产物
与源码的新旧。

## 115. Xcode GUI 打开示例工程时环境变量不一定生效 🟡 SwiftPM 分发

示例工程对仓库根是本地包引用，包清单靠 `SYPLAYER_LOCAL_BINARIES=1` 切到本地产物。
命令行 `xcodebuild` 会把环境传给清单求值（`tools/check-spm.sh` 每一路都核对解析
结果里两个产物的 `source.type == local`）；但从 Dock / Finder 启动的 Xcode 没有
终端里的环境变量，会走远程模式，发版前必然解析失败（#113）。README 写明从终端
`SYPLAYER_LOCAL_BINARIES=1 xed examples/iOSExample` 启动（Xcode 需先完全退出）。
**GUI 这条路径本轮没有实测**：Xcode 已在运行时环境变量不生效、切换模式后需要
Reset Package Caches，是按 Xcode 的一般行为写的，不是实测结论。

## 116. ~~Mac Catalyst 经 SwiftPM 接入没有门禁覆盖~~ ✅ 已修 🟡 SwiftPM 分发，spec §3.5 与落地差异

**已修（同一轮复审修复）**：iOS 示例打开了 `SUPPORTS_MACCATALYST`（Catalyst 部署目标
14.0、`ARCHS[sdk=macosx*] = arm64`、测试宿主与 rpath 按 macOS 布局分开写），
`tools/check-spm.sh` 加了第三路 `platform=macOS,variant=Mac Catalyst,arch=arm64`
（期望 2 条用例，同样核对产物来源为本地），`CODE_SIGNING_ALLOWED=NO` 下 hosted 测试能跑。
反证：挪走本地 `ios-arm64-maccatalyst` 切片后这一路 EXIT=65、门禁 exit 1。
以下是修复前的原文，保留作记录。


spec 原计划的接入样例要在 iOS 模拟器、Mac Catalyst、原生 macOS 三处跑冒烟；落地
改成两个示例 App（iOS、原生 macOS），`tools/check-spm.sh` 只跑这两路。xcframework
里的 `ios-arm64-maccatalyst` 切片只被 `build-xcframework.sh` 的逐切片结构检查
（framework 与 `.swiftinterface` 在不在）覆盖，没有任何接入方工程链接、运行过它。
补法：iOS 示例打开 `SUPPORTS_MACCATALYST`，门禁加一路
`platform=macOS,variant=Mac Catalyst`（Catalyst 的 hosted 测试可能需要 ad-hoc 签名，
要实测）。

## 117. ~~二进制目标名 `FFmpeg` 过于通用，可能与接入方的其他依赖撞名~~ ✅ 已修 🟡 SwiftPM 分发

**已修（FFmpeg 私有化隔离，首发 `0.1.0` 之前）**：binaryTarget、xcframework、framework
与 install name 统一改为 `SYFFmpeg`（`@rpath/SYFFmpeg.framework/...`；分发的
SYFFmpeg 不带模块，没有模块名要改）；同一轮还把
符号层面也隔离了——`SYFFmpeg` 对外只导出 `syp_` 前缀的符号，接入方自带任意版本的
FFmpeg 时两边互不串用（README「与自带 FFmpeg 的项目共存」）。守护：
`tools/check-ffmpeg-symbols.sh`（导出表零未加前缀符号、我们各静态库与分发的
SYPlayerKit 零未加前缀引用）与 ctest `ffmpeg_coexist_*`（五种链接方式下的冲突集成
测试）。以下是修复前的原文，保留作记录。

`Package.swift` 里的两个 binaryTarget 叫 `SYPlayerKit` 与 `FFmpeg`，产物里的 framework
也叫 `FFmpeg.framework`、模块名 `FFmpeg`。SwiftPM 要求同一张依赖图里的 target 名全局
唯一：接入方如果还依赖了别的同样把 FFmpeg 包成名为 `FFmpeg` 的 target 的包（这类包
不少），解析阶段就会报 target 名冲突；即使 SwiftPM 放过，App 里也会有两个
`@rpath/FFmpeg.framework` 抢同一个安装名。更具体的名字（如 `SYPFFmpeg`）能避开，但
要同时改 target 名、framework 名、install name 与 SYPlayerKit 的链接依赖——**`0.1.0`
发出去之后再改就是破坏性变更**（接入方的 `.product` 引用不受影响，但任何直接
`import FFmpeg` 或按名字引用 framework 的地方都会断）。要改，最好在首发前决定。

## 118. 分发的 FFmpeg 没有 strip，带着指向构建机本地路径的调试信息 🟢 SwiftPM 分发

`tools/build-ffmpeg.sh` 刻意 `--disable-stripping`、保留 `-g`（Debug/Release 共用一份，
"最终 App 出包再 strip"）。分发包原样沿用这份二进制，所以 `SYFFmpeg.framework`（改名前叫 `FFmpeg.framework`）里带
完整符号表和 debug map（N_OSO 等条目指向构建机上 `build-ffmpeg/work/...` 的绝对路径），
`avcodec_configuration()` 返回的配置串里也有 `--prefix` / `--sysroot` 的本机路径。
不影响功能：接入方 App 归档时 Xcode 默认会 strip 内嵌 framework；但 zip 体积偏大，
且泄露构建机的目录结构。SYPlayerKit 自己的二进制与 `.swiftinterface` 已核对不含
`/Users/`；它随 xcframework 附带的 dSYM 里有（DWARF 的编译目录与源路径，dSYM 的
本来用途，不进接入方 App）。补法：分发拷贝时对 FFmpeg 另做 `strip -S`（或单独产出 dSYM 再 strip），
注意要在算 checksum 之前做、并重新核对导出符号不受影响。

## 119. `check-spm.sh` 的"每个 .swift 都在工程里"只按文件名匹配 🟢 SwiftPM 分发

门禁对 `examples/` 下每个 `.swift` 取 basename，在两个示例 pbxproj 里找
`path = <basename>;`。两处盲区：两个不同目录下的同名文件，只要有一个进了工程，另一个
就会被当成"也在"放过；文件名带空格或特殊字符时 pbxproj 会给路径加引号
（`path = "My File.swift";`），这时文件明明在工程里也会被误判为缺失。目前示例源文件
都是唯一的普通文件名，两种情况都没有发生。要堵住得按生成器的清单比对完整相对路径，
而不是 grep 文件名。

## 120. 接入方自带 FFmpeg 时，App 里有两份 FFmpeg 🟢 FFmpeg 私有化隔离，spec §2 非目标

隔离的代价：`SYFFmpeg` 与接入方自己的 FFmpeg 各占一份体积。我们这份是精简构建，
`8.1.2` 未 strip 时每个架构约 4.5–4.8 MB（macOS arm64 4,794,552 字节、iOS 真机
4,706,040、Catalyst 4,757,320；模拟器 fat 包 9.6 MB 含两个架构，不进 App Store 包）。
不打算消除：跨大版本 ABI 不兼容（结构体布局、API 都会变），二进制 SDK 没法与接入方
共用一份。能做的只有继续压我们自己这份——strip（#118）与按需裁剪组件，都不改变
"两份"这一事实。

## 121. 映射头按名字全局改写，与 FFmpeg 符号同名的标识符会被一起改掉 🟢 FFmpeg 私有化隔离

`syp_ffmpeg_prefix.h` 是一串 `#define 原名 syp_原名`（`tools/build-ffmpeg.sh` 从静态库
的全局符号现生成，含 `ff_*` 等内部名），凡是强制包含它的编译单元（CMake 经
`syp_ffmpeg` 的 INTERFACE 选项，demo / 分发工程经 `OTHER_CFLAGS`），**任何**与这些名字
同名的标识符——局部变量、成员名、我们自己的函数名——都会被预处理器改写。处理方式：

- **与 FFmpeg 公开头里的宏撞名**：构建脚本的 `check_prefix_macro_clash` 在生成映射头前
  比对，撞了直接失败（否则映射头会重定义 FFmpeg 自己的宏）；`smoke_prefix_header` 再在
  强制包含映射头的前提下，把能独立编过的公开头在 C 与 C++ 下各编一遍，要求零告警。
- **与我们自己的代码撞名**：多数情况是无害的一致改写（声明与使用被改成同一个名字）；
  出问题时表现为编译错误或 `tools/check-ffmpeg-symbols.sh` 报出多余的未定义引用，
  不会静默出错。约定：我们的代码不要用 FFmpeg 的函数名/全局变量名当自己的标识符。
- **接入方不受影响**：映射头不随分发包下发（分发的 `SYFFmpeg.xcframework` 不带
  `Headers`），接入方的编译单元永远看不到它。

## 122. 冲突集成测试只覆盖静态 `syp_media` 形态，没覆盖分发的动态 SYFFmpeg 🟡 FFmpeg 私有化隔离

`tests/test_ffmpeg_coexist.cpp`（ctest `ffmpeg_coexist_*`）把假 FFmpeg 与
**静态库** `syp_media` + `SYFFmpeg.xcframework` 里的静态构建产物链在一起验证
隔离；SwiftPM 分发给接入方的其实是 `SYFFmpeg.framework` 这份**动态库**，链接
方式、符号解析时机都不一样。手工实验（本轮复审时做的，未固化成用例）显示动态
form 同样隔离——接入方对 `avformat_version()` 之类的调用仍然解析到自己那份，
不会被我们的 `syp_` 前缀符号影响。但这只是一次性观察，不是持续跑的门禁。
补法：`tools/check-spm.sh` 或单独脚本加一路，用分发的 xcframework（走
SwiftPM/Xcode 的真实动态链接）重跑等价的冲突场景，长期替代或补充手工实验。
