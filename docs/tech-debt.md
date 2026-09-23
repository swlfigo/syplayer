# 技术债

与 `docs/known-gaps.md` 分工：那边记**产品/行为**缺口（用户能感知到的落差），
这边记**代码与测试卫生**（不影响产品行为，但会让人踩坑或者是覆盖度上的洞）。

条目按优先级排序，不按发现时间。每条注明来源与现状；标了「登记为债务」的
都是明确判定"不阻塞合并、以后再修"的项，不是遗漏。

---

## Apple 后端死锁修复的配套项（见 `docs/2026-09-08-apple-backend-deadlock.md`）

- **方案 B** ✅ 已实施：逃生口判据改成"本线程是否持有本 Handle 的 inflight"
  与原来的 `in_callback_`/`callback_thread_` 的**并集**。`Handle` 新增
  `inflight_by_thread_`（`vector<pair<thread::id,int>>`），
  `begin_inflight()`/`end_inflight()` 在已有的 `state_mu_` 临界区里成对增减，
  不引入新锁、不改锁序。记账**必须 per-Handle**（GCD 复用 delegate 线程，
  全局记账会串味）、**必须计数不用布尔**（防将来出现同线程嵌套时被内层
  `end` 抹掉外层帧的记账）。
  **实施后的修正结论：B 不能覆盖 A'，两者是并集。** 有三条投递
  `on_complete` 的路径根本不经过 `begin_inflight()`（`Handle::cancel()` 的
  cancel-before-resume 合成、`Handle::start()` 的 cancel-before-start 合成、
  `on_timer()` 的超时合成），栈上没有任何 inflight —— 只上 B 会引入一条新的
  自死锁，所以 `in_callback_`/`callback_thread_` 保留、不删。
  增量价值已被证明：临时注入一条"在 `begin_inflight()` 括号内投递 sink 回调、
  却忘了 `in_callback_` 记账"的新 delegate 路径，A' 判据下自死锁变红、
  B 判据下绿；四条既有回归用例在"判据打回旧语义 + A' 收尾改回提前清标志"
  的反向自检下全部变红，改回后全绿。详见
  `docs/2026-09-08-apple-backend-deadlock.md` 第 6 节。
- **`syp_http.h` 生命周期重入规则** ✅ 已补齐：`cancel`/`destroy` 声明
  附近补了三条（cancel-after-complete 必须 no-op；destroy 可以在
  on_complete 回调内由后端自己的回调线程同步调用，逃生口判据看的是
  destroy 调用者所在的线程/帧；destroy 返回后不得再触碰 sink）。只加
  注释，`bash tools/check-abi.sh` 仍 exit 0。同时解决了
  `docs/known-gaps.md` #13（cancel-after-complete 隐含新要求）。
- **跨线程闩锁版回归用例（思路 2）** ✅ 已补：
  `test_apple_http_backend.cpp` 的
  `destroy_in_on_complete_after_cross_thread_cancel_in_on_data`，cancel
  从测试主线程（不是 delegate 线程）落在 `emit_data` 的执行窗口内，
  用两个闩锁把交错钉死。反向自检（临时把
  `finish_with_cancel_emit_locked` 打回旧语义）证明四条用例（含这条新的）
  全部会真的挂死变红，改回后全绿。
- **dl 层级桩后端版（思路 3）** ✅ 已补齐并完成完整闭环：
  `test_scheduler.cpp` 的
  `window_advance_reaps_task_reentrant_destroy_from_worker_thread`，给
  `tests/support/stub_backend` 加了
  `Script::pause_after_first_on_data_once` + `wait_paused()`/
  `release_paused()` 开关，在真实 `Scheduler`/`DLTask` 上复现"窗口推进 →
  `schedule()` 的 `to_cancel` → `reap_except()` → `~DLTask` 等
  `finished_emitted_`；释放暂停后 worker 线程在自己的回调栈上合成
  `on_complete` 并同线程重入 `backend_->destroy()`"这条链。
  上一轮欠的两步已于 2026-09-09 补上：
  **(1) 看门狗安全网** —— `Watchdog` 从 `test_apple_http_backend.cpp` 抽到
  `tests/support/watchdog.h`（顺带消掉一处将要重复的类），新增
  `block_until_hard_exit()`：挂死时不返回、不析构任何东西，把进程交给硬超时
  `_exit(70)`；用例把 `Watchdog` 声明为作用域内第一个局部量，整条析构链
  （`~Reaper` → `~Scheduler` → `~Harness`/`~StubBackend`）都在保护之下。
  硬超时取 20000ms（不是 apple 套件的 30000ms）：scheduler 目标 ctest
  TIMEOUT 45s，30000 叠上 5s 兜底离 45s 太近，与"ctest TIMEOUT 撞看门狗"
  那条教训同源。
  **(2) 完整反向自检** —— 在 `StubBackend::trampoline_destroy` 里注入
  "同线程重入 destroy 时永久自等"（即 Apple 后端修复前的形状），用例如实
  变红并按预期硬退出（进程 exit=70），`~DLTask` 打出的指纹与原始死锁一致
  （`state=2 finished_emitted=0 handle=0x0 user_canceled=1`）。注入已完整
  还原，仓库里不留。这一步证明的是"这条回归真的能抓住死锁那一类"，
  上一轮的阉割版自检只证明了"暂停机制是必需的"。
- **`finish_with_cancel_emit_locked` 与 `emit_complete` 的重复记账**：已核实
  合并完成，条目已移入下方"已解决"一节，不再算作本节未闭环项。
- **其余异步 scheduler 用例没有看门狗**（本轮自检顺带发现）：思路 3 的
  自死锁桩注入第一版没有限定用例范围时，`test_async_full_download` 等
  **异步模式**的用例也被同一条注入挂住 —— 它们同样会走"worker 线程在自己的
  回调栈上同线程重入 `destroy()`"这条路径，但都没有看门狗：后端一旦回归，
  表现是整个套件零输出挂到 ctest TIMEOUT（45s），连挂在哪条用例都看不出来。
  现在 `tests/support/watchdog.h` 已经是共享的，给这些用例配上的成本很低。
  仍未实施，登记为债务。
- **`test_apple_http_backend` 在 40 路并行下有约 5% 的负载性假红**
  （本轮连跑发现，**与本轮改动无关**）：40 路并行 × 20 轮 = 800 次里，
  改动前后各 40 次退出码非 0，失败指纹分布几乎一致（改动前 38/38/34/20 条，
  改动后 40/38/37/29 条，同样集中在四条 `destroy_in_on_complete_*` 与两条
  loopback 用例上）。根因是机器被打满时回环连接建不起来／响应超时：
  典型指纹是 `complete_st left=-20`（`SYP_ERR_NETWORK`）配 `redirect_n=0`
  / `data_n=0`，以及偶发的 `srv.port() != 0`；**800 次里
  `destroy still waiting` 出现 0 次**，不是死锁。16 路并行 × 30 轮 = 480 次
  则 0 失败。这类用例走真实 NSURLSession + 真实回环 socket，本身就有一个
  "并行度上界"，超过之后连跑的是机器而不是代码。要么给这些用例的连接/读
  超时按并行度放大，要么在连跑脚本里把并行度钉在 16。仍未处理，登记为债务。
- **死锁修复的看门狗文案 bug** ✅ 已修：三处 `if (wd.fired())` 改成
  `if (returned && wd.fired())`，真失败时不再打印「不算失败」。

---

## 测试/工具卫生，零误导风险

- **`gen-fixtures.sh` 里 `vbr_cap<2500` 的兜底分支，有意保留为死代码**：
  在当前 `dur` 取值范围（`40 + int(rand()*61)`，即 40~100 秒）内这行
  永远不会触发——dur 越大 `60*8000/dur` 越小，最紧的 dur=100 时
  `vbr_cap` 约 4672，仍远高于 2500，判定条件恒假。权衡过删掉 vs.
  注释说明两种做法：删掉更"干净"，但会让这条下界地板消失——以后有人
  把 dur 的取值范围调大时（duration 越长，估算码率越低），`vbr_cap`
  真的可能塌到不合理的低值甚至负数，那时候没有这行兜底会静默产出
  病态的码率参数而不是在这里露出来。保留的成本只是一行永远不执行的
  分支，收益是给未来的范围变更留一道地板，选择保留 + 在脚本里写清楚
  当前为什么不触发（`tools/gen-fixtures.sh`）。
- **packet 摘要有意不加 `pkt->side_data`**：`pkt->duration` 已经补上
  （见下面"已解决"），但 `side_data` 判断为不该加。`pos`/`pts`/`dts`/
  `duration` 都是容器解封装出的确定性标量，两条路径（`file:` 协议 vs.
  我们的 `syp_source` 走 `AVIOContext`）读的是同一份字节、走同一个
  demuxer，理应逐比特相同；但 `side_data` 的性质不一样——它不是简单
  标量，是任意长度的字节块集合（`AV_PKT_DATA_NEW_EXTRADATA`、
  `AV_PKT_DATA_DISPLAYMATRIX` 等等），某些类型在 FFmpeg 内部依赖探测
  过程中的中间状态（比如 SPS/PPS 在流内更新的时机），不能像
  `pos`/`duration` 那样简单论证"两条路径必然一致"——一旦不一致，摘要
  比对就会假红，而且假红的成因会很难诊断（side_data 的字节块本身不像
  payload 那样有清晰的"对不对"标准）。当前项目的两份素材（h264/aac）
  demux 时观察不到非空 side_data，加了在实测里也测不出什么，收益趋近
  于零而引入新假红源的风险不趋近于零，判定为不该加。登记为债务：如果
  以后素材矩阵扩展到会产生真实 side_data 的编码（比如 HDR 元数据、
  隐藏字幕），需要重新评估这个判断，而不是想当然地照抄 duration 那条
  的加法。
- **`averror` 与预期值的比较，有意保留为不可达的防御性冗余**：
  `diff_report()` 里 `e.averror != a.averror` 这条判断，在当前代码下
  确实永远走不到会返回 true 的那一支——走到这里之前两边 `error_stage`
  必然都判过空，而 `run_demux()` 里每一处写 `averror` 的地方都同时写了
  `error_stage`，所以两边 `error_stage` 皆空时两边 `averror` 也必然
  都是默认值 0。权衡过删掉 vs. 保留：删掉能去掉一行永远不会真的执行的
  分支，但这行本身是"如果以后有人在 `run_demux()` 加一条新失败路径时
  只写了 `averror` 忘了配 `error_stage`"这类回归的兜底——两者本该总是
  成对出现，但这个不变量目前只活在注释里，没有代码强制。保留成本几乎
  为零，能在那类回归发生时兜住而不是悄悄放过，选择保留 + 在
  `packet_digest.cpp` 里写清楚"当前不可达、为什么保留"。
- `std::hash<thread::id>` 理论上可能碰撞（标准没保证单射），当前用于
  日志/诊断场景，不影响正确性。
- `bounded_call` 的超时分支与紧随其后的 `remove_dir_recursive(cache)`
  之间存在理论上的文件系统竞态，从未在实测中触发过。

## 观察/覆盖度，不是缺陷

- **`syp::dl::resolve_url` 与 `ff_make_absolute_url` 还剩四类分歧（其中
  两类有意、两类只是没人会踩），全部落在扫描器产不出的输入上**（M5 Task 7 fix round 3；ruling R4 只要求做点段
  归一，这两类是顺带量出来的，**未修**）。本轮把探针直链
  `build-ffmpeg/slices/macos-arm64/lib/libavformat.a`、调真的
  `ff_make_absolute_url`（`hls.c:1045` 解析分片 URI 用的就是它），在
  29 base × 43 ref = 1,372 格全矩阵 + 1,982,504 次随机 URL 上逐字节对：
  点段类分歧 **540 → 0**，随机 fuzz **0 分歧**，剩下四类：
  | 类 | 次数 | 例 | 为什么不修 |
  |---|---|---|---|
  | A 空 base/ref | 27 | `("http://h/a/m.m3u8", "")` → 我们 `""` | 扫描器跳过空行，到不了 |
  | B base 没有 `scheme://` | 208 | `("//h/a/m.m3u8", "seg.ts")` → 我们 `""` | 复审已判为有意；`expand_playlist` 对空串 `continue` |
  | C ref 是 `scheme:` 但无 `//` | 46 | `"data:foo"` / `"x:y"` | FFmpeg 当绝对 URL，我们当相对路径拼。这类 URI 在 media 侧同样打不开，那条流本来就播不了 |
  | D ref 只有 query/fragment | 42 | `"?q=1"` → FFmpeg 保留 base 文件名 | 没有任何打包器这么写分片 URI |
  C 与 D 在**原理上**符合 ruling R4 判 Important 的那条判据（归一不一致 ⇒
  `CacheStore::make_key` 与播放侧两个 key ⇒ 100% 白下），但触发它们需要
  真实播放列表里出现 `x:y` 或 `?q=1` 形状的分片 URI，实务上为零。对齐 C
  要把 scheme 判据从 `"://"` 放宽成 FFmpeg 的"`:` 先于 `/?#`"，对齐 D 要
  加一条"ref 无路径段时保留 base 的文件名"的分支——两条都是新分支、新风险，
  而探针已经建好，Task 10 若决定做，改完直接重跑矩阵 + fuzz 即可验证。


- 可复现性回归目前只验了一个种子（`seed=12345`，`dur=50`，落在参数
  取值的未收窄分支），没有跑多颗种子。
- `demux_file` 不设 `interrupt_callback`——它读本地文件，正常不会挂住，
  唯一的看门狗是 ctest 自己的 TIMEOUT。
- `pos` 清零的断言可以加强为「断言两份素材（`moovend.mp4` /
  `faststart.mp4`）的 `pos` 差值恒定」，目前只验了各自清零。
- **`Retryable → 重试耗尽 → 致命` 这条弧在帧层没有用例**（M2a task-8）：
  M1 只在 dl 层验过这条完整弧（`test_dl_task.cpp`/`test_scheduler.cpp`
  的重试耗尽用例）。到了帧层，`test_decode_e2e.cpp` 场景 H
  （`h_dl_layer_error_propagates_and_step_stops_advancing`）只覆盖了
  "立即致命"这一臂——`LoopbackServer::set_config()` 直接切到
  `status_code=404`，命中 `classify_http_status` 判定的 `Fatal`，一次
  就粘滞（`SourceBridge::fatal_`），没有先经历几轮 `Retryable`（如
  408/429/5xx）重试、重试次数耗尽后才升级为致命这条路径。帧层目前没有
  用例验证"重试耗尽"这一步的升级本身在 `Pipeline::step()`/
  `decode_pipeline()` 这一层表现是否符合预期（比如是否会被误判成"该轨
  终止"而不是"整体致命"）。不是遗漏——场景 H 的设计取舍（见
  `test_decode_e2e.cpp` 文件顶部注释里 H 那一条）明确选了 404 这个能
  可靠触发的最简路径，重试耗尽这条弧留给以后需要时再补。
- **`syp_probe --decode` 走的 `lazy_pop=false` 路径结构性不会触发
  `Blocked`**（M2a task-8）：`DecodeOptions::lazy_pop` 默认为假，
  `decode_pipeline()` 在这个配置下对每个 `DecodedFrame` 同步立即
  `pop_frame()`——`Pipeline` 不持有线程，它自己驱动的循环里
  `FrameQueue` 永远堆不到 `max_frames_per_track`，`Blocked` 分支与它带
  的 `drain_all_tracks` 排空逻辑结构性地一次都不会被命中（跟场景 A~D
  最初撞见的盲区、`d2_` 用例上方长注释是同一个成因——见
  `tools/syp_probe/frame_digest.h` 里 `DecodeOptions::lazy_pop` 的注释）。
  这不是待修的缺陷，是记录这个事实本身：`lazy_pop=true` 是**测试专用**
  开关，`syp_probe` 工具本身不应该、也不会打开它（工具要的是尽快解完、
  给出判定，不是替 Blocked 分支陪跑）。登记的目的是防止将来有人翻
  `--decode` 的实现，看到 `Blocked` 分支覆盖率是 0，误以为这是漏测——
  `Blocked` 路径本身的正确性由 `test_decode_e2e.cpp` 场景 E（用
  `lazy_pop=true` 真正逼出 Blocked）单独守着，跟 CLI 工具走哪条路径是
  两件事。

## 假红风险但概率极低

- **`test_cache_store.cpp:856-860` 的 `no_progress_backoff_survives_across_rounds`，
  在 `ctest -j4` 满载下偶发假红**（M5 Task 7 fix-round-2 复审顺带观察到，
  M5 Task 7 fix round 3 登记）：断言是 `st.deleted`，复审在一次并发
  `ctest -j4` 里看到 `1 vs 0`，单独重跑 **23/23 全绿**。
  **不是本轮引入的**——它落在 Task 3 / Task 4 的 CacheStore TTL/LRU 那段，
  M5 Task 7 的 diff（`src/dl/m3u8_scan.*` + HLS 用例）一个字节都没碰过它。
  症状形状与本节其它条目同类：墙钟/调度敏感的计数断言在机器被压满时
  少数一次。本轮**不修**（ruling 明确要求只登记），留给 Task 10：
  该用例真正该钉的是"没有进展时退避确实生效"，而不是某一轮里恰好删了
  几个文件，修法多半是把计数断言换成确定性构造或谓词式等待。

- **`probe_e2e` 在高负载下偶发假红**（M5 Task 8 复审顺带观察到，M5 Task 8
  fix round 1 登记）：复审在一次**并发构建**进行时看到它红，**单独重跑绿**。
  与上一条是同一形状（墙钟/调度敏感的断言在机器被压满时少数一次），但它落在
  **Task 6** 那段（端到端探测：起本地服务端、开真源、量探测够到的字节与耗时），
  不在 Task 8 的 diff 里——Task 8 只改了 `normalize_cache_dir` 与 Swift/桥。
  本轮**不修**（ruling 明确要求只登记），与上一条一起留给 Task 10：
  这两条都要先判清楚"该用例真正想钉的性质是什么"，再把墙钟/计数断言换成
  确定性构造或谓词式等待，而不是把阈值往上调——调阈值只是把假红换成漏检。

- **`test_preloader.cpp:822` 的 `time_target_uses_provider`，在 `ctest -j4`
  满载下偶发假红**（M5 Task 8 fix round 2 实测观察到并登记）：断言是
  `st.downloaded_bytes >= 32000`，在一次带着变异的 `ctest -j4` 里看到它红，
  **单独重跑两次全绿、去掉变异后 `-j4` 也全绿**（也就是说它与那次变异无关，
  纯粹是负载敏感）。与上面两条同形状：墙钟/调度敏感的累计量断言在机器被
  压满时少数一次。本轮不修，与上面两条一起留给 Task 10 —— 三条都属于
  "把墙钟/计数断言换成确定性构造或谓词式等待"这一类，调阈值只是把假红换成漏检。
  **顺带一提，这类假红在变异验证里最危险**：它会把一条"变异其实存活"误判成
  "变异被杀掉"。本轮就靠"重跑 + 看失败的是哪条用例"识破了一次（失败的是
  `time_target_uses_provider`，而变异改的是播放列表抓取的容量字段，两者无关）。

- 场景 F 的 mdat 中点理论上可能落在 packet 之外，导致构造的断连点
  没有真正落在一个 packet 内部——概率很低，可以加"换偏移重试"来兜底，
  当前未加。
- **`test_scheduler.cpp:1383` 的 `async_dtor_with_inflight_tasks`，
  `CHECK(active_task_count() > 0)` 是墙钟竞速断言**：用例 `start()` 之后
  靠 `std::this_thread::yield()` 轮询、给自己设了 2s 墙钟截止时间等
  `active_task_count()` 变正，16 路并行压满机器时调度器被真正调度到的
  时机可能推迟到这个截止时间之后，断言偶发假红。做过 A/B 对照
  （16 路并行 × 30 轮 = 480 次为一组，同负载下改动前、改动后各命中
  1 次，命中率相同）：确认这是**既有**问题——与本轮（六波、约二十个
  提交）任何一次改动无关，它验证的是"调度器 `start()` 一个异步任务后
  确实进入 in-flight 状态"，不在死锁修复/方案 B/`emit_complete` 与
  `finish_with_cancel_emit_locked` 合并记账等本轮改动路径上。本轮不修，
  理由：(1) 实测命中率约 0.2%（1/480），量级与本节其它"概率极低"条目
  相当；(2) 真要修，办法是把"墙钟 2s + yield 轮询"换成确定性构造
  （比如给 `Scheduler` 加测试专用的"任务已提交"信号量/回调钩子，而不是
  外部轮询状态），这是改测试基础设施，超出本轮"只登记、不改逻辑"的
  范围；(3) 本轮清理的重点是六波引入或暴露的问题，这条是历史遗留、
  与本轮无关联，更适合单独立项处理。登记为债务，留给下一轮。

