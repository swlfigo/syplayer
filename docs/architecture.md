# 架构与跨平台边界

**当前范围：iOS / macOS 优先落地，预留跨平台接入点。**
**语言：核心 C++23，上层 Swift，渲染 Metal。**

## 一、原则：少数几条缝 + 纪律

"留口子"最常见的失败方式是提前抽象一堆用不上的东西——现在付设计成本，
将来发现抽象的位置还不对。本项目只在**三个位置**放缝，其余先写死平台实现。

判断某个东西该不该现在抽：**后补的代价是不是远大于现在抽的代价。**

| 现在就必须抽 | 理由 |
|---|---|
| HTTP 客户端 | dl 层若直接调平台网络 API，整层就废了。这是核心资产，必须平台无关 |
| 视频渲染器 | Metal / GLES / Vulkan 的 API 形态差异巨大，事后插抽象要动所有调用点 |
| 硬解码器 | VideoToolbox 与 MediaCodec 模型完全不同（前者同步 pull，后者 Surface 异步） |
| 音频输出 | AudioUnit / AudioTrack / WASAPI 的缓冲模型不同 |
| 存储路径 | 沙盒规则各不相同 |

| 现在不要抽（YAGNI） | 理由 |
|---|---|
| UI / 播控层 | 本来就该各平台原生各写各的 |
| 线程与同步 | `std::thread` / `std::mutex` 本身跨平台 |
| 时间 | `std::chrono` 够用 |
| 日志 / 统计后端 | 一个函数指针的事，随时能加 |
| 文件 IO | POSIX 够用 |

## 二、分层

```
┌────────────────────────┐   ┌────────────────────────┐
│  Swift UI / 播控        │   │  (未来) Kotlin UI       │  平台原生，各写各的
└───────────┬────────────┘   └───────────┬────────────┘
            └────────── C ABI ───────────┘                ← 缝 ①
┌─────────────────────────────────────────────────────┐
│  播放内核（C++）                                      │
│    demux/decode → FFmpeg                            │
│    AV sync / 时钟 / 缓冲水位                          │
│    ┌─────────────────────────────────────────┐      │
│    │ IVideoRenderer / IVideoDecoder /         │      │  ← 缝 ②
│    │ IAudioSink（纯虚，实现按平台挂）           │      │
│    └─────────────────────────────────────────┘      │
└───────────────────────┬─────────────────────────────┘
                        │  C ABI                         ← 缝 ③
┌─────────────────────────────────────────────────────┐
│  dl 层（核心资产，完全平台无关）                        │
│    HoleSet / CacheIndex / CacheFile /                │
│    DLTask / Scheduler / Preloader                   │
│    ┌──────────────────────────────┐                 │
│    │ syp_http_backend（函数表）     │                 │  ← 平台接入点
│    │   └── NSURLSession 后端 ✅     │                 │
│    │   └── libcurl / OkHttp（未来） │                 │
│    └──────────────────────────────┘                 │
└─────────────────────────────────────────────────────┘
```

## 三、缝上的纪律（最重要的一条）

**跨越 C ABI 的公开头文件里，只允许出现：**

- C 基本类型（`int32_t` / `int64_t` / `const char*` / `uint8_t*`）
- 不透明指针（`typedef struct syp_source syp_source;`）
- POD struct（只含上述类型）
- 函数指针 / 函数表

**绝对不允许出现：** 任何 Objective-C 类型、任何 C++ 类型、任何 Metal / CoreVideo 类型。

理由：一旦 `std::shared_ptr<DLTask>` 出现在公开头里，实现语言就永远锁死了，
Swift 也没法直接绑定，Android 更接不上。

由 `tools/check-abi.sh` 强制：C11 / C17 / C++23 / ObjC / ObjC++ / iOS13-arm64
六套编译矩阵 + 禁止类型扫描 + `extern "C"` 覆盖 + Swift 原生绑定验证。

