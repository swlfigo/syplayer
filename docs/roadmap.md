# 路线图

M = Milestone。每个里程碑都有独立的验收标准，不达标不进下一阶段。

---

## M1 — dl 层 MVP（已完成）

**目标**：能边下边播 + seek 到未缓存位置正确补洞。
**依赖**：标准库 + `syp_http_backend`。**不需要 FFmpeg**（dl 层只跟字节偏移打交道，不解析媒体）。

### 实现顺序与进度

| # | 内容 | 状态 |
|---|---|---|
| 1 | 工程骨架（CMake + C++23 + 自写测试框架）+ `HoleSet` | ✅ |
| 2 | `CacheIndex` / `CacheFile`：索引落盘、CRC32、原子替换、etag 校验 | ✅ |
| 3 | `DLTask` + 内存桩后端：状态机 / 重定向 / 重试续传 / 速度估计 / 取消 | ✅ |
| 4 | NSURLSession 后端 + 零依赖回环 HTTP 测试服务器 | ✅ |
| 5 | `Scheduler`：洞→分片、并发决策、连接复用启发式、Range 降级、重调度 | ✅ |
| 6 | `SourceBridge`：对外 `syp_source_read/seek`，串成闭环 | ✅ |
| 7 | headless 验证工具 | ✅ |

### headless 验证工具

不依赖现成播放器就没有端到端验证环境。解法是写个 ffprobe 式的小程序：
走我们的 `AVIOContext` 调 `avformat_open_input` + 循环 `av_read_frame` + `av_seek_frame`，
**不渲染不解码**。约两百行，能把 dl 层的正确性全测了。

体验类指标（首帧时间、卡顿率）等 M2 有播放器再测；**正确性可以现在就验证**。

**依赖已就绪**：`tools/build-ffmpeg.sh` 已产出 FFmpeg 8.1.2 的
`FFmpeg.xcframework`（动态 framework，三个 slice，iOS 13.0 / macOS 11.0），
并实跑验证过——demuxer 只编进了 mov/mp4，h264 / aac 解码器就位，
`avio_alloc_context` 可用。产物落 `build-ffmpeg/`，不进仓库。

**已交付**：`syp_probe`（可执行，可直接打真网址）+ `test_probe_e2e`
（八条验证矩阵，进 ctest）。口径是「同一份 mp4，一条走 file: 协议，
一条走 syp_source，逐 packet 比对元数据与 payload 的 SHA-256」。
素材由 `tools/gen-fixtures.sh` 随机生成、带种子可复现，不进仓库。

**一条更深的教训（本阶段实测撞见过一次真实死锁才发现的）**：看门狗对
FFmpeg **从不再轮询 `interrupt_callback`** 的内部同步阻塞点天生够不着——
主线程卡在 `syp_source_read` 内部时（例如 `SourceBridge::read()` →
`apply_window()` → `Scheduler::reap_except()` → `~DLTask` 这条析构等待
链），`request_abort()` 发出去了，却永远等不到下一个检查点来读它。
`test_probe_e2e` 的看门狗因此在 `scenarios.cpp` 里额外给 `close()` 兜了
一层独立的有界等待（`bounded_call`）——这不是替代方案，是承认"加宽
看门狗覆盖范围"这条思路本身**必要但不充分**：任何同步阻塞点，只要不
主动轮询 interrupt，都天生在看门狗的可达范围之外，得靠"调用方给自己
一个超时"这类外层兜底，不能指望内层的中断机制万能。完整诊断见
`docs/2026-09-08-apple-backend-deadlock.md`；
那次死锁的根因（Apple 后端单线程自等）已在 `ed1da0e` 修复。

### 验收标准

- [x] 全部单测可用内存桩跑通，**不依赖真实网络**
- [x] seek 到未缓存区间能正确补洞，不重下已有数据（调度器层已验证）
- [x] 进程重启后能从磁盘索引恢复，续下而非重下
- [x] 服务端不支持 Range 时能降级到单连接 —— **已达标**（`1e6c1c1`）。
      - 原判：端到端矩阵证伪，实测 `peak_concurrent=3`、流量
        4.04×文件大小，不是「降级到单连接、≈1×流量」。
      - `docs/known-gaps.md` #15 的**两半现已都修**：
        第 2 半（判定丢弃 200 响应体时用 `backend_->cancel` 掐断连接，
        `1cfb015`）把流量从 4.05× 压到 ~1.1×；第 1 半（`Range` 支持三态
        化，探明之前并发锁 1，`1e6c1c1`）把并发峰值压到 1。
      - 实测（端到端场景 E，31228460 字节 fixture）：
        `peak_concurrent_requests()` **4 → 1**、`requests` **5 → 3**、
        流量串行 1.00~1.13×。16 路并行连跑 n=1040：`peak==1` 占 96.3%，
        余下 3.7% 的 `peak==2` 是**服务端记账滞后**（`cancel` 异步，被
        cancel 的残条 `early_close=1` 还挂在 `RequestGuard` 里），不是
        客户端真开了两条。确定性证据在单测层：
        `tests/test_scheduler.cpp` 的
        `no_range_source_stays_single_connection` 在同步桩下断
        `active_task_count()` 峰值恒为 1，100% 确定。
      - 正常源（支持 Range）**零回归**，`delay_ms=100ms` 模拟 RTT、n=8：
        首 4 MiB 中位 453ms → 447ms，全文件中位 3306ms → 3119ms，
        `peak_concurrent` 仍到 3。差异全在噪声内。
- [x] etag / Content-Length 变化时能识别源文件已变（`SYP_ERR_CONTENT_CHANGED`）——
      etag 变化：`test_source_bridge.cpp` 的 `etag_change_is_content_changed`
      + 端到端场景 H（`h_etag_change_is_detected`，直接断言
      `error_status == SYP_ERR_CONTENT_CHANGED`，`on_error` 回调已接入
      probe）双重验证。Content-Length 变化：仅 `test_source_bridge.cpp`
      的 `content_length_change_is_content_changed` 验证，**未纳入端到端
      矩阵**（八条场景里没有一条覆盖它）。
- [x] `tools/check-abi.sh` 全绿

### M1 判定

**通过。** 七步交付全部完成；六条验收标准全部达标。

判定沿革（写全，免得以后只看到结论）：
1. 端到端矩阵第一次跑通时，第 ④ 条被**证伪**——实测 `peak_concurrent=3`、
   流量 4.04×，判定「有条件通过」。
2. `known-gaps` #15 第 2 半修复后流量降到 ~1.1×，但 `peak_concurrent` 没变。
   当时明确拒绝了"数字好看就改口"：④ 是一条关于**并发形状**的主张，不是
   关于流量倍数的主张，所以维持「有条件通过」、这一条不勾。
3. 第 1 半修复后（`1e6c1c1`，Range 支持三态化）`peak_concurrent` 端到端
   实测 **1**，单测层 `active_task_count()` 峰值恒为 1（确定性）。
   **行为本身**变成了契约写的样子，这一条才勾上。

判定改口的依据是并发形状变了，不是流量数字变好看了——第 2 步那次拒绝的
理由在第 3 步已经被正面解决。

**遗留的诚实说明**：端到端那条 `peak_concurrent` 断言是 `<= 2` 而不是
`== 1`，因为 `backend_->cancel()` 是异步的，被 cancel 的连接在测试用
`LoopbackServer` 那边还会挂一小会儿（16 路并行下 3.7% 的运行里会看到
`peak==2`）。「恒为 1」这个更强的主张由单测层的同步桩用例保证。
两条证据合起来才构成 ④ 的达标依据，单看端到端那条是不够的。

**其余仍然成立的保留**：⑤ 的 Content-Length 变化只有单测覆盖，未进端到端
矩阵；`known-gaps` #1（异步交付下与在途请求重叠的冗余请求）仍写着「M2 接
真实播放前应当处理」。这两条与 ④ 无关，不因本次改判而消失。

---

## M2 — 播放内核 MVP（已完成）

**目标**：能出画。软解 + 最简同步 + Metal 渲染，跑通一条完整链路。

**拆分**：M2 的七个组件按**验证方式**拆成两半——纯 headless 可验的（解码管线）
先做完，需要真实设备才能验的（播放/同步/渲染）另立一个里程碑：

| | 组件 | 验证方式 | 状态 |
|---|---|---|---|
| **M2a** | `Demuxer` / `Track`+`PacketQueue` / `Decoder` / `FrameQueue` + `Pipeline` 编排 | 完全 headless：逐帧与 FFmpeg 自己的解码结果全等 | ✅ |
| **M2b** | `TimeSource`+`TrackPlayer` / `MetalRenderer` / `AudioUnitSink` | 需真实设备：能播、同步、暂停恢复倍速 | ✅（有条件，见下「M2b 判定」） |

**裁定**：M2 原验收标准「音画同步无明显漂移」需要音频输出，而 roadmap
原把 `AudioUnitSink` 排在 M3——已裁定把最小 `AudioUnitSink` 提到 M2
（落在 M2b），否则该标准悬空，`TimeSource`/`TrackPlayer` 在 M2 阶段
没有真实对手可对齐。M3 保留的是 `AudioUnitSink` 之外的完整同步收尾
（缓冲水位、卡顿判定）与 `VTDecoder` 硬解。

### M2a 交付物（已完成）
- `Demuxer`：`libavformat` 薄封装，接 dl 层的 `AVIOContext`（`src/media/demuxer.h/.cpp`）
- `PacketQueue` / `FrameQueue`：有界队列，双重设限（packet 数+字节数 / 帧数）
- `IVideoDecoder`（纯虚，缝 ②）+ `FFmpegVideoDecoder`（唯一实现，M2a 只软解）；
  `FFmpegAudioDecoder`（**不抽接口**，见 `docs/architecture.md` 缝 ② 一节）
- `Pipeline`：把 Demuxer/PacketQueue/Decoder/FrameQueue 串成一条链，
  对外只暴露 `step()`；背压表达成返回值（`Blocked`），不在内部阻塞等待
- `frame_digest.h`：逐帧比对判据（含像素/采样数据 SHA-256），是
  `packet_digest.h` 在解码维度的延伸
- 八条端到端验证场景 A~H（`tests/test_decode_e2e.cpp`）：本地顺解 /
  经 dl 层顺解 / 多点 seek / 音视频交织 / 队列背压 / 损坏包跳过 /
  解码器初始化失败该轨终止 / dl 层错误传播
- `syp_probe --decode`（`--diff` 机器判定 / `--stats` 出
  `pix_fmt`/`sample_fmt` 等统计，M2b 的 Metal 着色器与 AudioUnit sink
  要用）

**这条分支用自己造的验证工具抓出了两个真实 Critical**（不是自夸，是
给后来人一个判断"这套验证成本值不值"的依据）：
1. **Apple 后端单线程自死锁**（M1）：`SourceBridge::read()` 永久死锁，
   自然复现率约千分之九——四套构建（Debug/Release/ASan+UBSan/TSan）
   跑几百次都盖不住，只有"普通 Debug + 多路并行制造竞争 + 看门狗"这套
   最朴素的矩阵抓到过。完整诊断见 `docs/2026-09-08-apple-backend-deadlock.md`。
2. **`Pipeline` 在带 B 帧素材上永久活锁丢帧**（M2a）：`step()` 卡死在
   `Blocked`，永远推进不到 `Eof`——而现实世界的 H.264/HEVC 几乎必带
   B 帧。整个 M2a 早期的验证矩阵在这条路径上从未真正绿过一次，因为
   素材生成脚本用的编码 preset 不产 B 帧，是`docs/known-gaps.md` #18
   记录的结构性测试盲区。见该条目完整机理与修复。

两次都不是"运气好抓到"：都是先有验证装置在跑，再靠它暴露的具体现象
（死锁复现分布 / 活锁不推进）反查出根因，而不是代码走查先猜到再验证。

### M2b 交付物（已完成）
- `AudioRing`：SPSC 环形缓冲，实时回调与调用方线程之间唯一的数据通道
- `TimeSource`（`SystemClock`/`AudioClock`）+ `IAudioSink` 接口（缝 ②）
- `IVideoRenderer` 接口（缝 ②）+ 测试替身（`FakeAudioSink`/
  `FakeRenderer`，见 `tests/support/fake_renderer.h`）
- `TrackPlayer`：同步核心，`step()` 三分支穷尽的视频判定 + 音频优先，
  `play`/`pause`/`set_speed`/`seek` 契约、三级错误分级（单帧可跳过/
  该轨终止/整体致命，含 sink 失败降级到 `SystemClock`）