- **`test_probe_e2e.cpp` 的 `f_mid_transfer_disconnect_recovers`，在
  ASan+UBSan 构建下 `ctest -j 4` 偶发假红**（M2a task-8 分支终审 M-5
  修复轮记录）：把 `build-asan`/`build-tsan` 的 `SYP_FFMPEG_XCFRAMEWORK`
  从被指向的一个不存在路径改回默认值后（这两套构建此前因为这个 cache
  变量探测不到 FFmpeg，整体跳过了 `src/media/`/`tools/syp_probe/` 相关
  的 6 个 ctest 目标——见 README.md「测试」一节的更正），重新跑通全部
  16 个目标：`ctest -j 1`（串行）在 ASan+UBSan 与 TSan 下都是稳定
  16/16、零 sanitizer 报告；但 `ctest -j 4`（并行）在 ASan+UBSan 下
  观测到 `probe_e2e` 的 `f_mid_transfer_disconnect_recovers` 场景
  间歇性失败（3 次里 1 次），单独重跑（`ctest -R probe_e2e`，同样
  `-j 4`）3/3 稳定通过，`-j 1` 更是从未见过它红。这条场景本身模拟
  "传输中途断连再恢复"，依赖真实的 loopback HTTP 服务器 + 时序（并非
  同步桩），ASan 的指令膨胀叠加 4 个测试二进制同时抢 8 个核，足以让这类
  时序假设偶尔踩空——是资源竞争导致的假红风险，不是新的正确性缺陷（同
  一份代码、同一份素材，`-j 1` 或单独跑都稳定绿）。未继续深挖，也不建议
  在这里加重试兜底：CI/日常验证如果对 ASan/TSan 套用 `-j 1` 跑
  （量级上完全可接受，ASan 全量 16 个目标串行也就一分钟左右），这条
  假红本来就不会出现；真要在 `-j 4` 下也做到 100% 稳，需要给这条场景本身
  的时序假设做确定性构造（比如钉死断连时机而不是靠 wall clock 竞速），
  超出这次修复轮的范围，留给下一轮。

  **补记（2026-09-21，M6a 合并时）：普通构建在重负载下也会红。** M6a 快进合并后
  第一次全量 `ctest -j4` 红了一条（当时日志被下一次运行覆盖、未能留存）；随后安静条件下
  全量 `-j4` 连跑 19 次全绿。人为加压复现：6 个 `yes > /dev/null` 满载 CPU + `ctest -j8`
  + `--repeat until-fail:20` 跑 6 个嫌疑目标，**第 7 轮 `probe_e2e` 红，正是本场景**：
  `test_probe_e2e.cpp:691`（`av_read_frame: Input/output error`）、`:692`
  （`failed_tasks` 1≠0）、`:693`（`downloaded_bytes` 14263298≠22832904）、`:757`（`covered`）。
  其余 5 个目标（含 M6a 新增的 `rate_limiter`，以及带限速端到端用例的 `preloader`）20 轮全绿。
  **与 M6a 无关**：本场景限速为 0，`RateLimiter::admit` 在第一次原子读即放行、`debit` 直接
  返回、分片上限为 `INT64_MAX`，M6a 在这条路径上不改变任何行为。结论不变：资源竞争下的时序
  假设踩空，确定性构造（钉死断连时机）仍是留给后续的修法。

## 检出为概率性，非确定性构造 🟡

- **场景 E「收敛到单连接」的检测力**（第八波登记，**第九波重新评估，
  见本条末尾的「第九波更新」**）：本轮把
  `LoopbackConfig::body_chunk_delay_ms` 接上（8MB/块、200ms/块），
  证明了"fallback 连接开始供体那一刻，抢跑连接确实还在途"这件事——30+90
  次干净树连跑，`full_concurrent_at_start`（拉完整个文件那条记录的
  `concurrent_at_start`）稳定为 4，无一例外。这部分**已经是确定性构造**，
  不再是概率性的了。
  但拿最初那条反向自检（`Scheduler::max_tasks_locked()` 忽略
  `range_supported_`）重新测，检出率不升反降：本轮 30 次注入只有
  1 次变红（约 3%），比之前记录的 30% 更低。查过原因：这个特定注入的
  实际影响面被 `occupied_locked()` 的地盘占用机制天然收窄了——一旦
  `enter_no_range_fallback()` 排出第一个 `wanted=[0,eof)` 的新任务，
  `holes_in(tgt)` 立刻变空，`schedule()` 的建槽循环不管 `cap` 是多少都会
  在建完这一个槽之后 break，所以"忽略 range_supported_"这个改动能实际
  多建出的槽位本就很有限、很偶然。另外试过更狠的注入
  （去掉 `enter_no_range_fallback()` 里的 `cancel_all_tasks()`）：
  30 次里 0 次检出——因为旧连接本来就靠 `DLTask::sink_on_response()` 里
  `task.allow_no_range_fallback=false`（注意这是 `SchedulerConfig::task`
  这一层的开关，不是 `SchedulerConfig::allow_no_range_fallback` 那层）
  触发的"判定丢弃即中止"（known-gaps #15 第 2 半，`src/dl/dl_task.cpp`
  `sink_on_response` 里的 `abort_h`）**在任务级别自行收敛**，根本用不上
  `Scheduler::cancel_all_tasks()`——去掉它不影响结果。
  换了一个更贴近真实回归形状的反向自检——直接还原 known-gaps #15 第 2 半
  修复前的状态（`dl_task.cpp` 判定丢弃但不中止连接，让 body 在网线上跑
  完）——20 次注入 20 次变红（100%），流量倍数落在 3.61x~4.00x，与该
  提交（`1cfb015`）自己记录的修复前实测（4.01~4.08x）同一量级；本轮新加
  的 `full_count == 1`（是否恰好一条连接拉完整个文件）与既有的流量上界
  断言都会命中。
  **结论（如实写，不夸大也不缩小）**：`body_chunk_delay_ms` 确定性化的是
  "重叠"这件事本身，不是"最初那条反向自检的检出率"——两者是不同的
  命题，第一个已经做成确定性构造，第二个没有；known-gaps #15 第 2 半
  修复后，"收敛到单连接"的责任已经从 Scheduler 层的并发上限/取消广播
  转移到了 DLTask 自己的"判定丢弃即中止"，最初那条反向自检戳的已经不是
  这条路径最敏感的点了，需要专门对着 `dl_task.cpp` 的这条自行中止逻辑
  设计新的反向自检才能验证"是否真的必然收敛到 1 条"这件事（本轮验证过
  这条新自检确实有 100% 检出力，见上，但只在临时注入下验证，没有做成
  仓库里长期保留的反向自检用例）。登记为债务：如果要进一步巩固，
  下一轮可以考虑给 `dl_task.cpp` 补一条专门覆盖"判定丢弃后是否真的中止
  连接"的单元级回归（不依赖端到端 loopback），而不是继续在场景 E 这个
  端到端层面加反向自检。

  **第九波更新（`1e6c1c1`，known-gaps #15 第 1 半修复后重新评估）**：
  上面记的"约 3% 检出率"是**在缺陷还在的树上**、拿"注释掉
  `max_tasks_locked()` 里那句 `if (!range_supported_) return 1;`"这个注入
  测出来的。第 1 半修好之后这个注入已经不存在对应的代码形状了（那句
  变成了 `if (range_state_ != RangeState::Supported) return 1;`，而且并发
  放开的时机整个前移到了 `on_response`），**这条债务的前提没了，按"前提
  消失"关闭，不是按"修好了"关闭**——两者要分清。

  替代它的是两条新证据，检出力都是实测出来的、不是估的：
  - **确定性**：`tests/test_scheduler.cpp` 的
    `no_range_source_stays_single_connection`（同步桩，断
    `active_task_count()` 峰值恒为 1）与
    `range_source_opens_concurrency_after_first_206`（正向对照，断 206
    一到并发就放开）。同步泵下无墙钟、无竞态，100% 确定。
  - **概率性（如实标注）**：场景 E 的 `peak_concurrent <= 2` 与
    `requests <= 3`。16 路并行 n=400 的**修复前**分布是
    peak 1/2/3/4 = 80/52/256/12、requests 3/4/5 = 100/81/219，
    两条断言的联合检出力 **75%**（400 次里 300 次会红）。
    不是 100%，如实记着——端到端这一层永远有"这一趟恰好没抢跑"的可能，
    它是补充证据，不是主证据。

## 平台探测

- `check-abi.sh` 的平台探测用 `CMAKE_SYSTEM_PROCESSOR`（宿主架构），
  本机上够用；**要上 CI 交叉编译时必修**，否则探测结果会与实际编译
  目标脱节。

- **`tools/build-ffmpeg.sh:58` 的 `CATALYST_MIN="13.1"` 与实际 Mach-O
  `minos` 不一致**（M2b Task 1，commit `e6d2f71`）：`maccatalyst-arm64`
  这个 slice 的 triple 请求的是 `arm64-apple-ios13.1-macabi`，但本机
  Xcode/SDK（26.5）的链接器把 Mac Catalyst 的最低部署版本钳制到了
  `14.0`——`xcrun vtool -show-build` 看到的 `LC_BUILD_VERSION minos` 是
  `14.0`，而脚本自己写进 `FFmpeg.framework/Versions/A/Resources/Info.plist`
  的 `LSMinimumSystemVersion` 仍然是 `13.1`，两处元数据不一致。

  钳制规律（裸 `clang -target arm64-apple-ios<ver>-macabi` 实测，不经过
  FFmpeg configure，排除是 configure 引入的）：`13.1`、`14.0` 都被钳到
  `14.0`；`14.5`、`15.0`、`16.0` 忠实反映请求值。只有用
  `-Wl,-platform_version,mac-catalyst,13.1,<sdk>` 强制覆写才能让 `minos`
  真变成 `13.1`，但链接器会警告"object file was built for newer
  'macCatalyst' version (14.0) than being linked (13.1)"——库里大概率有
  依赖 14.0 假设的代码路径，强行覆写等于对外撒谎，没有这么做。

  **现在为什么不炸**：xcframework 里每个 slice 的 `Info.plist`
  （`LSMinimumSystemVersion`/`MinimumOSVersion`）只是 Xcode 打包时用来
  挑选 slice 的元数据，dyld 实际加载期校验的是 Mach-O 自己的
  `LC_BUILD_VERSION`（这里已经是真实的 `14.0`），两者不一致时后者说了算，
  不会产生"声称支持却加载失败"的运行期问题。

  **风险方向**：如果未来 Xcode 工具链改成按 xcframework 的 `Info.plist`
  预先判定 slice 兼容性（而不是等 dyld 在加载期查 Mach-O），或者有别的
  下游工具读 `Info.plist` 的 `13.1` 当真，`13.1`~`13.9` 这个区间会出现
  "打包时说支持、加载时其实要 14.0"的不一致。`CATALYST_MIN` 的值本身
  按 M2b Task 1 brief 要求原样保留为 `13.1`，未改——这条只记录风险，不是
  待办修复项。

- **`pack_xcframework()` 的 slice 路径硬编码、且先清空再打包**
  （`tools/build-ffmpeg.sh`，`ios_p`/`sim_p`/`mac_p`/`cat_p` 四个变量，
  M2b Task 1 加了 `cat_p`，沿用既有写法）：这四个 framework 路径都是
  硬编码的固定路径，不是从上面的 `SLICES` 表动态派生的——`SLICES` 表只
  控制"要不要编译/组装某个 slice"，`pack_xcframework()` 打包哪些 slice
  完全是另一套独立的硬编码列表，两者靠人工保持同步。而且
  `pack_xcframework()` 一开始就 `rm -rf "$out"` 清空旧的
  `FFmpeg.xcframework`，然后才执行 `xcodebuild -create-xcframework`——
  校验（框架路径是否存在）晚于删除。

  **后果**：如果有人改了 `SLICES` 表（比如临时注释掉一行，或者某个
  slice 的中间构建产物被手动删掉/从未构建过），只要对应的 framework
  目录在打包那一刻不存在，`xcodebuild -create-xcframework` 会报错
  "the path does not point to a valid framework"，整个脚本以非零 exit
  失败——但这时旧的 `FFmpeg.xcframework` 已经被删了，不会优雅降级成
  "打出一个少一个 slice 的包"，而是**整个输出目录消失**，直到重新跑一遍
  完整脚本才能恢复。M2b Task 1 的反向自检（把 `maccatalyst-arm64` 从
  `SLICES` 表注释掉、同时挪走它已构建的中间产物、重跑
  `bash tools/build-ffmpeg.sh`）复现了这个行为：exit 70，
  `build-ffmpeg/out/FFmpeg.xcframework` 整个不存在。

  **这是既有设计，不是 M2b Task 1 引入的**——加 Catalyst slice 时只是
  照抄了 `mac_p`/`ios_p`/`sim_p` 的写法（brief 明确要求这样做），这个
  反向自检只是把一个已经存在三个 slice 时期就有的潜在盲区暴露出来。
  「硬失败并清空输出」相对「悄悄打出一个缺 slice 的包」是更安全的方向，
  所以不算阻塞性缺陷，但值得修：

  **建议修法**：（a）在临时目录里构建成功后再原子替换 `$OUT_DIR` 下的
  `FFmpeg.xcframework`（例如先输出到 `$OUT_DIR/.tmp`，成功后
  `mv` 覆盖），把"删旧"推迟到"新的已经确认打包成功"之后；或者
  （b）把四个 slice 路径改成从 `SLICES` 表动态派生，消除两份独立列表
  必须手动保持同步这件事本身。

---

## TSan 工具链盲区与 AudioRing 内存序守护缺口（M2b Task 2）

- **本仓库这套 TSan runtime 对大块 `memcpy`/`memmove` 的数据竞争检出率
  明显偏低**（`AudioRing` 反向自检 2 复审，审查独立复现）：同一台机器、
  同一套工具链（AppleClang 21 / macOS arm64 TSan runtime），对同一处
  真实的、按 C++ 内存模型可确定判定的数据竞争（`write()` 把
  `write_pos_.store` 从 `memory_order_release` 降级成 `relaxed`，
  破坏跟 `read()` 里 `acquire` 读的同步关系），走 `memcpy` 路径与走逐字节
  循环路径的检出率天差地别：

  ```
  memcpy 512B → 0/30      memcpy 4B → 0/30      memcpy 1B → 0/30
  逐字节循环 512B → 30/30
  ```

  另一处独立的竞争（`write()` 里把 `read_pos_` 的 `acquire` 读降成
  `relaxed`）交叉验证过同一现象：memcpy 0/15、逐字节循环 15/15——确认
  这是**通用盲区**，不是某一处竞争的巧合。（早期探索性记录里"1/2/4/8
  字节的 memcpy 能报"的说法**没有复现**：审查在 4B、1B 下各跑 30 次都是
  0 命中，那条细节高度依赖具体测试参数，不是一个干净的检出阈值，本条
  已更正，不再采信那个说法。）

  **操作性结论**：本仓库这套工具链（AppleClang 21 / macOS arm64 TSan
  runtime）对大块 `memcpy`/`memmove` 搬运的数据竞争检出率明显偏低。
  涉及 memcpy 的并发自检**必须配合逐字节循环的同构最小复现**做交叉
  验证，**不能仅凭 TSan 沉默判定无竞争**。这条对 Task 8 的
  `AudioUnitSink` render callback（同样是 memcpy 搬运音频字节）直接
  适用，也适用于今后任何走 memcpy 的并发代码。登记为工具链使用规范，
  不是代码缺陷，不阻塞合并。

- **`AudioRing` 的内存序目前没有任何自动化守护，覆盖度是假象**
  （审查变异测试发现）：给 `write()` 里 `read_pos_` 的读从 `acquire`
  降级成 `relaxed`——这是真实数据竞争（同构逐字节循环版 TSan 15/15
  命中）——但常规 `ctest` 10/10 全绿，TSan 在 memcpy 路径下也是 0/15。
  也就是说：**功能测试空白 + 工具链盲区双重叠加**，表面看这处代码"有
  并发测试覆盖"（`ring_spsc_concurrent_transfers_every_byte_in_order`
  确实起两条线程），但那条测试对"内存序配对是否正确"这件事实际上没有
  任何区分力——不管配对对不对，功能断言（"每个字节都到达且顺序不变"）
  在这台机器上都会通过，TSan 也不会报。

  如实记着：这条**今天没有守护**。将来任何改动 `write()`/`read()`
  内存序的人，验证手段是「做一个逐字节循环的孪生版本跑 TSan」，而不是
  「跑一遍 ctest 看绿」或「跑一遍 TSan 看沉默」——这两种都会给出错误的
  安全感。

  这条分支上"守卫没有守卫"已经栽过多次（`test_matrix_sentinel` 存在的
  理由就是"验证矩阵子集缺失"这类问题），这次是在合并前提前标出来，不是
  事后补救。登记为债务，不阻塞合并；如果以后要补一条真正有区分力的
  自动化守护，方向是仓库里长期保留一个逐字节循环的 `AudioRing` 孪生
  测试驱动（跟生产代码本身无关，只在 TSan 构建下跑），而不是指望现有的
  `test_audio_ring` 在任何构建下都能自证内存序正确。

---

## `SystemClock` 状态机组合覆盖度（M2b Task 3）

修复轮 1/5 审查用独立探针逐一验证了 `SystemClock` 六种操作交错组合，
逻辑全部正确，但**除新补的三条用例覆盖到的部分外，没有任何回归保护**：

| 组合 | 正确性（审查实测） | 有用例守着？ |
|---|---|---|
| `pause()→set_speed()→resume()` | ✅ | ❌ |
| `pause()→set_base()→resume()` | ✅ | ❌ |
| `set_speed()→pause()→resume()` | ✅ | ❌ |
| 连续两次 `pause()` | ✅ | ❌ |
| 连续两次 `resume()` | ✅ 修复轮 1 补上（见下） | ✅ `system_clock_resume_while_not_paused_is_noop` |
| `set_speed(0.5)` 后 `set_speed(2.0)` | ✅ | ❌ |

同一轮修复还补了「运行一段时间后再 `pause()`」
（`system_clock_pause_after_running_freezes_elapsed_value`，之前
`system_clock_freezes_while_paused` 在构造后零延迟就 `pause()`，
`base_us_` 本来就是 0，结算是空操作，巧合掩盖了"忘记结算"这类缺陷）
和「`AudioClock` 空指针 sink」（`audio_clock_nullptr_sink_reports_nopts`，
之前没有任何用例覆盖 `time_source.cpp:44` 的 `sink_ == nullptr` 防护，
反向自检显示去掉它会直接段错误，进程 exit 139）。这三条已配反向自检
证实真的会红（分别见对应 commit）。

**仍是空白**（上表前四行 + 最后一行）：这五种交错目前只存在于审查那次
临时探针里，探针一删就什么都不剩——不是"可能有 bug"，是"审查已经拿
实测数字验证过是对的，但没有自动化守护"：

- `pause()→set_speed()→resume()`：暂停期间换速度不影响冻结值，恢复后
  按新速度继续走。
- `pause()→set_base()→resume()`：暂停期间 `set_base()` 直接生效（覆盖
  冻结值），恢复后从新基准继续走。
- `set_speed()→pause()→resume()`：换速度后暂停，冻结值按新速度结算；
  恢复后仍按新速度继续走（`resume()` 不重置 `speed_`）。
- 连续两次 `pause()`：第二次是空操作（`pause()` 里 `if (paused_) return;`
  的早退，见下方"不用改的"）。
- `set_speed(0.5)` 后 `set_speed(2.0)`：两次结算都正确叠加，没有中间
  状态被跳过或重复计入。

**Task 5 的 `TrackPlayer` 会继承这个形状**——它有 play/pause/set_speed/
seek 四个操作，状态空间比 `SystemClock` 更大（还要接 `AudioClock`/
`SystemClock` 切换）。落地时要对照这张表逐项补齐组合覆盖，不要把
"逻辑对但没有回归保护"这个空白原样继承下去；尤其注意"零延迟就触发
状态切换"这种用例会像 `system_clock_freezes_while_paused` 一样巧合
掩盖忘记结算类的缺陷，写组合测试时要故意留出有意义的时间间隔。

登记为债务，不阻塞合并。

---

## `TrackPlayer` 的 `sample_fmt` 来源缺口（M2b Task 5，推给 Task 8）

`IAudioSink::open(sample_rate, channels, sample_fmt)` 需要 `sample_fmt`
（取 `AVSampleFormat`），但 `TrackInfo`（`src/media/demuxer.h`）只有
`sample_rate`/`channels`，没有这个字段；`Pipeline` 的公开接口也没有
暴露 codecpar 的口子。`TrackPlayer::create()`（`track_player.cpp`）当前
传 `AV_SAMPLE_FMT_NONE` 占位——`FakeAudioSink::open()` 忽略这个参数，
测试不受影响，但这是一个真实的接口空白，`AudioUnitSink` 接线时必须
解决。

**task-5 审查修复轮 1/5 给了明确的入场条件，Task 8 落地时照这个形状
做，不要按"TrackPlayer 晚点调 open()"（惰性 open）做**：

- `clock_kind()` 仍然在 `create()` 这里定死，判据改成**意图**（有
  音频轨 && `sink_ != nullptr`），保持只能 `Audio → System` 这一种
  降级方向。
- 惰性 open（挪到收到第一帧音频之后再调 `open()`）会打破一条隐式
  不变量：`clock_kind_ == Audio ⇒ sink 已经 open`。`played_us()`
  按接口契约在未 open 时返回 `AV_NOPTS_VALUE`（`INT64_MIN`），届时
  `step()` 里 `diff = pts - clock_->now_us()` 就是
  `pts - INT64_MIN`——有符号整数溢出，UB。
- 真正的修法是让 sink 自己处理"尚未 open"这件事：`played_us()` 在
  未 open 时返回**上一次 `flush()` 的基准**，而不是 `AV_NOPTS_VALUE`
  （`AV_NOPTS_VALUE` 应该只留给"已经 `failed()`"这一种情况）。
- 格式来源改用 `Frame`（`Frame::sample_fmt()` 等，收到第一帧后才能
  读到），**不要**给 `TrackInfo` 加 `sample_fmt` 字段——`demuxer.h`
  是 M2a 的文件，`TrackInfo` 目前只承载"打开解码器需要什么"，往里加
  一个只有音频输出路径用得上的字段会让这个结构体的职责变得模糊。

登记为债务，不阻塞合并。

---

## `TrackPlayer` 修复轮 1/5 之后仍未修的三条 Minor/组合空白（M2b Task 5）

审查（opus）第一轮修复确认的三条，本轮明确不修，如实登记：