**缝 ② 的实际形状（M2a 落地后）**：`IVideoDecoder`（`src/media/video_decoder.h`）
是纯虚接口，方法集是 `open(par, time_base)` / `send(pkt)` /
`receive(Frame*)` / `flush()` / `skipped_packets()`；M2a 只有一个实现
（`FFmpegVideoDecoder`，软解），M3 在这条缝上加 `VTDecoder`
（VideoToolbox 硬解）时不需要改这个接口一个字——新增实现类，不改签名。
`send`/`receive` 是一对状态机（跟 FFmpeg 自己的
`avcodec_send_packet`/`avcodec_receive_frame` 对应），`EAGAIN`/`EOF`
不是错误，是正常的流控信号；错误分三级——单包可跳过（计入
`skipped_packets()`）、该轨终止（`open()` 失败或内部不可恢复错误）、
整体终止（demux/IO 层错误），分级判定与处理在 `Pipeline`，不在
`IVideoDecoder` 里。

**音频解码没有走缝 ②**：`FFmpegAudioDecoder`（`src/media/ffmpeg_audio_decoder.h`）
不继承任何纯虚接口，是有意的 YAGNI——缝 ② 只要求抽象*音频输出*（各
平台的音频渲染/输出路径不同：AudioUnit / AAudio / WASAPI），不要求抽象
*音频解码*。音频解码在可见未来只有 FFmpeg 软解这一种实现，没有第二条
平台特定路径需要切换；现在抽一层纯虚接口不会被第二个实现消费，纯粹是
空中楼阁，跟本文件开头「现在不要抽（YAGNI）」的判断标准一致。方法集
与 `IVideoDecoder` 完全一致，因此直接复用它的 `Receive` 枚举
（`IVideoDecoder::Receive`），不再定义一份语义相同的
`AudioReceive`——两边定义两份枚举只会让调用方多写一份几乎一样的
`switch`，维护时容易漏改一份。等哪天真的出现第二种音频解码实现（比如
某平台的硬件音频解码器），再按需抽接口，届时视频/音频两边的
`Receive` 枚举需要一起挪到公共位置，不能留一个抽了一个没抽。

**缝 ② 的另外两个方法集（M2b 落地后）**：

- **`IAudioSink`**（`src/media/audio_sink.h`）：`open(sample_rate,
  channels, sample_fmt)` / `write(const Frame&)`（返回 `bool`，`false`
  = 环形缓冲没有空位，帧未被消费、所有权留在调用方——**整帧成败，没有
  部分写入**这个概念）/ `played_us()`（真正播出去的位置，媒体时间轴，
  微秒，已扣除设备输出延迟；`failed()` 时必须是 `AV_NOPTS_VALUE`，尚未
  `open()` 时是上一次 `flush()` 的基准，两种状态不能混用——见该头文件
  "单一契约"一节）/ `pause()`/`resume()`/`flush(base_us)`/
  `set_speed(speed)`/`failed()`。唯一实现是 `AudioUnitSink`
  （`src/platform/apple/audio_unit_sink.h/.mm`，macOS 走 HALOutput、iOS
  走 RemoteIO）。`write()` 内部要把 `AudioRing::write()`"按字节、允许
  部分写入"的语义折算成接口要求的"整帧成败"语义（环里空间不够整帧就
  整帧不写），`played_us()` 要用"**当前 `AudioRing::consumed_bytes()` −
  `flush()` 时快照的那个值**"算净消费量（`consumed_bytes()` 单调递增、
  `reset()` 不清它，不能直接当"自上次 flush 以来"的消费量用；实现见
  `audio_unit_sink.mm` 的 `compute_played_us()`：`net_consumed_bytes =
  consumed_bytes - base_consumed_bytes`）——**减法方向不能写反**：这里
  曾经写成"快照 − 当前值"，那是 `known-gaps.md` #21 全部论证围绕的那个
  公式的符号取反，等于把结论倒过来（净消费量恒为负 → 时钟永不推进），
  M2b 终审 I6 订正——这两处折算是 `known-gaps.md` #21 点名的、测试替身
  `FakeAudioSink` 天然测不出的真实架构落差，`AudioUnitSink` 拆出
  `commit_frame()`/`compute_played_us()` 两个不碰
  `AudioComponentInstance` 的 `static` 纯函数专门覆盖。