- 九条端到端同步/呈现场景 A~I（`tests/test_sync_e2e.cpp`）
- `AudioUnitSink`：`IAudioSink` 的真实实现（macOS HALOutput / iOS
  RemoteIO），处理 known-gaps #21 点名的两处架构落差（`consumed_bytes()`
  快照相减、字节级部分写入折算成整帧成败）
- `MetalRenderer`：`IVideoRenderer` 的真实实现，YUV→RGB 着色器，M2b
  Task 9.5 接上真实 `colorspace`/`color_range`（落实 known-gaps #26）
- `tools/build-ffmpeg.sh` 新增 Mac Catalyst slice（`ios-arm64-maccatalyst`，
  M2b Task 1）
- 两个 demo 壳（iOS + Mac Catalyst，`demo/`）：`SypPlayerBridge`
  把 `TrackPlayer`/`Pipeline`/`AudioUnitSink`/`MetalRenderer`/
  `syp_source` 串成一条可运行的播放链路，泵线程 + 一把 mutex 串起所有
  触达 `TrackPlayer` 的路径
- 一个 headless macOS 冒烟测试（`bridge.mm` 直接编译成命令行工具，绕开
  UIKit 层）：驱动真实 `SypPlayerBridge` 打开内置样片、`play()`、采样
  漂移读数，是 M2b 对"能播、同步"这条验收标准的主要证据来源（细节见下
  「M2b 判定」）

**M2b 抓到的真实缺陷值得单列**（跟上面 M2a 那节同款写法，给后来人一个
判断"这套验证成本值不值"的依据）：
1. **`TrackPlayer::step()` 视频早到分支的永久活锁**（Task 7.5，
   `known-gaps.md` #22）：音频 `FrameQueue` 恰好排空、同时视频判定落进
   "早到"分支时，`step()` 变成纯空操作——`set_speed()`/`seek()` 触发的
   `flush()` 之后最容易撞上这个窗口。先写复现用例
   （`step_does_not_livelock_when_audio_queue_empty_and_video_early`）
   在未修代码上跑红，确认测试真的抓得住，再动实现；修复过程中反向自检
   又当场带出两个新坑（`Pipeline::Kind::Blocked`/`Kind::Eof` 在"视频
   早到分支真的会驱动 Pipeline"这条新路径上的两处遗留缺口），一并堵上。
2. **`MetalRenderer` 着色器彻底编译不过，四条 GPU 用例仍然全绿**
   （Task 9，Critical M11）：`ready()` 把"没有 Metal 设备"（该 skip）
   与"设备存在但着色器/管线创建失败"（该 FAIL）合并成一个布尔，四条
   GPU 用例统一 `if (!ready()) skip`；审查变异体删掉着色器源码里一个
   分号，让全部颜色逻辑所在的文件彻底编译不过，`ctest` 依然报"100%
   tests passed"——skip 与 pass 在这套判据下完全不可分辨。修法是拆出
   独立的 `device_available()` 只回答"有没有找到 GPU 设备"，四条用例
   改用共用守卫，`device_available()==true && ready()==false` 时用
   `CHECK` 记一次真实失败，不再静默放过。
3. **demo 壳 `PlayerCore::close_internal()` 的 use-after-free**（Task 10
   修复轮 1/5，用 ASan 复现）：泵线程可能正睡在 `usleep()` 里（不持
   `mu_`），这时调用方紧接着释放 `PlayerCore` 本身，泵线程睡醒后会继续
   碰一块已经被 `free` 的内存。旧版本只有一句"不会在析构之后继续碰
   `this`"的注释，没有代码兑现——"play() 后立即释放最后一个强引用、
   循环 200 次"的驱动程序在 `build-asan` 下第一次迭代就复现
   heap-use-after-free。修法是 `close_internal()` 对泵队列做一次
   `dispatch_sync` 空 block，确认泵线程那次调用真的返回后才继续析构
   （细节与遗留的一层未记录因果见 `docs/known-gaps.md` #27 补记）。

三条都不是"代码走查先猜到再验证"——都是先有验证装置（回归用例/GPU
变异测试/ASan 驱动程序）在跑，再靠它暴露的具体现象反查出根因，跟 M2a
那两条 Critical 是同一种方法论。

### 验收标准
- [x] M2a：逐帧解码结果与 FFmpeg 直接解码全等（八条场景，见上）
- [x] 本地 mp4 能播，音画同步无明显漂移（**有条件**，见下「M2b 判定」——
      漂移读数来自无头冒烟测试而非屏幕观察）
- [x] 通过 dl 层播放网络视频，seek 正确（**有条件**，见下「M2b 判定」——
      仅验证到编译链接通过，未实际跑通一次网络请求）
- [x] 暂停/恢复/倍速正确（**有条件**，见下「M2b 判定」——契约由 Task 6
      单测 + Task 7 九条端到端场景验证，均使用测试替身而非真实
      `AudioUnitSink`/`MetalRenderer`；demo 壳的无头冒烟测试没有实际
      触发过 pause/setSpeed）

### M2b 判定

**通过，但三条验收标准全部是有条件勾回，如实写全，不含糊**：

1. **"本地 mp4 能播，音画同步无明显漂移"**——证据是 Task 10 的 headless
   macOS 冒烟测试：把 `demo/shared/bridge.mm`（连同 `src/dl/`、
   `src/media/`、`src/platform/apple/` 全套）直接编译成一个 macOS
   命令行可执行文件，驱动真实 `SypPlayerBridge` 打开内置样片、
   `play()`，每 300ms 采样一次 `snapshot`。40 次采样（跨约 12 秒）
   drift 落在约 **−25ms 到 +33ms** 之间，没有随时间单调增长的趋势，
   `dropped_frames`/`present_failures` 全程为 0，360 帧全部交付
   （30fps × 12s，与样片时长精确吻合）。**这是无头烟测跑出来的数字，
   不是在屏幕上看到的读数**——demo 壳的 UI（`PlayerView.swift` 的
   漂移读数控件）没有经过真机/模拟器的可视化验证，`screencapture`
   在沙箱环境里拿不到"屏幕录制"权限，没能截图确认 UI 像素级别的效果。
2. **"通过 dl 层播放网络视频，seek 正确"**——只验证到"编译链接通过"：
   四套 CMake 构建 + iOS/Catalyst 两个 Xcode 工程都把 `src/dl/*.cpp` +
   `apple_http_backend.mm` 正确编进了 demo 壳，`syp_dl_purity_check`
   全程为空改动，dl 层本身在 M1 早已端到端验证过（seek 到未缓存区间
   正确补洞，见 M1 一节）。**但 demo 壳的 `-openURLString:` 路径没有
   实际发起过一次网络请求**——headless 冒烟测试只测了本地文件
   （`-openLocalFile:`），"dl 层驱动真实网络请求 + 真实 `TrackPlayer`
   + 真实渲染"这个交叉点从未被这条分支实际跑通过。另外
   `known-gaps.md` #27（`Pipeline::create_avio()` 没有接
   `interrupt_callback`）意味着即使跑通，网络卡死时这条路径目前没有
   用户可见的取消口。
3. **"暂停/恢复/倍速正确"**——契约层面的正确性由 Task 6
   （`tests/test_track_player.cpp` 的 pause/倍速/seek 契约回归）与
   Task 7（九条端到端场景 A~I，`tests/test_sync_e2e.cpp`）验证，两者
   都是 headless、确定性、覆盖真实 `TrackPlayer`/`SystemClock`/
   `AudioClock` 的调度逻辑——**但音频侧用的是测试替身
   `FakeAudioSink`，不是真实 `AudioUnitSink`**（`AudioUnitSink` 是
   Task 8 才落地的，晚于 Task 7）。demo 壳的 headless 冒烟测试驱动的是
   真实 `AudioUnitSink`/`MetalRenderer`，但它只做了"打开样片 → 连续
   播放到 EOF → 采样漂移"，**没有在冒烟测试里实际调用过
   `pause()`/`setSpeed()`**——"暂停/倍速在真实设备栈上表现正确"这件事
   没有被端到端验证过，只有"契约在测试替身上正确" + "真实设备栈能连续
   播放且不漂移"这两条证据分别成立，两者的交集（真实设备栈上的
   暂停/倍速）是推论,不是直接观测。
4. **iOS 真机从未运行过**：`xcodebuild -destination 'generic/platform=iOS'`
   构建成功（`arm64` Mach-O、`FFmpeg.framework` 正确内嵌），但没有
   Apple 开发者证书/描述文件，**没有安装到设备、没有运行过**。上面
   三条的"有条件通过"全部只在 Mac Catalyst（本质是 macOS）这一个
   平台上验证过，iOS 真机的验证程度只到"能编能链"。

这份"有条件通过"沿用的是 M1 判定沿革那套写法：不因为验证不到位就整体
判"不通过"（三条验收标准背后的核心逻辑——同步算法、三级错误分级、
`played_us()` 的折算——确实都有过硬证据），但把"验证到了什么程度"和
"标准字面意思是什么"这两件事分开写清楚，不让读者从打勾误以为这三条
都经过了完整的真机端到端验证。

---

## M-HLS — HLS 支持（已完成）

**目标**：让 `syplayer` 能播 HLS（m3u8），VOD 与直播都支持，网络 IO 全部经由
项目自己的 dl 层——不是让 FFmpeg 自己联网，也不是自己重写一遍 HLS 协议。
架构落地见
`docs/architecture.md` 第六节。**依赖 M1（dl 层）+ M2a（解封装/解码管线）+
M2b（同步与呈现），与 M3 并行、互不阻塞**——分支 `dev/hls-support` 从
`dev/m1-dl-layer` 切出，不依赖 M3 的 `VTDecoder`/完整同步收尾。

**采用的方案**：FFmpeg 自带的 `hls` 解封装器 + 本项目接管它的全部 IO
（两个 `io_open`/`io_close2` 回调）。**这与 M6 一节原先写的"自写 m3u8 解析、
不用 FFmpeg 的 hls demuxer"这条计划不同**——spec 第 2 节权衡过三种方案，
自写 HLS 层的代价是播放列表解析、选轨、不连续点、跨分片 seek、直播刷新
全部要自己做，"本项目每引入一块新逻辑就会长出一批没有守卫的守卫"（M2b
的教训）这条风险对一版 HLS 支持不值得；FFmpeg 自己联网的代价是 dl 层——
项目的核心资产——在 HLS 这条路上形同虚设。M6 那条计划条目已在下面更正。

### 交付物

- `src/media/hls/`：`HlsSession`（装配 `AVFormatContext`、两个 IO 回调、
  scheme 改写与还原、通道分流、选轨、生命周期）+ `PlaylistFetcher`
  （异步 `syp_http_backend` 包成一次阻塞取回，零缓存）+ `url_rewrite`
  （纯函数：scheme 改写/还原、通道判定、播放列表加密声明扫描）
- `Pipeline::create_hls()`：与 `create_file()`/`create_avio()` 并列的
  第三个工厂，`TrackPlayer` 及以下完全不知道 HLS 存在；唯一的例外是
  选轨判据要排除 `AVDISCARD_ALL` 的流，而这条排除**只加在
  `TrackPlayer::create()` 一处**（`track_player.cpp:65/71`），不改任何
  已有签名。`Pipeline::create_common()` 仍按 `codec_type` 无条件托管
  全部音视频轨（被 discard 的轨也要有队列、也要被排空，否则会改变
  `step()` 的联合背压形状）
- 两条 IO 通道，缓存语义刻意相反：播放列表零缓存（直播刷新），分片走
  `syp_source` 带缓存（不可变内容，缓存永远正确）
- `protocol_whitelist` 结构护栏：把 FFmpeg 编进去但用不上的 `http`/`tcp`
  协议堵死，任何一条绕过我们 `io_open` 的路径都会当场失败而不是静默绕过
  ——真实性质经两轮变异实测收窄，见 `src/media/hls/hls_session.h` 顶部
  的 2×2 矩阵
- 多码率选轨（`HlsOptions::max_bandwidth_bps`，0=不限选最高、兜底选最低
  但先过滤掉不含视频流的 program）+ 分轨归组（`EXT-X-MEDIA`）
