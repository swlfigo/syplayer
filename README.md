# syplayer

中文 · [English](README.en.md)

**Apple 平台的视频播放 SDK，自带下载缓存层。**

业务侧写一个 SwiftUI 视图加一行 `open`，就能播本地文件、HTTP(S) 视频和 HLS；
底下是自研的分段并发下载 + 空洞调度缓存层：seek 到没下过的位置**只补缺口**，
已经下过的字节永远不重下；索引崩溃安全地落盘，进程重启后**续下而非重下**。

```swift
import SYPlayerKit

let player = SYPlayer()
try await player.open(.url(URL(string: "https://example.com/movie.m3u8")!))
player.play()
```

通过 SwiftPM 接入，两个预编译 xcframework，不用配置任何链接设置。
工程里已经有 FFmpeg 也不冲突——我们这份做了符号级隔离（见
[与自带 FFmpeg 的项目共存](#与自带-ffmpeg-的项目共存)）。

## 能做什么

- **播放**：本地文件、HTTP(S) 渐进式 MP4、HLS（点播 / 直播 / 多码率选轨 / 分轨音视频）
- **缓存**：seek 只补缺口、进程重启续下、播放与预加载共用同一份缓存、容量与 TTL 可配
- **预加载**：按时长或字节预热后续视频，三级优先级，预热过的字节播放时直接命中
- **画面**：VideoToolbox 硬解 + Metal 零拷贝上屏，三种填充方式，SAR 与旋转已规整
- **声音**：AudioUnit 输出（macOS HALOutput / iOS RemoteIO），音量与静音，音频钟为主时钟、失败降级到系统钟
- **流控**：进程级下行限速（播放优先）、预连接、坏任务自动替换（挂死与异常慢的连接）
- **共存**：接入方自带任意版本 FFmpeg 也能直接接入，两份互不串用
- **合规**：随包带隐私清单与 FFmpeg 的 LGPL 许可证正文、NOTICE

## 快速开始

### 1. 加依赖

Xcode：File → Add Package Dependencies…，填
`https://github.com/swlfigo/syplayer.git`，规则选 Up to Next Major `0.1.0`，
把 `SYPlayerKit` 加到 App target。

`Package.swift` 写法：

```swift
dependencies: [
    .package(url: "https://github.com/swlfigo/syplayer.git", from: "0.1.0"),
],
targets: [
    .target(name: "MyApp", dependencies: [
        .product(name: "SYPlayerKit", package: "syplayer"),
    ]),
]
```

### 2. 播起来

```swift
import SwiftUI
import SYPlayerKit

// player 由调用方创建并持有生命周期（例如放在 App 或上层视图的一个属性里），
// 这里用 @ObservedObject 接收，而不是 @StateObject——后者是 iOS 14 的 API，
// 本仓库最低支持 iOS 13。
struct PlayerScreen: View {
    @ObservedObject var player: SYPlayer

    var body: some View {
        VStack {
            SYPlayerViewRepresentable(player: player)
                .aspectRatio(player.state.videoSize.map { $0.width / $0.height } ?? 16.0 / 9.0,
                             contentMode: .fit)

            Text(statusText)

            HStack {
                Button("播放") { player.play() }
                Button("暂停") { player.pause() }
                Button("后退 10 秒") { player.seek(to: max(0, player.state.position - 10)) }
            }
        }
        .onAppear {
            Task {
                do {
                    try await player.open(
                        .url(URL(string: "https://example.com/movie.mp4")!),
                        decoding: SYPlayer.isHardwareDecodeAvailable ? .hardwarePreferred : .software)
                    player.play()
                } catch {
                    print("打开失败：\(error)")
                }
            }
        }
        .onDisappear { player.close() }
    }

    private var statusText: String {
        switch player.state.playback {
        case .idle:              return "未打开"
        case .buffering:         return "缓冲中"
        case .playing:           return "播放中"
        case .paused:            return "已暂停"
        case .ended:             return "已结束"
        case .failed(let error): return "失败：\(error)"
        }
    }
}
```

两点值得先知道：

- `open()` 成功后播放器停在 `.paused`，要出声出画得显式 `play()`。这是有意的：
  接入方常常要等首帧就绪再开始播。
- 解码方式默认 `.hardwarePreferred`，而**硬解不可用的平台会让 `open()` 抛
  `.notImplemented`**（Mac Catalyst 恒无硬解，部分模拟器也没有）。所以上面按
  `SYPlayer.isHardwareDecodeAvailable` 选，这也是仓库里示例 App 的写法。

完整的示例工程在 `examples/`（iOS 与原生 macOS 各一个，只经 SwiftPM 接入）。

## 使用说明

`SYPlayer` 与 `SYPlayerPreloader` 是 `@MainActor` 的，`SYPlayerNetwork` 可以在任意
线程调用。下面的代码对 iOS、Mac Catalyst 与原生 macOS 通用。

### 播放源

```swift
try await player.open(.url(URL(string: "https://example.com/movie.mp4")!))   // 远端，走缓存
try await player.open(.url(URL(string: "https://example.com/live.m3u8")!))   // 路径以 .m3u8 结尾 ⇒ HLS
try await player.open(.file(URL(fileURLWithPath: "/path/to/local.mp4")))     // 本地文件，不走缓存
try await player.open(.url(remote), decoding: .software)                      // 强制软解
```

### 传输控制与音量

```swift
player.play()
player.pause()
player.seek(to: 42)                  // 秒
try player.setRate(1.5)              // [0.5, 2.0]，越界抛 .invalidArgument
player.volume = 0.5                  // 0...1，非有限值按 NaN→0 / +inf→1 / 负数→0 规整
player.isMuted = true
player.close()                       // 释放解码与网络资源；之后可以再 open
```

### 观察状态

三种方式任选，内容相同：

```swift
// 1. SwiftUI：SYPlayer 是 ObservableObject，state 是 @Published
//    （视图里用 @ObservedObject 接收，见上面的快速开始）

// 2. 异步序列
for await state in player.stateStream {
    print(state.position, state.duration ?? -1, state.buffered ?? -1)
}

// 3. delegate（UIKit 常用；三个方法都有默认空实现，只实现关心的那个）
player.delegate = self
```

`SYPlayerState` 的字段：

| 字段 | 含义 |
|---|---|
| `playback` | `.idle` / `.buffering(原因)` / `.playing` / `.paused` / `.ended` / `.failed(错误)` |
| `hasMedia` | 是否已打开媒体 |
| `position` / `duration` | 当前位置 / 总时长（`nil` = 未知或直播） |
| `buffered` | 已缓冲到的时间点（`nil` = 在播轨都已读完） |
| `hasVideo` / `videoSize` | 是否有视频轨 / 显示尺寸（含 SAR 与旋转规整，纯音频为 `nil`） |
| `isHardwareDecoding` | 当前是否在用硬解 |
| `rate` | 当前倍速 |
| `clock` / `clockSwitchReason` | 主时钟来源（音频 / 系统）与切换原因 |
| `statistics` | 丢帧、卡顿次数、起播耗时、音频欠载等统计 |

状态轮询间隔默认 0.1 秒（下限 1/60）：`player.stateUpdateInterval = 0.25`。

### UIKit 接入

```swift
final class PlayerViewController: UIViewController, SYPlayerDelegate {
    private let player = SYPlayer()
    private let playerView = SYPlayerView()

    override func viewDidLoad() {
        super.viewDidLoad()
        playerView.frame = view.bounds
        playerView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        playerView.videoGravity = .aspectFit      // .aspectFill / .resize
        view.addSubview(playerView)

        player.delegate = self
        player.attach(to: playerView)
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        player.close()
    }

    func player(_ player: SYPlayer, didFailWith error: SYPlayerError) {
        print("播放失败：\(error)")
    }
}
```

原生 macOS 上 `SYPlayerView` 是 `NSView`，成员与 UIKit 版一致（`playerLayer`、
`videoGravity`），把 `UIViewController` 换成 `NSViewController` 即可。

### 预加载

```swift
let preloader = SYPlayerPreloader()               // 也可传 cache: / maxTotalTasks: / reservedForPlaying:

preloader.add(.url(nextVideoURL), priority: .next, seconds: 3)   // 预热前 3 秒
preloader.add(.url(laterURL), priority: .background)

preloader.setPriority(.playing, for: .url(nextVideoURL))          // 用户点开了，提优先级
preloader.remove(.url(laterURL))
preloader.removeAll()

let s = preloader.statistics      // entries / activeTasks / downloadedBytes / completed / failed …
```

- 只接受 `.url(...)`，本地文件不需要预加载（`add` 返回 `false`）。
- 预加载与播放**共用同一份缓存**：预热过的字节，播放时直接命中，不会重下。
- 三级优先级只影响并发额度：`.playing` 优先，`.next` 次之，`.background` 只用余量。
- `seconds` 传 `nil` 时按默认字节数预热；能从媒体信息估出 time→byte 时按时长，估不出时退化为按字节。

### 缓存配置

```swift
let cache = SYPlayerCacheConfiguration(
    directory: FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("my-video-cache"),
    maxBytes: 512 * 1024 * 1024,        // 磁盘上限
    minFreeSpaceBytes: 256 * 1024 * 1024,
    timeToLive: 7 * 24 * 60 * 60        // nil = 不按时间淘汰
)

let player = SYPlayer(cache: cache)
let preloader = SYPlayerPreloader(cache: cache)   // 同一目录 ⇒ 同一份缓存
```

默认目录是 `Caches/syplayer-http-cache`。配置是**每个实例一份**，但**目录相同即共享
同一份缓存索引**——播放器与预加载器想互相命中，就传同一个目录。

### 进程级网络设置

```swift
// 全进程下行限速（字节/秒），nil = 不限。播放优先：预加载只用余量。
SYPlayerNetwork.maximumDownloadRate = 2 * 1024 * 1024
SYPlayerNetwork.maximumDownloadRate = nil

// 预连接：提前把 DNS/TCP/TLS 预热进连接池，不下载内容、不占缓存、不受限速。
// 适合"用户很可能马上点开、但还不值得预加载"的条目。
SYPlayerNetwork.preconnect(URL(string: "https://cdn.example.com/movie.mp4")!)
```

同一服务器 30 秒内重复预连接只生效一次；同时在途上限 4 条，超出静默丢弃。

### 错误处理

`open()` 的失败有三条路径，按需要挑一条处理，**不要当成三次失败**：`try/catch`、
`delegate` 的 `didFailWith`、以及 `state.playback == .failed(错误)`。

`SYPlayerError` 的取值：`.invalidArgument`、`.canceled`、`.timeout`、`.outOfMemory`、
`.io`、`.noSpace`、`.cacheCorrupt`、`.network`、`.httpStatus(Int)`、`.tooManyRedirects`、
`.rangeUnsupported`、`.contentChanged`、`.notImplemented`、`.unknown(Int)`。
它实现了 `LocalizedError`，`errorDescription` 是中文描述。

## 技术栈

| 层 | 技术 |
|---|---|
| 下载缓存层（核心资产） | C++23，**零平台依赖**（由构建目标强制），只认一张 HTTP 函数表 |
| 播放内核 | C++23 + FFmpeg 8.1.2（精简构建：mov / hls / mpegts 解封装，H.264 / HEVC / AAC 解码） |
| 对外接口 | C11 ABI（`include/syplayer/*.h`）：基本类型 / 不透明指针 / POD / 函数指针 |
| 平台实现 | Objective-C++：NSURLSession（HTTP）、VideoToolbox（硬解）、Metal（上屏）、AudioUnit（输出） |
| 业务接口 | Swift（`SYPlayerKit`）：`SYPlayer` / `SYPlayerView` / SwiftUI 包装 / 预加载 / 网络设置 |
| 构建与分发 | CMake（桌面与测试）、Xcode 工程由 Ruby 生成器产出、SwiftPM 二进制分发（xcframework） |
| 测试 | 自写 `tiny_test` + ctest（内存桩、注入时钟、零依赖回环 HTTP 服务器）、XCTest（Swift 层） |

架构分三层，只在三处留缝：

```
┌──────────────────────────────────────────┐
│  Swift 门面 SYPlayerKit / 业务 UI          │
└──────────────────┬───────────────────────┘
                   │  C ABI                     ← 缝 ①
┌──────────────────────────────────────────┐
│  播放内核（C++）                            │
│    解封装 / 解码 / 音画同步 / 缓冲水位        │
│    IVideoRenderer / IVideoDecoder /        │  ← 缝 ②（纯虚，按平台挂实现）
│    IAudioSink                              │
└──────────────────┬───────────────────────┘
                   │  C ABI                     ← 缝 ③
┌──────────────────────────────────────────┐
│  下载缓存层（完全平台无关）                   │
│    HoleSet / CacheIndex / CacheStore /     │
│    DLTask / Scheduler / Preloader          │
│    syp_http_backend（函数表）                │  ← 平台接入点：NSURLSession ✅
└──────────────────────────────────────────┘
```

留缝的取舍，以及"为什么不抽 UI / 线程 / 时间 / 文件 IO"，见 `docs/architecture.md`。

## 性能与工程取舍

下面是**设计上的性能特性与本仓测试装置实测到的数字**。⚠️ 没有与 AVPlayer、
ijkplayer 等的对比基准，也没有真机 / 真实 CDN 的现场数据——每条都注明来源，
请按这个尺度看。

- **seek 不重下已有字节。** "还缺哪些字节"表达成区间集合（`HoleSet`），seek 后只对
  缺口发请求。端到端矩阵逐 packet 比对过：经缓存层读到的字节与 FFmpeg 直接走
  `file:` 读到的完全一致（含 payload 哈希），覆盖顺读、seek、Range 降级、断连续传、
  重启续下等八条场景。
- **进程重启后续下。** 索引是自定义二进制格式（magic + 版本 + CRC32 + 长度前缀），
  原子落盘、带 etag 校验；重启后按索引续下而不是重来。
- **服务端不支持 Range 时不再翻倍下载。** 这条曾被自己的端到端矩阵**证伪**：实测
  流量约 4.04× 文件大小、并发峰值 3。修复后流量降到 ~1.1×，Range 支持改成三态、
  探明之前并发锁 1（并发峰值 4 → 1，请求条数 5 → 3），支持 Range 的正常源零回归。
- **多连接并发 + 自适应分片。** 分片大小随剩余量与并发数变化；空槽出现时先判断
  "等某个快下完的任务"是否比新建连接更划算，而不是无脑新建。
- **坏任务自动替换。** 挂死（超过"后端超时 + 2 秒宽限"无进展）直接掐掉、从断点重发；
  明显比基线慢的连接（慢 4 倍且连续两次检查）换一条。端到端实测：一条细水长流
  （slow-loris）的连接在约 3.2 秒时被替换，整段读完约 4.8 秒。
- **硬解零拷贝上屏。** `VideoToolbox → CVPixelBuffer → CVMetalTextureCache → MTLTexture`，
  中间不落 CPU 内存；YUV→RGB 的转换矩阵按每帧真实的 colorspace / color_range 选取。
- **精简 FFmpeg。** 只编需要的解封装与解码组件，未 strip 时每个架构约 4.5–4.8 MB，
  且不含 TLS（HTTPS 由 NSURLSession 负责，有门禁盯着产物里没有 TLS 后端符号）。
- **限速是进程级的**，播放优先于预加载，只控平均速率；实现上不在任何回调线程阻塞。

## 平台支持

| 平台 | 下限 | 架构 |
|---|---|---|
| iOS（真机 / 模拟器） | 13.0 | 真机 arm64；模拟器 arm64 + x86_64 |
| Mac Catalyst | 14.0 | **仅 Apple Silicon**（arm64） |
| 原生 macOS（AppKit） | 11.0 | **仅 Apple Silicon**（arm64） |

**不支持**：Intel Mac（原生 macOS 与 Catalyst 都没有 x86_64 切片）、tvOS、visionOS、watchOS。

原生 macOS 的接入方工程要把 `ARCHS` 设成 `arm64`；勾了 Mac Catalyst 的 iOS 工程加一条
`ARCHS[sdk=macosx*] = arm64`（只作用于 Catalyst，不影响 iOS 真机与模拟器；
`examples/iOSExample` 就是这么设的）。否则 Release 默认连 x86_64 一起编、链接失败。

**工具链**：`0.1.0` 的两个 xcframework 用 **Xcode 26.6（Apple Swift 6.3.3）** 构建。
库演进模式保证**更新**的编译器能读它的 `.swiftinterface`；更老的 Xcode 没有保证、
也没有验证过，请用同版本或更新的 Xcode 接入。

## 与自带 FFmpeg 的项目共存

**接入方不需要任何配置。** 不用改链接顺序、不用加 `-force_load` / `-all_load` 的例外、
不用改自己的 FFmpeg 构建。

**原理**：我们随包分发的 FFmpeg 叫 `SYFFmpeg`，它对外**只导出 `syp_` 前缀的符号**——
构建时用 `ld -alias_list` 给每个全局符号起一个 `syp_` 别名，再用
`-exported_symbols_list` 只导出别名，原名降为库内本地符号；SYPlayerKit 编译时强制
包含同源生成的映射头，源码照写 `av_read_frame`，链接的却是 `syp_av_read_frame`。
于是接入方自己带的 FFmpeg（任意版本、静态或动态、任意链接顺序、开不开 `-all_load`）
与我们这份在符号层面没有交集：各用各的，不报 duplicate symbol，也不会串用。
两份的全局状态也各自独立（`av_log` 级别与回调、网络初始化、分配器互不影响），
FFmpeg 的对象不会跨两份传递——公开 API 不暴露任何 FFmpeg 类型。

**代价**：App 里会有两份 FFmpeg。我们这份是精简构建，未 strip 时每个架构约
4.5–4.8 MB。跨大版本 ABI 不兼容，二进制 SDK 做不到与接入方共用一份。

**回归**：`ffmpeg_coexist_*` 用一个导出真实 FFmpeg 函数名、返回假值并记账的
"接入方 FFmpeg"，按五种链接方式与我们链在一起，断言接入方直接调
`avformat_version()` 拿到假值、我们打开样片仍得到 640×360、且假库一次都没被我们调到。
`tools/check-ffmpeg-symbols.sh` 另外盯着导出表与我们各静态库的未定义引用。

维护者视角的升级与打补丁流程见[维护者：升级 FFmpeg 与打补丁](#维护者升级-ffmpeg-与打补丁)。

## 许可证与合规

- **FFmpeg 是动态 framework，按 LGPL v2.1 or later 分发**（未改源码；构建时
  `--disable-gpl`、没有 `--enable-version3`、不带 nonfree 组件）。对本仓库随 Release
  发出的这份二进制，分发者一侧的义务由本仓库履行：`SYFFmpeg.xcframework.zip` 的根目录
  带 `COPYING.LGPLv2.1` 与 `NOTICE`（FFmpeg 版本、源码地址、全部 configure 开关、
  由 `tools/build-ffmpeg.sh` 构建）。**接入方把它随 App 再分发时仍要自己做两件事**：
  在 App 里附上 FFmpeg 的 LGPL 许可声明；保持 `SYFFmpeg.framework` 动态链接、可被替换
  （SwiftPM 默认就是这样，不要改成静态链接或合进别的二进制）。替换用的 `SYFFmpeg`
  必须导出同样一批 `syp_` 前缀符号，要求见 `NOTICE`。
- **隐私清单已内置**：`SYPlayerKit.framework` 与 `SYFFmpeg.framework` 各带一份
  `PrivacyInfo.xcprivacy`——不跟踪、不收集数据，声明的 required-reason API 只有文件
  时间戳（`C617.1`，两者都有）与磁盘空间（`E174.1`，仅 SYPlayerKit，缓存写入前查剩余
  空间）。Xcode 归档时会汇总进 App 的隐私报告。
- **Audio session 由接入方管理**：框架不设置 `AVAudioSession` 的 category / mode，
  也不激活它（只读取输出延迟、监听路由变化）。iOS 上要在静音开关打开时出声、或与别的
  App 混音，请在播放前自己配置。

## 从源码构建

需要 CMake ≥ 3.20、支持 C++23 的 clang，以及 Xcode（Apple 平台目标）。

```bash
bash tools/build-ffmpeg.sh                     # 产出 build-ffmpeg/out/SYFFmpeg.xcframework（四个 slice）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure --no-tests=error
```

FFmpeg 产物不进仓库。CI 建议加 `-DSYP_REQUIRE_FULL_MATRIX=ON`——这样"CI 机器缺
FFmpeg 依赖"会判红，而不是矩阵悄悄跳过、`ctest` 仍退出 0。

**两个 demo 工程（`demo/ios`、`demo/mac`）在新克隆里打不开是预期行为**：它们的
`project.pbxproj` 内嵌了生成时那台机器上的绝对路径。先跑 `tools/build-ffmpeg.sh`，
再 `ruby demo/generate_xcodeprojects.rb` 重新生成。`examples/` 下的两个示例工程不受
此限，只含相对路径，clone 即可打开。

分发产物与示例：

```bash
tools/build-xcframework.sh --version 0.1.0   # 两个 xcframework + zip + checksum，并回填 Package.swift
tools/check-spm.sh                            # 三平台构建并跑示例 App 的测试 + 发版 zip 形态验证
SYPLAYER_LOCAL_BINARIES=1 xed examples/iOSExample     # 用本地产物打开示例（从终端启动 Xcode）
```

发版顺序（**不能反**）：构建 → 跑门禁 → 提交回填后的 `Package.swift` → 在该提交上打
tag → 推送提交与 tag → 在**已存在的 tag** 上建 GitHub Release，上传**第一步算出
checksum 的那两个 zip 本身**。先建 Release 会让 GitHub 在回填之前的提交上创建 tag，
接入方按 tag 拿到的 checksum 与 zip 对不上。

### 门禁

改完都要跑（CI 与本地一致）：

```bash
bash tools/check-abi.sh              # 公开头守住 C ABI 纪律 + Swift 绑定验证
bash tools/check-deploy-target.sh    # 下载缓存层在 iOS 13 部署目标下的严格 availability 检查
bash tools/check-demo-no-c-types.sh  # 业务侧源码里不出现任何 C / 桥类型
bash tools/check-ffmpeg-no-tls.sh    # FFmpeg 切片不含 TLS
bash tools/check-ffmpeg-symbols.sh --expect-slices 4 build-ffmpeg/out/SYFFmpeg.xcframework
cmake --build build --target syp_dl_purity_check      # 下载缓存层零 FFmpeg、零平台符号
ruby demo/generate_xcodeprojects.rb --check           # 两个 demo pbxproj 与生成器一致
bash tools/check-spm.sh                                # SwiftPM 接入（先跑 build-xcframework.sh）
```

⚠️ `xcodebuild` 的 Release 必须显式加 `ARCHS=arm64`，否则会连 x86_64 一起编、链接失败。

### 测试

测试代码量多于生产代码。下载缓存层全部用内存桩 + 同步泵 + 注入时钟，
**确定性、不 sleep、不碰网络**；平台后端与端到端矩阵走零依赖的回环 HTTP 服务器 +
真实素材。当前规模：**43 个 ctest 目标、829 个用例**（`build` 与 `build-tsan` 各全量
通过），外加 **Swift 侧 109 条 XCTest**（`SYPlayerKitTests`，在 Mac Catalyst 上跑）。

验证装置本身也当真：`syp_probe` 以"FFmpeg 直接走 `file:` 读"为参照，对经由缓存层的
读取逐 packet 差分比对（含 payload 哈希）；`--decode` 把参照换成"FFmpeg 直接解码"，
逐帧比对像素与采样的 SHA-256。新增用例必须做变异验证（改坏实现确认用例会红），
这是本仓纪律而不是建议——原因与踩过的坑见 `CLAUDE.md`。

### 维护者：升级 FFmpeg 与打补丁

- **升级**：改 `tools/build-ffmpeg.sh` 顶部的 `FFMPEG_VERSION`，重跑脚本。改名清单每次
  构建用 `nm` 从静态库现生成，映射头、导出清单与 `NOTICE` 同源生成，新版本增删符号
  自动跟上，不维护任何手写清单。之后重打 xcframework、回填 checksum。
- **改 FFmpeg 源码**：补丁放进 `tools/ffmpeg-patches/`（`*.patch`，`patch -p1` 格式），
  脚本解压官方源码后按文件名顺序打，任何一个失败即退出；`NOTICE` 会自动列出补丁名与
  改动文件（LGPL 要求的"显著标明修改"）。目录为空时 `NOTICE` 写"未修改"。
- **不要**直接改 `build-ffmpeg/src`（不进仓库，改了会丢、别人复现不了）。

## 现状与已知限制

功能与结构正确性有过硬证据，但有几条边界必须说清楚，不要只看"全绿"：

- **只在 127.0.0.1 的回环服务器上验证过**，没有真机、没有真实 CDN 的现场数据。
- **没有在物理 iOS 设备上安装运行过**（无设备与签名条件）；iOS 相关结论来自模拟器、
  Mac Catalyst 与代码级验证。
- **HLS 目前只在测试套件里跑**：`Pipeline::create_hls()`（点播 + 直播、多码率选轨、
  分轨音视频）已交付并全绿，但仓库内的 demo 壳走的是文件 / AVIO 路径。
- **Mac Catalyst 没有硬解**（该 slice 的 FFmpeg 禁用了 VideoToolbox），只能软解。
- **限速的速率准确度、缓冲水位等阈值没有真实网络实测**，默认值是经验起点。
- 各里程碑的逐条判定（含"有条件通过"的条件）在 `docs/roadmap.md`；编号的已知缺口在
  `docs/known-gaps.md`；登记在案的技术债在 `docs/tech-debt.md`。

## 目录

```
include/syplayer/   公开 C ABI（基本类型 / 不透明指针 / POD / 函数指针）
src/dl/             下载缓存层，完全平台无关
src/media/          解封装 / 解码 / 同步核心 / HLS
src/platform/apple/ NSURLSession、AudioUnit、Metal、VideoToolbox（Objective-C++）
swift/SYPlayerKit/  Swift 公开 API 与 ObjC++ 桥
demo/               iOS + Mac Catalyst 两个开发用 demo 壳（源码直编，非公开 ABI）
examples/           iOS 与原生 macOS 两个示例 App：只经 SwiftPM 接入，clone 即可打开
tests/              单元测试 + 内存桩后端 + 零依赖回环 HTTP 服务器
tools/              构建脚本（FFmpeg / xcframework）、门禁脚本、syp_probe 验证工具
docs/               架构、路线图、已知缺口、技术债、工具链
```

## 文档

- `docs/architecture.md` —— 分层、三处缝、HLS 接管方式
- `docs/roadmap.md` —— 里程碑、交付物与判定（含条件）
- `docs/known-gaps.md` —— 编号的已知缺口，每条带证据
- `docs/tech-debt.md` —— 登记在案的技术债与本仓踩过的"假绿"陷阱
- `CLAUDE.md` —— 贡献者须知：门禁、测试纪律、坑