- **`IVideoRenderer`**（`src/media/video_renderer.h`）：只有一个方法
  `present(const Frame&)`，返回非 `SYP_OK` 表示该帧未被呈现（计入统计，
  不终止播放，是错误分级里最轻的一级）。平台实现"只做上传纹理 + 一个
  着色器，薄到没地方藏逻辑"（头文件顶部原话）——唯一实现是
  `MetalRenderer`（`src/platform/apple/metal_renderer.h/.mm`），内部把
  YUV 三个 plane 上传成纹理、按帧的 `colorspace()`/`color_range()`
  选正确的 YUV→RGB 转换矩阵（M2b Task 9.5 落地，`known-gaps.md` #26），
  一个 `.metal` 着色器完成转换与合成。

**实时回调只碰环形缓冲，这是结构事实不是纪律要求**：`AudioUnitSink` 的
`render_cb`（`audio_unit_sink.mm`）跑在 CoreAudio 的实时线程上，
`inRefCon` 只指向 `RenderCtx`——一个 `AudioRing*`、一个
`std::atomic<bool>*`（欠载标志）、一个 `bytes_per_frame`，**三个字段，
没有第四个字段能塞下更多东西**。回调拿不到 `AudioUnitSink` 本身，更拿
不到 `TrackPlayer`/`Pipeline`——不是"写代码的人记得别在回调里加东西"
这种靠纪律维持的约定，是这个结构本身做不到：`RenderCtx` 的类型定义里就
没有第二个指针可以让你多塞一个对象进去。这样保证的是"实时线程里不会
出现阻塞、分配、锁、ObjC 消息发送、FFmpeg 调用、日志"这条 CoreAudio
对渲染回调的硬要求。Task 8 审查用**反汇编**核实过这条结构事实真的兑现
到了机器码层面：Debug（`-O0`）下 `render_cb` 函数体只有 4 条 `bl`
（`AudioRing::read` / `_bzero` / `atomic<bool>::store` /
`std::min`——`-O0` 下 `std::min` 不内联，才会出现在调用列表里)；Release
下只有 2 条 `bl`（`AudioRing::read` / `_bzero`——`std::min` 内联成
`csel`，`atomic<bool>::store(relaxed)` 内联成一条普通 `strb`，ARM64
内存模型下 relaxed store 本就不需要屏障指令)。两种构建下都是零分配、
零锁、零 ObjC 消息、零 FFmpeg 调用、零日志。

**帧数据怎么过缝？** 用不透明句柄 + 访问函数，不要把平台类型写进接口：

```c
typedef struct syp_frame syp_frame;
int64_t syp_frame_pts_us(const syp_frame*);
void*   syp_frame_native_handle(const syp_frame*);  // 平台后端内部 cast，接口不表态
```

## 四、dl 层的平台无关性是构建强制的

`syp_dl` 只含 `src/dl/` 下的 `.cpp`，不含 `.mm`、不链任何 framework；
Apple 后端是独立的 `syp_platform_apple` 目标，`PUBLIC` 依赖 `syp_dl`。

这条纪律有两层强制：

1. **configure 期**：断言 `syp_dl` 的 `SOURCES` 里没有 ObjC 源、
   `INTERFACE_LINK_LIBRARIES` 里没有 framework；
2. **link 期**：`syp_dl_purity_check` 目标用 `-Wl,-force_load` 把整个归档拉进来实链一遍，
   **绕开** CMake 的链接接口——任何平台符号泄漏都会在这里变成 undefined symbol。

## 五、渲染层：现在只写 Metal

iOS 上 Metal 是唯一有前途的选择（OpenGL ES 自 iOS 12 起已废弃），零拷贝链路也更干净：

```
VideoToolbox → CVPixelBuffer → CVMetalTextureCache → MTLTexture
```

`IVideoRenderer` 抽象类定义在 C++ 核心里，**不要定义在 Swift 里**——
定在 Swift 里等于没留口子，其他平台一行都用不上。

未来接 Android 时新增 `GLESRenderer`，走
`MediaCodec → SurfaceTexture → GL_TEXTURE_EXTERNAL_OES` 这条成熟的零拷贝路径。
**不要一上来上 Vulkan**：Android 上 `AHardwareBuffer` 导入碎片化严重。

## 六、HLS 支持（M-HLS）