- 直播：播放列表周期性刷新、`duration` 未知时上层不误判；`request_abort()`
  接通了 `docs/known-gaps.md` #27 在 HLS 这一条路径上的中止口（`create_file`/
  `create_avio` 仍未接，#27 状态更新为"部分接线"）
- 错误分级：加密流（`EXT-X-KEY:METHOD!=NONE`）整体拒绝、分片 404 精确
  分级为 `SYP_ERR_HTTP_STATUS` 而非通用 IO 错误、播放列表超过 8 MiB 上限
  整体终止
- 结构性验收（`every_byte_goes_through_our_dl_layer`）：全局 HTTP 后端
  换成内存桩、主机名用 DNS 上永不可解析的 `fake.invalid`，证明真实网络
  不可达下每个字节都经过 dl 层——反向自检两个方向都实测过（拆 `io_open`
  会红；`io_open` 与 `protocol_whitelist` 一起拿掉，FFmpeg 真的会自己开
  socket，日志逐字复现）

### 测试

新增 3 个 ctest 目标：`hls_url_rewrite`（15 条用例，纯函数穷举测，不依赖
FFmpeg）、`hls_playlist_fetcher`（8 条）、`hls_e2e`（16 条，真实 loopback
HTTP 服务器 + 合成 fixture）。全部 26 个 ctest 目标（含既有的 23 个）全绿，
`src/dl/`、`include/syplayer/` 全程零改动（`syp_dl_purity_check` 验证）。

### 已知缺口

本里程碑显式排除 ABR（中途切流）、加密流播放、字幕轨、I-frame 播放列表、
低延迟 HLS；另有 17 条实施过程中发现的缺口（后缀分流启发式、单 scheme
假设、直播缓存淘汰未实测、纯音频源回退分支覆盖不足、HLS `seek()` 在早于
流起始时间戳处会失败、直播中途变加密/重拉 404 都会静默 `Eof`、
`pending_segment_error_` 的粘性穿透 `seek()` 等）登记在
`docs/known-gaps.md` #32~#48，3 条代码/测试卫生债务登记在
`docs/tech-debt.md`。（后续：#46/#47/#48 已于 2026-09-14 修复，
短暂重拉失败的重试另登记为 #49。）**没有真机/真实 CDN 验证**——与 M2b 同款诚实记录，
自动化只覆盖到 127.0.0.1 上的 loopback server。

### M-HLS 判定

**通过。** spec 第 9.2 节要求的 10 条用例全部落地（3 条实现过程中发现
细节与设计时设想不同，判据据实调整，见 spec 该节「落地对照」）；第 9.3
节「结构性验收」两个方向都有实测证据；`src/dl/`/`include/syplayer/` 冻结
纪律全程遵守。「有条件」的部分与 M2b 同一类——功能与结构上的正确性有
过硬证据，但验证深度只到 loopback，真机/真实 CDN 未跑过，如实记在
「已知缺口」里，不打包进"通过"这个判断掩盖掉。

**还有一条同等分量、必须一起说清的**：`Pipeline::create_hls()` **全仓
没有任何生产调用方**。`grep` 只命中 `tests/`；两个 demo 壳的入口
（`demo/shared/bridge.mm:430` 的 `create_file()`、`:476` 的
`create_avio()`）都不走它。也就是说**这个里程碑的入口至今不在任何出货
路径上**——「验证深度只到 loopback」说的是自动化测试的边界，而这一条说
的是：连"被真实使用过一次"都还没发生。把 HLS 接进 demo 是 M3 及之后的
事，在那之前，本里程碑的一切结论都只在测试套件这个上下文里成立。

（后续，2026-09-14：demo 壳 `-openURLString:` 对 `.m3u8` URL 改走
`create_hls()`，这一条"零生产调用方"不再成立；同时发现 M-HLS 合并后两个
demo 工程**一直链接失败**——`pipeline.cpp` 引用了 `HlsSession`，而
`demo/generate_xcodeprojects.rb` 的源文件清单没有补 `src/media/hls/` 三个
文件，已补并重新生成。仍未在真机/模拟器上实际播放过 HLS。）

---

## M3 — 硬解、完整同步收尾

**`AudioUnitSink` 已提前到 M2b**（见 M2 一节的裁定），M3 不再包含最小音频
输出。M3 拆成三个各自独立走设计 → 实现计划 → 实现流程的子项目：

| 子项目 | 内容 | 状态 |
|---|---|---|
| **M3a** | 解码方式可选（软解/硬解）、H.264 + HEVC 硬解、零拷贝上屏、demo 切到真上屏、软解/硬解选择 UI | **已完成** |
| M3b | 同步正确性与呈现时序：`#23` 音频短于视频、`#25` iOS 设备延迟、`#52` 呈现不阻塞、按时刻上屏、快速出画、追帧丢帧 | **已完成** |
| M3c | 缓冲水位与卡顿判定：加载线程、按时长水位、起播/seek/卡顿缓冲状态机、异步 seek、路由变化延迟斜坡 | **已完成** |

⚠️ **M3 整体仍是本项目风险最高的阶段之一。** VideoToolbox 的边缘情况（流中途
分辨率变化、参考帧处理、内存压力下 session 失效）只能靠实机跑量暴露，
M3a 的验证深度尚未到那一层（见下面「M3a 判定」）。

### M3a — 可选硬解 + 零拷贝上屏（已完成）

**目标**：`PipelineConfig` 里可显式选择软解/硬解；硬解支持 H.264 与 HEVC
（8-bit 4:2:0），不支持时打开阶段明确报 `SYP_ERR_NOT_IMPLEMENTED`，不做
"选了硬解、静默退回软解"这种兜底；硬解帧 `CVPixelBuffer → CVMetalTextureCache
→ MTLTexture` 零拷贝上屏；`MetalRenderer` 新增生产上屏入口
（`set_output_layer()`，画到调用方给的 `CAMetalLayer`），demo 壳从"每帧
GPU→CPU 回读 + `CGImage`"切到真上屏，并加软解/硬解选择控件。

**交付物**：

- `PipelineConfig::video_decode`（`Software`/`Hardware`）+ `hw_backend`
  （`hw::IHwDecodeBackend*`，非拥有）；`FFmpegVideoDecoder` 硬解模式下
  `get_format` 只认硬件格式、不从候选里挑软件格式退回；`Pipeline` 硬解模式
  下任一非封面视频轨打开失败即整个 `create_*()` 失败（封面图轨恒软解）；
  `Pipeline::video_hardware_decoding()` 只读查询。
- `src/platform/apple/vt_decode_backend.{h,mm}`：`vt_supports()` 纯函数
  （单测穷举）+ 进程内单例后端，Catalyst slice 上 `supports()` 恒 false
  （决策 3，FFmpeg `videotoolbox.c` 在 Catalyst 上引用了不可用符号，见
  `known-gaps.md` #20 的裁定）；`VTIsHardwareDecodeSupported()` 给 HEVC 补
  一道 per-device 预检（`RequireHardwareAcceleratedVideoDecoder` 对 H.264
  已经够，FFmpeg 对 HEVC 只用 `EnableHardwareAcceleratedVideoDecoder`）。
- `tools/build-ffmpeg.sh`：`FF_COMPONENTS` 加 HEVC 解码器/解析器 +
  H.264/HEVC 的 videotoolbox hwaccel；构建期自检四个 slice 的
  `config_components.h`（并非 spec 原计划的 `config.h`——见下面「与 spec
  的偏差」）符合预期，产出 `syp_ffmpeg_features.h`。
- `MetalRenderer`：`AV_PIX_FMT_VIDEOTOOLBOX` 帧经 `hw_handle()` 取
  `CVPixelBuffer`（必须是 `420v`/`420f`，否则 `SYP_ERR_NOT_IMPLEMENTED`），
  `CVMetalTextureCache` 零拷贝取两张纹理，新增 `nv12_to_rgba` 着色器；
  着色器文件改名 `video_shaders.metal`（容纳 yuv420p/nv12 两个 kernel +
  blit 顶点/片元函数）；`set_output_layer()` 生产上屏入口——计算 pass 之后
  在同一命令缓冲追加 blit pass 等比适配画到 `CAMetalLayer` 的 drawable；
  `debug_cpu_upload_count()`/`debug_presented_drawable_count()` 两个结构性
  验收用计数。
- demo 壳（`demo/shared/`）：`PlayerView.MetalVideoView`（`layerClass =
  CAMetalLayer`，`layoutSubviews` 里设 `drawableSize`）替换旧的
  `UIImageView` + 逐帧 RGBA 回读；软解/硬解分段控件（`decodeModeControl`），
  平台不支持硬解时禁用并提示；统计标签显示当前解码方式。
  `generate_xcodeprojects.rb` 补 `vt_decode_backend.mm` 源文件与
  `VideoToolbox`/`CoreVideo`/`CoreMedia`/`QuartzCore` 系统 framework。

**测试**：新增 ctest 目标 `vt_decode`（`tests/test_vt_decode.mm`，macOS 宿主
原生跑，覆盖 `vt_supports()` 穷举、结构性验收"硬解全程无静默软解"、
硬解与软解逐帧逐比特比对、seek→flush→Eof、HLS 硬解帧数一致）；
`test_decoder`/`test_pipeline` 补硬解闸门用例（假后端）；`test_metal_renderer`
补 NV12 present、零拷贝结构性验收、`set_output_layer` 相关用例。全部
27 个 ctest 目标（含既有的 26 个）全绿。

**已知缺口**：HEVC 的 VideoToolbox 内部软件回退（VT 内部悄悄退到软解、
本方案的自动化手段测不出）登记在 `docs/known-gaps.md` #50；Mac Catalyst
无硬解是已裁定的平台差异（#20）。

**与 spec 的偏差**（据实调整，非遗漏）：FFmpeg 构建期自检读的是
`config_components.h` 而不是 spec 原计划的 `config.h`（FFmpeg 把细分组件
开关拆到了这个文件里，Task 1 核实后改的调用点）；`test_vt_decode` 是
`.mm`（ObjC++）而不是 `.cpp`；`vt_decode` ctest 的 `TIMEOUT` 从 120 调到
1800（覆盖 TSan/ASan 两套 sanitizer 构建下变重的用例）；`hw_backend` 统一
成非 const 指针（`prepare()` 本身非 const）；`MetalRenderer::encode_nv12`
额外拒绝比 `Frame` 声明宽高小的 `CVPixelBuffer`（NV12 路径的一条防御性
校验，spec 未明确要求但落地时判定该加）。完整对照见 spec 末尾「12. 落地
对照」一节。

### M3a 判定

**通过，但有条件。** spec 第 9 节要求的用例（9.1 平台无关闸门、9.2
Apple 集成测的逐比特一致性/结构性验收/seek/HLS、9.3 渲染层 NV12/零拷贝/
`set_output_layer`）全部落地并全绿；三个素材（H.264 faststart、H.264 B
帧、HEVC）硬解与软解逐帧逐比特完全一致（`mismatched=0`），没有触发 spec
预留的"退到 PSNR ≥ 50 dB"降级判据。

**"有条件"的部分，与 M2b/M-HLS 同一类**：验证深度只到 macOS 宿主原生跑
`ctest` + 两个 demo 工程用 `xcodebuild ... CODE_SIGNING_ALLOWED=NO build`
编译链接通过这一层。**真机/模拟器上的人工验证清单（spec §9.4、
task-10-brief.md Step 7）完全没有执行**——不是执行了但结果不理想，是
压根没有物理 iOS 设备/Mac 去交互式跑：Mac Catalyst 壳硬解分段禁用与状态
提示、iOS 真机硬解打开本地文件/`.m3u8` 出画、硬解播放中切后台再回来是否
继续出画（VT session 重建）——这三条如实记在 `docs/known-gaps.md` #51，
不打包进"通过"这个判断里掩盖掉。HEVC 的 VT 内部软件回退（#50）
是 spec §10 提前承认的结构性缺口，不是本次实现的遗漏。