- **m1 · `just_sought_` 分支的二次 `flush()` 可能抹掉 seek 后已写入的
  音频**（`track_player.cpp` 里 `just_sought_` 分支）：同一个 `step()`
  调用里音频优先于视频，若某次 `step()` 先 `Queued` 了一帧新位置的
  音频、后续某次 `step()` 才轮到 `just_sought_` 消费那帧视频，这里的
  `sink_->flush(pts)` 会把刚写进去的音频当没发生过一样清掉。审查在
  本轮的合成素材上没能复现（素材太短、时序窗口没对上）。是否要在
  spec 里单独定义"音频、视频各自何时消费 seek 效应"这件事的顺序——
  **归属订正（M2b 终审 m10，2026-09-11）**：这里原本写的是"属于 Task 8
  范围"。**Task 8 已经交付，这条没有做**，spec 第 6 节至今没有为
  "seek 效应的音视频消费顺序"定义任何东西。继续挂着 Task 8 的归属会让人
  以为它已经在某个任务的射程里。真实状态是**已知且接受，留给 M3**：
  修它要先在 spec 里把顺序定下来（是"视频侧不再二次 flush"，还是
  "seek 之后音频先不写、等视频消费完 just_sought_"），属于同步语义的
  设计决定，不是一处实现修补。
  **【已修 · M3c 终审 C1，2026-09-15】** M3c 线程模式让它可复现：Seek 缓冲
  很快满足，音频轨号在前的 mp4 seek 后音频先写入 ~209ms、随后首帧 flush +
  复位到关键帧 pts，此后音频持续领先 +183ms 直到下一次 seek（终审实测）。
  采用第二种顺序：seek 之后到首帧复位（`seek_rebased_` 置真，在第一次交给渲染器
  之前）之前，`step()` 音频分支不写 sink、帧留在 `pending_audio_`，该步报
  Waiting；视频轨 `track_failed()`/`track_drained()` 且无待处理帧时放弃
  `just_sought_`，不永久扣住。回归：`test_track_player.cpp`
  `seek_audio_first_file_{threaded,sync}_{buffering,no_buffering}_*`（修复前
  线程模式 216 帧音频先于首帧写入、同步无缓冲 1 帧）、
  `seek_with_failed_video_track_does_not_hold_audio_forever_{sync,threaded}`。
  spec §8 已记。
- **`pause() → set_speed() → play()`**：暂停期间换速度是否正确影响
  恢复后的行为，没有用例。`set_speed()` 里 `IAudioSink::set_speed()`
  这一步已经补上（裁定 B），但重采样器本身（真正改变写入样本数与
  媒体时长的比例关系）仍然不存在——等它落地后一起补这条组合更有意义。
- **连续两次 `pause()` / 连续两次 `play()`**（M10/M12，与 Task 3
  `SystemClock` 那节同族）：`paused_` 的早退逻辑（`if (paused_)
  return;` / `if (!paused_) return;`）没有单独配用例验证这个早退本身。
- **析构顺序零强制**（M11）：`track_player.h` 顶部那段"`clock_` 必须
  最后声明、最先析构"的注释立着，但没有任何自动化手段强制这个顺序不
  被后人不小心调换——跟 `Pipeline` 对 `Demuxer` 的同款处理是同一个
  遗留状况。

登记为债务，不阻塞合并。

## `test_sync_e2e.cpp` 审查修复轮的两条 Minor（M2b Task 7）

审查（opus）第二轮（变异体复审）确认的两条，本轮明确不修，如实登记：

- **m10 · 场景 H 降级后的墙钟循环（约 1100 万次 `step()`）里放
  `REQUIRE`**：`h_audio_sink_failure_degrades_and_keeps_playing` 降级后
  用真实墙钟循环驱动 `step()`（避免零延迟场景掩盖"忘记结算"类缺陷，
  见该场景注释），循环体内每次都 `REQUIRE(oo.kind !=
  PlayOutcome::Kind::Error)`——`REQUIRE` 失败时走 `tiny_test::fail()`
  只打一行就 `return`，不是每次迭代都打印，所以正常路径下不会刷屏；
  但如果哪天这条循环真的在**每次**迭代都失败（比如判据写反导致恒
  失败），第一次失败已经 `return` 退出了当前 `TEST_CASE`，也不会刷屏。
  这条 Minor 本身缺乏一个能复现"确实会刷屏"的场景，登记为观察项，不
  是要修的缺陷。
- **m11 · `analyze()` 里 `last_pts` 初值 `INT64_MIN` 与 `AV_NOPTS_VALUE`
  数值重合是巧合，不是设计**：两者恰好都是 `INT64_MIN`，`last_pts` 的
  初值选它只是为了让"第一帧不会被误判成重复/乱序"这件事成立（第一次
  比较时不可能等于任何真实 pts），跟下面对 `s.at_us ==
  AV_NOPTS_VALUE` 的显式哨兵检查（m8 的修复）不是同一件事、不能互相
  替代。`analyze()` 函数内部已经补了一行注释区分这两者（见
  `tests/test_sync_e2e.cpp`），不改代码，登记为"容易被后人合并成一件
  事"的可读性债务。

登记为债务，不阻塞合并。

---

## `track_player.h`/`track_player.cpp` 的 step() 步骤编号不一致（M2b Task 7.5，预存在，本轮被放大）

审查（opus）修复轮 1/5 的 m2：`track_player.h` 顶部把 `step()` 的调度
顺序编成 0~5 六步（0 排空不受管轨、1 暂停、2 音频优先、3 视频判定、
4 驱动 Pipeline、5 非暂停时的优先级判定），但 `src/media/track_player.cpp`
里对应的行内注释编号是 0（排空不受管轨）、[暂停分支不编号]、1（音频
优先）、2（视频判定）、3（驱动 Pipeline，含优先级判定）——两边"音频
优先"分别叫第 2 步/第 1 步，"视频判定"分别叫第 3 步/第 2 步，错开一位。

这个错位从 Task 5 定稿时就存在（头文件把"暂停"单独编了号，.cpp 没有），
不是本轮引入的；但 Task 7.5 往头文件的"第 5 步"（原来的"都不行 →
Blocked/Eof/Error"那一句）和 .cpp 对应位置各自新增了大段的优先级判定
说明，两边的内容都变复杂了，错位的编号也就更容易在未来被人当真、
按编号交叉核对时对错行。

现在不改——两边各自的编号在各自文件内部是自洽的，只是互相对不上，
不是内容错误；真要修需要通盘决定"以哪一份编号为准"，牵动两个文件里
所有编号引用（包括本轮新加的"第 3 步"/"第 5 步"这类交叉引用），风险
与收益不成比例，留给下次真正改动这段调度顺序的人顺手捎带。

---

## `AudioUnitSink` 审查修复轮 1/5 登记的两条 Minor（M2b Task 8）

审查（opus）修复轮 1/5 判定不阻塞合并、登记即可的两条（m8/m10/m11 已在
同一轮实际修复，不在这里——见 `docs/known-gaps.md` #21"落地"一节与
`src/platform/apple/audio_unit_sink.mm` 相应注释）：

- **m9 · `flush()` 不排空 `SwrContext`**：`speed_ != 1.0` 时
  `swresample` 内部会缓冲一段样本（重采样滤波器的固有延迟，量级约
  16~32 个样本，@48kHz 换算 <1ms），`flush()` 目前只 `ring_->reset()`
  丢弃环里已转换但未消费的内容，不调 `swr_convert(swr_, nullptr, 0,
  nullptr, 0)`／`swr_close()` 把 `SwrContext` 自己的内部状态一并冲掉。
  实际影响：seek/变速后紧跟着的下一次 `write()` 理论上可能带着一点点
  "跨 flush 边界"的历史样本残留，量级 <1ms，在 `kPresentWindowUs=
  40000`（40ms）的同步判定窗口下可以忽略。登记为债务，不在本轮修——
  真要修需要在 `ensure_swr_for()` 之外再给 `flush()` 加一条"冲刷但不
  重建"的路径，跟"任一参数变化就整个重建"这条现有逻辑不是一回事，
  值得单独设计。
- **m12 · `syp_platform_apple` `PUBLIC` 链 `syp_media`，把动态
  FFmpeg.framework 拖给了所有链它的下游**（含只需要 `apple_http_backend.mm`
  的调用方）。这是 Task 8 引入的：`audio_unit_sink.mm` 需要
  `AudioRing`/`IAudioSink`/`swresample`，最省事的接法是让整个
  `syp_platform_apple` 目标 `PUBLIC` 链 `syp_media`，但这意味着以后任何
  只想要 HTTP 后端、不想要音频/FFmpeg 依赖的下游也躲不开这条链接边。
  建议拆成 `syp_platform_apple_http`／`syp_platform_apple_audio` 两个
  子目标（或者反过来，`audio_unit_sink.mm` 单独成一个目标），本轮不做
  ——CMakeLists.txt 目前只有这一个下游消费者（`syp_probe`/demo 壳尚未
  接入音频），拆分的收益要等第二个下游出现、且明确不需要音频依赖时才
  能验证值不值得。

## `AudioUnitSink` 审查修复轮 2/5：`swr_precheck_ordering_same_frame_retry_exposes_phantom_consumption` 耦合 FFmpeg 内部行为（M2b Task 8）

`tests/test_audio_unit_sink.cpp` 的这条用例（覆盖 `write()` 里
"`swr_get_out_samples()` 预检必须在 `swr_convert()` 之前"这条顺序，即
known-gaps.md #21 的第三层折算）判据依赖 `libswresample` 内部滤波器
**冷启动预热瞬态**的具体样本数量级——这不是 FFmpeg 公开文档承诺的
契约，纯粹是当前 vendored 版本（钉死 8.1.2）的实现细节。用两个裸
`libswresample` 探针实测过这个瞬态的形状（同一份数据反复喂给热身程度
不同的 `SwrContext`，比较真正转换与丢弃重试的输出量）：

```
warm_calls=  0   ref=3184  attempt1(discarded)=3184  retry=3200  delta=16
warm_calls= 10   ref=3190  attempt1(discarded)=3190  retry=3200  delta=10
warm_calls=100   ref=3200  attempt1(discarded)=3200  retry=3200  delta=0
```

**升级 FFmpeg 版本时，这条用例需要重新验证**：如果新版本的
`swr_convert` 冷启动行为变了（比如预热瞬态消失、或者量级大幅变化），
`k_ref - k_main` 这个 delta 可能塌成 0（正确代码和"假装"有缺陷代码在
新版本下表现一致）——**这种情况下用例会一直绿，但不再具备检测力**，
不是产品代码坏了，是判据依赖的第三方内部行为变了。FFmpeg 升级时的
检查清单应该加一条：手动把 `write()` 里的背压预检挪到 `swr_convert()`
之后（临时注入），确认这条用例真的会变红（复现本条记录的
`delta=24`）；如果变绿了，说明判据失效，需要重新设计（比如換一个不
依赖冷启动瞬态、而是直接检查 `swr_get_delay()`/内部缓冲状态的判据，
如果 FFmpeg 那时候提供了合适的公开 API）。

---

## `MetalRenderer` 审查修复轮 1/5 登记不修的四条（M2b Task 9）

审查（opus）修复轮 1/5 判定不阻塞合并、登记即可的四条（Critical M11、
Important 1、Important 2、Minor M7/`object_level_ready_reflects_
construction_success` 已在同一轮实际修复，不在这里——见
`docs/known-gaps.md` #26、`task-9-report.md` 修复轮记录与
`src/platform/apple/metal_renderer.{h,mm}`/`tests/test_metal_renderer.cpp`
相应注释）：

- **`pack_plane_rows()` 不接收 `dst` 容量参数**：`public static` 接口
  只按 `row_bytes * rows` 往 `dst` 写，调用方（目前只有 `present()`
  内部，`y_scratch`/`u_scratch`/`v_scratch` 三个 `std::vector` 在
  `rebuild_textures()` 里按同一公式 `resize()` 过，两处公式必须始终
  保持一致）自己保证 `dst` 够大——这是"public static 接口"这类设计
  常见的陷阱：一旦以后有第二个调用点用错公式传了个偏小的 `dst`，会是
  一次静默堆缓冲区溢出，不会有任何返回值提示。当前只有一个调用点、
  且两处公式肉眼可核对，暂不为它加 `dst_capacity` 参数（会让签名从
  5 个参数变成 6 个，且这个仓库的 `pack_plane_rows` 单测已经覆盖了
  "该拒绝的输入"这一半，容量校验是不同维度的另一半）；真的出现第二个
  调用点时应该重新评估。
- **`gen-embedded-header.sh` 把绝对路径写进生成头的注释**：生成头顶部
  注释里带着调用方传入的输入文件绝对路径（比如
  `/Users/sylar/Documents/syplayer/src/platform/apple/yuv420p.metal`），
  这个路径在不同开发机/CI 环境下会不同，理论上会造成"同一份 `.metal`
  源码、两台机器生成的头文件 diff 不为空"这类噪音——但注释不进
  raw string 常量本身（不影响运行时字符串内容）、也不进最终二进制
  （生成头不是被 `#include` 进公开头，不会被发布），影响面仅限于
  "如果有人把生成产物意外提交进版本控制再跨机器比较 diff"这一种边缘
  场景，当前构建流程里生成头始终是构建期产物、不进 `git`，不修。
- **每次 `cmake` configure 无条件重写生成头**：顶层 `CMakeLists.txt`
  里生成 `syp_yuv420p_metal_source.h` 那段 `execute_process` 每次
  configure 都会重新执行、重新 `file(WRITE)`，不管 `yuv420p.metal` 内容
  有没有真的变化——文件 mtime 因此每次 configure 都会更新，即使内容
  字节级相同，Unix Makefiles 的依赖追踪会认为头"变了"，导致
  `metal_renderer.mm.o` 无谓地重新编译一次。代价是几秒钟的重编译时间，
  不是正确性问题（`CMAKE_CONFIGURE_DEPENDS` 已经保证了内容真的变化时
  会触发 reconfigure，这条债务只影响"没有实际内容变化时是否也会触发
  一次多余的重写+重编"）。修法是生成前先读旧内容比较，不同才写，工作
  量不大但不值得在这一轮附带做。
- **`syp_probe` 多链了一个用不上的 `Metal.framework`**：`MetalRenderer`
  挂在 `syp_platform_apple` 的 `SYP_HAVE_FFMPEG` 分支下，
  `target_link_libraries(syp_platform_apple PUBLIC "-framework Metal")`
  是 `PUBLIC`（不是 `PRIVATE`），任何链 `syp_platform_apple` 的下游都会
  连带链上 `Metal.framework`——`syp_probe`（`tools/CMakeLists.txt`）
  链的是 `syp_probe_core`/`syp_media`，不直接链 `syp_platform_apple`，
  所以实际不受影响；但如果以后有别的目标（比如 `test_apple_http_
  backend`，它已经链 `syp_platform_apple`）不需要 Metal，也会连带链上
  这个 framework。`Metal.framework` 是系统动态库、lazy-load、不占运行
  时初始化开销，链上它本身没有实际代价（跟 `AudioToolbox`/`CoreAudio`
  当初 `PUBLIC` 链给 `syp_platform_apple` 是同一个已接受的取舍，见上面
  `AudioUnitSink` 修复轮 1/5 的 m12 条目），不修；真要收紧就是 m12 提到
  的"拆分 `syp_platform_apple` 成多个子目标"这个更大的重构一并解决。

---

## 已解决

- ~~`test_probe_e2e` 在并发负载下概率性变红——原记根因"`SourceBridge::close()`
  与 backend 回调线程之间的数据竞争"~~ 🟢（2026-09-14 修复）：
  **原条目把两个独立问题当成了一个，重新排查后分开处理。**

  **① 用例变红的真正根因：`DLTask` 的重试预算按"总尝试数"计。** 带诊断的
  TSan 构建串行复现（10 轮 6 红）：失败轮次里 TSan **一条竞争报告都没有**，
  失败发生在传输中途（`failed_tasks=1`、`av_read_frame: Input/output error`），
  不在 `close()`。逐次尝试打点：失败的任务 9 次尝试挤在 ~13ms 内，每次
  `SYP_ERR_NETWORK`，交付字节是 0~97000 之间的随机一截（一轮 485 次
  `NSURLErrorDomain -1005` 里交付量分布：0 字节 152 次、满额 97000 仅 128 次）。
  服务端（改成优雅关闭 `shutdown(SHUT_WR)`+排空后对照）每次都如实发满
  `close_after_bytes`——**是 NSURLSession 在 -1005（连接丢失）时丢掉了已缓冲、
  未交付给 delegate 的字节**，负载越高 delegate 队列越慢、丢得越多。场景 F
  需要每片 ~3 次截断续传，`max_retries=8` 按总数计，随机的零进展尝试把它
  耗尽 → 任务失败 → Scheduler 连续失败达到上限 → 源致命。这不是测试夹具
  问题：真实弱网上"反复断流但一直在前进"的大分片同样会下着下着失败。
  **修复**：`DLTask::can_retry_locked()` 改看 `stalled_attempts_`（连续无进展
  尝试数，一次尝试推进过 `next_offset_` 即清零）；`attempt_count()` 仍是总数。
  回归 `retries_that_make_progress_do_not_consume_the_budget` /
  `stalled_attempts_after_progress_still_exhaust_the_budget`（修复前均红）。
  `syp_config.h` 的 `max_retries` 注释同步改成"连续无进展的重试次数"。
  代价如实记录：每次尝试至少推进 1 字节时重试次数只受区间长度约束。

  **② `close()` 的竞争是真的，但不是用例变红的原因。** 源码直接可读：
  `persist_chunk()` 锁内查 `closing_`、放锁、锁外 `file_->write_at()`；
  `close()` 在 `cv_.wait(in_public_==0)` 之后就 `file_.reset()`，早于真正等回调
  停下的 `dying.reset()`（`~Scheduler` 等每个 Slot 的 `in_cb` 归零）。M2b 终审
  时 TSan 报过 `CacheFile::close_fd` ↔ `CacheFile::write_at`。**修复**：
  `file_`/`index_` 的销毁挪到 `dying.reset()` 之后（重新持 `mu_`）。之间的
  窗口里所有回调入口（`on_data`/`on_total`/`on_validators`/`persist_chunk`）
  都先查 `closing_/closed_`，`fire_cached_ranges()` 只从 `open` 与
  `persist_chunk` 的 closing 检查之后调用，不会多出用户回调。
  **没有确定性回归**：写过一条 Sync 桩 + 旁路 pump 线程并发 `close()` 的压力
  用例，修复前在 TSan 下也不报——桩的 pump 与 cancel 串行化，打不开"查完
  放锁、尚未 write_at"那个窗口——没有辨别力，已删掉而不是留着充数。

  验证：修复后 TSan 构建串行跑 `probe_e2e`（结果见提交说明），普通构建
  `ctest -j4` 全绿。
- ~~`finish_with_cancel_emit_locked` 与 `emit_complete` 的重复记账~~ 🟢：
  **本轮核实**（第八波）：`src/platform/apple/apple_http_backend.mm` 里
  `emit_complete()`（714 行）与 `finish_with_cancel_emit_locked()`（743 行）
  确实都调用同一个 `Handle::deliver_complete_locked(lk, s, st, http)`
  （700 行）完成 `on_complete` 的实际投递与
  `in_callback_`/`callback_thread_`/`cv_.notify_all()` 记账，没有各自维护
  一份重复逻辑——提交 `d5d52a8`（"合并 emit_complete 与
  finish_with_cancel_emit_locked 的重复记账"）与上一轮自查报告属实，不是
  部分合并。抽出的 `deliver_complete_locked` 收 `unique_lock` 引用（内部
  unlock/lock），调用约定写死在函数上方注释里：调用方须持有 `emit_mu_` 与
  `lk(state_mu_)`，且已在同一临界区把 `completed_` 置好。原条目已从上面
  "Apple 后端死锁修复的配套项"一节移除（那里之前误留成"仍列在未解决区"，
  这条已经不属于该节剩余的未闭环项）。
- ~~场景 E「抢跑连接与 fallback 连接是否真的重叠」：从只能靠运气撞上
  改成机制上必然~~ 🟢（第八波）：`test_probe_e2e.cpp` 的
  `e_no_range_falls_back` 给 `LoopbackConfig` 接上
  `body_chunk_bytes=8MB`/`body_chunk_delay_ms=200ms`。依据是
  `LoopbackServer::sleep_interruptible()`（服务端两次 `write()` 之间的
  停顿）只看服务器级别的 `stop_` 信号，不轮询"这条连接是否已经被对端
  打断"——所以只要一条连接发出过第一块 chunk，它在服务端这边就至少
  "在途"（`RequestGuard` 未析构）`body_chunk_delay_ms` 那么久，不管客户端
  那边的 `cancel()` 落地得多快；这个机制与 known-gaps #15 第 2 半的
  修复（缩短了"判定丢弃后还能再赖活多久"）正交，不因为那次修复而失效。
  新增断言：找到真正拉完整个文件的那条记录（不依赖 `snapshot.back()`
  这种按 finish 顺序找的position——过程中连跑就撞见过服务端记账线程比
  客户端读到 EOF 慢半拍，把一条 `sent=0` 的记录错误地排到最后，见下面
  "结算窗口"那条），断言恰好一条（`full_count == 1`）且它的
  `concurrent_at_start >= 2`。实测：干净树连跑 120 次（40+80，补上结算
  窗口修复之后）零假红；`concurrent_at_start` 在全部连跑里稳定为 4，
  不是偶尔撞上。
  **结算窗口的连带修复**：`run_through_source()` 返回时客户端已经读到
  EOF，但服务端某条连接自己的线程可能还没跑到 `~RequestGuard()`（记账
  发生在那里）——转正之前的位置相关写法（`snapshot.back()`）连跑 40 次
  撞见过 1 次全体 `sent=0`；现在改成读到"至少一条记录 `bytes_sent==total`"
  为止的短暂重试（至多 50×20ms=1s），不在这道时序缝隙上 assert。这条
  race 与本轮任何 dl 层改动无关，是测试基础设施本身的既有缝隙，顺手堵上。
  反向自检：还原 known-gaps #15 第 2 半修复前的 `dl_task.cpp` 状态
  （判定丢弃但不中止连接）——20 次注入 20 次变红（100%），流量倍数
  3.61x~4.00x，与该提交自己记录的修复前实测同一量级；用最初那条历史
  反向自检（`Scheduler::max_tasks_locked()` 忽略 `range_supported_`）
  测出的检出率反而更低，原因与后续动作见上面"检出为概率性，非确定性
  构造"一节新增的条目——重叠这件事已经确定性构造，"收敛到单连接"这件事
  仍有检测力缺口，两者分开记账、不要混为一谈。
