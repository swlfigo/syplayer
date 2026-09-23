# 端到端拆卸期死锁 —— 诊断报告

日期：2026-09-08
分支：`dev/m1-step7-headless-probe`

> 这是诊断笔记的定本：删掉了纯过程性的内容（具体跑了哪条 lldb 命令之类），
> 保留成因链、锁序、修复取舍与确定性复现构造方式。根因已修复；这份笔记记录的是
> 诊断过程与方法论，供以后再遇到同类"回调栈里自等"问题时参考。

---

## 结论摘要（先说最重要的一条）

**这不是拆卸期死锁。** 它发生在**第一个用例 `a_faststart_sequential` 的顺序读过程中**
（`seeks=size=0`，没有 seek，也还没走到 `syp_source_close`）。
死锁点在 `SourceBridge::read()` → `apply_window()` → `Scheduler::set_read_position(196608)`
→ `reap_except()` → `~Slot` → `~DLTask`。

**核心成因是单线程自等**：Apple 后端的 delegate 线程在 `emit_data` 的收尾里合成
`on_complete(SYP_ERR_CANCELED)`，dl 层在这个 `on_complete` 里同步调 `backend->destroy`，
而 `Handle::destroy()` 去等 `inflight_ == 0` —— 那个 `inflight_` 正是**它自己下面几层栈帧**
持有的。它等的是自己，永远等不到。

后端本来有一条「同线程回调内 destroy 就跳过等待」的逃生口，但它的判据是 `in_callback_`，
而 `emit_data` 的取消收尾路径**在调用 `on_complete` 之前一行就把 `in_callback_` 清成了 false**。
逃生口因此没有武装。日志里那句 `inflight=1 in_callback=0` 就是这个 bug 的字面自白。

---

## 一手证据：lldb 全线程栈

`lldb -b -p <pid> -o "thread backtrace all"`。两次独立运行签名完全一致，
第二次同时有**三个** Handle 各自陷在同一形状里（见下），说明复现率接近 100%。

### 主线程 —— 二级受害者

```
frame #8:  syp::dl::DLTask::~DLTask(this=0x101e67b40) at dl_task.cpp:250:21   <- cv_.wait_for
frame #14: syp::dl::Scheduler::Slot::~Slot(this=0x101e5cba8) at scheduler.h:119:12
frame #28: syp::dl::Scheduler::reap_except(this=0x101e678c0) at scheduler.cpp:508:11
frame #29: syp::dl::Scheduler::set_read_position(this=0x101e678c0, pos=196608) at scheduler.cpp:170:5
frame #30: syp::dl::SourceBridge::apply_window(this=0x101e67540) at source_bridge.cpp:447:8
frame #31: syp::dl::SourceBridge::read(this=0x101e67540, buf="", size=65536) at source_bridge.cpp:928:9
frame #32: syp_source_read(s=0x101e60ed0, buf="", size=65536) at syp_source_api.cpp:90:21
frame #33: syp::media::AvioBridge::on_read(...) at avio_bridge.cpp:57:23
frame #44: syp::probe::(anonymous namespace)::run_demux(...) at packet_digest.cpp:112:9
frame #45: syp::probe::demux_avio(ctx=..., seeks=size=0, ...) at packet_digest.cpp:178:5
frame #46: syp::probe::run_through_source(spec=...) at scenarios.cpp:94:21
frame #47: (anonymous namespace)::sequential_read_matches(fixture_name="faststart.mp4") at test_probe_e2e.cpp:56:25
frame #48: test_a_faststart_sequential() at test_probe_e2e.cpp:76:5
```

注意 `frame #31` 是 `SourceBridge::read`，`frame #45` 的 `seeks=size=0`。
**不是 close，也不是 seek，是一次普通的顺序读把窗口推进到 196608 之后的回收。**

### delegate 线程 —— 死锁本体（同一个 Handle 自等）