**终审发现并修复了一个 Critical（C1）**：整分支终审发现上面"全部落地并全绿"的用例
没有覆盖 hwaccel 初始化失败这条路——硬解模式下 VT session 建不起来时，H.264 会绕过
`get_format` 静默软解出全部帧（假后端实测 1650 帧 yuv420p、轨未失败、干净 Eof），HEVC
会把所有包计成"可跳过"、视频 0 帧、音频照常——恰好违反 M3a 的头号保证"选硬解绝不静默
软解"。已修复（get_format 拒绝闩住 + `receive()` 输出闸 + 强制 `pix_fmt=NONE`，spec
§3.4），并补了先红后绿的平台无关用例；同轮修了硬解模式下 `avcodec_open2` 失败仍退化成
只剩音频（I2）、补了真实 VT 帧直通 `MetalRenderer` 的零拷贝/画面一致用例（I1）、
`set_output_layer` 不再跨线程改 CALayer（M4）。修复后判定维持**有条件通过**——条件
仍是上面那条：没有任何真机/模拟器实跑（#51），`nextDrawable` 持锁阻塞推迟到 M3b（#52，
已于 M3b 修复，见下面"M3b — 同步正确性与呈现时序"一节）。

---

### M3b — 同步正确性与呈现时序（已完成）

1. **#23**：音频轨比视频轨先结束时不再在尾部永久停摆——声音播完后从最后
   的音频位置无缝切到系统时钟，视频按时播完并报 `Eof`。
2. **#25**：iOS 上 `AudioUnitSink` 的设备输出延迟纳入
   `AVAudioSession.outputLatency + ioBufferDuration`，音频路由变化时重新
   计算。
3. **#52**：`MetalRenderer::present()` 不再阻塞调用线程（取 drawable 最多
   1 秒、同步等 GPU 完成都去掉）。
4. **按时刻上屏**：挂了 `CAMetalLayer` 时用 `presentDrawable:atTime:`，由
   系统把帧对齐到最近的屏幕刷新。
5. **快速出画**：打开后第一帧与 seek 后第一帧，即使暂停中也立即呈现，不
   推进时钟。
6. **丢帧策略（追帧）**：持续迟到丢帧时让视频解码器跳过非参考帧，追上后
   恢复。

**交付物**：

- `IAudioSink::output_drained()`（新增纯虚函数，#23 判据）+
  `TrackPlayer::clock_switch_reason()`（`None`/`AudioFailed`/
  `AudioEnded`）；切钟基准取当时的音频时钟读数（与 spec 原计划的"最后
  写入结束时刻 − 20ms 容差"不同，见下面"与 spec 的差异"）。
- `SYP_ERR_BUSY`（新增返回码）+ `TrackPlayer::render_busy_frames()`：
  渲染在途上限已满时的 BUSY 重试次数，独立于 `present_failures()`。终审 C1 修订：
  BUSY 的帧不再丢弃而是保留重试（真实窗口上提前 40ms 提交 + 名额上屏后才归还，
  BUSY 是常态，丢弃会成批丢帧），重试到迟到超过 80ms 才按迟到丢弃；见 spec §7.3、
  known-gaps #52"终审 C1"。
- `InflightGate`（`src/platform/apple/inflight_gate.h`，独立类，原子
  计数、带 `limit` 参数）：`MetalRenderer` 在途上限门，离屏
  `kMaxInflightFrames=3`（完成回调归还），挂 `CAMetalLayer`
  `min(3, maximumDrawableCount−1)`（`presentedHandler` 归还，完成回调
  兜底、exactly-once 令牌）；丢失 `presentedHandler` 的兜底回收（约
  1.2s 后强制认领）；`wait_until_idle()`（析构也用，因此改名，不再叫
  `debug_wait_until_idle`）有界。
- `presentDrawable:atTime:`：`due_in_us` 先夹到 `[0, 1s]`
  （`kMaxPresentDueUs`），`debug_last_present_host_time()` 供测试核对。
- `IVideoDecoder::set_skip_nonref(bool)` + `Pipeline::set_video_catchup(bool)`
  /`track_drained(int32_t)`（只读查询，Task 2 fix round 加）：追帧跳非
  参考帧；`TrackPlayer::catching_up()` 只读查询，`kCatchupDropsToEnable=5`
  /`kCatchupWindowUs=1s`/`kCatchupOnTimeToDisable=30` 三个经验常量（待
  真机调参，见 tech-debt）。
- demo 快照新增 `renderBusyFrames`/`catchingUp`/`clockSwitchReason` 三个
  字段，显示在统计标签（`demo/shared/bridge.h/.mm`、
  `PlayerViewController.swift`）。

**测试**：spec §5.1 全部落地（详见 spec 末尾"7. 落地对照"）。已知
`known-gaps.md` #23（切钟）、#52（呈现不阻塞）标记已修复；#25（iOS 设备
延迟）标记已落地但真机数值未验证。全部 28 个 ctest 目标（含既有的 27
个）全绿；TSan 覆盖 `track_player`/`metal_renderer`/`inflight_gate`/
`vt_decode`，ASan 覆盖 `track_player`/`metal_renderer`/`vt_decode`/
`audio_unit_sink`，均无 sanitizer 报告；`tools/check-deploy-target.sh`
通过；iOS 与 Mac Catalyst 两个 demo `xcodebuild ...
CODE_SIGNING_ALLOWED=NO build` 均 `BUILD SUCCEEDED`。

**与 spec 的差异**（据实调整，非遗漏，完整对照见 spec 末尾"7. 落地对照"）：

- **#23 切钟判据改用 `IAudioSink::output_drained()`**，不是 spec 原计划
  的"`played_us() ≥ 最后写入结束时刻 − 20ms`"——后者在设备延迟大于容差
  时永不成立（实测 45ms 即停摆），`output_drained()` 与延迟无关；基准
  也从"结束 pts"改成"当时的音频时钟读数"（AAC priming 导致取结束 pts
  会回跳）。
- **`InflightGate` 独立成类并单测**，而不是内联在 `MetalRenderer` 里。
- **挂 layer 时在途名额在 `presentedHandler` 里归还**，不是 spec 原方案
  的"一律在命令缓冲完成回调归还"——后者会让门形同虚设（归还太早，
  drawable 还没真正回到系统池，`nextDrawable` 照样阻塞）。
- **快速出画（打开后第一帧）目前只在暂停中生效**，未暂停时首帧行为不变
  ——保护既有的首帧判定用例，与 spec §4"暂停中打开/seek"这条边界一致；
  spec 没有明确排除"未暂停打开"这条分支，落地时判定收窄范围更安全。
- **新增的测试缝** `debug_advance_system_clock_us`（推进内部系统时钟
  用于确定性测试）、`video_catchup_active()`（只读查询
  `pipeline_->video_catchup()`）、`wait_until_idle()`（`MetalRenderer`，
  见上）——均为测试可观测性新增，不改变生产行为。

### M3b 判定

**通过，但有条件。** spec 第 9 节要求的验收标准全部落地并全绿（全量
ctest 28/28、TSan/ASan 相关目标零报告、两个 demo 编译通过）。**"有条件"
与 M2b/M-HLS/M3a 同一类——验证深度只到 macOS 宿主原生跑 ctest + 两个
demo 工程 `xcodebuild ... CODE_SIGNING_ALLOWED=NO build` 编译链接通过这
一层，没有任何真机/模拟器实跑**。人工验证清单（如实记在
`docs/known-gaps.md`，不打包进"通过"里掩盖掉）：

1. 真实窗口上"在途上限门 + `nextDrawable`"延迟量级——spec §2 决策 2 的
   "≤0.2ms"是**独立命令行程序**测的，不是本仓库测试框架量出来的（见
   known-gaps #52 补记）。终审 C1 之后另有 `tools/present_window_harness`
   可复测"BUSY 重试"策略在真实窗口上的丢帧数；本机无人值守会话里窗口不可见、
   数字不稳定，需在点亮的显示器上重跑（见 known-gaps #52"终审 C1"）。
2. iOS 后台运行、窗口被遮挡时 `presentedHandler` 的实际到达时机（见
   known-gaps #52 补记）。
3. `AudioUnitSink::output_drained()` 在真实硬件 `open()` 之后的分支
   （`ring_->readable() == 0`）没有自动化覆盖，需要真实设备（见
   known-gaps #23 修复一节）。
4. `AVAudioSessionRouteChangeNotification` 触发时 `outputLatency` 是否
   已经反映新路由（蓝牙量级延迟协商可能比通知慢），未经真机验证；路由变化让
   音频时钟阶跃 Δ（画面冻结 / 误开追帧）的实际观感同样未验证，平滑留给 M3c
   （见 known-gaps #25 补记）。
5. 按时刻上屏（`presentDrawable:atTime:`）的实际观感——是否真的更平滑、
   对齐刷新——自动化只验证了参数正确，没有肉眼/设备时间戳数据（见
   known-gaps #53）。

**非目标显式记录**（spec §1，用户与作者均确认无真实需求，不留作待办）：
主时钟可切换 API（现有"音频优先、失败/播完自动降级"覆盖实际场景）；
无缝变速（样本数→媒体时长分段映射表，#29 的断点仍是有意接受的体验取舍，
留在 tech-debt）；屏幕刷新驱动的呈现（`CADisplayLink`，本轮采用"播放线程
挑帧 + `presentDrawable:atTime:`"这条风险更低的路线）。

---

### M3c — 缓冲水位与卡顿判定（已完成）

1. **泵线程不再被网络 IO 阻塞**：demux（含网络读）移到 `Pipeline` 内部的
   独立加载线程；`step()` 只做解码与呈现判定，数据不够时立即返回。
2. **按时长的缓冲水位**：加载线程按"已缓冲时长"读前向数据，到上限
   （默认 30s 或 64MiB）停读，消费后续读。
3. **缓冲状态机**：`TrackPlayer` 区分起播缓冲 / 播放 / 卡顿缓冲 / seek
   缓冲；缓冲期间冻结时钟、不写音频、不推进视频，达到恢复水位（或读到
   文件尾）后自动继续，与用户暂停正交。
4. **可观测**：`buffering()`、缓冲原因、已缓冲时长、卡顿次数与累计时长、
   起播耗时、音频欠载次数；demo 显示"缓冲中"与已缓冲时长。
5. **异步 seek**：`Pipeline::seek()` 立即返回，加载线程执行实际定位；
   旧位置的数据按代号丢弃。
6. **路由变化时钟平滑**（M3b 遗留）：设备延迟变化按限速斜坡生效，音频
   时钟不回跳、不突跳。

**交付物**（按 Task 1–8）：

- Task 1：`PacketQueue` 加锁成为跨线程边界，新增 `buffered_until_us()`/
  `queued_duration_us()`；新增 `test_packet_queue` 套件（ctest 28 → 29）。
- Task 2：`Pipeline::buffer_stats()`（同步模式）+ `BufferStats{buffered_
  until_us, demux_eof, loading, queued_bytes, full}` + `config()` 访问器 +
  `debug_override_buffer_stats()` 测试缝。
- Task 3：加载线程——`PipelineConfig::demux_thread`、线程模式 `step()`
  不再 demux、终止标记入队、`request_abort()`/析构与加载线程的生命周期、
  `seek()` 异步化（代号丢弃旧包）。
- Task 4：停读水位——`max_buffer_ms`（时长）与 `max_buffer_bytes`（字节）
  达到即停读，消费后唤醒续读；`PacketQueue::take_all()`（锁外释放）；
  ctest `pipeline` 目标 `TIMEOUT` 60 → 180。
- Task 5：`IAudioSink::underrun_count()`（欠载段计数）；`AudioUnitSink`
  路由变化延迟斜坡（`kLatencySlewRate=0.1`，即 100ms/s）+ 单调护栏。
- Task 6：`TrackPlayer::BufferPolicy` 与缓冲状态机（起播/seek/卡顿三种
  缓冲原因、卡顿闩锁 `stall_armed_`、统计口径）。
- Task 7：线程模式端到端——`test_decode_e2e`/`test_hls_e2e` 线程模式跑到
  EOF 帧数与同步模式一致；`LoopbackServer::hold_route()`/`release_route()`
  扣住分片验证卡顿进出。
- Task 8：demo 接线——`PipelineConfig::demux_thread = true`、`SypPlayer
  Snapshot` 新增 `buffering`/`bufferingReason`/`bufferedMs`/`rebufferCount`、
  统计标签显示缓冲状态。