- ~~`gen-fixtures.sh` 的 `--seed`/`--out` 缺值时报 bash 的 unbound
  variable~~ 🟢：给 `--seed`/`--out` 分支各加了 `[ $# -lt 2 ]` 前置
  检查，缺值时打印跟"完全没给参数/给了未知 flag"一样的用法提示再
  `exit 2`，不再让 `set -u` 直接把内部错误甩到用户脸上。用
  `bash tools/gen-fixtures.sh --seed`（不给值）/`--out`（不给值）
  验证过确实走新的用法提示分支，不是 unbound variable 报错。
- ~~`gen-fixtures.sh` 第二遍 `-c copy` remux 少了 `-flags +bitexact`~~
  🟢：补上，跟第一遍编码那条命令保持对称，并在脚本里注释说明这个
  flag 在 remux 命令里本来就不生效（remux 不走编码器），补它纯粹是
  消除不对称，不改变任何产物字节。
- ~~`tools/syp_probe/packet_digest.cpp` 有一个未使用的 `<cstdio>`
  include~~ 🟢：删掉。
- ~~`hex32(const uint8_t v[32])` 的数组参数退化成指针，没有长度
  校验~~ 🟢：签名改成 `hex32(const uint8_t (&v)[32])`（数组引用），
  长度 32 由类型系统钉死，传短了/传裸指针过不了编译，不用再靠调用
  约定人肉保证。`packet_digest_internal.h`/`packet_digest.cpp` 同步改。
- ~~packet 摘要漏了 `pkt->duration`~~ 🟢：`PacketDigest` 加了
  `duration` 字段，`run_demux()` 里 `d.duration = pkt->duration`，
  `diff_report()` 的 packet 差异打印同步加上这一项（`operator==` 是
  `= default`，自动纳入比对）。反向自检：新增
  `test_packet_digest.cpp` 的 `packet_digest_carries_a_real_duration`，
  先证明真实素材上确有 `duration>0` 的 packet（不是恒为 0 的空实现），
  再把某个 packet 的 `duration` 改 +1，证明 `diff_report` 真的会报红
  且差异描述里带上 "duration" 字样。`pkt->side_data` 判断为有意不加，
  见"测试/工具卫生，零误导风险"一节的说明。
- ~~端到端损坏用例的失败路径会遗留约 29MB 的临时素材副本，不清理~~
  🟢：`test_packet_digest.cpp` 的
  `diff_report_catches_a_corrupted_payload_byte` 照抄
  `test_apple_http_backend.cpp` 的 `TempFileRemover` 模式，`copy_file`
  之后立刻用 RAII 兜底删除——`find_mdat_range`/`flip_byte_at` 任何一条
  `REQUIRE` 提前 `return` 都不会再留孤儿副本；正常路径仍然像原来一样
  在两次 `demux_file` 之后尽快手动删（RAII 只是兜底，不是替代原有
  时序）。
- ~~异常/崩溃路径上临时缓存目录不清理~~ 🟢：`scenarios.h`/`.cpp`
  新增 `TempCacheDir`（RAII 版 `make_temp_cache_dir`），
  `test_probe_e2e.cpp` 全部 8 处 `make_temp_cache_dir` 调用点换成
  `TempCacheDir cache_guard(...)`，析构时无条件 `remove_dir_recursive`，
  覆盖所有 `REQUIRE` 提前 `return` 与正常 C++ 栈展开路径。进程被信号
  杀（比如 ctest TIMEOUT 命中）时析构不会跑，这种情况仍不在覆盖范围
  内——下一次同 pid 复用时 `make_temp_cache_dir()` 自己的 `remove_all`
  兜底，代价很小，保持原样未处理。
- ~~看门狗触发时会连打两条 FAIL（`r.failure` 一条、`diff_report`
  一条）~~ 🟢：`test_probe_e2e.cpp` 加了 `CHECK_RUN_OK(run, expected)`
  宏，`r.failure` 非空时跳过 `diff_report` 那一步，只留一条 FAIL；
  写成宏（不是辅助函数）是为了让 `CHECK_EQ` 内部的 `__FILE__`/
  `__LINE__` 落在调用处，FAIL 日志仍能指到具体是哪个场景挂的。替换了
  全部 6 处原来"`CHECK_EQ(r.failure, ...)` 紧跟
  `CHECK_EQ(diff_report(...), ...)`"的重复写法。
- ~~`tests/CMakeLists.txt` 的 ctest TIMEOUT 余量尚未系统性复核~~ 🟢：
  逐个测试目标核对了 TIMEOUT 与"自己套件内部的软/硬看门狗阈值"或
  "实测最坏耗时"之间的余量（build/ 下 Debug 配置，`ctest -T Test`
  连跑取值，另见下方 10 次连跑记录）：

  | 目标 | 当前 TIMEOUT | 实测耗时 | 套件内部阈值 | 结论 |
  |---|---|---|---|---|
  | matrix_sentinel | 15s | ~0.00s | 无 | 余量充足，不改 |
  | hole_set | 15s | ~0.06s | 无（纯内存数据结构） | 余量充足，不改 |
  | cache_index | 15s | ~0.02s | 无（纯内存数据结构） | 余量充足，不改 |
  | dl_task | 30s | ~0.01s | 单条 `cv.wait_for` 上限 2s（`test_dl_task.cpp:1115`） | 30/2=15x，余量充足，不改 |
  | scheduler | 45s | ~0.76s | 单条 `wait_idle_ge` 默认上限 8s（`test_scheduler.cpp:235-237`）；`window_advance_reaps_task_reentrant_destroy_from_worker_thread` 现已自带硬看门狗 `kReentrantHardMs=20000ms`（软 4000ms），**其余异步用例仍无内部安全网**（见上面"其余异步 scheduler 用例没有看门狗"一条） | 45/20=2.25x（对新看门狗）、45/8=5.6x（对 `wait_idle_ge`），余量充足，不改 |
  | source_bridge | 60s | ~0.05s | 单条 `wait_pred` 默认上限 8s（`test_source_bridge.cpp:168-170`） | 60/8=7.5x，余量充足，不改 |
  | apple_http_backend | 60s | ~1.27s | 硬看门狗 `kSelfDestroyHardMs=30000ms`（软 `kSelfDestroySoftMs=4000ms`） | 60/30=2.0x，本轮之前的提交已从 30 调到 60（撞硬看门狗的问题已修），刚好卡在"余量 2 倍"这条线上，判定为已经处理过，不再改 |
  | avio_bridge | 60s | ~0.04s | 单条 `wait_known` 默认上限 5s（`test_avio_bridge.cpp:102-104`） | 60/5=12x，余量充足，不改 |
  | packet_digest | 180s | ~0.77s | 无（纯本地文件 demux，不涉及网络/线程等待） | 余量充足，不改 |
  | probe_e2e | 900s | ~1.9s（正常路径） | 单场景最大 `watchdog_ms=300000`（300s，场景 F `f_mid_transfer_disconnect_recovers`） | 900/300=3.0x，余量充足，不改。**注**：如果 9 个 TEST_CASE 同时都撞上各自的看门狗上限（不现实的复合最坏情形），累计可达约 1561s，超过 900s TIMEOUT——判定为不现实的复合场景，接受这个残余风险，不为它抬高 TIMEOUT |

  结论：除 `apple_http_backend`（本轮之前已处理）外，其余 9 个目标的
  TIMEOUT 相对各自套件内部阈值/实测最坏耗时都有远超 2 倍的余量，
  当前不需要调整任何 TIMEOUT 值。
- ~~Task 10 的 `err_capture` 注释精度~~ 🟢：`scenarios.cpp` 里
  `run_through_source()` 尾部那条注释原来说"close() 正常返回之后…
  读 err_capture 不会再有并发写入"——这句话对"要不要 delete"成立，
  对"要不要 load"不成立：两条 `load()` 不管 `closed` 是 true 还是
  false 都会执行，`closed==false`（close() 超时）的路径上后台线程可能
  仍在跑、仍可能并发 `store()`。改准：注释现在分开讲清楚 load 和
  delete 各自的前提，并说明并发 load/store 在 `std::atomic` 上没有
  安全隐患（只影响读到新值还是旧值，不是数据竞争）。

- ~~场景 F 的 truncated 断言：按每段实际请求长度直接核对~~ 🟡：
  提交 `4abc1ab`（`测试：test_probe_e2e 四条断言加固 + 反向自检（tech-debt #1~#4）`）
  把 `CHECK(srv.server_capped_count() > 0)` 换成逐条核对：每一条被服务端
  按 `close_after_bytes` 主动截断的响应，断点之后的第一个字节
  （`continuation_point`）必须被快照里某条请求的实际发出区间覆盖到——
  断点不会变成永远没人认领的洞。反向自检发现最初设想的"续传请求的
  start 恰好接在断点"这个精确匹配站不住（即使把 `max_concurrent_tasks`
  钉成 1 排除并发冗余也仍会复现）：任务耗尽 `max_retries` 后被回收，
  调度器用回收那一刻的新快照重新派发一个任务接手剩余的洞，新任务的
  起点不保证与旧任务 `DLTask::next_offset_` 精确对齐，可能有小段无损
  重叠——这正是 known-gaps #1 描述的冗余请求的另一种表现，改用"覆盖"
  而不是"精确接续"。用 `dl_task.cpp` 的临时注入（重试时 `range_start`
  故意多跳 1 字节）验证新断言会如实变红，注入已从 `src/dl/` 完整还原。
- ~~场景 C（`c_cold_cache_seeks`）的流量断言 N3：注入手段没设计对~~ 🟡：
  同一提交（`4abc1ab`）把 `CHECK(bytes_sent <= total * 2)` 换成
  `CHECK(bytes_sent <= r.metrics.cached_bytes * 105 / 100)`——绑 dl 层
  自己维护的 `cached_bytes`（与 `bytes_sent` 互相印证），不再绑 `total`
  （文件大小）。反向自检用签收闸门复审给出的正确注入（`occupied_locked()`
  只删 `cached_`/`received_` 两项、保留在途 slot 那段循环——之前三次
  注入没试过这个组合）：实测流量放大到约 600 倍（19GB / 31MB），断言
  应声变红，且比预想的"温和地多下几遍"猛得多——已如实记在测试文件的
  注释里，不是"1.5x~3x"这种温和量级。摘掉了"不作为验收证据"的标注。
- ~~场景 E 的并发断言：摘掉"空转"（不是"改成确定性构造"）~~ 🟡：
  同一提交（`4abc1ab`）把 `CHECK_EQ(last.concurrent_at_start, 1)` 换成
  断"组成"：请求条数上界（机制上限 `kMaxConcurrentTasks + 2` 加实测
  噪声余量）+ 最后一条请求记录必须把整个文件拉满。反向自检用
  `Scheduler::max_tasks_locked()` 忽略 `range_supported_` 的注入复现：
  30 次注入 12 次命中（其中 9 次是"没拉满全文件"这条新断言命中），
  不是 100% 确定性红，但检测力真实、可重复；调整余量避免对干净跑假红
  后又验证了 40 次连跑零误报。摘掉了"不作为验收证据"的标注。
  **订正（本轮）**：这条当初被记成完全"已解决"过头了——4abc1ab 解决的
  只是"空转"（断言原来断的是一个不影响结果的时刻），"概率性检出"
  （30% 检出率）没有被解决，也不应该被算作已解决。已把这条重新登记为
  未解决的技术债，见上面"检出为概率性，非确定性构造"一节。
- ~~场景 D 的 4.5MB 上界：换成恒等式~~ 🟢：
  同一提交（`4abc1ab`）把魔法数字 `kSecondDownloadCeiling = 4.5MB` 换成
  恒等式 `first.downloaded + second.downloaded == second.cached_bytes`
  （与场景 G 同思路），不再依赖 `min_segment_size`/`max_concurrent_tasks`
  的默认值。反向自检用 `source_bridge.cpp` 的临时注入（`open()` 时无视
  磁盘上已缓存区间，当空缓存处理）验证会如实变红，且连带验证了场景 G
  用的同一条恒等式也会同步变红。
- ~~哨兵测试缺失：矩阵子集缺失时 `ctest` 仍退出 0~~ 🔴：
  提交 `58bab75`（`测试：哨兵测试锁住验证矩阵子集缺失，堵上 ctest 无声退出 0`）
  新增 `tests/test_matrix_sentinel.cpp`，始终注册（不在任何
  `SYP_HAVE_FFMPEG`/`SYP_HAVE_FIXTURES`/`SYP_REQUIRE_FULL_MATRIX` 的
  `if()` 块内），四档判据：完全没装 FFmpeg → 通过但打印能力报告；
  有 xcframework 缺 ffmpeg CLI → 失败；有 macos-arm64 slice 缺
  ios-arm64 slice → 失败；`-DSYP_REQUIRE_FULL_MATRIX=ON`（CI 用）时
  缺任何一项都失败。三次反向自检（强制 `SYP_HAVE_FIXTURES` 假、
  改名 `ios-arm64` 目录、移走整个 xcframework）均确认哨兵在该红时
  真的红、该绿时真的绿，详见该提交与 `docs/toolchain.md`。
- ~~`check-deploy-target.sh` 与 `CMakeLists.txt` 探测不同的 FFmpeg
  slice~~ 🔴：与上一条同一次提交（`58bab75`）解决——`CMakeLists.txt`
  新增独立的 `SYP_HAVE_IOS_SLICE` 探测（路径与
  `check-deploy-target.sh` 探测的 `ios-arm64` slice 保持一致），
  哨兵测试的判据 3 覆盖"有 macos-arm64 slice 但缺 ios-arm64 slice"
  这一子集缺失，`check-deploy-target.sh` 静默跳过（⏭️、exit 0）的
  情况现在会被哨兵测试判红，不再无声消失。

- ~~`videoGravity` 只有 `.aspectFit` 一个 case~~ 🟢（M4 收尾登记 #5，
  2026-09-21 M6d 解决）：`SYPlayerVideoGravity` 补齐 `.aspectFill`/
  `.resize` 三态，`MetalRenderer` 的填充方式计算不再是原描述里的
  `aspect_fit_scale()`——那个函数已被 `src/media/video_geometry.h` 的
  平台无关纯函数 `blit_transform()` 取代（`metal_renderer.mm:447` 的
  旧调用点相应改写，原条目 `docs/tech-debt.md:1212` 附近点名的
  `aspect_fit_scale` 这句描述本身也随之过时，一并订正）。三种 gravity
  的分工与旋转是正交关系。

## 无缝变速需要 (样本数 → 媒体时长) 分段映射表（M2b Task 11，推给 M3）

`docs/known-gaps.md` #29 记的"变速点有一次可听断点"是有意接受的产品
取舍，但它背后的技术根因值得单独记一笔，免得 M3 真要做无缝变速时要
重新分析一遍。

当前 `TrackPlayer::set_speed()` 处理变速的方式是"先 `flush()` 清空
`AudioRing` 里已经转换好但还没被硬件消费的样本，再切换
`swresample` 的重采样比例"——本质是把"变速"当成一次隐式的
seek/断点来处理，不需要追踪"环里这批样本、以及 `SwrContext` 内部
还压着的那批样本，分别对应原始媒体时间轴上的哪一段"。这个简化是
`played_us()` 的算术能正确、且不需要在 flush 之外维护任何跨速率的
时间戳映射的前提。

**无缝变速（不炸邊界、不丢音频、不产生断点）需要的是反过来**：写入
`AudioRing` 的每一段样本，都要能在变速发生之后仍然被换算回"这批样本
播放到第几个字节时，对应原始媒体时间轴上的哪一个 `pts_us`"。也就是
一张 (样本数 → 媒体时长) 的分段映射表——每次 `set_speed()` 切换速率、
或者 `swresample` 因为参数变化重建时，新起一段映射，`played_us()`
按"当前消费到的样本落在哪一段、那一段的比例是多少"分段换算，而不是
像现在这样只维护一个全局的 `speed_` 标量 + 一次快照相减
（`compute_played_us()`，见 `docs/known-gaps.md` #21）。

**范围与工作量**：这不是给 `AudioUnitSink` 打个补丁能做完的——`flush()`
需要改成"冲刷但不重建"（`docs/tech-debt.md` 前面 m9 那条已经记过这一半：
`swr_convert(swr_, nullptr, 0, nullptr, 0)` 把 `SwrContext` 内部状态
冲刷干净，而不是整个丢弃环内容），`played_us()` 的算术需要从"一个
`base_us_` + 一次快照相减"升级成"分段映射表按比例查找"，`IAudioSink`
的接口契约本身可能也要变（`flush()`/`set_speed()` 的调用顺序、语义都要
重新定义）。这个量级的改动属于 M3（`docs/roadmap.md` M3 一节"完整同步
收尾"的范围），M2b 不做。

## M3a 收尾登记（逐任务审查与终审，2026-09-14）

以下为 M3a 各任务审查判定为 Minor、终审分诊为"可继续延后"的条目，以及终审修复轮挂起的三条。已在 known-gaps（#50~#52）或本分支内关闭的不重复列出。

- **Task 1**（审查登记）：config_value() missing-file diagnostic is a raw sed error, not "自检失败" (still fails hard)
- **Task 2**（审查登记）：test_decoder HEVC test frees non-video packets inline instead of early-continue idiom (style)
- **Task 2**（审查登记）：CMake fixture sentinel comment block is 4 stacked annotations — consider data-driven list
- **Task 3**（审查登记）：backend_ lifetime is doc-only contract (no runtime check)
- **Task 4**（审查登记）：create_common comment "该轨终止，不是整体终止" reads ambiguous next to new hardware early-return comment
- **Task 6**（审查登记）：supports() calls VTIsHardwareDecodeSupported each time (cheap)
- **Task 7**（审查登记）：CVPixelBufferLockBaseAddress CVReturn ignored in test helper
- **Task 7**（审查登记）：content-type mapping duplicated from test_hls_e2e.cpp
- **Task 7**（审查登记）：vt_decode TIMEOUT 1800 shared by normal build — a hang takes 30 min to flag
- **Task 8**（审查登记）：hardware frames still allocate unused yuv420p textures + scratch vectors (~24MB at 4K)
- **Task 8**（审查登记）：color-matrix log reports AVFrame range, not 420v/420f actually used
- **Task 8**（审查登记）：rejected frame after rebuild_textures can leave debug readback returning a never-written texture
- **Task 8**（审查登记）：tests don't cover 420v+JPEG-range mismatch, chroma coordinate bugs (constant colour), helper alloc failure checks
- **Task 8**（审查登记）：CVMetalTextureCacheFlush never called
- **Task 9**（审查登记）：drawables_presented incremented before commit (debug counter over-counts on SYP_ERR_IO)
- **Task 9**（审查登记）：no test for nextDrawable nil → SYP_ERR_TIMEOUT (make_offscreen_metal_layer(0,0) likely triggers it)
- **Task 9**（审查登记）：has_output stays true across early returns after rebuild_textures (pre-existing)
- **Task 10**（审查登记）：hardware_decoding_ not reset in close_internal (harmless; snapshot checks player_)
- **Final**（终审挂起）：reopen latch test only checks receive()==NeedInput (a send() OK would prove reset) — Ruling: code is correct (close() clears latch); test tightening optional.
- **Final**（终审挂起）：attachVideoLayer: sets device only when nil; a layer bound to another device gets refused off-main with ERROR log — Ruling: unreachable from PlayerView; logged loudly.
- **Final**（终审挂起）：Pipeline::video_hardware_decoding() ignores ts.failed, demo label may show 硬解 after a C1-style track failure — Ruling: non-blocking; follow-up.

## M-HLS 收尾登记的三条（Task 11）

- **`LoopbackServer::handle_conn()` 已有 `config()` 快照，路由分支又单独
  锁读了一次 `cfg_.routes`**（M-HLS Task 3 审查发现）：`handle_conn()`
  开头 `const LoopbackConfig cfg = config();`（`tests/support/loopback_server.cpp:459`）
  拿了一份配置快照，但路由分支（`:488` 起）为了读 `cfg_.routes` 又单独
  `std::lock_guard<std::mutex> g(mu_);` 加锁读了一次——不是复用上面那份
  快照。两次加锁之间如果有另一个线程调用 `set_route()` 并发插入新路由，
  同一次请求处理内部看到的路由表状态可能不一致（快照那次没有的路由，
  路由分支那次锁到了；反之同理）。

  **现在为什么不修**：当前所有 HLS/M1 测试的用法都是"先配好全部路由，
  再发请求"，测试运行期间路由表不会变，触发不到这个窗口。真要修，
  把 `cfg_.routes` 也塞进 `config()` 返回的快照结构体里、路由分支直接用
  快照即可，改动很小，但目前没有任何用例能证明修了会让哪条红变绿，
  纯粹是防御性加固，登记为债务、不在本轮做。