```
frame #8:  (anonymous namespace)::Handle::destroy(this=0x101e79638) at apple_http_backend.mm:563:25   <- cv_.wait_for(inflight_==0 && !in_callback_)
frame #9:  (anonymous namespace)::trampoline_destroy(raw=0x101e79638) at apple_http_backend.mm:923:8
frame #10: syp::dl::DLTask::sink_on_complete(ctx=0x101e73320, status=-3, http_status=0) at dl_task.cpp:895:9
frame #11: (anonymous namespace)::Handle::emit_data(this=0x101e79638, p=..., n=65536) at apple_http_backend.mm:672:13
frame #12: (anonymous namespace)::Handle::did_receive_data(this=0x101e79638, ...) at apple_http_backend.mm:825:9
frame #13: -[SypHttpDelegate URLSession:dataTask:didReceiveData:](...) at apple_http_backend.mm:963:12
frame #14: CFNetwork`__77-[__NSCFURLSessionDelegateWrapper dataTask:didReceiveData:completionHandler:]_block_invoke.159
...
frame #27: libsystem_pthread.dylib`_pthread_wqthread
```

`frame #11` 的 `this=0x101e79638` 与 `frame #8` 的 `this=0x101e79638` **是同一个 Handle**。
`frame #13` 的 delegate 里做过 `begin_inflight()`（`inflight_ = 1`），
它对应的 `end_inflight()` 就在 `frame #13`，位于 `frame #8` **下方**。
`frame #8` 在等 `frame #13` 执行完，而 `frame #13` 在等 `frame #8` 返回。

第二次运行里三个 Handle（`0x101e67dd8` / `0x101e79638` / `0x101e71758`）
各自在自己的 delegate 线程上陷在完全相同的 8→9→10→11→12→13 形状里。

### 另一条 delegate 线程 —— 解释 `inflight=2`

```
frame #6: (anonymous namespace)::Handle::emit_complete(this=0x101e71758, st=-3, http=0) at apple_http_backend.mm:581:37  <- lock_guard(emit_mu_) 上阻塞
frame #7: (anonymous namespace)::Handle::did_complete(this=0x101e71758, error=NSURLErrorDomain -999) at apple_http_backend.mm:863:5
frame #8: -[SypHttpDelegate URLSession:task:didCompleteWithError:](...) at apple_http_backend.mm:990:12
```

`[task cancel]` 触发的 `didCompleteWithError(-999)` 已经到了，它也做了 `begin_inflight()`
（`inflight_` 变 2），然后堵在 `emit_mu_` 上 —— 那把锁被同一 Handle 的 `emit_data` 持着，
而 `emit_data` 正卡在 `destroy` 里。日志里 `inflight=1` 与 `inflight=2` 交替出现就是这两条的叠加。

---

## 1. 死锁的确切成因

三条执行流，但**真正的环只需要一条线程**：

### 流 X（delegate 线程 D1，环的本体）

锁的获取顺序，逐步：

| 步 | 位置 | 拿什么 / 放什么 |
|---|---|---|
| 1 | `didReceiveData` → `handle_for_task` | 拿 `Owner::mu` → 放（拿到 `shared_ptr<Handle>`） |
| 2 | `begin_inflight()` | 拿 `state_mu_` → `++inflight_`（=1）→ 放 |
| 3 | `did_receive_data` 入口闸 | 拿 `state_mu_` 看 `completed_/destroyed_/canceled_` → 放 |
| 4 | `emit_data` | **拿 `emit_mu_`（此后一直持有）** |
| 5 | `emit_data` 头 | 拿 `state_mu_` → `in_callback_ = true`、`callback_thread_ = D1` → 放 |
| 6 | `s.on_data(...)` | 不持锁调 dl 层（写 cache、唤醒等待中的 reader）。**取消就在这段窗口里落地** |
| 7 | `emit_data` 尾（`apple_http_backend.mm:666-671`） | 拿 `state_mu_` → **`in_callback_ = false`** → 见 `canceled_` → `completed_ = true`、`do_cancel_complete = true` → 放 |
| 8 | `apple_http_backend.mm:672` | `s.on_complete(ctx, SYP_ERR_CANCELED, 0)`（**仍持 `emit_mu_`**） |
| 9 | `DLTask::sink_on_complete` | 拿 `DLTask::mu_` → `old = take_handle_for_destroy_locked()`（`handle_` 置空）→ 放 |
| 10 | `dl_task.cpp:895` | `backend_->destroy(old)` |
| 11 | `trampoline_destroy` → `lock_live` | 拿 `Owner::mu` → 放 |
| 12 | `Handle::destroy()` | **拿 `state_mu_`**，`destroyed_ = true`，算 `same_thread = in_callback_ && callback_thread_ == this_thread` |
| 13 | `apple_http_backend.mm:563` | `in_callback_` 在第 7 步已被清成 false ⇒ `same_thread == false` ⇒ 进入 `cv_.wait_for(inflight_ == 0 && !in_callback_)` |