**测试**：新增 ctest 目标 `test_packet_queue`（`packet_queue_take_all_
hands_out_packets_and_resets_like_clear` 等）；`test_pipeline` 新增线程
模式套件（`pipeline_demux_thread_decodes_same_frames_as_sync_mode`、
`pipeline_demux_thread_stops_at_max_buffer_ms_and_resumes_on_consume`、
`pipeline_demux_thread_async_seek_first_frames_match_sync_mode`、
`pipeline_demux_thread_seek_while_read_blocked_applies_after_read_returns`
等）；`test_track_player` 新增缓冲状态机套件（`buffering_stall_enters_
below_trigger_counts_freezes_and_resumes`、`buffering_full_exit_below_
trigger_does_not_flap_until_rearmed`、`buffering_threaded_mode_audio_not_
starved_by_slow_video_consumption` 等）；`test_audio_unit_sink` 新增延迟
斜坡与欠载段套件（`slew_latency_keeps_played_us_monotonic_across_route_
change`、`underrun_edge_counts_each_starvation_episode_once` 等）；
`test_hls_e2e`/`test_decode_e2e` 新增线程模式用例。完整 `TEST_CASE` 名单
逐条对照见 spec 末尾「8. 落地对照」一节。全部 29 个 ctest 目标全绿；TSan
覆盖 `packet_queue`/`pipeline`/`track_player`/`hls_e2e`，ASan 覆盖
`pipeline`/`track_player`/`audio_unit_sink`，均无 sanitizer 报告；两个
demo 工程 `xcodebuild ... CODE_SIGNING_ALLOWED=NO build` 均 `BUILD
SUCCEEDED`；`tools/check-deploy-target.sh` 通过。

**非目标**（spec §2，据实排除）：自适应水位（按网络速度/卡顿历史调整
恢复水位）；解码也移到独立线程（解码在泵线程上是否会成为瓶颈待真机数据）；
seek 打断正在阻塞的网络读（HLS `AVERROR_EXIT` 会被 hls.c 当作分片读完而
跳段，打断不可安全复用，见 known-gaps #58）；ABR、预加载（M5/M6/HLS 后续）。

### M3c 判定

**通过，但有条件。** spec 第 6.2 节要求的验收标准全部落地并全绿：全量
ctest 29/29；TSan（`packet_queue`/`pipeline`/`track_player`/`hls_e2e`）
与 ASan（`pipeline`/`track_player`/`audio_unit_sink`）零报告；两个 demo
工程编译通过。**"有条件"与 M3b 同一类**——没有真机/弱网/蓝牙路由的实际
播放验证，人工验证清单如实记在 `docs/known-gaps.md` #62（demo 冒烟、
蓝牙路由变化斜坡观感、弱网卡顿进入/恢复体验、水位默认值在真实网络下的
表现，见 #61），不打包进"通过"里掩盖掉。另外两条已知设计取舍同样如实
记录、不影响判定：某轨在网络流上先于文件尾结束会误判一次卡顿，若之后
`full` 恒真（字节上限或同步模式 128 包上限）该卡顿会被闩锁永久抑制、
不再显示缓冲状态（known-gaps #57，ledger "Ruling R1 update"）；线程模式
seek 不打断阻塞中的网络读，最坏要等 dl 层读超时（known-gaps #58）。

---

## M4 — Swift 上层（已完成）

- `SYPlayer` / `SYPlayerLayer`（Swift 封装）+ Demo App
- 验收：业务侧代码不出现任何 C 类型；支持 SwiftUI 与 UIKit 两种接入

### M4 交付物

1. **`SYPlayerKit` 静态 framework**（`demo/generate_xcodeprojects.rb`）：C++
   内核、ObjC++ 桥、Swift 公开 API 全部收进一个目标，`MACH_O_TYPE = staticlib`，
   两个 demo App 链接（不内嵌）它。着色器内嵌头的 Run Script 一并迁入。
2. **ObjC++ 桥内部化**：`demo/shared/bridge.{h,mm}` →
   `swift/SYPlayerKit/Internal/SYPBridge.{h,mm}`，走 `MODULEMAP_PRIVATE_FILE` +
   PrivateHeaders（模块 `SYPlayerKit_Private`），公开伞头 `SYPlayerKit.h` 不提它；
   `bridge-Bridging-Header.h` 删除。新增 `SypStatusCode` 镜像枚举（`static_assert`
   钉住 `syp_types.h`）与 `startupUs`/`audioUnderruns` 两个快照字段。
3. **Swift 公开 API**：`SYPlayerError`（穷举 `syp_status` + `LocalizedError` 中文
   文案）、`SYPlayerState`/`SYPlayerStatistics`（值类型，一次带全字段）、`SYPlayer`
   （`@MainActor`、`async open`、10Hz 主线程轮询、`@Published`/`AsyncStream`/
   `delegate` 三种投递、双闸门防串会话回调、`pendingError` 起播期错误暂存补投）、
   `SYPlayerLayer`/`SYPlayerView`/`SYPlayerViewRepresentable`（终审 M5 由
   `SYPlayerVideoView` 改名）。详见 spec §8 落地对照。
4. **`SYPlayerKitTests`**（XCTest，Mac Catalyst，无 host）：错误映射、时间换算、
   状态映射、`drawableSize` 同步、挂接生命周期、双闸门与暂存补投、
   `open→seek→play→EOF`、`close()` 幂等可重开、`deinit` 不崩且真的释放（连同
   桥与资源盒一起回收）、`close()` 后的传输控制不复活状态、回调在主线程，
   **共 58 条**（用例清单与增长历史见 known-gaps #68、spec §8）。
5. **demo 改造**：UIKit 页改用 `SYPlayer`（布局与控件不变，按钮判据改按
   `playback == .paused`）；新增 SwiftUI 页，`UITabBarController` 两页并存；
   App 源码不再出现任何 `Syp*`/`int32_t`/`UnsafeMutablePointer`（grep 断言进
   验收步骤）。

**非目标**（spec §2，据实排除）：公开的播放 C ABI（`include/syplayer/syp_player.h`，
缝 ①）；SwiftPM / CocoaPods / xcframework 分发；字幕、AirPlay、画中画、后台
播放会话策略；重写 demo 的全部 UI。

**与 spec 的两处实质细化**（已回填 spec §4.2/§5，不是偏离，见 spec §8）：
`open()` 成功后 Swift 门面把状态落地为 `.paused`，业务需显式调用 `play()`
才开始播放（Task 6 产品裁定，AVPlayer 惯例；核心 `paused_` 默认值未改，
29 个 ctest 与假想的直接桥调用方不受影响）；`SYPlayerDelegate` 及其默认实现
标注 `@MainActor`（Task 7 fix round 1，源码兼容，为语言模式 6 迁移预先铺路）。

### M4 判定

**通过，但有条件。**

① **`xcodebuild test` 真的被执行过，不是只编译**：Mac Catalyst、无签名、无
host（逻辑测试）destination 上从 Task 3 起一次性跑通，没有降级路线；本次
Task 8 复跑 `Executed 57 tests, with 0 failures (0 unexpected) in 7.963
(7.985) seconds`，`** TEST SUCCEEDED **`；终审修复波次后复跑
`Executed 58 tests, with 0 failures (0 unexpected) in 8.748 (8.772) seconds`
（详见 known-gaps #68、spec §8）。

② **真机与人工清单大部分未执行**：本环境没有 `osascript` 辅助访问权限，
只做到"demo 能启动、两页 tab 入口在、不崩"（截图确认），其余全部点击类
验证（UIKit 页播放/暂停/漂移/进度条/倍速/硬解、SwiftUI 页起播/URL/进度、
ruling P4 的 UIKit↔SwiftUI 来回切、旋转、真机硬解、后台/前台）都还在
`known-gaps.md` #67 的人工清单里，未执行，如实记录，不打包进"通过"。

③ **Task 2 的 R1（私有模块路线）走的是主路线，没有降级**：`MODULEMAP_PRIVATE_FILE`
+ PrivateHeaders 一次就通，两级降级（`-Xcc -fmodule-map-file=…`、公开伞头）
都没有用到，spec 决策 2（桥不进公开头）完整保持，不构成对 spec 的偏离。

④ **静态 framework（spec 决策 1）没有退成动态**：`MACH_O_TYPE = staticlib` 在
Mac Catalyst / iOS Simulator / generic iOS 三个 destination 上编译链接通过
（Task 8 复跑同样三档全部 `** BUILD SUCCEEDED **`），产物确认是
`current ar archive`，两个 App 的 `Frameworks/` 里只有 `FFmpeg.framework`，
没有内嵌 `SYPlayerKit.framework`。

**C++ 侧与部署目标三条，Task 8 当场重新验证**：`cd build && ctest -j4` →
`100% tests passed out of 29`；`tools/check-deploy-target.sh` → `全部通过
（25 个源文件，arm64-apple-ios13.0）`；两个 demo（Mac Catalyst + iOS
Simulator + generic iOS 共三档）编译全部 `** BUILD SUCCEEDED **`；验收
grep（`demo/shared/*.swift` 与整个 `demo/` 目录）零命中，反向确认 21 处
`SYPlayer` 命中 ≥ 10。

### M4 终审修复波次（2026-09-16，一波五类）

全仓终审在 `a305653` 之上又开了一轮修复，落在两个提交里，验证口径不变：

- **C1（Critical，保留环）**：`SYPlayerResources → SypPlayerBridge →
  onEof/onError block → SYPlayerResources`。桥以 `copy` 属性持有两个 block，
  block 又强捕获资源盒去读会话号，且没有任何地方把 block 置空——**每个
  `SYPlayer` 都永久泄漏桥、`PlayerCore`（含一条 dispatch 队列）与挂上去的
  `CAMetalLayer`**。`[weak self]` 是对的，泄漏的从来不是 `self`，所以原来的
  `deinit` 用例断言在了唯一一个不泄漏的对象上。修法：会话号拆成
  `SYPlayerSessionCounter`（一把锁 + 一个 `UInt64`，什么都不持有），block 只
  捕获它；`deinit` 在 `close()` 之后补一次置空作为第二道防线。回归用例加了
  两条弱引用断言，变异验证成立。
- **I1（Important，状态诈尸）**：`close()` 同步发布 `.idle`、异步拆桥，那个
  窗口里快照仍报 `hasMedia == YES`；而 `play/pause/seek/setRate` 都无条件调
  `refresh()`。于是 `close()` 后紧跟 `play()` 会把状态**永久**拉回 `.playing`。
  修法：`refresh()` 顶部 `guard isSessionLive`；新增回归用例（58 条里的那一条）。
- **I2**：`play()` 补清错误闩（与 `seek()` 对齐）。另一半——`.failed` 在门面层
  是终态而桥把错误当可恢复——登记为 known-gaps #72，正确修法（快照加错误字段）
  属于后续里程碑。
- **I3 / M7 的一半**：没有音量/静音、没有原始视频尺寸（两个 demo 页都硬编码
  16:9）、HTTP 缓存目录不可配置——**登记不实现**，见 known-gaps #73/#74 与下面
  M5/M6 的 API 缺口段。
- **API 卫生（趁没有外部消费者）**：`SYPlayerState`/`SYPlayerStatistics` 补公开
  逐字段构造器；`SYPlayerVideoView` → `SYPlayerViewRepresentable`；错误文案改走
  `NSLocalizedString`（中文仍是缺省值，行为不变）；framework 里的 demo 字样
  清理（泵队列标签、缓存目录名、`DemoVideoRenderer` → `PresentTrackingRenderer`）；
  `ENABLE_TESTABILITY` 收成 Debug-only。
- **纪律固化**：`tools/check-demo-no-c-types.sh`——spec §6.2 那条"业务侧不出现
  C 类型"的验收 grep 原先只活在计划文档的一行命令里，现在是脚本，与
  `check-abi.sh`/`check-deploy-target.sh` 同一档。

复跑结果：`Executed 58 tests, with 0 failures`；`100% tests passed out of 29`；
Mac Catalyst / iOS Simulator / generic iOS 三档 `** BUILD SUCCEEDED **`；
`check-deploy-target.sh` 与新的 grep 闸均通过。

**已知缺口不掩盖**：起播的暂停落地有一处结构上关不严的 pump-hop race
（known-gaps #70，实现细节与推演见 spec §8.5）；窗口相关路径
（`didMoveToWindow` 的 `window != nil` 分支、SwiftUI attach/detach）因
host-free 测试 bundle 里 `UIWindow` 不可构造，未被自动化覆盖，转成人工清单
（known-gaps #67）；SwiftUI 页没有文件选择器与软硬解开关（known-gaps #69）；
本轮不提供公开播放 C ABI，非 Swift 宿主仍需自己的一层（known-gaps #71，
本节"非目标"段）（以上均另见 docs/tech-debt.md"M4 收尾登记"）。

---

## M5 / M6 — 从 M4 带过来的 Swift API 缺口