- **`tests/support/stub_backend.cpp:751` 的 `pump_all()` 在不持 `mu_`
  时调 `note_violation()`**（M-HLS Task 10 审查发现，**先于本分支存在，
  本次未触碰**）：`note_violation()`（`stub_backend.cpp:195`）本身只是
  `violations_.emplace_back(msg)`，不是线程安全的容器操作；全仓其余 6 处
  调用点（`:202`、`:316`、`:320`、`:324`、`:377`、`:651`）都在已持有
  `mu_` 的临界区内调用，只有 `pump_all()`（`:751`，"步数超过守卫上限"
  分支）是在锁外调用的，与其余调用点的纪律不一致。

  **现在为什么不修**：`pump_all()` 是测试专用的同步泵循环，单线程调用，
  实践中不存在真实的并发写 `violations_` 的场景——这条不一致目前不会
  导致任何可观测的问题，登记为债务，留给下次真正touch这段代码的人顺手
  统一加锁。M-HLS 分支的 diff 里没有碰过这一行。

- **`Demuxer::open_prepared()` 把 `avformat_open_input`/
  `avformat_find_stream_info` 的所有失败压成 `SYP_ERR_IO`，AVERROR 细节
  丢失**（M-HLS Task 6 审查发现）：`src/media/demuxer.cpp:170-179`（open）
  与 `:180-185`（find_stream_info）两处都是"不管 `rc`/`find_rc` 具体是
  什么负值，一律 `*err = SYP_ERR_IO`"。这与既有的 `open_file()`/
  `open_avio()` 风格完全一致（同一份代码历史上就是这么写的），不是
  M-HLS 新引入的问题；M-HLS Task 9 在 HLS 路径上用
  `HlsSession::precise_open_error()`（`src/media/hls/hls_session.h`）
  单独绕过了它——HLS 会话在自己主动拒绝一次 `open`（比如加密判定）时
  记一份精确原因，供 `Pipeline::create_hls()` 在 `open_prepared()` 返回
  笼统的 `SYP_ERR_IO` 之后换回去——但这只覆盖 `HlsSession` 自己主动拒绝
  的那几种情况，`avformat_open_input`/`avformat_find_stream_info`
  本身返回的 AVERROR（比如具体是"协议不支持"还是"探测超时"还是别的）
  依然被压平，根子还在 `open_prepared()` 本身。

  **现在为什么不修**：`open_prepared()` 是 M2a 就有的既有代码路径，
  `create_file()`/`create_avio()` 两条非 HLS 路径同样受这个限制，
  改动它的错误分级风格是一次跨路径的行为变更，不是 M-HLS 一个里程碑
  该单独决定的事，登记为债务留给专门做"错误分级"的任务统一处理。

## M3b 收尾登记（Task 8，2026-09-15）

- **追帧阈值（`kCatchupDropsToEnable=5` / `kCatchupWindowUs=1s` / `kCatchupOnTimeToDisable=30`）是经验常量，待真机调参**（`src/media/track_player.h`）：spec §6 明确点名——没有真实素材/真实设备跑量做过校准，数字是"看起来合理"的直觉值，不是测出来的。真机验证时如果发现追帧触发过于敏感（正常网络抖动就误触发）或过于迟钝（明显卡顿很久才追），应重新调这三个常量，而不是改判定逻辑本身。
- **追帧窗口按媒体时间计量，会随倍速缩放**（M2b Task 5 遗留，`late_drop_times_us_` 滑动窗口用 `pts`/`clock_->now_us()` 而非墙钟）：`kCatchupWindowUs=1s` 是媒体时间上的 1 秒，2x 倍速下墙钟只过 0.5 秒就能塞满同样多次迟到丢帧，触发更快；0.5x 倍速下则要墙钟 2 秒。这是沿用既有窗口设计的自然结果，不是本轮引入的新缺陷，但真机调参（上一条）时必须把当前倍速一并记录，否则同一组常量在不同倍速下的"灵敏度"没有可比性。
- **`demo/generate_xcodeprojects.rb` 每次运行都生成全新 UUID，导致约 1600 行 pbxproj churn**（脚本文件头 2026-09-14 已有一段自述，本条在 tech-debt.md 里正式登记；M3b Task 7 复核 `review-ff00000..a2004b4.diff` 时再次实测确认了"纯 UUID 换新"这一性质）：`xcodeproj` gem 默认给每个 `PBXBuildFile`/`PBXFileReference` 等对象随机生成 UUID，不是 CocoaPods 集成 Pod 时那种按内容哈希算出来的确定性 UUID——脚本没有从已有 `project.pbxproj` 里读旧 ID 复用，同样的输入两次跑出来的 UUID 完全不同。所以哪怕源文件列表一行没变，重新生成一次也会让 git diff 炸出铺满全文件的改动，约 1600 行。现在为什么不修：两个 demo 工程本身不进正常的 code review diff 关注范围（审查/复审 `project.pbxproj` 时看 `git diff --stat` 的新增/删除文件列表即可，逐行比对内容会被 UUID 噪声淹没，找不出真正的改动点），改成"按文件相对路径算稳定哈希"的确定性 UUID 需要跟 `xcodeproj` gem 的内部 UUID 分配机制对齐，是一块独立的开发投入，demo 壳的 pbxproj 改动频率低，这次判断不值。将来若 pbxproj churn 开始影响 review 效率（比如需要频繁重新生成），再重新评估这笔投入。

## M3b 终审登记（2026-09-15）

- **iOS/Catalyst 路由变化让音频时钟阶跃 Δ（设备延迟差）——已缓解（M3c 斜坡），真机未验证**（`src/platform/apple/audio_unit_sink.mm` 路由变化 block，`docs/known-gaps.md` #25"终审补记（I1）"/"M3c 补记"）：有线→蓝牙时钟往回跳（物理上正确，但画面冻结约 Δ、追帧窗口旧记录多挂 Δ）；蓝牙→扬声器时钟往前跳（Δ > 80ms 时连续迟到丢帧，可能误开追帧）。M3c 已用 `kLatencySlewRate`（100ms/s）把 Δ 平滑成限速斜坡而不是瞬间阶跃（回归用例 `slew_latency_keeps_played_us_monotonic_across_route_change`）；不能把 Δ 吸收进基准（会留下永久 A/V 偏移）。斜坡速率是否让人耳察觉不到、蓝牙真实 Δ 量级——仍然只能真机验证，见 known-gaps #62。
- **macOS 不监听默认输出设备变化**（同文件 macOS 分支，known-gaps #25"终审补记（M2）"）：延迟分量只在 `open()` 读一次，播放中切到 AirPods 不更新；iOS/Catalyst 会更新，两端不对称。
- **`present_window_harness` 的可见窗口实测待补**（`tools/present_window_harness/`，known-gaps #52"终审 C1"）：本机无人值守会话里窗口不可见，数字不稳定，需要在点亮的显示器上重跑确认 BUSY 重试策略的丢帧接近 0。

## M3c 收尾登记（Task 9，2026-09-15）

- **水位默认值（`startup_buffer_ms=500`/`rebuffer_trigger_ms=100`/
  `rebuffer_resume_ms=2000`/`max_buffer_ms=30000`/`max_buffer_bytes=64MiB`）
  是经验值，待真机网络调参**（`src/media/track_player.h` `BufferPolicy`、
  `src/media/pipeline.h` `PipelineConfig`）：spec §2 非目标 1 明确排除自适应
  水位，这五个数字是设计阶段给出的直觉值，不是真实网络跑量测出来的；用户
  可感知的影响（卡顿触发过敏/过钝、恢复太快/太慢）登记在
  `docs/known-gaps.md` #61。
- **自适应水位（按网络速度/卡顿历史动态调整恢复水位）未实现**（spec §2
  非目标 1）：现在固定读配置值，网络时好时坏时不会自动收紧或放松触发线/
  恢复水位；留给之后有真机数据支撑再做。
- **加载线程停读判据用"包队列时长"近似"距播放位置的已缓冲时长"**（spec
  §3 决策 4、Ruling P3）：加载线程不知道播放位置，用队尾结束时刻 − 队首
  开始时刻近似"还能播多久"；差值是 `FrameQueue` 与音频环里已解码但未播放
  的那部分时长（≤ 各自的帧/样本上限，量级在亚秒），不是不能接受的误差，
  但如果以后需要更精确的水位判定，这里是已知的近似点。
- **解码仍在泵线程上，未拆到独立线程**（spec §2 非目标 2）：M3c 只把
  demux（含网络读）移出泵线程，解码判定"软解音频、硬解视频在泵线程上足够
  快"目前只是设计假设，没有真机跑量数据支撑；待有数据证明解码本身成为
  瓶颈（比如高分辨率软解、多轨同时解码）再评估独立线程。
- **`resume()`/`played_us()` 的延迟斜坡携带逻辑没有对象级测试**（Task 5
  deferred minor，`src/platform/apple/audio_unit_sink.mm`）：斜坡限速、
  单调护栏、暂停不推进这几条纯函数层面的数学都有单测覆盖（`slew_latency_*`
  系列），但"暂停期间调用方完全不调 `played_us()`，恢复后 `resume()` 把
  `latency_updated_at_` 对齐到 `steady_clock::now()`"这条跨函数的时序契约，
  在真实对象（而不是纯函数）粒度上没有测试钉住，因为需要一个可控的时钟
  注入点（fake-clock seam）——`AudioUnitSink` 目前用真实 `steady_clock`，
  没有测试缝可以冻结/推进它来确定性地构造"暂停 N 秒后恢复"这类场景。