第 13 步的谓词只能由 `end_inflight()` 满足，而 `end_inflight()` 在 `didReceiveData`
里、**第 13 步这一帧的下方**。它在等自己返回。这是一条**单线程自死锁**，
不需要第二条线程参与就已经无解。

### 流 Y（delegate 线程 D2，同一个 Handle 的 `didCompleteWithError`）

`Owner::mu` → `begin_inflight`（`inflight_` 变 2）→ `emit_complete` 里**堵在 `emit_mu_`**
（被 D1 在第 4 步拿着，永不释放）。它让谓词更加不可能成立，并解释了 `inflight=2`。

### 流 Z（主线程，二级受害者）

`SourceBridge::mu_`（`apply_window` 里已放）→ `Scheduler::mu_`（`reap_except` 里已放，
`dying.clear()` 在锁外）→ `~DLTask` → **持 `DLTask::mu_` 等 `finished_emitted_ && in_user_callback_==0 && handle_refs_==0`**。
`finished_emitted_` 只能由流 X 的 `sink_on_complete` 在 `backend_->destroy(old)`
**返回之后**调 `finish()` 置位。流 X 永不返回 ⇒ 主线程永不醒。

### 一句话

`Handle::destroy()` 的等待条件包含了「调用者自己所在的 delegate 帧持有的 inflight」，
而识别这种自调用的逃生口（`in_callback_`）被 `emit_data` 的取消收尾路径提前熄火了。

---

## 2. `inflight=1` 而 `in_callback=0` 说明卡在哪一段

**它不是「回调阻塞在某把锁上」，而是「根本不在回调里，但仍持有 inflight」。**

delegate 帧当前的位置精确到行：`apple_http_backend.mm:672`，即 `emit_data` 收尾的

```c
if (do_cancel_complete && s.on_complete != nullptr) {
    s.on_complete(s.ctx, SYP_ERR_CANCELED, 0);   // <- 672
}
```

它**没有阻塞在任何互斥量上**，而是同步下钻进了 dl 层的 `sink_on_complete`，
再下钻回本 Handle 的 `destroy()`，然后在 `destroy()` 里睡在自己的条件变量上。

- `inflight=1` = `didReceiveData` 那一帧的 `begin_inflight()`，也就是**等待者自己的**那一份。
- `in_callback=0` = 紧邻 672 行上方（666 行）的 `in_callback_ = false;`。

`in_callback_ = false` 与 `s.on_complete(...)` 之间只隔了一个 `if`，
但正是这一行让 `destroy()` 的 `same_thread` 判据失效。
**日志那句诊断的括号「contract: on_complete already delivered」也是误导：
契约并没有被违反，on_complete 正在这条栈上被投递。**

对照：正常路径 `Handle::emit_complete()` 是**先置 `in_callback_ = true` 再调 `s.on_complete`**，
所以从 `emit_complete` 里 destroy 会命中 `same_thread` 分支、跳过等待，一切正常。
坏掉的只有 `emit_data` / `emit_response` / `emit_redirect` 三处**取消合成收尾**。
（这也解释了为什么 155 个 dl 单测 + 20 个后端用例全绿：没有一个用例从 `on_complete`
里面调 `destroy`，更没有一个让 cancel 落在 `on_data` 执行期间。）

---

## 3. 触发条件

需要同时满足：

1. **DLTask 处于 `Receiving`**（`state=2`，`DLTaskState::Receiving`；枚举顺序
   `Idle=0, Connecting=1, Receiving=2, Done=3, Failed=4, Canceled=5`）。
   即 `handle_ != nullptr`、`resume_called_ == true`、字节正在流。
2. **调度器把这条 Slot 标 `dead` 并回收**。观察到的路径是
   `SourceBridge::read` → `apply_window` → `Scheduler::set_read_position(196608)`
   → `reap_except()`（`scheduler.cpp:508` 的 `dying.clear()`）→ `~Slot` → `~DLTask`。
   标 `dead` 的地方是 `scheduler.cpp:705`（`should_stop_slot_locked` —— 窗口推进后
   这条任务不再需要）。