M4 交付的 `SYPlayerKit` 公开 API 少了三样东西。**它们不是 bug，是 M4 的范围
边界**，但都会挡住真实接入方，所以在这里点名，等 M5/M6 动到相关层次时一并做，
而不是让它们只躺在 known-gaps 里。

1. ~~**音量与静音**（known-gaps #74）~~ —— **M6d 已交付**：`SYPlayer.volume`
   （`0...1`，非有限值口径 NaN→0/+inf→1/−inf 与负数→0）与 `SYPlayer.isMuted`，
   合并成单一增益作用在 `AudioUnitSink` 上（15ms 线性斜坡），不碰
   `AVAudioSession`（框架仍不管理 audio session，#66 依旧非目标）。
2. ~~**原始视频尺寸**（known-gaps #74）~~ —— **M6d 已交付**：
   `SYPlayerState.videoSize: CGSize?` 给显示尺寸（含 SAR 拉伸与旋转规整），
   随 `open()` 写入、`hasMedia == false` 或纯音频源时为 nil；两个 demo 页
   已删掉硬编码的 16:9，改用它。`videoGravity` 三态（`.aspectFit`/
   `.aspectFill`/`.resize`）一并交付，取代原计划的 tech-debt "M4 收尾登记"
   #5（该条已移入 tech-debt「已解决」）。
3. ~~**HTTP 缓存目录不可配置**（known-gaps #73）~~ —— **M5 已交付**：
   `SYPlayerCacheConfiguration`（目录 / `maxBytes` / `minFreeSpaceBytes` /
   `timeToLive`）+ `SYPlayer(cache:)` + `SYPlayerPreloader(cache:)`；默认目录
   从 tmp 挪到 `Caches/syplayer-http-cache`；容量三项在
   `CacheStore::enforce_capacity()` 里真正生效。作用域是**每个实例一份**，但
   目录相同即共享同一份缓存索引（目录经 `syp::dl::normalize_cache_dir()` 归一化）。
   **三条 M4 遗留 API 缺口本节记的全部已交付**（音量/静音、原始视频尺寸见
   M6d 一节判定；缓存目录见 M5 判定）。

---

## M5 — 预加载　✅ 已交付（2026-09-20）

- ✅ 三级优先级（`Playing` / `Next` / `Background`），只影响并发额度分配，
  不改写 `Scheduler` 的窗口语义
- ✅ 时间维度预加载：`syp_media_info_provider` 函数表把 time→byte 从 media 层
  注入 dl 层，**接口里没有 `AVFormatContext*`**；未安装或估不出时退化为按字节
- ✅ 预加载与播放共用缓存：`CacheStore` 按 cache key 在进程内共享一份
  `CacheIndex`/`CacheFile`，顺带修掉了"同 URL 两个 source 互相整份覆盖区间表"
  这个一直存在的 bug
- ✅ 容量策略落地：`max_cache_bytes` / `min_free_space_bytes` / `cache_ttl_ms`
  三项真正生效，LRU 淘汰**跳过正被打开的 key**
- ✅ HLS 预加载：最小播放列表扫描器（只认 URI 与 EXTINF），master → 第一个
  variant → `#EXT-X-MAP` + 前 K 个分片；加密一律放弃，密钥一次都不请求
- ✅ 公开 API：C 层 `syp_preloader_*`（`include/syplayer/syp_preload.h`），
  Swift 层 `SYPlayerPreloader` + `SYPlayerCacheConfiguration`
- ✅ demo 预加载演示页（含进程内只监听环回的样例服务器 + 无人值守自检入口）

**未做（明确推给以后）**：跨进程缓存共享、预测性预加载、预解码首帧、
socket 池/预连接（M6）。遗留登记见 known-gaps #76~#85 与
`docs/tech-debt.md` 的「M5 收尾登记」。

**M5 顺带交付的一道流程闸**：`ruby demo/generate_xcodeprojects.rb --check`
——两个 demo 的 `project.pbxproj` 漏了 5 个源文件，`xcodebuild test` 因此
**从 Task 1 红到 Task 8**，整整七个 task 没人跑过一条 Swift 测试。详见
`CLAUDE.md`。

---

## M6 — 流控、连接池、HLS

M6 按提交顺序拆成四个子里程碑，**M6d 先行**（不依赖其余三块，且直接关掉
M4 遗留的两条 API 缺口），之后按 M6a → M6c → M6b 的顺序推进：

| 子里程碑 | 内容 | 依赖 | 状态 |
|---|---|---|---|
| **M6d** | 显示几何（SAR / 旋转 / gravity）与音量/静音 | 无（先行） | 已完成，判定见下 |
| **M6a** | 令牌桶限速，**进程全局一份**（不是按源/按任务） | 无 | 已完成，判定见下 |
| **M6c** | 坏任务检测（滑动窗口相对基线 + 乘法衰减恢复）——**须区分"因限速未发出"与"已发出但卡住"**（M6a spec §7，见下面 M6a 判定） | M6a（判定语义耦合，非代码依赖） | 已完成，判定见下 |
| **M6b** | 连接池/预连接，**已大幅缩水**：本轮只做"让 `enable_socket_pool`/`idle_task_keep`/`idle_task_ttl_ms` 三个死旋钮诚实 + 预连接"，不做完整连接池管理——那三个字段今天只有 `source_bridge.cpp:117-119` 的 `syp_config_init()` 在赋值，全仓库无任何一处读，是公开头上一条与实现不符的契约（与 known-gaps #3 同类） | 无 | 已完成，判定见下 |

四块之间无接口层面耦合，可独立排期，这是拆分与排序的依据。

**M6 整体已完成（2026-09-22）**：四个子里程碑全部交付并各有判定（见下）。"完成"
不等于"没有遗留"——实网数据缺口（known-gaps #100 / #104 / #107 / #108）、预加载
突发透支（#102）、显示几何的镜像与非 90° 旋转（#94 / #95）、人工验证清单未执行
（#96）都已登记、明确不在 M6 内修。原标题里的"HLS"早已由独立的 M-HLS 里程碑交付。

### M6d — 显示几何与音量（已完成，2026-09-21，判定见下）

- ✅ `TrackInfo` 追加 `sar_num`/`sar_den`/`rotation_deg` 三字段，采集点在
  `demuxer.cpp`；SAR 按 ffplay 取法 `av_guess_sample_aspect_ratio()`（优先容器层，
  终审 I2 订正）；`rotation_deg` 的符号按 FFmpeg 8.1.2 源码钉死
  （`ffmpeg.texi:1549`/`cmdutils.c:1557`/`ffplay.c:2025`），用真实带
  `rotate=90` 元数据的素材双向验证
- ✅ 平台无关纯函数 `blit_transform()`/`display_size()`
  （`src/media/video_geometry.h`），`video_geometry` 新 ctest 目标穷举
  3 种 gravity × 4 象限 × 3 组 SAR × 3 种容器形状
- ✅ `MetalRenderer` 覆盖 `set_source_geometry()`/`set_gravity()`，着色器
  `BlitParams` 与 C++ 结构体逐字段一致（`packed_float2`），GPU 离屏回读
  钉住四象限旋转与三种 gravity 的实际出图
- ✅ `IAudioSink::set_gain()`，`AudioUnitSink` 按帧插值、15ms 线性斜坡，
  静音与暂停正交（`played_us()` 照常推进）
- ✅ `TrackPlayer` 汇合：音量/静音/gravity 的持久状态放在桥的
  `PlayerCore` 上（不是 spec §3.7 原写的 `TrackPlayer`，见下面的偏差
  记录），跨 `open()` 保留；每次 open 作为 `InitialSettings` 传进
  `TrackPlayer::create()`、在 sink `open()` 之前落下（终审 I1 订正时序）
- ✅ Swift 层：`SYPlayer.volume`/`isMuted`、`SYPlayerState.videoSize`、
  `SYPlayerVideoGravity` 三态；两个 demo 页删掉硬编码 16:9；UIKit 播放页加
  音量/静音/gravity 控件（**SwiftUI 页没加**，见偏差）

**与 spec 的偏差**（详见设计文档 §8「落地对照」，此处只列摘要）：
pbxproj 生成器同步从计划 T7 前移到 T1（否则中间任务 Xcode 侧链接失败）；
持久状态放桥的 `PlayerCore` 而非 `TrackPlayer`（`TrackPlayer::create()` 是
每次 open 新建的静态工厂，spec 指定的存放位置与其生命周期矛盾）；旋转
符号按 FFmpeg 源码订正（计划原文的兜底指示作废）；测试素材现场合成
（`tests/support/synth_media.h`），不进 `gen-fixtures.sh`；终审修复波：SAR
改为优先取容器层（原计划只读 `codecpar`，丢 mp4 `pasp`）、持久音量/静音改为
在 sink `open()` 之前下发（原"create 之后重新下发"让持久静音每次 open 开头
漏出约 15ms 近满音量）、SwiftUI 页未加音量/静音/gravity 控件（gravity 需要改
公开类型 `SYPlayerViewRepresentable`，且该页无自检载体）。

#### M6d 判定

**通过，但有条件。** 自动化验收全部通过（终审修复波之后重跑）：ctest
34/34、713 个用例（含新增 `video_geometry` 目标）、TSan/ASan 34/34 零报告、
`xcodebuild test` 全绿（Executed 103 tests, 0 failures）、五道闸门 +
`--check` 全绿、四套 Xcode 构建（mac/ios × Debug/Release，Release 带
`ARCHS=arm64`）全部 `BUILD SUCCEEDED` 且零新增编译告警、两个 demo 自检 `EXIT=0`。

但**不是** spec 第 5 节的每一条验收标准都落地了，如实列出：
- §5.2 的"`set_gain` 与 `flush()`/路由变化并发"TSan 专门用例**没有写**
  （spec §5.2 已就地标注理由：两者无共享可变状态；TSan 只覆盖全量 ctest）。
- §3.8 要求两个 demo 页都加音量/静音/gravity 控件，**SwiftUI 页没有加**，
  只改了宽高比（spec §8.3 第 16 条登记为偏差，known-gaps #96 补记）。
- §5.2 的"`open()` 不淡入"在 Task 1–8 **从未编写**，终审修复波（I1）才补上，
  现已存在，不再是缺口。

**"有条件"的部分，与 M3a/M3b/M3c 同一类——下面几项人工验证明确没有做，
不打包进"通过"里掩盖掉**：

1. **旋转画面在真机上的方向正确性**——自动化只验证了"画面按预期的象限
   变换了"（GPU 离屏回读），"在真机屏幕上看起来是正立的"需要人眼，见
   known-gaps #96。
2. **增益斜坡（15ms）的实际听感**——自动化只验证了数值单调逼近、不过冲，
   "听不出咔哒声"需要真实设备，见 known-gaps #96。
3. **gravity 效果在 demo 里肉眼验不出**——画面区宽高比等于视频宽高比，
   Fit/Fill/Resize 出图几乎一样，自检只验接线（终审 M6，known-gaps #96）。

已知不在本轮范围内、登记不修：显示矩阵的镜像分量未处理（known-gaps #94）、
非 90° 倍数旋转就近取整会静默改变方向（known-gaps #95，spec §1 非目标）、
`AVAudioSession` 仍不受管理（known-gaps #66，本轮非目标）、
`MetalRenderer` 硬件路径仍有两处结构性盲区（known-gaps #24 M6d 补记）。

---

### M6a — 令牌桶限速（已完成，2026-09-21，判定见下）

- ✅ `RateLimiter`（`src/dl/rate_limiter.{h,cpp}`，新文件）：进程全局单例令牌桶，
  毫字节记账、`Playing`/`Preload` 两类准入阈值（余额 `>0` / `>容量/2`）、订阅式
  唤醒（`arm()` + 回调，取代 spec 原写的一次性 `Waiter`，写实现计划时发现两种
  自锁，spec §3.1 已同步）、唤醒线程创建失败时 fail-open（记 WARN、速率置 0、
  同步补跑一次已 armed 订阅的唤醒，避免 spec §2 决策 5 点名的"播放永久卡住"）
- ✅ `Scheduler` 接线（`src/dl/scheduler.{h,cpp}`）：到达记账、发片前准入、
  分片上限（`R × 1 秒`）覆盖 `segment_size_locked()` 全部出口、`set_rate_class`
  下一次 `schedule()` 生效