新增 `src/media/hls/`：`HlsSession`（装配 `AVFormatContext` 的 `io_open`/
`io_close2`、scheme 改写与还原、URL 通道分流、选轨）+ `PlaylistFetcher`
（把异步的 `syp_http_backend` 包成一次阻塞取回）+ `url_rewrite`（纯函数：
scheme 改写、通道判定、播放列表文本扫描）。`SegmentSource` 不是新类——
就是 `syp_source` + 既有的 `AvioBridge`，由 `HlsSession` 持有和管理。下面只写
**按实测钉死、不按设计时的设想写**的几条。

### 两条 IO 通道，缓存语义故意相反

播放列表与分片走两条完全独立的通道，不是为了省事而是因为**语义相反**：

- **播放列表**（`.m3u8`/`.m3u`）走 `PlaylistFetcher`——直接用
  `syp_http_backend`、全量读进内存、**零缓存**。直播下同一个 URL 的内容
  每隔几秒就变，缓存它会把直播冻在第一次拉到的那份上。
- **分片**（其余一切）走 `syp_source` + `AvioBridge`——内容不可变、URL
  唯一，缓存它**永远正确**，直播也一样。

之所以不能塞进一条通道，是因为 dl 层在区间已缓存时**一个请求都不发**
（`src/dl/source_bridge.cpp` 的 `SourceBridge::read()`：命中缓存的分支
直接 `file_->read_at()` 读盘，不经过 `apply_window()`/调度器，见该函数
`do_io` 分支）——这对分片是对的，对播放列表是错的。给 `syp_source` 加一
个"这次别缓存"的模式要改 `src/dl/` 和公开 C ABI，而 `syp_source` 的整套
机器（`HoleSet`、分段并发、磁盘索引）对一个几 KB、必须永不缓存的文本文件
毫无用处。两条通道是语义的诚实反映。

### 协议名门与 scheme 改写

`hls.c` 的 `open_url()`（`libavformat/hls.c:656`）在调 `io_open`
**之前**先查 `avio_find_protocol_name()`，命中的是**编译进去的协议表**，
且要求名字是 `file`/`http*`/`data` 之一（:673-693）。本项目只编了
`--enable-protocol=http`（拉 `tcp_protocol`，不拉 TLS，`configure:4023`；
`--enable-protocol=https` 会连带拉 `tls_protocol`，`configure:4027`），
为了不引入一整套 TLS 栈（Apple 上会落到已被 Apple 废弃的 Secure
Transport），HLS 会话把递给 FFmpeg 的 URL 里 `https` 一律改写成 `http`
（`url_rewrite.h::to_ffmpeg_url`），在 `io_open` 收到之后再用会话记下的
真实 scheme 换回来（`to_real_url`）。scheme 对 FFmpeg 只是个标签，真正
的连接由 `HlsSession::io_open` 发起，所以降级是安全的。

**这道门播放列表不用过**：`parse_playlist()`（`hls.c:790`）直接调
`c->ctx->io_open(...)`（`hls.c:834`），不经过 `open_url()` 的协议名检查；
只有分片（`hls.c:1430`/`1436`）与密钥（`hls.c:1364`）经 `open_url()`。

**局限（有意接受、方向安全）**：`HlsSession` 只记一份"原始 master URL 的
scheme"，还原时一律用它，所以混合 scheme 的流（https 播放列表 + http
分片）会被整体当成 https 处理。这是降级失败而非降级成功——把 http 资源
当 https 请求会失败并报错，不会静默明文传输。

### `protocol_whitelist` 护栏：它守的是哪一种失效

`AVFormatContext::protocol_whitelist` 只留 `"file"`，让 FFmpeg 内建的
`http`/`tcp` 协议**编进去了但用不了**——任何一条子资源打开路径一旦没经过
我们装的 `io_open`，会被这道白名单当场拒绝，变成一次可见的错误，而不是
一次静默绕过（`AVFormatContext::io_open` 没装上时才会退到
`io_open_default`，它是这道白名单唯一的消费者）。