3. **`user_canceled=1` 的来源是 `~DLTask()` 自己的第一行 `cancel()`**（`dl_task.cpp:241`），
   **不是**看门狗的 `syp_source_interrupt`，**也不是** `syp_source_close`。
   看门狗线程（`scenarios.cpp:83`）在栈上只是在 20ms 轮询里 sleep；而且即便它触发
   `request_abort()` 也救不了 —— 主线程卡在 `syp_source_read` **下方**的 `apply_window` 里，
   FFmpeg 的 interrupt 回调根本没有机会被检查。
4. **cancel 落在 `emit_data` 的执行窗口内**：即在 `did_receive_data`/`emit_data`
   入口闸（`completed_||destroyed_||canceled_` 检查）之后、`emit_data` 收尾之前。
   这个窗口 = dl 层 `on_data` 的整个执行时间（写 cache 文件 + 唤醒等待中的 reader），
   实测足够宽，**复现率接近 100%**（第二次运行里三条任务同时中招）。

`finished_emitted=0` 配 `handle=0x0` 的含义（顺序证据）：
`sink_on_complete` 一进门就在锁里 `old = take_handle_for_destroy_locked()`，
把 `handle_` 置空；随后在 `dl_task.cpp:895` 同步 `backend_->destroy(old)`；
`finish()`（置 `finished_emitted_`）在更后面。destroy 卡住 ⇒ `finish()` 永远没被调到。
所以「句柄已经交出去、终态还没发」正是**卡在 destroy 那一行**的指纹。

---

## 4. 是谁的缺陷

**主责在 Apple 后端；次责是 `syp_http.h` 契约没写清楚。dl 层无责。**

依据：

1. dl 层的行为是契约允许的。`syp_http.h` 对 `destroy` 只写了
   「释放。调用前 dl 层保证已收到 on_complete。」—— 在 `on_complete` **里面**调 destroy
   满足这句话（on_complete 正在被投递、已经收到），头文件没有禁止。
2. Apple 后端**自己就是按支持这条来写的**：`Handle::destroy()` 里那段
   `same_thread = in_callback_ && callback_thread_ == std::this_thread::get_id()`
   与注释「同线程回调内调用则跳过，避免等自己返回」，就是专为这个场景准备的逃生口。
   正常的 `emit_complete` 路径也确实工作正常。
3. 后端的 bug 是**逃生口在三条取消合成路径上没有武装**：
   `emit_data` / `emit_response` / `emit_redirect` 的收尾把 `in_callback_` 清零之后
   才调 `on_complete`，于是自调用被误判为「别的线程还在回调里」。
4. 附带的设计缺陷：`destroy()` 等的是 `inflight_`，但 `inflight_` 与
   「调用 destroy 的线程自己是不是就在那条 delegate 栈上」没有任何关联。
   注释里担心的 UAF（`forget` 之后 inflight 回调野指针）其实由
   `handle_for_task()` 返回的 `shared_ptr<Handle>` 兜住了 —— delegate 帧全程握着一份强引用，
   `forget()` 只是从 `live`/`by_task` 两张表里擦掉，对象不会死。
   也就是说这个 `inflight_` 等待的**必要性本身**值得重新审视。
5. 契约侧的责任：头文件既没说「destroy 可以在 on_complete 内调用」，
   也没说「后端不得在 destroy 里等待调用线程自身」。正因为没写，
   20 个后端用例全都是「主线程等 `wait_complete()` 之后再 destroy」，
   一个都没覆盖到 dl 层真实的调用形状。这是**验证矩阵的盲区**，
   与步骤 6 记下的那条教训同源。

---

## 5. 与 known-gaps #13 的关系

**不是同一个问题。是同一片区域（cancel 与 complete 的竞争）里的两个不同缺陷。**

| | #13 | 本次 |
|---|---|---|
| 窗口 | `on_complete` **已经回调完**、`destroy` 尚未调用之间 | `on_complete` **正在被回调**（栈上） |
| 谁在动 | 另一线程的 `DLTask::cancel()` 拿着非空 `handle_` 调 `backend->cancel` | 同一条 delegate 线程在 `on_complete` 里调 `backend->destroy` |
| 症状 | 严格后端可能补发**第二次** `on_complete` | 单线程**自死锁**，没有任何重复回调 |
| 后果 | dl 层能容（`finished_emitted_` 吞掉） | 永久挂死 |
| 修在哪 | 后端把 complete 之后的 cancel 当 no-op | 后端的 destroy 不得等待自己所在的 delegate 帧 |