- ✅ 类别来源：播放源恒为 `Playing`；预加载条目按 `PreloadPriority` 映射
  （`Playing` → `Playing`，`Next`/`Background` → `Preload`），`Preloader` 驱动
  线程锁外同步下发
- ✅ 公开 C API `include/syplayer/syp_net.h`（新文件）：`syp_rate_limit_set/get`，
  进程级下行限速（字节/秒），0 = 不限；`tools/check-abi.sh` 清单已加入
- ✅ Swift `SYPlayerNetwork.maximumDownloadRate`（`swift/SYPlayerKit/
  SYPlayerNetwork.swift`），经 `SypNetworkBridge` 转发；预加载演示页加限速档位
  （`UISegmentedControl` 四档，非 spec §3.6 原写的滑块）与状态行，无人值守自检
  新增一步

**与 spec 的偏差**（详见设计文档 §8「落地对照」，此处只列摘要）：唤醒机制从
一次性 `Waiter` 改为订阅式 `Subscription`（brainstorming 定稿后写计划时改，
spec 正文已同步）；Δt 夹取与"已 armed 时 arm 的语义"两处、`~Scheduler` 析构
顺序、demo 用分段控件而非滑块——均已在 spec 正文就地标注；`fire_due` 逐个派发
防同批 UAF、`wake` 异常兜底降级为软契约；分片上限覆盖全部出口，no-Range 源
多一次探测往返；C 入口异常兜底 + fail-open 降速（M5 终审 F11 先例，spec 原文
未预料到这条健壮性要求）；Swift 不标 `@MainActor`；三处公开文档补写 fail-open
同步调度的调用方契约。

#### M6a 判定

**通过，但有条件。** 自动化验收全部通过：ctest 35/35（35 个目标、748 个用例，
含新增 `rate_limiter` 目标；终审修复波后 749，`scheduler` +1）、TSan/ASan 35/35 零报告、`xcodebuild test` 全绿
（Executed 106 tests, 0 failures）、五道闸门 + `--check` 全绿、四套 Xcode 构建
（mac/ios × Debug/Release，Release 带 `ARCHS=arm64`）全部 `BUILD SUCCEEDED` 且
零新增编译告警、两个 demo 自检 `EXIT=0`、`probe_e2e` 单独重跑 3 次均稳定通过
（未观察到 CLAUDE.md 点名的 `-j4` 负载抖动）。

**"有条件"的部分，如实列出，不打包进"通过"里掩盖掉**：

1. **真实网络（蜂窝 / 高 RTT）下的速率准确度未经实测**——1 秒容量与保留线
   （容量 / 2）都只在 127.0.0.1 回环上验证过，见 known-gaps #100。
2. **fail-open 路径仍有三条已知窄窗口**（均只在"唤醒线程创建失败"这个罕见
   路径上出现）：(b) `thread_started_` 判据的顺序竞争（后果仅限速静默关闭）；
   (c) fail-open 成为 `fire_due` 的第二个派发者，理论 UAF 需多重叠加；(d)
   "调用线程同步调度"本身是一条已写进公开契约的约束。三条均未修，见
   known-gaps #101。原列的 (a)（调度器 `admit` 被拒后、`arm` 之前与 fail-open
   派发交错 ⇒ 该 `Scheduler` 永久卡住）已在终审修复波修掉（终审 I1：`arm`
   之后复查一次准入，用例 `fail_open_between_rejected_admit_and_arm_does_not_stall`）。
3. **"为播放保留余量"在预加载突发时不成立**（终审 I2，只登记、不改算法）：
   准入只看此刻余额、扣账要等数据到达，一次调度能放行
   `max_concurrent_tasks` 个预加载分片，余额被扣到远低于 0（实测 Preload
   6 槽 ⇒ −5R，Playing 随后等 5001 ms；生产默认非 Playing 至多 3 条连接 ⇒
   播放最多约 2.5 秒在还预加载的账）。公开措辞已改为"预加载只在余额高于
   保留线时才能开始新请求，播放在预加载突发之后可能短暂等待"；真修需要
   在途预加载记账，列为后续项（可与 M6c 一起考虑），见 known-gaps #102。

已知不在本轮范围内、登记不修：只控平均速率（known-gaps #97，spec §1 非目标）、
**播放过程中**抓取的 HLS 播放列表不受限（预加载抓取的播放列表**受限**，按
`Playing` 类别走正常准入，见 known-gaps #98，有意豁免；预加载的元数据探测同样
按 `Playing` 准入，终审 I3 补登于同一条）、服务端不支持 Range
的整文件下载不受限（known-gaps #99，spec §1 非目标）。

**对 M6c 的影响**（spec §7）：坏任务检测在 `DLTask`/`Scheduler` 上读"本次尝试
已持续时长"与速度时，**被限速器拒绝准入的等待时间不是坏任务**——M6c 设计时
必须把"因限速未发出"（根本没有在途任务）与"已发出但卡住"区分开，已加进上面
M6 拆分表 M6c 行的备注。

---

### M6c — 坏任务检测（已完成，2026-09-22，判定见下）

- ✅ `HealthTicker`（`src/dl/health_ticker.{h,cpp}`，新文件）：进程单例计时线程，
  订阅模型照抄 `RateLimiter::Subscription`；`arm_after(delay)`（相对延迟，不是 spec
  原写的绝对时刻——单例用系统时钟、`Scheduler` 可能用假时钟）、`warm_up()` 把懒启动
  提前到 `Scheduler` 构造期（不持锁）；线程起不来时 fail-open = 检测关闭
- ✅ `DLTask::attempt_started_ms()` / `last_progress_ms()`：本次尝试起点与最近进展
- ✅ 挂死兜底（`Scheduler::health_check()`）：`Connecting`/`Receiving` 的尝试在
  `max(起点, 最近进展)` 之后超过"对应超时 + `stall_grace_ms`（2000）"⇒ cancel、从
  `next_offset` 重发、按一次 `SYP_ERR_TIMEOUT` 计错，满 `max_consecutive_errors` fatal
- ✅ 慢任务替换：相对本 `Scheduler` 基线（健康样本 EWMA α=1/8）慢 4 倍、连续 2 次
  检查才判；替换后基线 ×½ 并冷却一个速度窗口；每轮至多换一个；按当前速度剩余时间
  不超过建连估计的不换；不支持 Range 时不换；正速度慢替换不计错；整窗零字节的
  "零速"替换照样快速换，但只有静默已达有效读超时才按超时计错（终审 I1，推翻 Task 4
  的"零速一律计错"——那条规则在生产默认下让"连着不走数据"约 12s 就 fatal）
- ✅ 挂死阈值在 `connect_timeout_ms ≤ 0` 时借 `read_timeout_ms`（与 Apple 后端一致，终审 M2）；
  被判坏的任务在 cancel 落地前自己失败不重复计错（`killed_by_health`，终审 I1b）
- ✅ 限速器拒绝准入的等待**不是坏任务**：被拒时没有在途任务、ticker 不 arm
  （M6a spec §7 的要求，`rate_limited_wait_is_not_a_bad_task` 钉住）
- ✅ ticker 线程永不成为 `DLTask` 的最后持有者（`deferred_release_`，Task 3 复审 I1/N1）
- ✅ 端到端：`test_source_bridge` 的 `slow_loris_connection_is_replaced_end_to_end`
  （回环服务器 + 真实 Apple 后端 + 真实时钟，只在 APPLE 下编译）；回环服务器新增
  "只让前 K 个供体请求细水长流"

**与 spec 的偏差**（详见设计文档 §7「落地对照」，此处只列摘要）：`arm_at` → `arm_after`
与新增 `warm_up()`（spec §3.1 就地标注）；零速慢替换的计错规则（终审改为"静默达读超时
才计"）、冷却按每轮开头判定、Connecting 阈值借读超时、`killed_by_health`（spec §3.3
就地标注）；`health.enabled == false` 时不订阅、不 warm_up；新增 `deferred_release_`
机制（spec 未预料）；端到端用例无法断言 `slow_kills`（`SourceBridge` 不暴露），改为从
服务端侧断言；`stall_replacement_resumes_from_next_offset` 并入
`stall_in_receiving_uses_read_timeout_from_last_progress`；`test_source_bridge` 在
APPLE 下改链 `syp_platform_apple`。

#### M6c 判定

**通过，但有条件。**（终审修复波后：ctest 36/36、**794** 个用例、五道闸门 + `--check` 全绿；
零速计错规则改为"静默达读超时才计"、Connecting 阈值借读超时、`killed_by_health`，见上面
列表。下面是 Task 5 收尾时的数字。）自动化验收全部通过：ctest 36/36（36 个目标、790 个用例，含
新增 `health_ticker` 目标）、TSan 36/36 零报告（790 个用例）、`xcodebuild test` 全绿
（Executed 106 tests, 0 failures）、五道闸门 + `--check` 全绿、四套 Xcode 构建
（mac/ios × Debug/Release，Release 带 `ARCHS=arm64`）全部 `BUILD SUCCEEDED`。
`scheduler` 目标的 ctest TIMEOUT 从 45 抬到 120：TSan 下单独跑实测 46.7s，收尾时
全量 TSan `-j4` 在第 64/65 条被判 Timeout（Task 4 新增的大尺寸慢判定用例所致，不是
挂死）；抬高后重跑 TSan 全量 36/36。`probe_e2e` 在 `-j4` 下未观察到抖动。

**"有条件"的部分，如实列出，不打包进"通过"里掩盖掉**：

1. **阈值没有实网数据**——1/4、2 strikes、α=1/8、×½、2s 宽限都只在假时钟与
   127.0.0.1 回环上验证过，见 known-gaps #104。
2. **后端对 cancel 也不回调时被判坏的任务收不回**（spec §1 非目标）：播放不受影响
   （区间照常重发），但对象留到 `~Scheduler`，见 known-gaps #103。
3. ~~**health 判坏与真实失败交错时会双重计错**~~——终审修复轮已修（`killed_by_health`，
   `docs/tech-debt.md`「M6c 收尾登记」#1）。
3a. **"连着不走数据"到 fatal 的时间**（终审 I1 之后）：生产默认下约 T0+24s（三条原任务
   快速替换不计错，替换任务各自 Connecting 挂死计错，第三次 fatal），见 spec §4 表
   "全断 / 连着不走数据"一行。比 M6c 之前（复审者估计约 45–60s）快、比旧规则（约 12s）慢；
   同样没有实网数据（known-gaps #104）。
4. **端到端只能从服务端侧断言**，且挂死兜底没有端到端用例，见 known-gaps #106；
   端到端用例的 20s 上界是负载敏感类（tech-debt「M6c 收尾登记」#4）。
5. 存活/只经测试缝杀死的变异（Task 3 #8 等价、修复轮变异 F、NB3 守卫无确定性用例、
   变异 C/D 只经计数缝）逐条见 tech-debt「M6c 收尾登记」#3。

已知不在本轮范围内、登记不修：基线每 `Scheduler` 一份、新源前一两个窗口不做慢判定
（known-gaps #105，spec §6.2）；HLS 播放列表抓取不受检测（spec §1 非目标）；
M6a 遗留的"预加载突发透支"（known-gaps #102）**没有**随 M6c 顺带处理。

---

### M6b — 死旋钮与预连接（已完成，2026-09-22，判定见下）

- ✅ 三个死旋钮如实：`syp_config` 的 `enable_socket_pool` / `idle_task_keep` /
  `idle_task_ttl_ms` 注释改为"保留字段，当前忽略"，说明连接复用由后端负责、提前建连
  请用 `syp_preconnect`。字段不删（`struct_size` ABI 只能追加），`syp_config_init`
  照旧填默认值
- ✅ `Preconnector`（`src/dl/preconnector.{h,cpp}`，新文件）：进程单例、无自有线程；
  经全局后端用 `DLTask` 发 `Range: bytes=0-0` 的 GET（`max_retries = 0`），响应丢弃、
  不写缓存、不受限速；按 scheme+host+port 30 秒去重、同时在途上限 4（超出丢弃）；
  206 不 cancel（连接还回池），200 全量收到首块即 cancel；已结束任务在下一次调用时
  锁外回收
- ✅ 公开入口：C `syp_preconnect(url, headers)`（`syp_net.h`）、Swift
  `SYPlayerNetwork.preconnect(_:headers:)`；demo 预加载演示页加"预连接"按钮