这道护栏的真实性质**不是设计时能推出来的，是两轮实测收窄出来的结论**：
`io_open` 一旦装上，`io_open_default` 就再也到不了，这道白名单在**正常
路径上不生效、也不该生效**；它只在"`io_open` 根本没装上"这一种失效模式
下才真正起作用，对"我们的通道装上了但坏掉了"（比如内部返回错误）它是
**冗余的**——那条路径上 FFmpeg 不会退回自己的 IO。完整的 2×2 实测矩阵
（`io_open` 装上但坏掉 / 根本不装 × whitelist 保留 `"file"` / 放开成
`"file,http,tcp"`，含每一格具体红几条、日志里出现了什么）钉在
`src/media/hls/hls_session.h` 顶部注释里——**这里不重复一份，原文引用
它，避免两份表述漂移**。

### `HlsSession` 的生命周期：必须活得比 `Demuxer` 久

`HlsSession::release_fmt()` 把装配好的 `AVFormatContext` 交给
`Demuxer`，但 `io_open`/`io_close2` 回调在播放期间还会持续被调用（下一
个分片、直播刷新的下一次播放列表拉取），回调里用的正是 `HlsSession`
自己——它必须比 `AVFormatContext` 活得久。`avformat_close_input()` 析构
时也会经 `ff_format_io_close()` 回调一次 `io_close2` 来关顶层播放列表。

这条保证落在 `src/media/pipeline.h` 的**成员声明顺序**上：`hls_`
（`std::unique_ptr<hls::HlsSession>`）声明在 `demuxer_` **之前**。C++
按声明的逆序析构，所以 `demuxer_` 先毁（`avformat_close_input()` 触发
`on_io_close` 把每条还开着的分片/播放列表关掉），`hls_` 后毁——反过来
的顺序是确定的 UAF，不是"以后可能出问题"。为了让 `pipeline.h` 不必
`#include "media/hls/hls_session.h"`（那会把 FFmpeg + dl 层的一整串头
拖进解码路径上几乎每个 TU），`HlsSession` 只做前置声明，`HlsOptions`
单独抽进零依赖的 `media/hls/hls_options.h`；前置声明 + `unique_ptr`
成员要求析构函数看到完整类型，`Pipeline` 因此新增了一个**out-of-line**
的 `~Pipeline()`（头里声明、`.cpp` 里 `= default`）——这是本设计给
`Pipeline` 带来的唯一一处签名变化。

### 缝的形状：`create_hls()` 与既有工厂函数并列

`Pipeline::create_hls(url, cfg, dl_cfg, hls_opts, &err)` 与
`create_file()`/`create_avio()` 三足并立，`TrackPlayer` 及以下（音视频
解码、同步、渲染）完全不知道 HLS 这个概念存在，一行签名都没改。

唯一的例外是选轨判据：FFmpeg 的 `AVDISCARD_ALL` 不会把流从
`AVFormatContext::streams`/`nb_streams` 里移除，被 discard 的 variant
流仍然可见。新增的排除条件**只有一处**：`TrackPlayer::create()`
（`track_player.cpp:65/71`）挑选主视频/主音频轨时跳过 `t.discard` 为真
的流——不改任何已有签名，纯追加，理由与 `AV_DISPOSITION_ATTACHED_PIC`
那条排除同一形状。

`Pipeline`（`pipeline.cpp`）**没有也不该有**这条排除：`create_common()`
（`pipeline.cpp:155-171`）按 `codec_type` 无条件托管全部音视频轨，被
discard 的轨也在内。这是刻意的——托管才有 PacketQueue，有 PacketQueue
才会被排空；不托管的话那条轨的 packet 虽然在 demux 阶段就丢掉了，但
`step()` 的联合背压判据（第 3 步的 all_done、第 4 步的 Blocked）是按
「所有 managed 轨」算的，把一条轨移出托管反而会改变背压的形状。

## 七、M3a：可选硬解

第一节的表格早就把"硬解码器"列进了**现在就必须抽**的三个缝之一——
VideoToolbox 是同步 pull 模型，MediaCodec 是 Surface 异步模型，事后插
抽象要动所有调用点。M3a 落地了 Apple 侧这半（VideoToolbox），下面只写
贯穿几层的接线本身。