两者的**共同根因**是 `syp_http.h` 完全没有规定生命周期方法的重入规则。
所以将来补契约时应该一次写全，而不是只补 #13 那一句：

- `cancel` 在 `on_complete` 之后、`destroy` 之前必须是 no-op，不得再回调（#13）；
- `destroy` **可以**在 `on_complete` 回调内、由后端自己的回调线程调用；
  后端不得在 `destroy` 中等待调用线程自身所在的回调帧（本次）；
- `destroy` 返回后后端不得再触碰 sink。

补充一点：本次 `emit_data` 收尾合成的那次 `on_complete` **是第一次**（`completed_` 当时为 false），
随后真正到达的 `didCompleteWithError(-999)` 会被 `completed_` 吞掉。所以本次**没有**踩到 #13。

---

## 6. 修复方向（只给方向与权衡）

### 方案 A —— 收尾路径保持 `in_callback_`（最小正确修法，推荐）

在 `emit_data` / `emit_response` / `emit_redirect` 的收尾里，当 `do_cancel_complete` 为真时
**不要提前把 `in_callback_` 清成 false**：让它跨过 `s.on_complete(...)` 保持为 true
（`callback_thread_` 本来就还是当前线程），调用返回后再清零并 `notify_all`。

- 改动量：三处，各 ~5 行。
- 风险：低。`destroy()` 走 `same_thread` 分支跳过等待、`forget()` 之后 `in_callback_ = false`
  写在一个已被 forget 但仍被 delegate 帧的 `shared_ptr` 保活的对象上，安全 —— 这与今天
  `emit_complete` 正常路径的行为完全一致（那条路径每天都在这么跑）。
- 需要注意：`notify_all` 仍要保留（虽然此刻没有别的等待者）。
- 变体 A'：把三处收尾统一抽成一个 `finish_with_cancel_emit_locked()`（不再抢 `emit_mu_`，
  因为调用点已经持有），做与 `emit_complete` 完全一样的 `in_callback_` 记账。
  更干净，避免三处各写一遍再漏一处。**推荐 A'。**（**已按 A' 实施**，见
  `apple_http_backend.mm:175` 的 `finish_with_cancel_emit_locked`。）

### 方案 B —— 把逃生口的判据换成「本线程是否持有本 Handle 的 inflight」

在 `begin_inflight()` 里记下 delegate 线程 id（或用 `thread_local` 记录
「当前线程正在为哪些 `Handle*` 跑回调」），`destroy()` 用它来判断要不要跳过等待。

- 好处：修的是**整类**问题，而不是今天已知的三个点；将来再加一条 delegate 回调、
  或再多一条合成 complete 的路径，都不会重新踩坑。
- 风险：中低。需要处理同一线程重入计数；`thread_local` 方案要注意 GCD 线程复用
  （每次 `begin/end_inflight` 成对增减即可，不跨回调残留）。
- 建议：A'（立即止血）+ B（收口），或直接上 B。

#### ✅ 方案 B 已实施（2026-09-09），以及实施后的实际结论

实现：`Handle` 新增 `inflight_by_thread_`
（`std::vector<std::pair<std::thread::id,int>>`），`begin_inflight()` /
`end_inflight()` 在**已有的 `state_mu_` 临界区内**成对增减；
`destroy()` 的逃生口判据变成
`holds_inflight_on_this_thread_locked() || (in_callback_ && callback_thread_ == 本线程)`。
`Handle::cancel()` 里那条防「同线程重入非递归 `emit_mu_`」的判据同步改成同一个并集。

写这条时预判的四个风险点，落地后的实际结论：

1. **GCD 线程复用会不会串味** —— 不会，但**前提是记账必须 per-Handle**。
   记账是 `Handle` 的成员，不是全局的「线程 → 是否在回调里」集合。
   delegate 跑在共享的并发 `NSOperationQueue` 上，线程 T 这一刻服务 Handle A、
   下一刻服务 Handle B；如果记账是全局的，T 为 A 记上的账会让 B 的 `destroy()`
   误判「调用者就在我的回调帧里」而跳过本该做的等待 —— 那是把一个死锁换成一个
   UAF。per-Handle + `begin`/`end` 成对增减 ⇒ T 去服务 B 时，A 的表里早已没有 T，
   线程复用本身不留任何残留（条目在计数归零时就被 erase 掉，不是留一个 0）。