- **加载线程没有回滞，`full` 在稳态下大约每帧翻转一次**（Task 4 报告"设计
  观察/顾虑"）：正确性有 `buffering_full_flicker_with_healthy_level_never_
  transitions` 兜底（不影响缓冲状态机判定），代价仅是一次可忽略的加锁/
  唤醒开销；用户可感知的一面登记在 `docs/known-gaps.md` #59，本条记录的是
  "值不值得加低水位阈值"这项工程判断本身——目前判断不值，HLS 单次网络读
  成本变高时应重新评估。
- **`wake_loader()` 在每次 `drive_decoder` 返回后都会拿一次 `load_mu_`，
  包括 `NoWork` 的情况**（Task 4 deferred minor）：属于 brief 规定的做法，
  多出来的是一次无竞争的加锁，泵线程每步都要付这个代价，可忽略不计，登记
  在案以免以后有人误以为是遗漏优化。
- **`take_all()`/`seek_async` 摘出的临时 vector 在 `bad_alloc` 下可能泄漏
  已摘出但尚未 `av_packet_free` 的包**（Task 4 deferred minor，
  `src/media/packet_queue.cpp` `PacketQueue::take_all()`）：`take_all()`
  本身不是 `noexcept`（内部有分配），如果调用方在拿到 `deque<AVPacket*>`
  之后、逐个 `av_packet_free` 之前的这段窗口里发生 `bad_alloc`（比如
  `seek_async` 里的其他分配失败），已从队列摘出的包会跟着异常传播丢失，
  不会被自动释放。实践中这条路径极少触发（`bad_alloc` 本身就罕见），登记
  为债务，不阻塞合并。
- **字节水位统计口径的组合空白**：`water_full_locked()` 的字节条件覆盖全部
  受管轨、时长条件只看 `buffer_track_ && live_` 的轨，如果某条轨把字节额度
  占满而调用方长期不消费另一条空轨，加载线程会停住——这与同步模式下"调用方
  必须消费每一条受管轨"的既有联合背压契约相同，`pipeline.h` 头部注释已写明，
  不是新缺陷，本条只是把它明确记入技术债清单以防遗漏。
- **`frame_digest.h` 的 `DecodeOptions::blocked_waits` 只在线程模式测试里
  使用**（Task 7，`tools/syp_probe/frame_digest.h`）：这是给"泵线程看到
  `Blocked` 且没有数据可排空"这种瞬态竞态兜底的测试专用选项（Blocked 让出
  200µs 再重试），生产路径不使用，也不需要使用；记录在案以说明这个开关的
  存在理由，避免以后被误当作生产配置项使用或删除。

### M3c 终审补登（2026-09-15）

- **每条 Pipeline 的内存上限按单路播放设计，M5 预加载必须自设更小的限额**
  （`src/media/pipeline.h` `PipelineConfig`）：包缓冲上限 `max_buffer_bytes`
  = 64MiB，外加每轨 `max_frames_per_track` = 8 帧已解码帧。4K 软解
  yuv420p 一帧约 3840×2160×1.5 ≈ 12.4MB，8 帧约 100MB——单条 Pipeline 最坏
  约 64MiB + 100MB ≈ **160MB**（硬解帧在 CVPixelBuffer 池里，量级相近但不计入
  进程堆）。单路播放可以接受；M5 预加载同时持有多条 Pipeline 时这个数字按条数
  线性叠加，必须为预加载实例单独配小 `max_buffer_bytes`/`max_buffer_ms`/
  `max_frames_per_track`，不能沿用默认值。另见 known-gaps #57：某条在播轨提前
  结束时时长上限失效，实际占用就是字节上限。
- **缺用例：封面图轨不计入 `buffered_until_us`、但参与 `full` 判定**（Task 2
  deferred minor，`src/media/pipeline.cpp` `buffer_stats()`）：同步模式下
  `full` 统计全部受管轨（含封面图，`ts.packets->full()`），`buffered_until_us`
  的 min 只看 `buffer_track_`（排除封面图）。两条口径不同是有意的（封面图只有
  一个包，不该把已缓冲时长钉在 0；但它的包队列满了一样挡 demux），可没有用例
  钉住任何一半——改动 `buffer_track_` 判据时可能静默回归。
- **~~字节水位用例 `pipeline_demux_thread_stops_at_max_buffer_bytes` 无
  Watchdog、素材码率前提无注释~~**（Task 9 审查登记项）：已在终审修复波次
  直接补上 Watchdog 与码率/单包大小前提注释（`tests/test_pipeline.cpp`），
  不再是债务，留条目便于追溯。
- **缺用例：线程模式下加载线程正在入队时某条轨失效**（Task 3/4 deferred minor，
  `Pipeline::mark_track_failed()`）：`mark_track_failed()` 先持 `load_mu_` 置
  `live_[i]=0` 再清队列，保证清空之后不会再有这条轨的包入队；这条时序只有代码
  评审，没有 Pipeline 层用例（例如失效后断言该轨 `packets` 恒空、`buffer_stats()`
  不再计入它）。终审 C1 的 `seek_with_failed_video_track_does_not_hold_audio_forever_threaded`
  （`tests/test_track_player.cpp`，硬解假后端让视频轨在加载线程运行中失效）只从
  TrackPlayer 层验证了"音频继续播"，不覆盖包队列层面的断言。
- **demo 的 onError 在同一次错误里可能回调 2~3 次**（`demo/shared/bridge.mm` 泵循环错误闩）：Pipeline 在
  队列排空后才报终止 Error，而 TrackPlayer 手里可能还有一帧背压中的音频、一帧早到的视频，它们随后报
  Queued/Presented 会复位闩，下一步再报 Error 时再通知一次。上限受这两个待处理槽约束；`bridge.h` 的注释
  字面上允许，但"每次错误只通知一次"并不严格成立。修法：只在 seek 驱动的恢复时复位闩。
- **seek 后音频暂停写入带来约 20~40ms 额外静音/画面停顿**（`src/media/track_player.cpp` 终审 C1 的暂停写入）：
  暂停期间每步只做一个 Pipeline 单位工作并报 Waiting，demo 泵在 Waiting 上睡 2ms；线程模式下音频先解满
  FrameQueue（8 步）再轮到视频。在裁决声明的代价之内，未经真机感知验证。
- **同步模式下 seek 音频暂停写入"不会卡死"依赖解封装器的交织性**：成立前提是 seek 后第一帧视频在约 3 秒
  音频之内送达（128 包 + 8 帧的背压上限之前）；mov 在各流 dts 相差超过 1 秒时切换交织，满足。将来若接入
  不交织的解封装器且走同步模式，视频永远到不了、也不会 drained，暂停写入不会解除，会永久停住。

## M4 收尾登记（Task 8，2026-09-16）

1. **pbxproj 全量重排，且现在更严重**：生成器新增了 `SYPlayerKit` 与
   `SYPlayerKitTests` 两个目标（`demo/generate_xcodeprojects.rb`）与共享
   scheme，`xcodeproj` gem 的随机 UUID 让 `git diff` 铺满两个工程文件的全文（既有
   问题，生成器顶部已说明）；M1 只有一个 App 目标时铺得还没现在多——现在两个工程
   里各自有三个目标（iOS 工程两个：`SYPlayerKit`/`syplayer-ios`；mac 工程三个：
   再加 `SYPlayerKitTests`）。审查只看 `git diff --stat`，不逐行比对。确定性 UUID
   仍未做。
2. **`SYPlayerKit` 不参与 CMake 四套构建**：framework 只在 Xcode 工程里存在，
   Swift 层没有 ASan/TSan 覆盖，C++ 内核的 sanitizer 覆盖仍来自 `build-asan`/
   `build-tsan` 里的 `syp_media`/`syp_platform_apple`。Swift 层本身没有裸内存
   操作，风险集中在它与桥的边界（`deinit` 时序、双闸门的时序竞争），靠
   `SYPlayerLifecycleTests.testDeinitWhilePlayingDoesNotCrashAndActuallyFreesThePlayer`
   与 `SYPlayerSessionGateTests` 的变异验证兜，不是 sanitizer 级别的证据。
3. **两个工程各生成一份 `SYPlayerKit`（及 `SYPlayerKitTests`）目标**：源文件
   集合相同、build settings 相同，但是两份独立的目标定义，改设置要改生成器的
   同一个函数（已经是同一个函数，不会漂移）；代价是 pbxproj 里有两份。跨工程
   引用子工程能去重，但会引入 `PBXContainerItemProxy` 的路径耦合，本轮判断不值。
4. **`SWIFT_VERSION` 停在 `5.0`，语言模式 6 的迁移仍未开始**（ruling P8）：
   `demo/generate_xcodeprojects.rb` 里三处 `'SWIFT_VERSION' => '5.0'`（`SYPlayerKit`/
   App/`SYPlayerKitTests` 目标）均未动。已知至少两处代码依赖"语言模式 5 不报错、
   模式 6 会报错"的间隙：`SYPlayer.open()`/`close()`/`deinit` 里的
   `let res = resources`（**订正，终审 M8**：上一版写的是 `let b = bridge`，
   代码里没有这个绑定，桥从来不单独跨闭包捕获）这类跨 `@Sendable` 闭包捕获
   非 Sendable 类型——正是用 `SYPlayerResources` 的 `@unchecked Sendable`
   盒子规避的那一类，注释里写明了纪律；`SYPlayerDelegate` 在
   Task 7 fix round 1 已补 `@MainActor`，消掉了其中一半风险，但迁移本身
   （连带评估 `@preconcurrency`/严格并发检查全量打开后的其余告警）仍是独立
   待办。
5. ~~**`videoGravity` 只有 `.aspectFit` 一个 case**~~ —— **已解决（M6d）**，
   条目移入下面「已解决」一节，不在此重复正文。
6. **SwiftUI 页的进度条是边拖边 seek**（`demo/shared/SwiftUIPlayerViewController.swift`）：
   SwiftUI 的 `Slider` 没有"拖动结束"回调（`onEditingChanged` 在 iOS 13 上只有
   按下/抬起两态，不足以复刻 UIKit 页的 `isScrubbing` 节流）。高频 seek 在框架
   里是安全的（幂等），但在弱网 HLS 上会反复触发缓冲，真机体验未验证（见
   known-gaps #67）。
7. **`SYPlayerKit` 的桥不在 `check-deploy-target.sh` 扫描内**（known-gaps #65）。
8. **`stateStream` 每次访问建一条新流**（`SYPlayer.stateStream`）：多次访问会
   注册多条 continuation，都在 `deinit` 或订阅方结束时清。没有做"同一个订阅者
   复用同一条流"的去重——`AsyncStream` 的语义本来就是一条流一个消费者。
9. **`SYPlayerKit` 独立 scheme 解析不出 Mac Catalyst destination**（Task 5 偏离
   4）：只含静态 framework 目标的那个 scheme（`-scheme SYPlayerKit`）报
   `Unable to find a destination matching …Mac Catalyst`；验证一律改走
   `syplayer-mac`/`SYPlayerKitTests` scheme，kit 在这两条路径上都被真实编译，
   不影响验收，但那个独立 scheme 本身目前用不了。
10. **`open()` 的暂停落地存在残余的 pump-hop race**（Task 6 fix round 1，见
    known-gaps #70 与 roadmap M4 判定；**订正**：上一版写的是 #67，那是人工清单条目）：`-openLocalFile:`/`-openURLString:`
    内部把 `pump_loop()` 异步派发到另一条串行队列，Swift 侧的 `pause()` 与那次
    线程切换是一场真实的赛跑；结构性关严需要改核心"以暂停态启动"，本轮明确
    不做。`testOpenLeavesPlayerPausedWithNoPlaybackBeforePlay` 用 `position`
    做代理断言，只能在事后观测，不能阻止竞争本身发生。
11. **`generate_xcodeprojects.rb` 里 `add_kit_target`/`add_app_target` 之间仍有
    重复的平台三元表达式与警告设置**（Task 1 遗留 minor，未处理）：本轮没有再
    往这两处加新的重复项，抽公共 helper 仍留给"下次真正大改这个文件时"。
12. **`APP_HEADERS` 是空数组，`add_app_target` 里有一个恒为空的 `.each` 循环**
    （Task 2 遗留 minor，`demo/generate_xcodeprojects.rb:109,236`）：桥接头已
    随 Task 2 的桥迁移删除，这个常量此后再没有被填过；留着是给以后 App 真要加
    公开头用，但截至 M4 收尾仍是空的，可以考虑连同常量一起删掉。
13. **`SYPlayerLayer.init(layer:)` 重新配置而非从源 layer 拷贝**（Task 4 遗留
    minor）：presentation copy 目前重新跑一遍 `configure()`（device/pixelFormat/
    framebufferOnly 硬编码常量），只有将来给 `SYPlayerLayer` 加"覆盖 device"之类
    的 API 时才会变脆弱，本轮无需修。
14. **`SYPlayer` 的几处零散 minor（均 Task 5 遗留，未修）**：`open()` 不响应
    `Task` 取消（调用方 `.cancel()` 手里那个 `Task` 不会打断桥的 `-open*`）；
    delegate 回调的重入行为未文档化；没有独立的 `.opening` 播放阶段（起播期间
    `playback` 仍是 `.idle` 或上一次的值，直到桥返回）；两条偏弱的测试
    （具体见 task-5-report.md 遗留段）；同会话错误在 `close()` 刚过之后到达会
    被暂存而非丢弃（无害——`open()` 会在下一次 open 前清空 `pendingError`）。
15. **`.autocapitalization(.none)` 是 iOS 14 起废弃的 API**（Task 7 遗留 minor）：
    **订正（终审 M8）**——上一版把它记在 `PlayerViewController.swift`（UIKit 页）
    名下，实际位置是 `demo/shared/SwiftUIPlayerViewController.swift:96` 的 URL
    输入框，**SwiftUI 页**。UIKit 页用的是 `UITextField` 的
    `autocapitalizationType = .none`（`demo/shared/PlayerView.swift:101`），
    那是 UIKit API，没有被废弃，不在本条范围内。部署目标是 iOS 13 不能换成
    `.textInputAutocapitalization`，当前 SDK 未就此告警，将来可能会。

16. **`.failed` 在 Swift 门面层是终态，而桥把错误当作可恢复的**（终审 I2，
    见 `docs/known-gaps.md` #72）：正确修法是给快照加错误字段、让 `refresh()`
    从桥的活闩派生 `failed`，属于后续里程碑。本轮只做了"`play()` 清错误闩"
    这一半，让显式重试能走通。

17. **HTTP 缓存目录不可配置**（终审 M7 的另一半，见 `docs/known-gaps.md` #73）：
    `SYPBridge.mm` 的 `-openURLString:` 把它写死在 `NSTemporaryDirectory()`
    下的 `syplayer-http-cache`（本轮只把目录名里的 "demo" 去掉）。公开 API
    上没有任何入口能改路径、容量或清理策略。

18. **没有音量 / 静音，也没有原始视频尺寸**（终审 I3，见
    `docs/known-gaps.md` #74）：两个 demo 页都把画面区硬编码成 16:9。

19. **`SourceBridge::save_index_locked()` 在 `mu_` 与句柄锁双持下做 fsync**
    （M5 Task 2 复审登记，`src/dl/source_bridge.cpp`）：
    落盘那一对 `handle_.file->sync()` + `handle_.index->save()` 必须原子
    （持久性不变式：字节没 fsync 成功就不许把区间写进 `.idx`，见
    `cache_index.h` 的"写入顺序"段），所以它整段在 `*handle_.mu` 下；
    而三个调用点（`persist_chunk` / `on_idle` / `close`）都已持 `SourceBridge::mu_`，
    于是这段文件 IO 是**双锁下**执行的。

    **代价比"查询会等"严重得多，不要低估。** 对等的 `SourceBridge` 要先取
    自己的 `mu_`、再取**同一把** `*handle_.mu`，所以一次 fsync 期间被卡住的
    不只是 `cached_ranges()` / `get_stats()`，而是那个源的
    **`seek()`、`interrupt()`、`close()`，以及它整条调度器回调路径**
    （`on_data` → `persist_chunk` 的两个 `mu_` 临界区、`on_total`、
    `on_validators`、`on_error`、`on_idle`）。放到 M5 的目标配置里就是：
    后台预加载源一次例行的 `save()`，会把正在播放那个源的**读取路径**整条
    停住——包括本该立刻生效的 `interrupt()`。慢盘或大索引上这不是理论问题。

    **结构性修法（M5 之后做）**：把 `mu_` 从这一对上摘掉，只留句柄锁围住
    sync+save。做法是把 `save_index_locked()` 拆成两段——在 `mu_` 下判脏、
    取一份句柄的 `shared_ptr`、先行清掉 `index_dirty_` / `unsaved_bytes_`
    （失败时回置），放开 `mu_` 之后再在 `*handle_.mu` 下做 sync+save。
    难点是失败回置与并发标脏的交互，值得单独一轮带用例做，不适合塞进 M5。

    相关的一处同形隐患**已在 M5 Task 3 清掉**：`CacheStore::acquire()`
    原先在 `CacheStore::mu_` 下调 `log_msg()`（索引损坏时那条 warn，
    `src/dl/cache_store.cpp`）。用户的日志回调若在里面碰
    `syp_source_open/close`，会再次进 `acquire/release` 取**同一把**
    `CacheStore::mu_`，同线程重入挂死。Task 3 要加的 TTL 过期日志正好在
    同一段里，于是一并改成"锁内只记结论、由一个声明在 `lock_guard`
    之前的 RAII 守卫在放锁之后才真的打"。`save_index_locked` 那处的 fsync
    双持仍在本条范围内，未解决。

## M5 收尾登记（Task 10，2026-09-20）

### 0. 六种让测试给出**假结果**的机制（长版）

短版在仓库根的 `CLAUDE.md`（每次会话都会读到）。这里是现场与数字——
**本里程碑每一种都真实发生过，其中三种当场制造了错误结论**。它们的共同点是：
你看到的那个"绿"或"红"**不是你以为的那次运行的结果**，所以任何"我跑了，是绿的"
在没排除这六条之前都不成立。

1. **`make` 的 mtime 粒度 1 秒 ⇒ 跑没重编的二进制。**
   Task 4 fix-round-2 的实现者自己发现：前几轮的变异检查一直在跑旧二进制，
   于是"这条变异被杀掉了"的结论是假的。**改完 `rm` 掉对应的 `.o`**，别把
   "make 说要重编"当成判据。

2. **构建失败，而测试照跑上一个二进制并报它的结果。** 两个方向都撞过：
   · Task 7 fix-round-2：变异 10 因 `-Werror` 未使用函数**编译失败**，`ctest`
     接着跑旧二进制、报出了**上一个变异**的失败——差点把两条变异的因果接反。
   · Task 8 fix-round-1 复审：make 的 target 名写错 ⇒ `No rule to make target`、
     **退出码 2**，而 `ctest` 报 `100% passed`。
   · Task 9 复审：包装脚本最后一条是 `echo`，把 `xcodebuild` 的 **exit 65**
     整个吞掉，报出 exit 0。
   ⇒ **显式读构建退出码，打一个 `BUILD_OK` 闸再跑测试**；包装脚本的最后一条
   命令必须是被测命令本身，或者显式 `exit $RC`。

3. **变异根本没落进文件。** `perl -0pi -e 's{…}{…}'` 里的 `$0` 即便在
   `\Q…\E` 内也**仍然被当变量插值**，替换静默不发生；`make` 成功、测试全绿，
   看起来就是"变异存活"。Task 8 复审与 Task 9 各踩过一次。
   ⇒ **改完 `grep` 一遍确认改动真的在文件里**，再谈结果。

4. **批量跑变异会假绿。** Task 7 fix-round-1 复审：5 个变异放进一个 shell
   循环，3 个报绿；单独重跑**全红**，且两次构建日志都显示重编了。
   ⇒ **一个变异一次工具调用。**

5. **用 `cp` / `git checkout` 还原头文件会把 mtime 盖成当前秒**，可能与已编好的
   `.o` 同一秒，于是 `make` 认为那个 `.o` 是新的。Task 7 fix-round-3 复审因此
   报出一条假存活（跑的是用例表里还缺三条的旧二进制），把**测试那个 TU** 的
   `.o` 也删掉才变红。
   ⇒ "删 `.o`" 这条规则必须扩展到**所有 include 了被还原头文件的 TU**。
   同源：`git checkout` 还原变异**不会触发重建**。

6. **负载假红把存活变异伪装成"被杀掉"。**（Task 8 fix-round-2 新发现，比前五条
   更阴，因为"红了"看起来正是你想要的结果。）跑 MUT_R8f 时第一轮 `ctest` 报
   1 failed，差点记成"变异被杀"——红的是 `time_target_uses_provider`，**与变异
   毫无因果关系**，重跑全绿。
   ⇒ **看到红必须确认红的是你预期的那一条用例**，不是"有红就算数"。
   本仓已知的负载敏感用例见下面第 8 条。

附带一条不属于上面六类的：**直接跑测试二进制时 cwd 不对**会让读 fixture 的
用例集体 FAIL，同样与变异毫无因果（Task 8 fix-round-2 复审踩到）。

### 1. `SourceBridge` 的读路径每次都拷一份 `shared_ptr<CacheFile>`

`read()` / `persist_chunk()` 在锁内拷一份出来、锁外用，这是"对象存活"的保证
（`CacheStore::Handle` 换成共享所有权之后，直接解引用成员就是对 shared_ptr
本身的竞态）。代价是每次读写一对原子增减。实测没有可观测影响（读路径本来就要
做一次 `pread`），但如果将来读路径成为瓶颈，可以考虑把文件引用在 open 时缓存
成一个只在 close 时才变的裸指针 + 显式的引用计数护栏——那会比现在难证明得多，
**不要为了省两个原子操作去换**。

### 2. `enforce_capacity` 的节流状态放在 `SourceBridge` 而不是 `CacheStore`

理由是 dl 层的时间基准只有注入式 `Clock`（`clock.h`，走 steady_clock），而
`CacheStore` 是进程内单例、没有自己的时钟。后果是**每个 source 各自节流**：
同时开着 N 个 source 时，最坏每秒会扫 N 遍目录。

**M5 Task 4 把这条放大了一档（ruling I3，登记不修）**：每一次 `close()` 都会
跑 `enforce_capacity`（每桥的节流从 0 起步），而 `Preloader` 在**每一次配额下调、
让路、优先级变化、条目完成**时都关一条 source ⇒ "每次 close 扫一遍"实际变成
"每次预加载策略变化扫一遍"，每轮最多 32 次删除，而让路循环能以 50ms 的周期转。
耐久的修法是给 `CacheStore` 一个**按 cache key 的上次 enforce 时间戳**（它同时
解决"每个 source 各自节流"），那要先给 `CacheStore` 一个时钟。

### 3. `syp_preload_config::struct_size` 是 `int32_t`，`syp_config` 的是 `uint32_t`

前者照抄 spec §4.1，后者是既有 ABI。两个公开结构体的同名字段类型不一致，是真实的
不一致。没有统一是因为改任何一边都是 ABI 变更。下次真的要动这两个头文件时一并统一。

### 4. 预加载的"额度"与 `SourceBridge` 的 `max_concurrent_tasks` 是同一个旋钮

`Preloader` 通过改每条目的 `syp_config::max_concurrent_tasks` 来分配额度，所以
"额度"其实是"这条 source 允许开几条连接"。这在语义上成立，但它同时也影响
`lookahead_bytes_locked()` 的窗口计算——本轮靠 `min_segment_size = ceil(target / quota)`
把两者重新解耦。这条耦合是 `Scheduler` 没有优先级队列的直接后果（spec §1 最后
一条），M6 若真的做跨源调度，应该把额度做成 `Scheduler` 的一等概念，而不是继续
借用配置项。

### 5. `Preloader` 是 `src/dl` 里第一个自己起 `std::thread` 的组件

在此之前 dl 层的所有并发都来自 `syp_http_backend` 的回调线程（`dl_task.h` 只
include `<thread>` 用 `thread::id`）。`std::thread` 在 macOS 上落到 libSystem 的
pthread，不破坏 `syp_dl_purity_check` 的零平台符号约束；但它确实是一条新的先例，
将来若要把 dl 层移植到没有 pthread 的环境（某些 RTOS），这里是第一个要重写的地方。

### 6. 容量/TTL 三件套的**四条**读回缝（M5 Task 10 / ruling R11 落地）

`max_cache_bytes` / `min_free_space_bytes` / `cache_ttl_ms` 这三个字段从调用方
进来之后要一路原样抵达每一条真正开出去的 `SourceBridge`。中途任何一处"拍照之后
再改一手配置"都让缓存变成**无上限、不过期、无限涨**，而外部**没有任何可观测
症状**——这正是它在 M5 里被漏掉**四次**的原因（R7 补了一条、R8 补了第二条、
R11 点名了第三条，Task 10 又量出了第四条）。四条缝与它们各自对应的变异：

| 缝 | 断在哪一跳 | 对应变异 | 修前 |
|---|---|---|---|
| `MediaInfoProvider::config_for_test()` | `cfg_`，provider 拿到的起点 | N2（`prepare_cache_dir` 三行清零） | `xcodebuild 74/0` 绿 |
| `PreloadStack::preloader_config_for_test()` → `Preloader::dl_config_for_test()` | `base_config()`，preloader 的起点 | MUT_R8（provider 构造后清零） | `ctest 33/33` + `xcodebuild 82/0` 双绿 |
| `Preloader::playlist_config_for_test()` | `fetch_text` 交给 `SourceBridge::open` 的那一份 | MUT_R8f | `ctest 33/33` + `xcodebuild 82/0` 双绿 |
| `MediaInfoProvider::probe_config_for_test()` | `probe()` 交给 `syp_source_open` 的那一份 | MUT_PROBE | `ctest 33/33` + `xcodebuild 82/0` 双绿 |
| `hls::HlsSession::dl_config_for_test()` | `dl_cfg_`，**每一条 HLS 媒体分片源**的来源 | MUT_HLS | `ctest 33/33` + `xcodebuild 82/0` 双绿 |

**MUT_HLS 是五条里后果最大的一条，而且它是 M5 最后一轮复审才发现的**：它落在
**HLS 播放**（不是预加载）上，一条流的每一个分片都不看上限、不过期、不看可用
空间，永久累积。

**留在这里的方法学**：R11 点名了三处，实测是**四处**。"给 X 补一条缝"这种裁决
落地时，**先把 X 的孪生体找出来**——本里程碑这条规则兑现了四次（R5 的 `\v`/`\f`
洞从 dl 侧搬到了 media 侧、R9 的 `enforce_capacity` 手工拼 key、R8 的
`config_for()`、R11 的 `probe()`）。

**还没关的一处不是缝，是形状**：读回缝断的是"配置长什么样"，它挡不住"配置对了
但这条路径根本没走 `enforce_capacity`"，也可以被"把清零挪到缝的调用点之后"绕过。
所以 `Preloader`（条目源）、`fetch_text`（播放列表源）、`HlsSession`（分片源）
三条路径各配了**一条真的淘汰行为用例**；`probe()` 那条只有缝，没有行为用例
（探测源上界 2 MiB/URL，且同目录里总有别的源会替它扫），**这是已知缺口**。

### 7. `resolve_url` 与 `ff_make_absolute_url` 的四类既有分歧（登记，不修）

判据是"与 `ff_make_absolute_url` 逐字节一致"（播放侧解析分片 URI 用的就是它，
而 `CacheStore::make_key` 哈希 URL **原串**，差一个字节就永远命中不了）。
Task 7 fix-round-3 之后：焦点矩阵 39×94=3,666 格点段类差异 **0**，6 seed
**11,885,541** 条 fuzz 里点段类 **0**。剩下 4 类全部登记为**有意/不可达**：

- **A** base 或 ref 为空 → 我们返回空串；扫描器跳过空行，到不了。
- **B** base 没有 `scheme://` → 我们返回空串（有意为之）。
- **C** ref 形如 `x:y` / `data:foo` / `seg:0.ts` / `00:00:01.ts` → FFmpeg 认它是
  绝对 URL，我们当相对路径拼。**已测量为不可达**：`avio_open2` 对每一个都返回
  `AVERROR_PROTOCOL_NOT_FOUND`，播放侧同样取不到这个分片，不存在"播放的 key"
  去错配。（报告里曾给过一个更弱的理由"没有打包器会这么写"，以头文件那条为准。）
- **D** 只有 query/fragment 的 ref → 同 C，已测量不可达；真实形状
  `index.m3u8?_HLS_msn=5`（LL-HLS）与 FFmpeg **完全一致**。

### 8. 负载敏感与种子敏感的测试（看到红先查这里）

- **`SYP_FIXTURE_SEED=28761281` 可复现地让
  `pipeline_demux_thread_async_seek_first_frames_match_sync_mode` 变红**
  （`ctest` 3/3 次重跑确定性失败）；换回参考种子 `00818168` → 33/33。
  本仓"可复现性回归只验了一个种子"那条债**现在有了具体反例种子**，就是这个。
  它与 M5 的改动无关（发现它的那一轮里 `src/` 只动了 `m3u8_scan.{h,cpp}`）。
- **第二个反例种子：`SYP_FIXTURE_SEED=94888618`**（全支终审修复波次发现）。
  该种子下 `moovend.mp4` 只有 16,715,274 B，使既有用例
  `probe_is_bounded_on_moov_at_end_and_falls_back` 的
  `REQUIRE(total > 8 * kProbeMaxBytes)` **确定性失败**；参考种子 `00818168`
  下该文件是 22,832,904 B，通过。**两个反例种子打在两个完全不同的用例上**，
  说明"只验一个种子"这条债的暴露面比原先以为的宽——不是某一条用例对种子敏感，
  而是**素材尺寸本身参与了判据**的那一类用例都可能中。
- **`no_progress_backoff_survives_across_rounds`**（`tests/test_cache_store.cpp:856-860`）
  在 `ctest -j4` 满载下出现过 `st.deleted 1 vs 0`；单独重跑 23/23 绿。
- **`probe_e2e`** 在高负载下 flaky（Task 6 遗留）。

这几条正是上面机制 6（负载假红）最容易咬人的地方：**变异验证里看到它们红，
几乎一定与你的变异无关。**

### 9. 两个已知无害的存活变异——**别再花轮次去杀**

- `src/dl/m3u8_scan.cpp` 的 `sep > 0`（带 scheme 支）：只影响以 `://` 开头的 ref，
  发布代码本就与 FFmpeg 分歧，且 `avio_open2("://x")` 返回 Protocol not found
  ⇒ 与上面的分歧类 C 同属**不可达桶**。
- `src/dl/m3u8_scan.cpp` 的 `url[path_begin] != '/'`：**可证明的等价变异**——两个
  调用点都传 `path_slash_offset()` 的结果，它只会返回 `npos` 或某个 `/` 的下标。
  **任何用例都杀不掉它**，写一条只会写出一条假装有意义的断言。
- demo 预加载页的 `buildLayout()` 开头早退 ⇒ **一个控件都不上屏、页面全白**，
  而自检输出与基线**逐字节相同**、exit 0。杀它需要真正的上屏判据（UI 测试），
  而无 host 的逻辑测试 bundle 里 `UIWindow` 不可构造（known-gaps #67/#68）。
  ⇒ demo 的自检入口证明的是"数字来自 label 的 `text` 属性"，**不是**"页面渲染
  正确"；渲染/约束/层级这一整类故障它一条都看不见。

### 10. `test_preloader` / `test_preloader_hls` 挂在 `if(APPLE)` 门下

非 Apple 平台上它们会**无声消失**。这与 `test_apple_http_backend` 同待遇，但
`test_matrix_sentinel` 目前不盯这一类（它盯的是 FFmpeg/fixtures/切片三个维度）。
本仓只在 Apple 上构建，所以今天没有实际后果；真要移植时这里是第一个要补的。

### 11. 样例服务器两个够不到的 Range 角落（别当通用 fixture 复用）

`demo/shared/PreloadSampleServer.swift`：`bytes=-500`（后缀区间）**静默退化成
200 全量**；`bytes=5-4`（start > end）返 **206 + 0 字节**而不是 416。dl 层够不到
这两种形状（它只发 `bytes=N-` 与 `bytes=N-M`），所以对预加载演示页无影响；
**但它不是一个符合 RFC 7233 的服务器**，拿去当别的东西的 fixture 会得到错误结论。
`tests/support/loopback_server.cpp` 那一份是另一套实现，Range 正确（Task 9 复审
实测 206/416/夹取全对）。

### 12. M5 逐 task 的 Minor 汇总登记（不单独开 known-gaps 条目）

**这一节存在的理由**：M5 有 10 个 task、20 多轮复审，每一轮都留下几条"成立但
不值得单开条目"的 Minor。它们散落在台账里，而台账不是给后来人读的。全部抄在
这里，一条都不丢——**没被登记的遗留等于消失**。

**`CacheStore` / `SourceBridge`（Task 1–3）**
- 注册表的快速路径**跳过了 `CacheIndex` 的 URL-vs-key 碰撞重查**：命中已有条目时
  直接返回共享句柄，不再核对索引里记的 URL 是不是同一个。哈希碰撞才触发。
- 损坏索引那条分支会把 `CacheIndex::create` 构造两遍（从 `source_bridge` 带过来的）。
- `enforce_capacity` 的**空间退避没有滞回**：空闲卷上实测约 **5 次/分钟**的"解除→
  重新退避"，每次解除都要花一整轮去重新探测。
- **≥32 条删不掉的 LRU 头部条目仍会把淘汰顶住**（`chflags uchg` 这类）。删除尝试
  的上界是 32，它们每轮把上界吃光 ⇒ 后面的条目永远轮不到。头文件注释里把这一类
  说成"已消除"是**说过头了**，真正消除要认 immutable 标志。
- `BUSY` 可以在 `deleted == 0` 时返回（查看条目数上界先触发），与那一版头文件措辞
  自相矛盾。
- `last_round_` 的重置排在三个提前返回之后 ⇒ 那几条路上会漏出**上一轮的统计**。
- `walks_past_undeletable` 那条用例的"毒条目"构造比它读起来弱：`.idx` 先被 unlink，
  于是条目直接从扫描结果里消失了，并没有真的走到"删不掉"那一步。
- `still_over` 用的是**陈旧的逻辑大小估算**；`scan_cache_dir` 是 O(N²) 且以最高
  1Hz/源的频率无锁跑。