- ✅ 回环服务器 keep-alive 模式（`LoopbackConfig::keep_alive`，默认 false）与
  `accepted_connections()`；端到端 `preconnect_warms_connection_reused_by_playback`
  （`test_source_bridge`，真实 Apple 后端）断言预连接的连接被播放复用

**与 spec 的偏差**（详见 spec §7）：构造多一个 `ctx`；206 不 cancel（spec 原写"≥ 1 字节
即 cancel"）；Entry 多 `task_raw` / `start_returned`；`start()` 抛异常可回收；
`origin_key` 多几条端口/IPv6 规则；端到端用例放在 `test_source_bridge` 而非计划首选的
`test_preloader`。

#### M6b 判定

**通过。** ctest 37/37（**814** 个用例，新增 `preconnector` 目标 18 条、`source_bridge`
+2）、TSan 全量 37/37、`xcodebuild test` 全绿（Executed 108 tests, 0 failures）、五道闸门
+ `--check` 全绿、四套 Xcode 构建（mac/ios × Debug/Release，Release 带 `ARCHS=arm64`）
全部 `BUILD SUCCEEDED`。spec §5.2 的降级条款**没有触发**：主端到端用例单独连跑 10 次，
`accepted_connections() == 1` 10/10。

**遗留，如实列出**：

1. 复用只在明文 HTTP/1.1 + 回环 + Apple 后端上验证过；TLS、HTTP/2、真实 CDN 的空闲
   回收、其它后端都没测，也没有"首请求变快"的数字，见 known-gaps #107。
2. 30 秒 / 4 是经验值（#108）；不支持 Range 时多收一个 chunk（#109）；预连接对调用方
   没有可观测结果（#110）。
3. "206 也 cancel"这条变异在端到端上存活，206 不 cancel 的修复只被桩计数缝钉住；
   `Preconnector` 三处并发防护没有确定性用例——见 `docs/tech-debt.md`「M6b 收尾登记」
   #1 / #2。

---

## M6b — 连接池（已完成，由上面"M6b — 死旋钮与预连接"一节交付）

以下是 M6 拆分前的原计划条目，保留作历史说明：

- ~~连接池预热 / 预连接 / DNS 缓存（M6b，已缩水，见上面拆分表）~~ —— **已交付为
  "死旋钮如实 + 预连接"**：连接池本身是后端职责（不在 dl 层自建），DNS 预解析交给
  后端（spec §1 非目标）

  ⚠️ （历史警告，M6c 之前写的）**`DLTask::speed_bps()` 兜不住"建连挂死"。** 它对
  "从未收到过任何字节"的任务返回 `nullopt`（"估不出"），包括 TLS 握手卡死 30 秒。纯
  `Connecting` 挂死在速度维度上**不可见**，只剩后端的 `connect_timeout_ms` 能兜。坏任务
  检测的输入里必须显式包含 `state()` 与本次尝试已持续时长。
  （反过来，**收过数据之后**再断流，窗口滑空后返回 `0`，这种停滞是可见的。）
  **M6c 已按此把 `state()` 与尝试时长（`attempt_started_ms` / `last_progress_ms`）纳入
  输入**：挂死判定不看速度，看"`max(尝试起点, 最近进展)` 起过了多久"。

- ~~HLS：m3u8 解析（自写，不用 FFmpeg 的 hls demuxer，因为需要 ts 级调度粒度）~~
  ——**已被 M-HLS 里程碑取代，方案不同**：M-HLS（见上面独立一节）用的是
  FFmpeg 自带的 `hls` 解封装器 + 本项目接管其全部 IO，不是自写解析器。留着
  这一行是为了让读到旧计划的人知道它已被更新的决定取代，不是两条并存
  的待办。

---

## SwiftPM 分发（已完成，2026-09-22，判定见下）

- ✅ Swift 层平台适配：原生 macOS 的 AppKit 版 `SYPlayerView`（`NSView`）与
  `SYPlayerViewRepresentable`（`NSViewRepresentable`），公开成员与 UIKit 版一致
- ✅ 分发工程生成：`demo/generate_xcodeprojects.rb --dist <dir>`，一个动态 SYPlayerKit
  framework 目标、库演进模式，构建期生成、不入库
- ✅ `tools/build-xcframework.sh [--version X.Y.Z]`：四个平台逐个 archive，合成
  `SYPlayerKit.xcframework`（带 dSYM 与 `.swiftinterface`），拷入 `FFmpeg.xcframework`，
  打 zip、算 checksum；带 `--version` 时回填 `Package.swift`
- ✅ 根目录 `Package.swift`：产品 `SYPlayerKit`（两个 binaryTarget），iOS 13 /
  Mac Catalyst 14 / macOS 11；默认远程 Release，`SYPLAYER_LOCAL_BINARIES=1` 走本地产物
- ✅ 两个示例 App（`examples/iOSExample`——同时跑 Mac Catalyst、`examples/macOSExample`，
  `ruby examples/generate.rb` 生成，确定性 UUID + `--check`，只含相对路径）：唯一依赖是
  仓库根的包，共用 SwiftUI 播放页，各带一个 hosted 测试目标
- ✅ 门禁 `tools/check-spm.sh`：本地产物模式下 iOS 模拟器、Mac Catalyst、原生 macOS 三路
  `xcodebuild test`（钉死期望用例数 2 / 2 / 3），核对解析出的产物来源为本地；外加生成器
  `--check`、工程文件路径/库引用正则、不引私有模块、`.swift` 都在工程里四项静态检查

**与计划的偏差**：计划里的独立 SwiftPM 包 `examples/SPMConsumer`（三平台冒烟）按用户
追加要求改成两个示例 App（iOS、原生 macOS），测试 hosted 在 App 里跑——既是给接入方看
的样例，又顺带验证了"样片作为 App 资源打包""SwiftPM 把两个动态 framework 内嵌进 App"
这两件独立包测不到的事。Mac Catalyst 由 iOS 示例打开 `SUPPORTS_MACCATALYST` 覆盖（首轮漏了，
复审补上，known-gaps #116 已标已修）。

#### SwiftPM 分发判定

**通过（远程模式待首次发版后验证）。** `tools/check-spm.sh` exit 0：iOS 模拟器
Executed 2 tests、Mac Catalyst Executed 2 tests、原生 macOS Executed 3 tests，全部
0 failures，三路解析出的 `SYPlayerKit` / `FFmpeg` 来源都是 `local`（`build-spm/out`）。
反证：把本地 `SYPlayerKit.xcframework` 的 `macos-arm64` 切片挪走再跑，macOS 那一路
EXIT=65、没有 Executed 行，门禁 exit 1；挪走 `ios-arm64-maccatalyst` 切片同理，只有
Catalyst 那一路红。新增用例做了变异验证（每个一次、红的
都是预期的那一条）。既有五道门禁 + `--check` 全绿、ctest 37/37、demo 的
`xcodebuild test` Executed 109 tests 0 failures、demo 四套 Xcode 构建（mac/ios ×
Debug/Release，Release 带 `ARCHS=arm64`）全部 `BUILD SUCCEEDED`。

**遗留，如实列出**：

1. 远程模式（默认、接入方的正常路径）要等人上传 GitHub Release 之后才能解析，发版前
   必然 404（known-gaps #113）。
2. Intel Mac、tvOS / visionOS / watchOS 不支持（#111、#112）。
3. demo 静态 / 分发动态两套配置可能漂移，门禁依赖先手动重编产物（#114）。
4. Xcode GUI 下本地包环境变量是否生效没有实测（#115）。

---

## FFmpeg 私有化隔离（已完成，2026-09-22，判定见下）

- ✅ `tools/build-ffmpeg.sh` 产出 `SYFFmpeg.xcframework`：`ld -alias_list` 给静态库里每个
  全局符号起 `syp_` 别名、`-exported_symbols_list` 只导出别名；改名清单、映射头
  `syp_ffmpeg_prefix.h`、`NOTICE` 同源现生成；源码补丁目录 `tools/ffmpeg-patches/`
  （为空也跑通）
- ✅ 全仓改用 `SYFFmpeg`：CMake INTERFACE 目标 `syp_ffmpeg`（`-include` 映射头，经
  `syp_media` 传给所有消费方）、demo 两工程与分发工程的 `OTHER_CFLAGS`、SwiftPM
  `binaryTarget` 改名 `SYFFmpeg`（分发件不带头文件），`Package.swift` 回填新 checksum
- ✅ 门禁 `tools/check-ffmpeg-symbols.sh`：导出表零未加前缀符号（逐 slice、逐架构），
  映射头与导出表同源，我们各静态库 / 分发 SYPlayerKit 零未加前缀引用；挂在 CMake
  `ALL` 与 ctest `ffmpeg_symbols`，`check-spm.sh` 对分发件再跑一遍
- ✅ 冲突集成测试 ctest `ffmpeg_coexist_*`：假的"接入方 FFmpeg"（导出真实函数名、返回
  假值、记账）与 syp_media + SYFFmpeg 按五种方式链接（静态假库 × SYFFmpeg 在前 / 假库在前 /
  `-all_load`，动态假库 × SYFFmpeg 在前 / 假库在前），断言无 duplicate symbol、接入方
  直接调用拿到假值、我们经 `Demuxer` 打开样片得到 640×360、假库零次被我们调到
- ✅ 文档：README「与自带 FFmpeg 的项目共存」，known-gaps #117 标已修、新增 #120（两份
  FFmpeg 的体积）、#121（映射头宏名的撞名处理）

**与设计的偏差**：设计里冲突测试只有静态假库的三种链接方式，落地加了动态假库的两种，
两种形态的接入方 FFmpeg 都覆盖。假静态库按真实 libav* 的样子拆成多个归档成员（记账一个、
每个假函数各一个）：ld64 里静态库的定义只有在其成员被加载时才参与——被别的引用强制加载，
或静态库排在前面先被搜到——否则排在前面的动态库胜出。首版把记账与假函数放在同一个成员，
接入方对记账接口的引用强制加载了它，`static_syffmpeg_first` 因此在下面的第一条变异下假绿；
复审指出后拆开，现在它按预期变红。设计 §1 问题 1（静态链接的接入方 FFmpeg 排在导出原名的
我们之后会被抢走调用）成立。

#### FFmpeg 私有化隔离判定

**通过。** 冲突测试五个变体全绿；变异验证（一个变异一次工具调用）：

- 用同一份静态库重新链出**不带 `-exported_symbols_list`**、原名与 `syp_` 名都导出的
  SYFFmpeg，指给一个独立构建目录：`static_syffmpeg_first` 与 `dylib_syffmpeg_first`
  红，都红在断言②（接入方 `avformat_version()` 拿到 `0x3E0C66` 而不是假值 `0xFA0001`，
  假库计数全为 0）；`static_fake_first`、`static_all_load`、`dylib_fake_first` 保持绿
  ——假库在前或被 `-all_load` 全部加载时本来就该由它胜出，这三个变体守的是"不串用、
  不报 duplicate symbol"，不是这条退化。
- 我们这一侧绕过映射头直接调原名 `av_malloc`/`av_free`：`static_syffmpeg_first` 红，
  红在断言④（假库记到 `av_malloc`、`av_free` 各 1 次，`total_calls` 2≠0）。

全量回归：门禁（check-abi、check-deploy-target、check-demo-no-c-types、
check-ffmpeg-no-tls `--expect-slices 4`、check-ffmpeg-symbols、`syp_dl_purity_check`、
demo 生成器 `--check`、examples `--check`）全部 EXIT 0；ctest `build` 43/43、
`build-tsan` 43/43（829 个用例）；demo `xcodebuild test` Executed 109 tests 0 failures；
`tools/check-spm.sh` EXIT 0（三路 2/2/3 + 三个 zip 形态）。

**遗留，如实列出**：

1. App 里两份 FFmpeg 的体积（known-gaps #120），不打算消除。
2. 分发的 SYFFmpeg 仍未 strip（#118）。
3. 冲突测试只在 CMake 桌面构建（macOS arm64）上跑，SwiftPM 接入层没有接入方自带
   FFmpeg 的变体；那一层的隔离靠 `check-spm.sh` 对分发件的符号门禁保证。

---

## 未来 — Android 接入

- 实现 `syp_http_backend`（OkHttp / Cronet）
- `GLESRenderer`：`MediaCodec → SurfaceTexture → GL_TEXTURE_EXTERNAL_OES` 零拷贝路径
- MediaCodec 解码器
- ⚠️ 届时需实测 NDK 的 libc++ 对 C++23 的支持程度