2. **同线程重入** —— 今天不可达：`begin_inflight()` 只有四条 delegate 回调会调，
   `NSOperationQueue` 的一次 operation 不会在自己的线程上嵌套执行另一条，
   `didReceiveResponse` 的 `completionHandler` 也在 `end_inflight()` 之后才调。
   但仍然**按计数记账而不是布尔**：布尔一旦哪天真的嵌套，内层的 `end` 会把外层帧
   的记账抹掉、逃生口随即失效 —— 那正是本次要根治的那一类缺陷，成本只是一个 `int`。
3. **热路径开销与线程安全** —— 不引入新锁、不改锁序：`begin`/`end_inflight` 本来
   就要拿 `state_mu_`，记账写在同一个临界区里。数据结构选小 `vector` + 线性扫描
   而不是 `unordered_map`：条目数 = 当前在本 handle 回调里的线程数（实测 0~2），
   线性扫描比哈希便宜，且 `vector` 容量在第一次 `push_back` 之后复用，
   稳态（0↔1 来回）零分配；`unordered_map` 会在每次回调 malloc/free 一个节点。
   也没有选 `thread_local`：那会引入进程级全局状态与线程退出期的析构时序问题，
   收益只是省掉一次几个元素的线性扫描。
4. **`in_callback_` / `callback_thread_` 该删还是该留** —— **必须留**。
   这是实施过程中最重要的一条修正：**方案 B 并不能覆盖方案 A'**，两者是并集关系。
   B 只覆盖「经由 `begin_inflight()` 进来的 delegate 路径」，而今天有三条投递
   `on_complete` 的路径**根本不经过 `begin_inflight()`**：
   `Handle::cancel()` 的 cancel-before-resume 合成、`Handle::start()` 的
   cancel-before-start 合成、`on_timer()` 的超时合成 —— 它们在调用方线程或 GCD
   定时器线程上直接调 `emit_complete()`，栈上没有任何 inflight。
   sink 在那次 `on_complete` 里同线程 `destroy()` 时，只有 A' 的判据能救。
   （`cancel_before_start_completes_once` 这条既有用例走的正是这个形状。）
   所以本文原来那句「A'（立即止血）+ B（收口），或**直接上 B**」里的
   「直接上 B」是错的：只上 B 会引入一条新的自死锁。
   `in_callback_`/`callback_thread_` 另外还有两个不可替代的用途：
   `Handle::cancel()` 判断要不要合成（防同线程重入非递归的 `emit_mu_`），
   以及 `destroy()` 超时诊断里打印「等的是哪条线程」。

**增量价值已被证明**（这是 B 的全部理由，不证明就只是重构）：
临时在 `did_receive_data` 里注入一条「在 `begin_inflight()` 括号内投递 sink 回调、
却忘了 `in_callback_` 记账」的新路径（模拟将来有人加了一条 delegate 回调），
sink 在 `on_complete` 里同线程 `destroy`：

- A' 判据（把 `own_inflight` 打回 `false`）下 → **自死锁**，用例变红，
  诊断打出 `inflight=1 in_callback=0 own_inflight=0`
  且 `this_thread == callback_thread`；
- B 判据下 → 绿。

同时四条既有回归用例（三条 `destroy_in_on_complete_after_cancel_in_on_*`
+ 跨线程闩锁版）在「判据打回旧 `in_callback_` 语义 + A' 收尾改回提前清标志」
的反向自检下**全部变红**，改回后全绿。

配套：`finish_with_cancel_emit_locked()` 与 `emit_complete()` 那两份重复的
记账已合并成唯一的投递点 `deliver_complete_locked()`（见 `docs/tech-debt.md`）。

### 方案 C —— 干脆不合成这次 `on_complete`

`emit_data` 走到收尾时 `resume_called_` 必然为真（有数据才有 `emit_data`），
`Handle::cancel()` 已经调过 `[task cancel]`，`didCompleteWithError(-999)` **必然会来**。
所以这次合成是冗余的：只在 `!resume_called_` 时合成，其余交给 `did_complete` →
`emit_complete` 走正规记账。