- `last_round_` 是一个**全局单槽**，只在单线程测试下成立。

**`Preloader`（Task 4–7）**
- **停滞条目没有"无进展退出"**：驱动线程会一直以 20 次/秒醒着。
- `Entry::open_quota` 写了从不读。
- `driver_busy_for_test()` 的文档比它真正的谓词说得强。
- 让路与共享**都直接拿裸 `cache_dir` 字符串做 key**（归一化之后仍是字符串比较）。
- **析构那条调用点没有用例钉住**：只变异析构函数仍然全绿。
- `add()` 之后立刻 `set_priority()` 传**同一个优先级**时，HLS 子条目的优先级可能
  卡在低档。
- 一个**正在被 remove 的条目**完成展开时，`completed_` 会漂。
- **播放列表条目在展开期间占着一整份 per-entry 额度**，而它自己不开源。
- **只有 CR（`\r`）行尾的播放列表扫出 0 个分片**（FFmpeg 的 `ff_get_chomp_line`
  认它，我们的行切分不认）。
- 暖/冷估算一致性那条用例的容差**吸收掉了真实的波动**，鉴别力比看起来弱。
- **取回的播放列表本身进了缓存**（`fetch_text` 用的是普通 `SourceBridge`）。播放
  不受影响（HLS 的播放列表通道根本不读缓存），受影响的是**下一次对同一直播 URL
  的预加载**：可能扫到一份过期的分片列表，白暖几个已经滚出窗口的分片。要修得给
  这条抓取一条"不缓存"或"强制重验"的通道。
- **终态是粘的**：一次瞬时的磁盘满把条目打成 `Failed` 之后，这个 URL 在本
  `Preloader` 的余生里再也不预加载；`Done` 的条目在缓存被淘汰后也不会自动重下。
  出路只有调用方 `remove()` + 再 `add()`。要自动重试得区分可重试错误
  （`NO_SPACE`/`IO`）与真终态（404）并加退避。

**公开 API（Task 5–6）**
- `to_priority` 把**越界的优先级静默压成 `Background`** 而不是拒绝，且没写进文档。
- `syp_preloader_remove` / `_remove_all` 接受空 URL 当 no-op，而 `add` /
  `set_priority` 对空 URL 报错 —— 同一个头文件里两套约定。
- provider 的函数指针**没有判空**。
- `estimate` 的两条退化路径（非媒体 URL、时长未知/chunked）**没有用例**。
- 毫秒数溢出的那道保护**要先花一次完整探测**才返回。

**Swift / demo（Task 8–9）**
- 用例从不驱动真正的 `-openURLString:` 这条分叉（今天它与 `prepare_cache_dir`
  共用一份实现，所以分不出来；将来只改一侧不会有人发现）。R7 的 N2d 变异已经
  给这条补上了一条真驱动它的用例，但"两侧共用"这个前提本身没有断言守着。
- `add() -> Bool` 对"非空但非法的 URL"也返回 `true`，失败要到
  `statistics.failed` 才看得见（已写进文档 + 补了契约用例，没改返回值——判它要把
  dl 层的 scheme/HLS 判据在 Swift 侧再转写**第三份**）。
- demo 的 HLS 分片**不是可解码的 fMP4**，那条 m3u8 不能拿去播放页。
- 样例服务器每次换端口 ⇒ **跨进程没有缓存命中**可演示。
- `activeTasks` 的可观测性**依赖样例服务器那 250ms 的人为延迟**：不加延迟时三条
  URL 从 `add` 到全部暖够只要 110ms，比 300ms 的轮询周期还短，这一栏永远是 0。
- 预加载演示页**没有新增 XCTest**（无 host 的逻辑测试 bundle 里 `UIWindow` 不可
  构造，known-gaps #67/#68），代之以自检入口 + 四条变异。

## M6d 收尾登记（Task 8，2026-09-21）

### 1. `render_cb` 反汇编实测（对着旧注释订正数字，不是重新起个话题）

`src/platform/apple/audio_unit_sink.mm:92-103` 的注释是这次重测的权威文本，
这里原样引一遍以免两处漂移：

> 【M6d Task 4 重测，2026-09-21】数的是
> `syp::platform::(anonymous namespace)::render_cb(void*, unsigned int*,
> AudioTimeStamp const*, unsigned int, unsigned int, AudioBufferList*)`
> 这个符号（用 `otool -tV audio_unit_sink.mm.o | c++filt` 还原可读签名之后
> 按函数边界数）：
> - Debug（`build/`，-O0）：**10 条 bl**。M3c 那版之前的旧注释写的是 4 条——
>   M6d 新增的增益/斜坡逻辑在 -O0 下没有被内联（`std::atomic<float>::load`、
>   `AudioUnitSink::apply_gain` 本身、`std::atomic<bool>::exchange` 等各自
>   多出一条 bl），4 → 10 是真实增长，不是测错。
> - Release（`build-rel/`，-O2）：仍是 **2 条 bl**（`AudioRing::read` /
>   `_bzero`），与 M6d 之前记的数字**巧合一致**——`apply_gain()`/
>   `advance_gain()` 都是 static 纯函数、跟 `render_cb` 同一个 TU，-O2 下
>   被完整内联进 `render_cb` 本体，一条调用指令都不剩。

两种情况下都零分配、零锁、零 ObjC 消息、零 FFmpeg、零日志——这条护栏没因为
这次改动被打破。

### 2. 新的假结果机制：**变异本身被 `-Werror` 挡住，编译不过**

CLAUDE.md 长版记了六种；本里程碑 Task 1 的变异 3 撞见了第七种，与机制 #2
（构建失败但测试照跑旧二进制）相邻但不是同一件事——这次是**变异改动本身
让构建失败**，而不是"另一处改动让构建失败、测试跑了旧二进制"。