```
PipelineConfig{ video_decode, hw_backend }
        │
        ▼
FFmpegVideoDecoder::open()  ── 硬解模式 ──▶  hw_backend->supports()/prepare()
        │                                        （不支持 → SYP_ERR_NOT_IMPLEMENTED，
        │                                         不留半开状态、不静默退软解）
        ▼
get_format 回调：候选里有硬件格式就选它，
否则返回 AV_PIX_FMT_NONE（唯一的"不兜底"闸门）
        │
        ▼
AVFrame（pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX，data[3] = CVPixelBufferRef）
        │  Frame::from_av 原样接管；hw_handle() 平台无关地暴露 data[3]
        ▼
MetalRenderer::present()
  ├─ yuv420p（软解）──▶ replaceRegion CPU 上传三张纹理 ──┐
  └─ VIDEOTOOLBOX（硬解）──▶ CVMetalTextureCache 零拷贝取两张纹理 ─┤
                                                          ▼
                                        nv12_to_rgba / yuv420p_to_rgba kernel
                                        写同一张 out_tex
                                                          │
                                        set_output_layer() 非空？
                                                          │
                                        ┌─────────────────┴─────────────────┐
                                        ▼                                   ▼
                              blit pass 画到 CAMetalLayer            仅离屏（out_tex，
                              的 drawable 并 present                 debug_copy_output_rgba
                                                                      测试/调试回读）
```

**配置 → 注入**：`hw_backend` 是 `syp::media::hw::IHwDecodeBackend*`，由调用方
（demo 壳、未来的 M4 上层）经 `PipelineConfig` 注入，`src/media/` 本身不 `#include`
任何 Apple 头——分层纪律（第四节）在硬解这条路上同样成立，`syp_media` 不知道
VideoToolbox 存在，只知道一个抽象后端接口。

**"不兜底"闸门**：`get_format` 回调在候选像素格式列表里没有硬件格式时返回
`AV_PIX_FMT_NONE`（不是退回列表里的第一个软件格式）——但光这一处**不够**：FFmpeg 在
hwaccel 初始化失败后只让当前 slice 报错，H.264 随后会拿 codecpar 带进来的 yuv420p 绕过
`get_format` 继续软解，HEVC 则把包逐个计成"可跳过"一路到 Eof（M3a 终审 C1 实测）。所以
`FFmpegVideoDecoder` 在硬解模式下另加三件事：open 时强制 `ctx->pix_fmt = AV_PIX_FMT_NONE`；
回调拒绝一次就闩住，此后 `send()`/`receive()` 报错；`receive()` 输出闸丢弃任何非硬件格式的
帧并报错。Pipeline 据此把视频轨标记 `track_failed()`；open 阶段的任何失败则让 `create_*()`
整体失败（spec §3.4/§3.5/§8）。

**Apple 实现**：`src/platform/apple/vt_decode_backend.{h,mm}`，经 FFmpeg hwaccel 调
VideoToolbox（不直调 `VTDecompressionSession`——FFmpeg 已经处理了 AnnexB/AVCC、参数集
变化、B 帧重排序、`kVTInvalidSessionErr` 后重建 session，直调要把这些全部重写）。HEVC
额外加一道 `VTIsHardwareDecodeSupported()` 预检（FFmpeg 对 HEVC 只用
`EnableHardwareAcceleratedVideoDecoder`，这个 flag 允许 VT 内部静默走软件，预检堵不住
per-stream 那一半，见 `docs/known-gaps.md` #50）。Mac Catalyst 恒不支持（#20 的裁定）。

**`AV_PIX_FMT_VIDEOTOOLBOX` 帧不新增 `Backend::Pixel`**：仍是 `Frame(Backend::Av)`，
`hw_handle()` 是唯一新增的平台无关访问器（Apple 上转成 `CVPixelBufferRef`，Android 未来
对应 `AVMediaCodecBuffer*`）——`Frame`/`FrameQueue`/`TrackPlayer` 对硬解帧零改动，硬解只是
"帧内部数据换了个来源"，不是一条平行的新数据通路。

**Android 口子**：`prepare()`（`IHwDecodeBackend`）的签名就是留给 `AVCodec` 替换点——
Android 侧新增一个 `MediaCodecDecodeBackend` 实现同一接口即可，`FFmpegVideoDecoder`/
`Pipeline`/`MetalRenderer` 的接线不需要再改一次；渲染这一侧对应第五节已经写好的
`GLESRenderer` 口子（`MediaCodec → SurfaceTexture → GL_TEXTURE_EXTERNAL_OES`）。