- 好处：直接消灭一条重入路径，代码更少。
- 风险：**中**。把「恰好一次 on_complete」的保证从自己手里交给了 NSURLSession。
  一旦某种情况下 `didCompleteWithError` 不来，任务就永久挂着 ——
  而 `dl_task.h` 明确写了「后端不守约（永不回调）时，任务不会自行超时」，
  没有兜底。另外 cancel 的响应延迟会变大（要等一个 RTT 级别的 delegate 回调）。
- 可以作为 A'/B 之上的**简化**，但不建议单独作为修复。

### 方案 D —— dl 层不在 `on_complete` 里同步 destroy（不推荐作为主修）

让 `DLTask::sink_on_complete` 把 `backend_->destroy(old)` 推迟到回调栈之外。

- 风险：**高**。需要一个 deferred-destroy 的所有者（谁来跑、跑在哪条线程、
  与 `~DLTask` 的等待条件怎么协调），生命周期规则大改；而且它是在给后端的 bug 打补丁——
  任何第三方后端只要同样自等就还会挂。
- 唯一的价值：如果决定把「destroy 不得在 on_complete 内调用」写进契约，
  那就必须走这条。但那等于放弃 Apple 后端已经写好的 `same_thread` 逃生口，
  且让所有后端的 destroy 都变复杂。**不建议**。

### 配套（无论选哪条）

1. `syp_http.h` 补齐重入规则（见第 5 节三条）。**未实施**，登记为技术债。
2. `Handle::destroy()` 那句诊断的括号文案要改 —— 现在的
   「contract: on_complete already delivered」把人往「调用方违约」上带，
   实际正好相反。至少加上 `this_thread` 与 `callback_thread_` 的对比信息。
3. 主线程侧的健壮性：`SourceBridge::read` → `apply_window` → `reap_except` 这条链上
   `~DLTask` 会**无限期**阻塞在 read 路径里，看门狗的 `request_abort()` 够不着。
   即使后端修好，「回收一条任务」也不该是 read 路径上的同步无界等待 ——
   值得单独记一条（把回收挪出 read 路径，或给它一个有界等待 + 降级）。

---

## 7. 能不能构造确定性复现

**能，而且比 `in_schedule_` 死锁容易得多 —— 连注入钩子都不需要。**

关键在于：触发这条死锁的两个动作（cancel、destroy）**都可以由 sink 自己在回调线程上发出**，
所以整个交错可以由测试代码单线程强制排定，没有任何时序赌博。

### 思路 1：后端级最小用例（首选，纯确定性，已实施）

在 `tests/test_apple_http_backend.cpp` 里加一个用例，复用现成的 `LoopbackServer`：

```
LoopbackServer srv{ .resource_length = 10000, .support_range = true,
                    .pause_after_bytes = 64, .pause_ms = 5000,
                    .body_chunk_bytes = 32 };   // 与 cancel_while_receiving 同款配置
sink.on_data     = [](...) { if (first_time) b->cancel(h); };   // 在回调线程上取消
sink.on_complete = [](...) { b->destroy(h); };                  // 在回调线程上销毁
```

执行顺序被代码结构完全钉死：
`emit_data` 持 `emit_mu_` → `in_callback_ = true` → `on_data` → `cancel()` 置 `canceled_`
→ 收尾必然见到 `canceled_` → `in_callback_ = false` → 合成 `on_complete`
→ `destroy()` → `same_thread == false` → 自等。**100% 命中，无竞争成分。**

用例必须自带看门狗（另起一条线程，N 秒后 `CHECK(false)` 并 `_exit` 或
标记失败），否则回归时会像今天一样把 ctest 拖到超时而不是给出失败点。

这个用例同时也是**契约用例**：任何后端实现都应该通过它。
已顺带补第二个：`on_response` 里 cancel（覆盖 `emit_response` 收尾）、
`on_redirect` 返回 false（覆盖 `emit_redirect` 收尾）——三个都在
`test_apple_http_backend.cpp` 里，`destroy_in_on_complete_after_cancel_in_on_*`
三个 `TEST_CASE`。跨线程闩锁版（思路 2）与 dl 层级桩后端版（思路 3）
**均已补齐**，见 `docs/tech-debt.md`。