计划要求的变异写法是"删掉 `normalize_rotation` 的一次调用、丢弃 `rot`
参数"，字面实现会产出一个未使用的形参，在本仓 `-Werror -Wconversion
-Wsign-conversion -Wshadow` 的编译矩阵下触发 `-Wunused-parameter`，整个
目标编译失败——如果这时候没有显式检查构建退出码（CLAUDE.md 机制 #2 的
`BUILD_OK` 闸），`ctest` 会照跑上一个（未变异的）二进制，**全绿**，看起来
像"这条变异也被杀了"，实际上这条变异从未被真正构造出来过。改写成保留一次
`normalize_rotation` 调用 + `(void)rot` 才能在编译矩阵下存活，审查判定这样
改写没有削弱变异的检测力（删掉的是这次调用的**结果**是否被使用，不是删掉
调用本身）。

**教训**：`-Werror` 不只挡生产代码的疏忽，也会挡"变异写法过于字面"这件事
本身；变异验证的通用步骤（改完先看构建退出码、再看红不红）在这种情况下
要多问一句"这条变异到底有没有编译进去"，不能假设"改了源码就等于构造出了
那条变异"。

### 3. 计划给的 `.o` 路径/通配少一层，照删会删空

计划与本任务书里给的 `.o` 清理指令普遍写成"删 `<target>.dir/` 下匹配
`<文件名>.o` 的那个",但实测两处系统性偏差：**路径少一层 `src/`**
（真实路径带一层 `src/` 前缀，计划写的路径对不上）、且
`<target>.dir/*.o` 的浅层通配**漏了 `support/` 子目录**（测试辅助源文件
如 `tests/support/synth_media.h` 编译产物落在 `<target>.dir/support/*.o`
下，浅层 glob 找不到）。按计划字面执行会静默删空（`rm` 一个不存在的路径
不报错），旧 `.o` 原样留着，下一次 `make` 判定"不需要重编"，正是 CLAUDE.md
机制 #1（mtime 粒度）的另一种触发方式，只是根因换成了"根本没打算删的
文件"而不是"改完立刻重编"。

**统一改法**（T3 顾虑 5 已订正到 task-4/5-brief.md，此处收尾确认）：一律用
`find <target>.dir -name '<文件>.o' -print -delete`（`-print` 在 `-delete`
之前，保证输出里能看到到底删了什么），并且**确认删到了东西**（输出非空）
才继续跑变异；不再手写猜测路径。

### 4. `current_blit_params()` 逐个 `load` 四个独立原子量

`MetalRenderer` 没有 `mu_`（见设计 §8.3 P6），几何参数（SAR/旋转/gravity）
存成四个独立的 `std::atomic`，`current_blit_params()` 依次 `load()` 它们
再传给 `blit_transform()`。

**线程契约（终审 M1 统一口径，`metal_renderer.h`/`track_player.h` 同一句）：
`set_source_geometry()`/`set_gravity()` 与 `present()` 由调用方串行。** 当前
由桥的 `_core->mu_` 保证——`-setVideoGravity:`、`TrackPlayer::create()` 与泵
线程的 `step()`（进而 `present()`）都持 `mu_`；测试缝
`-debugSetSourceGeometryForTest` 在首帧之后调用，安全也正是因为它持 `mu_`。
（旧版此处写"`set_source_geometry()` 只在首帧 `present()` 之前调一次"，与测试缝
的真实调用时机不符，不变量从来就是"串行"而不是"首帧之前"。）

四个原子量只是防御，不是允许并发的依据：如果将来出现**不能与 `present()`
串行**的调用方（例如在 UI 线程直接改几何、或播放中途切流改几何而不经 `mu_`），
四次独立 `load` 之间可能读到新旧混合的组合（比如新的 `rotation_deg` 配旧的
`sar_num`），产出一帧错误的构图。届时要把四个量打包成单个原子（比如一个
`packed BlitSource` 结构体整体 `store`/`load`）整体替换，而不是给四个字段
各自再加同步。

### 5. `probe_e2e` 在 `ctest -j4` 下的负载抖动（本轮再次观测，非新问题）

T5 fix round 1 复审时观测到全量 `-j4` 跑一次 `probe_e2e` 抖动一次，单独
重跑与再跑整套均通过。Task 8 本轮全量验收 `ctest --test-dir build -j4`
34/34 一次通过，未复现，但既有登记（"检出为概率性，非确定性构造"一节、
`test_probe_e2e` 在并发负载下概率性变红那条"已解决"记录）覆盖的正是这一类
——`probe_e2e` 覆盖 dl 层 + 真实回环 HTTP 服务器的时序敏感场景，在高并行度
下与其它测试目标抢 CPU/网络栈资源时偶发超时/失败，重跑通常过，**不是
M6d 引入的新缺陷**，归入本文件「负载敏感与种子敏感的测试」一节同一类。

### 6. `xcodebuild test` 标准命令缺签名参数——已补进 `CLAUDE.md`

任务书/计划里所有 `xcodebuild test` 命令都缺
`CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY=""`，
照原样跑会 `EXIT=65`（签名错误，不是用例失败），Task 6 首次踩到。已把
known-gaps #68 验证过的完整命令写进 `CLAUDE.md`「跑测试时的三个已知坑」
第三条，往后不用每次任务书里重复交代。

### 7. 工具债：`generate_xcodeprojects.rb` 每次运行重排全部 UUID

`xcodeproj` gem 的随机 UUID 让每次重新生成的 pbxproj 与上一版相比**全文
铺满 diff**（M5 实测约 953 增/953 删，M6d Task 6 约 1090 行）——两个
pbxproj 的改动因此几乎不可 `git diff` 审查，`ruby demo/generate_xcodeprojects.rb
--check`（第五道闸）是唯一可信的判据，不能指望"看 diff 就能看出改了什么"。
与 M4 收尾登记 #1 是同一条债的延续，本轮不修（需要给生成器接入确定性 UUID
生成器，是独立的工具投入），登记备查。

### 8. 桥的装饰层几何转发——已有覆盖，收尾确认不是新缺口

`PresentTrackingRenderer`（`swift/SYPlayerKit/Internal/SYPBridge.mm`）转发
`set_source_geometry`/`set_gravity` 给 `MetalRenderer` 这两跳，Task 6
审查 Important 1 发现过 gravity 那一跳的变异存活，fix round 1（`ee312c9`）
已经补上装饰层透传 `debug_blit_to_bgra()` + 两条经装饰层的像素回读用例
（`SYPlayerRendererForwardingTests`），gravity/几何两条转发变异各自只红
预期用例。本轮全量验收确认这两条测试仍在（`testGravityChangeReachesMetalRenderer`/
`testSourceGeometryReachesMetalRenderer`），**不需要**再补记进 known-gaps #24
——B.5 里"若 Task 6 审查后仍无覆盖才补记"的条件不成立。

### 9. 各任务 deferred minor 里仍然成立的几条（挑仍有效的，已被后续任务修掉的不重复）

- `blit_transform()` 里 `normalize_rotation()` 算两次（`display_size()`
  内一次、`rot_quadrant` 计算一次），计划原文如此，无害但确实是重复计算
  （Task 1）。
- ~~Task 2 修复轮重写旋转用例之后，变异 #2（SAR 恒为 1）/#3（删掉
  `displaymatrix` 判断分支）未重新跑过~~——**终审修复波已重跑（SAR 取法改为
  `av_guess_sample_aspect_ratio()` 之后）**：#2 红在 `track_info_reports_sample_aspect_ratio`、
  `track_info_reports_container_level_sample_aspect_ratio`、
  `open_pushes_source_geometry_once_for_non_square_pixels`；#3 红在两条
  `track_info_reports_display_rotation_*` 与 `open_pushes_source_geometry_once_for_rotated_video`。
  均为预期用例。
- `synth_video_with_sar` 的整除防呆分支无覆盖（测试辅助代码本身的盲区，
  不影响生产正确性，Task 2）。
- `create_pushes_default_gain_and_gravity_to_the_new_instances` 与
  `make_fixture_with_audio_and_video` 之间约 15 行装配重复（fixture 硬编码
  `FakeRenderer` 类型），Task 5，纯冗余不影响正确性。
- 无用例覆盖"视图在 `attach` 且 `open()` 之前就设了 `gravity`"这个组合
  顺序（两条路径——先 attach 后设 gravity、先设 gravity 后 attach——各自
  有覆盖，但没有覆盖交叉顺序），Task 6。
- 像素回读用例依赖 `sample.mp4` 首帧在若干采样点附近非黑（经验核对过，
  见测试文件注释"ffmpeg 缩到 8×4 看过"），换素材需要重新核对这些采样点，
  Task 6/7 共用同一份假设。

## 已从清单删除（过期条目）

- ~~`syp_ffmpeg_smoke` 默认参与 `ALL`~~：那个目标在 Task 10 删
  `smoke.cpp` 时已经消失（见 `tools/syp_probe/main.cpp` 第 50 行的
  注释），条目本身已经没有对应的代码，留着只会让人去找一个不存在
  的东西，故删除不再登记。
- **`SYPLAYER_SKIP_RACE_SENSITIVE_TESTS` 的跳过范围偏大**（`swift/SYPlayerKitTests/SYPlayerLifecycleTests.swift`
  的 `testOpenLeavesPlayerPausedWithNoPlaybackBeforePlay`）：这个开关是为 known-gaps #70 那条已接受的
  pump-hop 竞争设的，但它跳掉的是整条用例——包括两条与调度无关的断言（`.paused` 本身、300ms 不推进探针）。
  CI 一旦打开它，真正的回归探针也跟着没了。修法：只跳过对时间敏感的那一条断言，保留其余两条。
- **`play()` 清 `pendingError` 重新打开了一条窄窗口**（`swift/SYPlayerKit/SYPlayer.swift`，终审复审 Minor 1）：
  `open()` 还挂在 await 上时若当前会话的泵错误先到，`handleError` 会把它暂存；此时主线程上的一次 `play()`
  会把暂存清掉，于是回到 fix round 3 修掉的 N1 症状（静默冻结、无 `.failed`、无 `didFailWith`）。
  `seek()` 早就有同形隐患。两个 demo 页都用 `state.hasMedia` 挡住了 Play 按钮，碰不到；彻底的修法与
  known-gaps #72 一样，是给快照加错误字段，由 `refresh()` 派生 `.failed`。

## M6a 收尾登记（Task 6，2026-09-21）

### 1. `RateLimiter` 是 dl 层第二个自带线程的组件；单例故意泄漏

在 `Preloader`（M5 #5）之后，`RateLimiter` 是 `src/dl` 里第二个自己起
`std::thread` 的组件——唤醒线程睡到"最早一个等待者的阈值可被越过的时刻"，
醒来后在锁外依次调用到点订阅的 `wake`。与 `Preloader` 同类的生命周期与
测试负担（需要独立实例 + 注入时钟 + `pump_for_test()` 才能在 ctest 里做到
确定性，不能靠真线程 sleep 验证）。这是继 M5 #5 之后 dl 层第二次踩同一类
先例：若将来要把 dl 层移植到没有 `std::thread`/pthread 的环境，`Preloader`
与 `RateLimiter` 是仅有的两处要重写的地方。

`RateLimiter::instance()` 用 `static RateLimiter* p = new RateLimiter(...)`
**故意泄漏**，不走静态析构：spec §4 的理由——静态析构期 `join` 一个可能
正在回调某个已被销毁的 `Scheduler` 的线程，是经典的静态析构顺序陷阱；
进程退出时不需要这份清理。**测试用的独立实例**（`start_thread` 为真时）
析构则正常 `notify` + `join`，这条差异是有意的，不是遗漏——生产单例与
测试实例走两条不同的析构路径，头文件注释已写明。

### 2. 订阅式唤醒取代一次性 `Waiter`；毫字节记账的精度理由

spec §3.1 原文设计的是"每次被拒创建一个一次性 `Waiter`"，写计划时（提交
`4f58503`）改为"每个调用方构造时订阅一次，之后只 `arm()`"。理由是两种
自锁：① 若持 `Scheduler::mu_` 析构 `Waiter`，它等的回调正卡在拿同一把
`mu_` ⇒ 死锁；② 若回调自己跑进 `schedule()` 发现不必再等、去析构自己的
`Waiter` ⇒ 自己等自己。订阅对象只在 `~Scheduler` 开头、不持任何锁时析构，
两种死锁都不存在——这是 brainstorming 定稿之后、写实现计划时才发现的
问题，spec 正文的代码块本身没有改，只在附近加了 `> ⚠️ 落地时已改` 标注
（M6a Task 1）。

**毫字节记账**（`balance_mb_` = 字节 × 1000）：补充公式是
`balance_mb_ += R × Δt`（字节/秒 × 毫秒 = 毫字节，无除法无舍入）。精度
理由——若直接按字节记账，R = 500 B/s 时每毫秒只该回补 0.5 字节，整数
除法会把它截断成 0，唤醒线程按"每毫秒回补量"算出的到点时刻会系统性地
晚于真实值；改成毫字节记账后这类低速率场景不再有舍入误差，到点时刻的
"`+1`"这一位是唯一需要的保守量（证明见 `rate_limiter.cpp` 注释）。

### 3. M6a Task 5 变异 2 是等价变异；I1（C 入口 `bad_alloc`）无可变异用例

- **Task 5 变异 2**（Swift setter 去掉 `v > 0 ?` 的负数夹取，让负数原样
  传下去）：`EXIT=0`，106 个 XCTest 全绿，**如实记录为等价变异**——C 层
  `syp_rate_limit_set` 自己已经把负数夹成 0（Task 3/4 落地的行为），
  Swift 侧少了这层夹取，消费者（`rateLimitInCLayerForTest`）读到的仍是
  0，测不出来。这是**两层夹取**中的外层被删掉，内层（C 层）兜底了它，
  不是用例设计缺陷，按任务书预告不为它硬造用例。
- **I1（`syp_net_api.cpp` 的 `RateLimiter::instance()` 未兜 `bad_alloc`）**：
  修复轮 1（提交 `9da357c`）加了 `try { … } catch (...) {}`，但**没有**
  对应的可变异用例——要在测试里真的触发 `bad_alloc`，唯一现实的办法是
  全局重载 `operator new` 让它按需失败，这会污染整个测试二进制里所有
  线程、所有其它用例的堆分配（含 tiny_test 框架自身），风险与成本远高于
  I2（fail-open）用的线程注入点（后者只影响调用它的这一个 `RateLimiter`
  实例）。这条修复的正确性依据是**结构性的**（`try/catch` 语法上能截住
  `instance()` 抛出的任何异常）和**先例一致性**（与 `syp_preload_api.cpp`
  同一套降级口径），不是靠用例钉住的，登记为无覆盖路径。

### 4. 负载敏感用例（看到红先查这里，与 M5 #8 同一类目录）

- **`playback_outpaces_preload_under_a_shared_limit` 的 `pre < R` 断言**
  （`tests/test_preloader.cpp`）：负载敏感——只有播放读线程被饿住
  ≳0.5 秒、桶回到半满以上时，预加载才可能合法地拿到一片，届时这条断言
  会假红。实测：普通构建 ×3、并行 `ctest -j8`、TSan 下都是 `pre == 0`
  稳定；一次合法准入就能让预加载下满一整个 `R`（分片粒度决定），所以
  "假红"的方向是**误报缺陷**，不是"漏杀变异"。
- **G2（`playback_outpaces_preload_under_a_shared_limit` 第二阶段——预加载
  条目改 Playing 后的比值区间 `[1/3, 3]`）**（终审 M4 订正：原先误记在
  `throughput_under_a_limit_tracks_the_rate` 名下，那条用例断的是 R·t 容差、
  没有比值区间）：受分片上限与并发数影响——每次准入按整片 `R`
  分配，粒度越粗，比值越容易成片跳动（3 秒窗口实测过 2.00 这种整数比）。
  当前实现下 3 次连跑、`-j8` 负载、TSan 均稳定在 1.33，但**改分片上限
  或并发配置之后必须重新评估这个区间**，不能假设它是永久成立的不变量。

### 5. 既有：`dl_task.cpp:253` 与 `scheduler.cpp:152` 直接 `fprintf(stderr)`

与 `log_msg` 惯例不一致，M6a Task 1 复核时确认是既有问题、非本里程碑
引入。`RateLimiter` 自己的所有日志（WARN 级别的 fail-open 提示等）都走
`log_msg`，没有重复这个问题；下次动这两行时顺手改掉。

（行号按 M6a Task 6 修复轮 1 时的 HEAD `b1993a8` 核对：`scheduler.cpp` 的
这一行原记的是 Task 1 时期的 132，Task 2 在析构处新增约 20 行代码后实际
挪到了 152——`grep -n fprintf src/dl/scheduler.cpp src/dl/dl_task.cpp` 复核
过，`dl_task.cpp:253` 未变。这类行号引用天然会随后续改动漂移，读到这条
的人如果发现又对不上了，直接用 `grep -n fprintf` 重新定位，不必假设行号
本身是权威来源。）

### 6. 其余各任务 deferred minor 中仍然成立的

- **睡眠中被 `debit` 推后、早醒重算无用例**（Task 1）：唤醒线程睡到算好
  的到点时刻，若睡眠期间又有扣账使阈值推后，它醒来时会重新判定、按新
  余额重算时刻再睡——这条"早醒不漏醒"的行为只有代码逻辑与注释保证，
  没有专门的用例覆盖（性质上很难在不注入线程调度钩子的情况下做成
  确定性用例）。
- **`pump_for_test()` 与唤醒线程同时派发只靠注释约定**（Task 1）：
  `fire_due()` 假定同一时刻只有一个派发线程，`pump_for_test()` 只应
  用于不起线程的独立实例（`start_thread=false`），这条前提没有代码
  层面的断言或锁去强制，只在头文件注释里写明。
- **析构与 `arm` 并发无 TSan 覆盖**（Task 2 顾虑 1）：`~Scheduler` 把
  `sub_` 在 `mu_` 下移出、放锁后再 `reset()`，理由是防止裸 `reset()`
  与另一线程 `mu_` 下的 `sub_->arm()` 发生数据竞争；但现有的 async 用例
  全部用单例、`R == 0`，`arm()` 根本不会被触发，所以 TSan **没有**实际
  覆盖到这条竞争，只有构造上的论证（复审 Approved，接受这个覆盖缺口）。
- **`test_scheduler.cpp` 用例 5（`unlimited_scheduler_behaves_as_before`）
  复刻基线字面期望而非对比**（Task 2）：断言的是硬编码的分片序列，不是
  "与关闭限速前的行为逐位相同"这种对比式断言，鉴别力略弱但不影响正确性。
- **`test_source_bridge` 桥关闭后 set/read 类别只证不崩**（Task 3）：
  没有断言关闭后再 `set_rate_class`/读类别的具体返回值语义，只验证不
  UB/不崩溃。
- **demo 限速档位映射表只经自检间接验证一档**（Task 5）：预加载页的
  `rateControl` 四档（不限/256 KB/s/1 MB/s/4 MB/s）里，`SYPLAYER_DEMO_
  PRELOAD_AUTORUN=1` 自检只切换验证了"1 MB/s"与"不限"两档，其余两档
  没有自动化断言，只在人工点击时能看到状态行数字变化。

## M6c 收尾登记（Task 5，2026-09-22）

### 1. ~~被 health 判坏的 Slot 若在 cancel 落地前自己真失败，会被计两次错~~（已修，M6c 终审 I1b）

`health_check()` 判挂死（或按规则计错的零速慢替换）时已经 `++consecutive_errors_`，
随后在锁外 `cancel()`；设计上被杀的任务稍后回 `on_task_finished(SYP_ERR_CANCELED)`，
走 canceled 分支只重调度、不计错（spec §3.3 第 5 步）。但若在"判坏"与"cancel 落地"
之间，后端恰好先以**真实错误**（例如 `SYP_ERR_TIMEOUT`，后端自己的超时也快到了）
回调了 `on_complete`，`on_task_finished` 走的是非 canceled 分支——它不知道这个 Slot
已经被 health 算过一次，于是**同一次失败计两次** `consecutive_errors_`。

- 自 Task 3 起即存在（Task 4 复审登记，Task 5 登记未修）；
- 概率低：窗口只有"health 判坏 → 锁外 cancel 落地"那一段；
- 后果有界：至多让 fatal 提前一次。原稿写"`max_consecutive_errors` 默认 5"不准
  （M6c 终审 M3 订正）：5 只是 `SchedulerConfig` 的字段默认值；生产路径经
  `SourceBridge` 构造调度器时 `max_consecutive_errors = max_retries`（`syp_config`
  默认 **3**，≤ 0 时才退回 5，见 `source_bridge.cpp`），所以提前一次在默认配置下
  是 3 次里占 1 次。

**已修**（M6c 终审 I1b）：`Scheduler::Slot` 加 `killed_by_health`（受 `mu_` 保护），
health 判坏（挂死或慢替换）时与 `dead` 一起置位；`on_task_finished` 见到它就把任何
非 OK 终态当 `SYP_ERR_CANCELED` 处理（只重调度、不计错）。确定性用例
`health_victim_failing_before_its_cancel_does_not_count_twice`：同一轮杀两条，第一条
的 cancel 被 `DeferCancelBackend` 扣下，扣下的钩子里让第二条（尚未被 cancel）以
`SYP_ERR_TIMEOUT` 结束；变异"忽略该标志"⇒ `consecutive_errors 3 ≠ 2`，只红这一条。

### 2. `HealthTicker` 是 dl 层第三个自带线程的组件；单例故意泄漏

继 `Preloader`（M5 #5）、`RateLimiter`（M6a 收尾登记 #1）之后，`HealthTicker`
（`src/dl/health_ticker.{h,cpp}`）是 `src/dl` 里第三个自己起 `std::thread` 的组件：
懒启动（首次 `arm_after` 或 `warm_up()`），睡到最早一个 armed 订阅到点、锁外逐个回调。
生命周期与测试负担与前两者同类（确定性用例靠 `start_thread=false` 的独立实例 +
注入时钟 + `pump_for_test()`）。将来移植到没有 `std::thread` 的环境时，要重写的
地方从两处变成三处。

`HealthTicker::instance()` 与 `RateLimiter::instance()` 一样 `new` 出来**故意泄漏**，
不走静态析构（静态析构期 `join` 一条可能正在回调某个已销毁 `Scheduler` 的线程是
经典顺序陷阱）。测试用的独立实例析构照常 `notify` + `join`——两条析构路径的差异是
有意的。线程起不来时 fail-open = **检测关闭**（WARN 一次），下载行为退回 M6c 之前
（spec 决策 11）。

### 3. 存活的变异：等价或无法确定性构造（逐条如实列出）

- **Task 3 变异 8**（`health_check` 早退条件去掉 `paused_`）：**等价**。`pause()` 在同
  一个 `mu_` 临界区里置 `paused_` 并把所有 Slot 标 dead，暂停期间 `schedule()` 不建新槽，
  `arm_health_locked` 自己也查 `paused_` ⇒ `paused_` 为真时不存在可判定的 Slot，早退里的
  `paused_` 只是纵深防护。
- **Task 3 修复轮变异 F**（删掉 `dtor_idle_locked` 里对 `deferred_release_` 的 in_cb
  检查）：**存活**。杀它需要 `~Scheduler` 与后端线程上某个 deferred Slot 的迟到回调
  真实并发，没有可注入的钩子，无法确定性构造。它是与 `reap_except` 同一条不变量
  （不在 Slot 自己的回调栈上拆它）的防御性对称。
- **`HealthTicker::ensure_thread` 的 NB3 守卫**（`thread_started_`，镜像
  `RateLimiter` NB3）：**无确定性用例**。竞态本身要两条线程在 `call_once` 内外精确
  交错；正确性依据是与 `RateLimiter` 同构（该处已过 M6a 多轮复审）。
- **Task 3 变异 C / D**（删 `schedule()` 的两个 `SlotRelease` / 删 `cancel_all_tasks` 的
  `SlotRelease`）：**被杀，但只经测试缝** `deferred_release_count_for_test()` 的计数
  （`1≠2`、`2≠4`、`5≠11`）。行为层面的后果（ticker 线程成为 `DLTask` 最后持有者、被违约
  后端卡死）没有端到端可观测形式。

### 4. 负载敏感用例（看到红先查这里，与 M5 #8、M6a 收尾登记 #4 同一类目录）

- **`slow_loris_connection_is_replaced_end_to_end` 的 20s 耗时上界**
  （`tests/test_source_bridge.cpp`，本任务新增，只在 APPLE 下编译）：真实时钟 +
  真实 Apple 后端 + 进程单例 `HealthTicker`。正常路径实测约 4.8s（替换发生在约
  3.2s：慢探测请求只送出 16 KiB）；没有替换时慢区间要约 38s。机器被重度占用时，
  快连接（回环节流到约 320 KiB/s）可能来不及在一个速度窗口里被采样，基线形成推迟，
  替换也随之推迟——**假红的方向是"超时没读完"**。看到它红，先单独重跑
  （`build/tests/test_source_bridge`，cwd 无要求），确认红的是 `in_time` /
  `elapsed_ms` 这几条而不是字节比对，再下结论。
  终审修复轮把它的看门狗从软 25s / 硬 30s 抬到软 35s / 硬 40s（25s 离 20s 截止太近；
  `Watchdog` 的硬超时是从起点算的绝对值、第二段等 `hard − soft`，所以硬值必须一起抬），
  仍低于 ctest 的 60s。
- 该用例的变异验证里值得记一笔：brief 原定的 `total_requests() >= 4` **单独杀不掉**
  "`HealthConfig::enabled` 默认 false"这条变异（没有替换时探测 + 快片也有 ≥ 4 条
  请求）——真正杀它的是耗时、字节数、`early_close_count() >= 1`、以及"`start == 0`
  那条被客户端断开 / 存在从已收字节处续起的替换请求"这几条。

- **`scheduler` 目标在 TSan 下的耗时**：Task 4 的慢判定用例（每片 3072000 字节 × 3，
  同步桩逐块泵）让 `test_scheduler` 在 TSan 下单独跑到 46.7s，越过了原先的 ctest
  TIMEOUT 45，收尾时全量 TSan `-j4` 在第 64/65 条被判 Timeout（不是挂死，也不是
  TSan 报告）。本任务把 TIMEOUT 抬到 120。看到 `scheduler ***Timeout` 先看它停在哪一条、
  那一条是不是大尺寸用例，再怀疑死锁。

### 5. `test_source_bridge` 在 APPLE 下改链 `syp_platform_apple`（连带 FFmpeg rpath）

为了那条端到端用例，`test_source_bridge` 在 APPLE 下从"只链 `syp_dl`"改为链
`syp_platform_apple`（后者传递带上 `syp_dl`，两者同写 ld 会报 duplicate
libraries），并补了 `BUILD_RPATH`——`syp_platform_apple` PUBLIC 链了 `syp_media`，
运行期要找得到动态 `FFmpeg.framework`，与 `test_preloader` 同一条理由。代价：这个
原本零平台依赖的 dl 层测试二进制在 Apple 上多了一层平台库 + FFmpeg 的链接期/运行期
依赖。其余 24 条用例都不碰真实后端（StubBackend 或裸 socket）。若将来要把它拆干净，把端到端用例挪进
`test_preloader`（已链真实后端）或新建一个 `source_bridge_e2e` 目标即可。

### 6. 判坏后 cancel 落地前收到的 `SYP_ERR_RANGE_UNSUPPORTED` 会被当 CANCELED 吞掉（终审复审登记，不修）

`killed_by_health` 让被健康检查判坏的 Slot 的任何非 OK 终态都按 CANCELED 处理（防双重计错，见 #1）。
副作用：若恰在"判坏"与"cancel 生效"之间，该任务收到 `start > 0` 的 200（`SYP_ERR_RANGE_UNSUPPORTED`），
降级信号被吞，本轮只重调度。下一条任务会拿到同样的 200 再触发降级（或 fallback 关闭时的 fatal），
代价是多一次请求，自愈。窗口极窄（判坏到 cancel 之间），不改。真修：`canceled` 判定里排除
`SYP_ERR_RANGE_UNSUPPORTED`（及 `no_range_signal` 的 200 情形）。

## M6b 收尾登记（Task 3，2026-09-22）

### 1. `Preconnector` 的三处并发防护没有确定性用例

- **I1（Task 1 复审）**：`~unique_ptr` 先把指针置空再调 `~DLTask`，而 `~DLTask` 会等
  在途回调结束——回调里若读 `Entry::task` 会读到 nullptr。修法是回调与析构只用
  构造后不再改的 `task_raw`。竞态要"回收线程正在析构 + 后端线程正在回调"精确交错，
  没有可注入的钩子。
- **`start_returned`**：后端可在 `start()` 内同步回调到 `on_finished`，此时 `done`
  已真而发起线程还在 `DLTask::start` 里；回收条件因此是 `done && start_returned`。
  同样要两条线程精确交错才能构造。
- **`start()` 抛异常**：包了 try/catch，抛时先 `cancel()`（Idle 任务立即以 CANCELED
  结束、置 `done`）再置 `start_returned`、上抛，保证该 Entry 下次调用可回收、不永久
  占在途名额。构造它需要注入分配失败。

三处的正确性依据都是代码走读 + Task 1 复审，与 M6c 收尾登记 #3 的 NB3 守卫同类。

### 2. "206 也 cancel"这条变异在端到端上**存活**，只被桩计数缝杀死

Task 1 修复轮把 `on_data` 的 cancel 限定到非 206（`!range_confirmed()`），理由是
206 路径 cancel 可能让连接不回池、预热白做。M6b Task 3 在端到端上复核：把条件改回
`if (true)`（206 也 cancel）⇒ `preconnect_warms_connection_reused_by_playback`
**仍然绿**（`accepted_connections() == 1`）。原因：1 字节的 body 在 `on_data` 回调时
已经完整收下，此时 cancel 对 URLSession 来说是对一个"数据已收完"的任务取消，
连接照样回池——至少在回环 + HTTP/1.1 上如此。所以这条修复在行为层面**没有被证明
必要**；它只被 `test_preconnector` 的 `range_206_one_byte_is_not_canceled`
（桩的 `cancel_calls()` 计数）钉住。保留它的理由不变：让请求自然结束是最不依赖
后端实现细节的写法（别的后端、HTTP/2 下 cancel 可能发 RST_STREAM 或关连接）。

### 3. `Preconnector::instance()` 是 dl 层第四个故意泄漏的进程单例（但没有线程）

继 `CacheStore`、`RateLimiter`、`HealthTicker` 之后。与后两者不同，它**不起线程**：
已结束的任务在下一次 `preconnect()` 时回收（spec 决策 8），于是"最后一次调用之后
结束的任务"会滞留到进程退出（至多 4 个 `DLTask`，`retained_for_test()` 看得到）。
单例永不析构，所以 `~Preconnector` 的"cancel 全部在途并等待"只在测试实例上跑。

### 4. 端到端用例放在 `test_source_bridge` 而不是计划首选的 `test_preloader`

计划写"首选 `tests/test_preloader.cpp`，实现者核对"。核对后放进
`test_source_bridge`：用例走的是 `syp_source`（`SourceBridge`）读，不是预加载；
该目标自 M6c Task 5 起在 APPLE 下已链真实后端与回环服务器，`TempDir` /
`bytes_match` / 看门狗常量都现成。代价是加深了 M6c 收尾登记 #5 说的那条依赖：
`test_source_bridge` 里真实后端用例从 1 条变 3 条（都在 `#if defined(__APPLE__)`
里）。将来若拆 `source_bridge_e2e` 目标，这三条一起搬。

### 5. 回环服务器 keep-alive 只覆盖两条供体分支

`LoopbackConfig::keep_alive`（默认 false，既有用例零影响）只让**路由命中**与
**合成公式**的 200/206 保活；redirect、路由未命中 404、非 200/206 错误状态、gzip、
`body_file`、`close_after_bytes` 截断、`hang` 在 keep-alive 下仍写 `Connection: close`
并关连接（头文件注释写明）。将来要测"重定向后复用""错误响应后复用"之类，需要先把
对应分支补成 Content-Length 如实 + 返回 keep。另：请求头读取改成了带 carry 的
逐请求切分（空行之后多读的字节留给下一个请求），非保活连接读完一个请求就关，
carry 被丢弃，线上字节与改动前相同——全量 ctest 37/37 为证。

### 6. demo"预连接"按钮打的是 URL 输入框的当前地址（Task 2 复审接受）

不是固定的样例服务器地址：点「用本地样例」后即样例 MP4，也可手填任意 http(s)。
spec §3.5 只要求按钮 + 状态行"已发出预连接"；预连接无可观测结果（known-gaps #110），
状态行只表示"已调用"，不表示"已预热成功"。

### 7. 负载敏感点（与 M6c 收尾登记 #4 同一类目录）

`preconnect_warms_connection_reused_by_playback` 等预连接收尾的轮询上界是 5s
（250 × 20ms，等"服务端记下 1 个请求"且"预连接任务到终态"两件事）。正常路径
毫秒级。重负载下若红在 `srv.total_requests() == 1` 这条 REQUIRE，先单独重跑
`build/tests/test_source_bridge` 再下结论——变异"预连接不发请求"红的也是这一条，
二者只能靠单独重跑区分。

## M6b 最终修复波登记（2026-09-22）

### 8. `preconnectURL:` 补的 `ensure_http_backend_registered()` 调用——变异结果与进程执行方式有关，人工验证点

最终修复波 I1：`SypNetworkBridge.preconnectURL:...`（`swift/SYPlayerKit/Internal/SYPBridge.mm`）
此前不调用 `ensure_http_backend_registered()`，只有 `SypPlayerBridge`/`SypPreloaderBridge`
的初始化路径会调；进程里若先调用 `SYPlayerNetwork.preconnect`、还没创建过任何
`SYPlayer`/`SYPlayerPreloader`，预连接会静默什么都不做。修复：`preconnectURL:` 开头
补一行调用。新增 `SypNetworkBridge.preconnectBackendRegisteredForTest`（读
`current_http_backend() != nullptr`）与 Swift 侧 `SYPlayerNetwork.preconnectBackendRegisteredForTest`，
新增用例 `testPreconnectRegistersBackendLazily`（`SYPlayerNetworkTests.swift`）。

**变异验证（一次一个变异、一次工具调用，按 CLAUDE.md 机制 #4 的纪律）**：把新增的
那行 `ensure_http_backend_registered();` 删掉——

- `xcodebuild test -only-testing:SYPlayerKitTests/SYPlayerNetworkTests`（只跑本测试类，
  6 条）：**变异被杀死**——`testPreconnectRegistersBackendLazily` 报
  `XCTAssertTrue failed`（`SYPlayerNetworkTests.swift:64`），`Executed 6 tests, with 1 failure`。
- 同一个变异、`xcodebuild test` 跑全量 `SYPlayerKitTests`（109 条，标准门禁命令）：
  **变异存活**——`Executed 109 tests, with 0 failures`，`testPreconnectRegistersBackendLazily`
  本身 `passed`。原因正是本条修复自己描述的那类进程内状态污染：同一个 xctest 进程里，
  别的测试类（例如经 `SypPlayerBridge`/`SypPreloaderBridge` 打开过素材的用例）先于
  `SYPlayerNetworkTests` 跑到，`ensure_http_backend_registered()` 的 `std::once_flag`
  已经在别处触发过，删掉 `preconnectURL:` 里这行调用之后，全局后端依旧非空，断言照样
  为真。

**结论**：这条用例在**进程执行顺序确定的场景下不可靠**——它只在"预连接是进程里第一次
触达 dl 层的调用"这一前提下能钉住回归，而 Xcode 默认按类顺序在同一进程里跑完整个
target，该前提通常不成立。用例本身不撤（诚实注释已写在 `SYPlayerNetworkTests.swift`
与 `SypNetworkBridge.h`），但**它不能被当作 I1 这条修复的唯一防线**：真正的回归证据
要靠人工核对——`git diff` 确认 `preconnectURL:` 开头确有
`ensure_http_backend_registered();` 这一行，或用
`-only-testing:SYPlayerKitTests/SYPlayerNetworkTests` 单独跑本类（此时进程内没有
先跑的其它测试类，用例可靠）。

## FFmpeg 私有化隔离登记（2026-09-22）

### 1. TSan 下 `probe_e2e` 的 `d_warm_cache_seeks_download_nothing` 字节数断言红过一次

`build-tsan` 全量 `ctest -j4`（改用 SYFFmpeg 之后的第一轮）里，
`tests/test_probe_e2e.cpp:357` 的
`first.metrics.downloaded_bytes + second.metrics.downloaded_bytes == second.metrics.cached_bytes`
失败：`left=8974154 right=9056074`，缓存字节数比两趟下载之和**多出 81,920 字节**
（`second_downloaded=3338058`，`second_cached=9056074`）。同一轮 `time_source` 的
`system_clock_freezes_while_paused` 也因计时红了一次（`t2 - t0 < 55000`，已知的负载
敏感点）。两条单独重跑都通过；整轮 `build-tsan` 全量重跑 37/37 通过。

判断：更可能是缓存写入与指标快照之间的竞态——快照取 `downloaded_bytes` 时，已经落进
缓存、计入 `cached_bytes` 的那部分数据还没计入下载计数（或者反过来，统计时机不一致），
TSan 下线程调度被拉长后才暴露。与这次的符号改名无关：改名只把 FFmpeg 符号换成
`syp_` 前缀，不碰 dl 层的计数与缓存逻辑。

**观察**：这是一条不变量性质的断言（"绝不重下已有数据"），不是纯计时用例，不能按负载
敏感一律放过。再红时要记下两个数的差值是否总是整块（81,920 = 5 × 16 KiB）、是否只在
TSan 下出现；复现两次以上就要查指标快照的取数时机。