### 思路 2：跨线程版本（用闩锁定序，仍然确定性）—— 未实施

如果想复现「cancel 来自另一条线程」的真实形状（也就是今天 `~DLTask` 的形状）：
sink 的 `on_data` 里先 `entered.set()`、再阻塞等 `release`；
主线程 `entered.wait()` → `b->cancel(h)` → `release.set()`。
交错由两个闩锁排定，不靠 sleep、不靠撞。

### 思路 3：dl 层级（用 stub_backend，回归 `SourceBridge` 那条链）—— ✅ 已实施并完成完整反向自检

给 `tests/support/stub_backend` 加一个开关：
「在 `on_data` 交付期间若收到 cancel，则在同一条回调栈上合成
`on_complete(SYP_ERR_CANCELED)`，并且 `destroy()` 时断言/阻塞若检测到自身重入」。
这样 `test_scheduler` / `test_source_bridge` 里就能构造
「窗口推进 → `reap_except` → `~DLTask` → 后端回调栈上合成 complete」
的完整链，锁住 dl 层这一侧的行为不回退。

不过要强调：**桩后端不是缺陷所在**，思路 3 是补验证矩阵的洞（真实后端 × 调度器/桥
这个交叉点从来没被覆盖过），思路 1 才是这条 bug 的回归测试。

落地形态是 `test_scheduler.cpp` 的
`window_advance_reaps_task_reentrant_destroy_from_worker_thread`。
2026-09-09 补齐了它欠的两步（见 `docs/tech-debt.md`）：

1. **看门狗安全网**：`Watchdog` 从 `test_apple_http_backend.cpp` 抽到
   `tests/support/watchdog.h`，新增 `block_until_hard_exit()` —— 挂死时不返回、
   不析构任何东西，把进程交给硬超时 `_exit(70)`。用例把 `Watchdog` 声明为作用域内
   第一个局部量，整条析构链（`~Reaper` → `~Scheduler` → `~Harness`/`~StubBackend`）
   都在它的保护之下。
2. **完整反向自检**：在 `StubBackend::trampoline_destroy` 里注入「同线程重入
   destroy 时永久自等」——即本文诊断的那个形状。用例如实变红、按预期硬退出
   （exit 70），且 `~DLTask` 打出的指纹与本文第 3 节记录的原始死锁一致
   （`state=2 finished_emitted=0 handle=0x0 user_canceled=1`）。注入已完整还原。

自检还顺带暴露一件事：注入第一版没有限定用例范围时，`test_async_full_download`
等异步用例**也**被同一条注入挂住 —— 它们同样会走「worker 线程在自己的回调栈上
同线程重入 destroy」这条路径，但都没有看门狗，后端一旦回归就是整个套件零输出
挂到 ctest TIMEOUT。已登记为技术债。

### 顺带补矩阵

现有 20 个后端用例的共同形状是「主线程 `wait_complete()` 之后再 destroy」。
应当引入一个维度：**destroy 的发起线程 ×（主线程 / on_complete 回调内）**，
以及 **cancel 的发起时机 ×（start 前 / connecting / on_response 内 / on_data 内）**。
本次挂死的那格正是「on_data 内 cancel × on_complete 内 destroy」，已补三条契约用例，
矩阵仍不完整（见上面思路 2/3）。

---

## 附：为什么必须先修这个死锁，`test_probe_e2e` 才能产出结论

死锁发生在**第一个用例的顺序读中段**（`test_probe_e2e.cpp:76` →
`sequential_read_matches("faststart.mp4")`，`seeks=size=0`，读到 196608 附近），
远在 `syp_source_close` 之前。逐 packet 比对断言一条也没跑完。

而且看门狗救不了：`bridge->request_abort()` 只能让 FFmpeg 在**下一次**检查 interrupt 时退出，
但主线程已经卡在 `syp_source_read` 内部的 `apply_window` 里，永远不会回到 FFmpeg 的检查点。

所以：**这条缺陷必须先修，`test_probe_e2e` 才能产出任何有效结论。**
（唯一的例外是 `SYP_PROBE_SELFCHECK_FILE` 反向自检那条支路 —— 它走 `file:` 协议，
根本不碰下载层，所以不受影响，但也正因如此它证明不了任何关于下载层的事。）
