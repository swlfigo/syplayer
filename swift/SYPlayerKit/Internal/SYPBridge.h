// SYPBridge.h — SYPlayerKit 的**框架内部**实现头，把 TrackPlayer
// （src/media/track_player.h）包成 Swift 可以直接调用的 Objective-C 形态。
//
// **这不是公开 API。** 本文件走 framework 的 PrivateHeaders + 独立的
// `module.private.modulemap`（模块名 SYPlayerKit_Private，见
// swift/SYPlayerKit/SYPlayerKit.private.modulemap），公开伞头
// SYPlayerKit.h 一个字都不提它。业务侧看见的是 SYPlayer / SYPlayerState /
// SYPlayerError 那一层纯 Swift API；这里的 int32_t* 出参、裸 syp_status、
// 微秒整数一律不许穿透上去。
//
// 【把"不可见"和"不可达"分清楚】上面说的是**不可见**：
// `import SYPlayerKit` 不会带出这个模块，业务侧不会不小心用到它。它并不是
// **不可达**——`SYPBridge.h` 实打实拷在 framework 的 PrivateHeaders/ 里，
// 任何人显式写一行 `import SYPlayerKit_Private` 就能拿到这里的全部接口，
// 编译器不会拦。Swift / Clang 没有"私有模块只许同一 framework 内部使用"
// 这种强制机制。
//
// 在本仓库里，拦住这件事的是 `tools/check-demo-no-c-types.sh`：
// 它扫描 demo/ 下所有 Swift 源，`SYPlayerKit_Private` 与 `Syp[A-Z]` 一样
// 属于命中即失败。换句话说这是一条**有闸门守着的纪律**，不是语言级保证；
// 换一个不跑这个脚本的仓库，纪律就只剩纪律。
//
// 为什么不做成公开的 C ABI（缝 ①，include/syplayer/syp_player.h）：
// 验收标准是"业务侧没有 C 类型"，而这层桥目前只有 Swift 一个消费者，中间再加一层
// C 转接是纯成本；将来真要接 Flutter/RN 再补，接口形状那时才知道。
//
// 纯 Objective-C 接口（不出现任何 C++ 类型），可以被 Swift 直接 import。
// 所有 C++/FFmpeg 细节都封在 SYPBridge.mm 里。
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>

NS_ASSUME_NONNULL_BEGIN

// syp_status 取值的镜像。本头刻意不 #include <syplayer/syp_types.h>——那是公开
// 的 C ABI 头，把它牵进这层内部桥等于让它的类型有机会漏进 Swift 公开 API。
// 数值一致由 SYPBridge.mm 里的 static_assert 钉住：改动任一侧都会编译失败，
// 所以 Swift 侧的 SYPlayerError 可以放心按这些常量做双向映射，而不是抄一份
// 会悄悄漂移的魔数（旧版 PlayerViewController.swift:115 那个硬编码 -99 正是
// 这个问题的实例）。
//
// SypStatusCodeEof / SypStatusCodeBusy 列在这里只为静态断言完整，Swift 侧
// **不**把它们映射成错误：前者是正常结束（playback == .ended），后者是内部
// 瞬态（渲染器保留帧重试的契约）。
typedef NS_ENUM(NSInteger, SypStatusCode) {
    SypStatusCodeOK               =   0,
    SypStatusCodeEof              =  -1,
    SypStatusCodeInvalidArg       =  -2,
    SypStatusCodeCanceled         =  -3,
    SypStatusCodeTimeout          =  -4,
    SypStatusCodeOutOfMemory      =  -5,
    SypStatusCodeBusy             =  -6,
    SypStatusCodeIO               = -10,
    SypStatusCodeNoSpace          = -11,
    SypStatusCodeCacheCorrupt     = -12,
    SypStatusCodeNetwork          = -20,
    SypStatusCodeHttpStatus       = -21,
    SypStatusCodeTooManyRedirects = -22,
    SypStatusCodeRangeUnsupported = -23,
    SypStatusCodeContentChanged   = -24,
    SypStatusCodeNotImplemented   = -99,
};

typedef NS_ENUM(NSInteger, SypClockKind) {
    SypClockKindAudio  = 0,
    SypClockKindSystem = 1,
};

// 主时钟从音频切到系统时钟的原因，数值与 TrackPlayer::ClockSwitchReason 的枚举顺序一一对应
// （SYPBridge.mm 里有静态断言钉住）。
typedef NS_ENUM(NSInteger, SypClockSwitchReason) {
    SypClockSwitchReasonNone        = 0,   // 仍是音频钟（或从来没有音频钟）
    SypClockSwitchReasonAudioFailed = 1,   // sink 失败，不可逆
    SypClockSwitchReasonAudioEnded  = 2,   // 音频播完，seek 后可逆
};

// 缓冲原因，数值与 TrackPlayer::BufferingReason 的枚举顺序一一对应（SYPBridge.mm 里有静态断言钉住）。
typedef NS_ENUM(NSInteger, SypBufferingReason) {
    SypBufferingReasonNone    = 0,   // 未缓冲
    SypBufferingReasonStartup = 1,   // 起播缓冲
    SypBufferingReasonSeek    = 2,   // seek 后缓冲
    SypBufferingReasonStall   = 3,   // 播放中卡顿
};

// 画面填充方式，数值与 syp::media::Gravity 的枚举顺序一一对应
// （SYPBridge.mm 里有静态断言钉住；两个方向的转换都是 switch 全枚举、不写
// default，任一侧加 case 编译期就会报）。
typedef NS_ENUM(int32_t, SypVideoGravity) {
    SypVideoGravityAspectFit  = 0,   // 等比缩放、留黑边（默认）
    SypVideoGravityAspectFill = 1,   // 等比缩放、铺满、居中裁剪
    SypVideoGravityResize     = 2,   // 拉伸铺满、不保持宽高比
};

// 一次轮询快照。**driftUs = lastPresentedPtsUs − positionUs 是这个壳存在
// 的主要理由**：AudioUnitSink 的
// 设备延迟常数（query_and_log_device_latency() 缓存的那三个分量之和）
// 没有自动化覆盖，取错时 played_us() 会稳定地偏，这个数就会稳定地偏在
// 某个非零值上而不是在 0 附近抖动——调用方（PlayerViewController）只管
// 显示，不做任何平滑/滤波，免得把该看见的偏差抹掉。
@interface SypPlayerSnapshot : NSObject

// 【读这条再读下面的字段】hasMedia 为 NO 时（尚未 open，或 open 失败、
// 已 close），**除 hasMedia 自己与 lastPresentedPtsUs 之外的每一个字段都
// 没有意义**，不要显示给用户。
//
// 为什么要单独立一个标志、而不是给那些字段填"合理默认值"：它们的零值
// 在语义上恰好都是一个**看起来合法**的值——speed = 0.0 会显示成
// "倍速 0.00x"，clockKind = 0 恰好是 SypClockKindAudio（于是没有媒体时
// 显示"时钟: Audio"），paused = NO 会让按钮显示成 "Pause"（读起来像正在
// 播放）。没有任何一处会报错，只是安静地显示错的东西。填默认值等于替一
// 个不存在的播放器编造状态，而调用方仍然无法分辨"真的在 1.0x 播"和
// "根本没打开"。所以这里给出的是**可分辨性**，显示成什么由调用方决定
// （PlayerViewController 显示成 "—"）。
@property (nonatomic, readonly) BOOL hasMedia;

// 【契约声明，不许省】positionUs **保证不是哨兵值**：调用方
// 可以无条件对它做整数减法。
//
// 这条保证不是白来的：TrackPlayer::position_us() 直接转发
// clock_->now_us()，而 AudioClock 转发的 IAudioSink::played_us() 在
// failed() 时按接口契约返回 AV_NOPTS_VALUE（= INT64_MIN，见
// src/media/audio_sink.h 那 25 行论证）。「sink 已失效、TrackPlayer 还没
// 降级」这个窗口一旦被 snapshot 采到，positionUs 就会是 INT64_MIN，而
// PlayerViewController 里 `lastPresentedPtsUs - positionUs` 是无守卫的
// Int64 减法——**Swift 的整数溢出是 trap，demo 会直接崩**，不是显示一个
// 离谱数字而已。TrackPlayer 那一侧的窗口已经关上了，但这层
// 桥不该把自己的正确性押在下游的内部不变量上：-snapshot 会把哨兵值钳成
// 上一次读到的有效位置（从未读到过则为 0），实现见 SYPBridge.mm。
//
// lastPresentedPtsUs 是另一回事：它**可以**是 INT64_MIN（从未呈现过），
// 由 hasPresentedFrame 把关——见下面那条属性自己的注释。两者的契约方向
// 相反，别混。
@property (nonatomic, readonly) int64_t positionUs;
@property (nonatomic, readonly) int64_t durationUs;        // 0 表示未知
@property (nonatomic, readonly) int64_t lastPresentedPtsUs; // 从未呈现过时为 INT64_MIN（AV_NOPTS_VALUE）
@property (nonatomic, readonly) BOOL hasPresentedFrame;     // lastPresentedPtsUs 是否有效
@property (nonatomic, readonly) BOOL hasVideo;              // 本次 open 是否绑定了视频轨
@property (nonatomic, readonly) SypClockKind clockKind;
@property (nonatomic, readonly) int64_t droppedFrames;
@property (nonatomic, readonly) int64_t presentFailures;
@property (nonatomic, readonly) BOOL paused;
@property (nonatomic, readonly) double speed;
@property (nonatomic, readonly) BOOL eof;

// 本次 open 是否用的是硬解（Pipeline::video_hardware_decoding()）。跟其余大部分
// 字段一样，hasMedia == NO 时没有意义（恒 NO）。
@property (nonatomic, readonly) BOOL hardwareDecoding;

// 渲染器 BUSY（在途上限已满）的重试次数（TrackPlayer::render_busy_frames()；
// BUSY 的帧不丢弃、留着下一轮重试，同一帧重试几次计几次，不是丢帧数——
// 真正丢掉的帧仍计入 droppedFrames）；是否处于追帧丢帧状态（TrackPlayer::catching_up()）；
// 主时钟切换原因（SypClockSwitchReason，对应 TrackPlayer::ClockSwitchReason，见
// track_player.h）。三者 hasMedia == NO 时同样没有意义。
@property (nonatomic, readonly) int64_t renderBusyFrames;
@property (nonatomic, readonly) BOOL catchingUp;
@property (nonatomic, readonly) SypClockSwitchReason clockSwitchReason;

// 缓冲状态（TrackPlayer::buffering()/buffering_reason()）、已缓冲时长（毫秒，
// TrackPlayer::buffered_us()/1000；所有在播轨都已读完、无上限时为 -1）、卡顿次数
// （TrackPlayer::rebuffer_count()）。hasMedia == NO 时同样没有意义。
@property (nonatomic, readonly) BOOL buffering;
@property (nonatomic, readonly) SypBufferingReason bufferingReason;
@property (nonatomic, readonly) int64_t bufferedMs;
@property (nonatomic, readonly) int64_t rebufferCount;

// 起播耗时（TrackPlayer::startup_us()，微秒；**未起播完成时为 -1**，
// 不是 0——0 是一个看起来合法的"瞬间起播"读数）与音频欠载段数
// （TrackPlayer::audio_underruns()）。Swift 侧把 -1 映射成
// SYPlayerStatistics.startupDuration == nil。hasMedia == NO 时同样没有意义。
@property (nonatomic, readonly) int64_t startupUs;
@property (nonatomic, readonly) int64_t audioUnderruns;

// 显示尺寸（TrackPlayer::video_display_size()）：SAR 拉伸、按旋转
// 交换宽高之后的像素尺寸，即"画面该按什么宽高比摆"。无视频轨（纯音频源，或
// 唯一的 is_video 轨是封面图）时两者都是 0。hasMedia == NO 时同样没有意义（恒 0）。
@property (nonatomic, readonly) int32_t videoDisplayWidth;
@property (nonatomic, readonly) int32_t videoDisplayHeight;

@end

// ---------------------------------------------------------------- 缓存配置
// 缓存目录/容量从"写死在 -openURLString: 里"变成可配。
// 作用域是**每个桥实例一份**：SypPlayerBridge 与 SypPreloaderBridge 各自持有
// 自己的一份配置，但只要归一化之后的目录相同，底层的 CacheStore 就会让它们
// 共享同一份索引，于是"预加载暖好的字节播放时直接命中"是结构事实，不靠调用
// 方对齐。
//
// 【directory 会被**词法**归一化】折叠 "//"、消掉 "."、按字面弹掉 ".."，再剥
// 掉尾部的 '/'，然后才交给 dl 层；两侧走的是同一个
// syp::dl::normalize_cache_dir()。理由见 src/dl/cache_store.h 里那个函数
// 上方的长注释：CacheStore::make_key **不**归一化目录，同一个目录的两种拼法
// 在注册表里就是两份缓存——不报任何错，只是预加载暖的字节播放时命不中。
// 实测 Apple 这边四种拼法都出得来：NSTemporaryDirectory() 带尾斜杠，而
// URL.path 会原样保留 "//"、"." 与 ".."（实测，见那条长注释里的表）。
// **不做** Unicode 归一化、**不解析**符号链接，两条都是有意的局限，同上。
@interface SypCacheSettings : NSObject
@property (nonatomic, copy) NSString* directory;
@property (nonatomic) int64_t maxBytes;            // 0 = 不限
@property (nonatomic) int64_t minFreeSpaceBytes;   // 0 = 不看可用空间
@property (nonatomic) int64_t ttlMs;               // 0 = 不按时间过期

// 默认目录：Caches/syplayer-http-cache（取不到 Caches 时退回 tmp）。
+ (NSString *)defaultDirectory;
@end

// syp_preload_stats 的一次快照，**六个字段全映射**。
@interface SypPreloadStatisticsSnapshot : NSObject
@property (nonatomic, readonly) int64_t entries;
@property (nonatomic, readonly) int64_t activeTasks;
// ⚠️ 低报：provider 为了读 header 自己开的那条源不计入。
// 上界是"**每一次探测** 2 MiB"，**不是"每个 URL
// 2 MiB"**——失败的探测不被记忆，同一个 URL 会被反复探测。实测同一 URL
// 连探三次：2,215,936 + 49,669 + 0 = 2,265,605 B > 2,097,152。
// 见 src/media/media_info_provider.h。
@property (nonatomic, readonly) int64_t downloadedBytes;
@property (nonatomic, readonly) int64_t completed;
@property (nonatomic, readonly) int64_t failed;
// 装了 provider 但该 URL 估不出的累计次数（它涨就说明退化成了按字节）。
// 上一版写的是"唯一可观测信号"，而它**依赖缓存冷热**：
// moov 在尾部的容器只退化**一次**（第一次探测拉下来的前缀留在缓存里，第二
// 次探测从暖缓存起步就成功），所以同一个 URL 冷态涨、热态不涨。可以用来
// 看"整体上有多少条目退化了"，不能用来判断单个 URL 支不支持按时间预加载。
@property (nonatomic, readonly) int64_t providerMiss;
@end

typedef NS_ENUM(NSInteger, SypPreloadPriority) {
    SypPreloadPriorityBackground = 0,
    SypPreloadPriorityNext       = 1,
    SypPreloadPriorityPlaying    = 2,
};

// 预加载门面的 ObjC++ 桥。**除 -dealloc 外所有方法都不阻塞**（真正的下载在
// 驱动线程上），可以直接在主线程调用。
//
// ⚠️ **-dealloc 会阻塞**，而且可能阻塞很久：它要等驱动线程退出，而那条线程
// 可能正卡在 provider 的 estimate_range_for_ms 里。两类慢服务端都量过：
// "只接受连接、永不响应"实测 **6,015–6,021ms**；"1 KiB/s 慢速滴流"此前
// **无界**（跑满 150 秒未结束，理论 ~34 分钟），后来给它补了一条
// 15,000ms 的墙钟看门狗，实测 15,047–15,065ms 返回。所以今天的最坏值是
// ~15 秒，见 media_info_provider.h 的 kProbeWallClockMs。
// Swift 侧的 SYPlayerPreloader.deinit 因此**不在原线程上释放本对象**，而是
// 把最后一份强引用交给后台队列——本类没有任何主线程亲和性，在别的队列上
// -dealloc 是安全的。
//
// 创建时用 syp::media::PreloadStack 一次造出 provider + preloader（不是各建
// 各的），所以 seconds 参数是有效的，且两者的 cache_dir 逐字节相同。
@interface SypPreloaderBridge : NSObject
- (instancetype)initWithCache:(SypCacheSettings *)cache
                maxTotalTasks:(int32_t)maxTotalTasks
           reservedForPlaying:(int32_t)reservedForPlaying
          defaultPreloadBytes:(int64_t)defaultPreloadBytes
             defaultPreloadMs:(int64_t)defaultPreloadMs NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

// 返回 NO 表示参数非法（空 URL）或底层未就绪（目录为空 / 后端没注册上）。
// **"这个 URL 已经在表里"不是失败**，返回 YES（只抬优先级，见
// syp_preload.h）。
//
// 【这几条都写了 NS_SWIFT_NAME】Swift 的"省略多余词"规则会不会把
// `addURLString:` 的第一个参数标签吃掉，取决于导入器对 "URLString" 这个
// 词尾的判断——那是个会随工具链变的细节，而 Swift 侧的调用点写死了名字。
// 显式钉住，改了名就是编译错误，不是"某次升级后突然编不过"。
- (BOOL)addURLString:(NSString *)urlString
            priority:(SypPreloadPriority)priority
        milliseconds:(int64_t)milliseconds NS_SWIFT_NAME(add(_:priority:milliseconds:));
- (void)setPriority:(SypPreloadPriority)priority forURLString:(NSString *)urlString
    NS_SWIFT_NAME(setPriority(_:forURLString:));
- (void)removeURLString:(NSString *)urlString NS_SWIFT_NAME(remove(_:));
- (void)removeAll;
- (SypPreloadStatisticsSnapshot *)statistics;

// 检验缝：本对象**真正在用**的那份缓存目录（已归一化）。存在的理由与
// PreloadStack::provider_cache_dir_for_test() 一样——"预加载与播放落在同一份
// 缓存里"这条要可断言，断的是两边实际拿去 make_key 的那个字符串，而不是
// "我们传了同一个变量进去"。底层没建起来时返回 nil。
- (nullable NSString *)cacheDirectoryInUse;

// 检验缝（行为版，比上面那个字符串版更硬）：这个 URL 在**本对象那份
// cache_dir** 上，进程内的 CacheStore 当前有几个持有者——播放用的
// syp_source、provider 的探测源、预加载条目自己的源都算。
// 走的正是 CacheStore::make_key(cache_dir, url)，也正是 Preloader 判断
// "要不要给同一 key 上的播放源让路"用的那个数。> 0 就意味着"有别人开着
// 同一份缓存"。底层没建起来时返回 -1。
- (int64_t)cacheOpenCountForURLString:(NSString *)urlString
    NS_SWIFT_NAME(cacheOpenCount(forURLString:));

// 检验缝：容量/最小可用空间/TTL
// **真的穿过 ObjC++→C 这一跳了没有**。返回的是 provider 自己手里那份
// syp_config 的四元组快照（directory / maxBytes / minFreeSpaceBytes / ttlMs），
// 不是 -initWithCache: 收到的那个 SypCacheSettings 的回声。
//
// 为什么需要：这三个数从 Swift 到 C 要过 bridged() → SypCacheSettings →
// prepare_cache_dir → syp_config 三跳，而此前只有第一跳有用例——把
// prepare_cache_dir 里那三行改成硬 0（缓存无上限、永不过期、无限涨），74 条
// 用例一条都不红。底层没建起来时返回 nil。
- (nullable SypCacheSettings *)cacheSettingsInUse;

// 检验缝：上面那条读的是 **provider**
// 手里那份 syp_config，也就是"探测"那一支；这一条读的是 **Preloader** 那一支
// ——真正承担全部预加载下载、给每一条 SourceBridge 派配置的那个对象
// （syp::dl::Preloader::base_config()）。
//
// 为什么两条都要：把三个容量字段在 provider 构造之后、
// Preloader::create 之前清零（media_info_provider.cpp），`ctest 33/33` 与
// `xcodebuild 82 tests, 0 failures` **同时全绿**——缓存不再有上限、不再过期、
// 无限涨，而上面那条缝看不见，因为它装在支流上。底层没建起来时返回 nil。
- (nullable SypCacheSettings *)preloaderCacheSettingsInUse;
@end

@interface SypPlayerBridge : NSObject

// 【下面两个回调 block 共同的调用方约束】
// **不能在 onEof / onError 里释放持有 SypPlayerBridge 的唯一一个强引用。**
// 两者都在泵线程上被调用，栈里还压着 pump_loop()；在回调内让引用计数归零
// 会把 -dealloc（进而 ~PlayerCore、进而等待泵线程停下的 dispatch_sync）
// 嵌套进泵线程自己的调用栈里 —— 那次 dispatch_sync 会等一个永远回不来的
// 自己。**没有任何自动化手段强制这条**，它是纯纪律。常规写法（把 bridge
// 存在 view controller 的属性里、在主线程释放）天然不会踩到；会踩到的是
// "回调里把持有它的那个对象置 nil"这种写法。
//
// onFrame（每帧 RGBA 回读）已删除——真上屏路径不再需要
// CPU 回读，见 -attachVideoLayer: 与 metal_renderer.h 的 set_output_layer()。

// Pipeline::step() 报 Eof 时触发一次（此后不会重复触发，直到下一次
// open/seek）。同样在泵线程上被调用。
@property (nonatomic, copy, nullable) void (^onEof)(void);

// step() 报 Error，或 open 失败之外的运行期错误时触发，携带 syp_status
// （见 include/syplayer/syp_types.h 的整数值——本头不 #include 它，
// 免得把 C ABI 头文件牵进这个"非公开 ABI"的桥接层，调用方按需要自己转译
// 成文案）。连续多次 Error 只触发一次；报错后泵线程不停止，
// 出现非 Error 结果（如 seek 成功后恢复播放）即解除，再次出错会再触发。
@property (nonatomic, copy, nullable) void (^onError)(NSInteger sypStatus);

// 本平台是否支持硬解 H.264 8-bit（videotoolbox_available_for_h264()）。Mac
// Catalyst 上恒 NO（FFmpeg videotoolbox.c 在 Catalyst 上禁用了
// hwaccel，见 vt_decode_backend.h）；不需要具体流，UI 用它提前禁用硬解选项。
+ (BOOL)hardwareDecodeAvailable;

// 挂上（或替换）真上屏用的 CAMetalLayer。可以在 open 之前调用一次（比如
// viewDidLoad 里），也可以在任意时刻替换成另一个 layer；-open* 内部建好
// renderer 之后会自动把当前挂着的 layer 接到新的 renderer 上，调用方不需要
// 每次 open 之后重新调用一次。layer 由本对象 CFBridgingRetain 持有一份强
// 引用，dealloc / 再次 attach 时 CFBridgingRelease 释放旧的一份。
// 须在主线程调用：本方法在这里把 layer 配成 device/BGRA8Unorm/
// framebufferOnly；后台 open 队列上的 MetalRenderer::set_output_layer() 只校验不改，
// 未配置的 layer 在非主线程会被拒绝挂载（仅离屏、无画面）。
// 传 nil 即解挂（渲染器不再有输出 layer，画面停在最后一帧、音频照常）；
// SYPlayer.detachVideo() 与视图移出窗口时走的就是这条路。
- (void)attachVideoLayer:(nullable CAMetalLayer *)layer;

- (instancetype)init NS_DESIGNATED_INITIALIZER;

// 设置本实例用的缓存目录与容量上限。必须在 -open* **之前**调用才对那次打开
// 生效（-openURLString: 在建 syp_source 时读它）。不调用则用 SypCacheSettings
// 的默认值：Caches/syplayer-http-cache + 512MiB/256MiB/7 天。
// 只影响 -openURLString:（本地文件不走缓存）。
//
// **目录会在这里就被建出来**（D-3），但**建不出来这件事不会被上报**：本方法
// 返回 void，底层 createDirectoryAtPath: 的 error: 传的是 nil。实测词法父目录
// chmod 0500 时目录建不出来而调用方拿到零信号，真实后果是第一次 open 报
// SYP_ERR_IO。提前建的收益是"少一次失败往返 + 不把文件系统操作压到 open 的
// IO 路径上"，不是可诊断性。完整说明见 SYPBridge.mm 的 prepare_cache_dir。
- (void)setCacheSettings:(SypCacheSettings *)cache;

// 检验缝，语义同 SypPreloaderBridge 的同名方法：下一次 -openURLString: 会
// 交给 dl 层的那份缓存目录（已归一化）。
// **纯 getter，不建目录**（两侧同名方法原先一侧有副作用一侧没有）。
- (NSString *)cacheDirectoryInUse;

// 检验缝，语义同 SypPreloaderBridge 的同名方法，但方向是
// "**下一次** open 会用什么"：目录 + 容量/最小可用空间/TTL 四个数。纯 getter。
- (SypCacheSettings *)cacheSettingsInUse;

// 检验缝：
// **最近一次 -openURLString: 真的构造出来交给 dl 层**的那份 syp_config 的缓存
// 四元组，在任何网络 IO 之前拍下，所以那次 open 失败与否都已落定。
// nil = 本对象还没 open 过 URL。
//
// 与 -cacheSettingsInUse 的区别值得写明：那一条是"再算一遍"，这一条是
// "真的发生了什么"。只有这一条能发现"有人让 -openURLString: 不走
// prepare_cache_dir 了"——今天两者共用，所以分叉不了；明天不一定。
@property (nonatomic, readonly, nullable) SypCacheSettings* lastOpenCacheConfig;

// 打开本地文件路径（UIDocumentPickerViewController 拿到的安全作用域 URL
// 转成 path，或者 app bundle 里的资源路径）。成功后处于暂停态，播放/
// 出声要调用 -play。泵线程在这一步启动。hw=YES 时按硬解打开（Pipeline
// 硬解模式）；平台/编码/位深不支持时打开失败，sypStatusOut 回填
// SYP_ERR_NOT_IMPLEMENTED（= -99）。
- (BOOL)openLocalFile:(NSString *)path hardwareDecode:(BOOL)hw errorOut:(int32_t *)sypStatusOut;

// 打开远程 URL——走 src/dl 那一层，第一次调用时惰性注册 NSURLSession 版
// HTTP 后端（syp_apple_http_backend()）。URL 路径以 .m3u8 结尾时走 HLS
// （Pipeline::create_hls），否则当单个媒体文件（syp_source + AvioBridge）。
// 两条路径上 -close 都能打断卡住的网络读，不会冻结调用线程。hw 语义同上。
- (BOOL)openURLString:(NSString *)urlString hardwareDecode:(BOOL)hw errorOut:(int32_t *)sypStatusOut;

- (void)play;
- (void)pause;

// 音量 / 静音 / 填充方式。
//
// **跨 -openXxx: 与 -close 保留**：三者的持久值存在桥自己身上（PlayerCore），
// 不在 TrackPlayer 里——TrackPlayer 每次 open 都被新建，它自己只管"本实例内"
// （见 track_player.h 里对应的注释）。每次 open 时桥把这三个值作为
// InitialSettings 传进 TrackPlayer::create()——在 sink open 之前就落下，持久静音
// 不会在开头漏出一段近满音量；运行中调 setter 则持久值与当前
// TrackPlayer 同时更新。
// getter 读的是持久值，所以未 open 时也读得到调用方设过的值。
//
// 与 play/pause 一样持 mu_，任意线程安全（Swift 侧只从主 actor 调）。

/// 线性音量，0...1。默认 1.0。跨 -openXxx: 保留。
/// **本层不夹取**：原样存、原样转发给 TrackPlayer（它为自己的消费者再夹一次）。
/// 夹取（NaN → 0，+inf → 1，-inf/负数 → 0，>1 → 1）归唯一调用方 Swift 侧
/// SYPlayer.volume——这层若也夹，Swift 那一道的变异就全部等价、没有用例守着。
/// 所以直接调本属性传越界值时，getter 会原样读回越界值，而 -volumeInUseForTest
/// 读到的是 TrackPlayer 夹过的值。
@property (nonatomic) double volume;
/// 静音。与 volume 相互独立——静音不擦掉 volume 的值。默认 NO。跨 -openXxx: 保留。
@property (nonatomic, getter=isMuted) BOOL muted;
/// 填充方式。默认 SypVideoGravityAspectFit。跨 -openXxx: 保留。
@property (nonatomic) SypVideoGravity videoGravity;

// 检验缝：**当前 TrackPlayer（player_，消费者）手里**的音量 /
// 静音 / 填充方式，不是上面三个属性的持久副本。
//
// 为什么必须读消费者：上面三个 getter 读的是桥上的持久值，桥哪怕从来没把它
// 下发给新建的 TrackPlayer，getter 照样返回调用方设的那个数——只断言 getter
// 就是前面提过的那种假绿（与 -cacheSettingsInUse / -lastOpenCacheConfig 两条
// 缝"再算一遍"与"真的发生了什么"的区别同理）。这三条读的是 TrackPlayer 自己的
// volume()/muted()/gravity 镜像，只有桥真的下发过它才会变。
//
// 没有 player_（未 open / open 失败 / 已 close）时一律返回 -1。
// -mutedInUseForTest 否则返回 0/1；-videoGravityInUseForTest 否则返回
// SypVideoGravity 的枚举值。
- (double)volumeInUseForTest;
- (int32_t)mutedInUseForTest;
- (int32_t)videoGravityInUseForTest;

// 检验缝：当前 TrackPlayer 的 AudioUnitSink 在**最近一次 open()
// 那一刻**的增益起点（AudioUnitSink::gain_at_open_for_test()，即 render_ctx_ 的
// gain_current 快照，float 精度）。上面 -volumeInUseForTest 读的是 TrackPlayer 的
// 最终值，"create() 之后才下发"与"作为 InitialSettings 传进 create()"两种写法
// 读到的一样；只有这条能区分——前者 open 那一刻是 1.0，开头约 15ms 近满音量。
// 无 player_ 返回 -1；sink 的 open() 没走到快照那一步返回 NaN。
- (double)sinkGainAtOpenForTest;

// 检验缝：装饰层 → MetalRenderer 那一跳转发的**像素级**
// 证据。上面的 -videoGravityInUseForTest 读的是装饰层自己记的值，记在转发之前；
// 装饰层漏转发时它照旧、画面却不对。这两条走 PresentTrackingRenderer（不绕过它）：
//
// -debugBlitPixelForTest:…：用 MetalRenderer 当前的几何与 gravity 把最近一次呈现的
//   帧经生产同款 blit 画进 dstW×dstH 离屏纹理，回读归一化坐标 (fx, fy)（(0,0) = 左上）
//   处一个像素，按 B | G<<8 | R<<16 | A<<24 打包写进 *out。无 player / 尚无已呈现帧 /
//   尺寸非正 / 设备不可用返回 NO，不写 *out。
// -debugSetSourceGeometryForTest:…：经装饰层调 set_source_geometry()。只为让 SAR 1:1、
//   不旋转的 sample.mp4 也能区分"几何转发了没有"；生产上几何只由 TrackPlayer::create()
//   下发。无 player 返回 NO。
//
// 都持 mu_ 调用，与泵线程的 step()/present() 串行（debug_blit_to_bgra 的契约要求）。
- (BOOL)debugBlitPixelForTest:(int32_t)dstW
                       height:(int32_t)dstH
                          atX:(double)fx
                            y:(double)fy
                         bgra:(uint32_t *)out;
- (BOOL)debugSetSourceGeometryForTest:(int32_t)sarNum
                               sarDen:(int32_t)sarDen
                          rotationDeg:(int32_t)rotationDeg;

// [0.5, 2.0]。越界返回 NO，不改变当前倍速。
- (BOOL)setSpeed:(double)speed;

// 定位。**同时解除 EOF 闩**（上面 onEof 那条"直到下一次 open/seek"由这里兑现）：
// seek 之后 -snapshot 的 eof 回到 NO，播到结尾再 seek 回去能重新播、也能重新
// 触发一次 onEof。未打开时是 no-op。
- (void)seekToUs:(int64_t)us;

- (SypPlayerSnapshot *)snapshot;

// 停泵线程、释放 pipeline/sink/renderer/dl 层资源。之后可以再 open 一次
// （close 会把内部状态复位，不需要重新创建 SypPlayerBridge 实例）。
// dealloc 会自动调用一次，UI 层通常不需要显式调用，除非要主动切换素材。
- (void)close;

@end

/// 进程级下行限速的桥。转发 syp_net.h，不持有任何状态。
/// 异常与夹取（负数/超大值）已在 C 层兜底，这里不重复。
@interface SypNetworkBridge : NSObject
+ (void)setRateLimitBytesPerSecond:(int64_t)bytesPerSecond;
+ (int64_t)rateLimitBytesPerSecond;

// 预连接（syp_preconnect）。headerNames/headerValues 必须等长，按下标
// 一一对应；两者都为 nil 等价于不传头。
+ (void)preconnectURL:(NSString *)url
           headerNames:(NSArray<NSString *> *)names
          headerValues:(NSArray<NSString *> *)values;

// 测试缝：读 Preconnector::instance().inflight_for_test()。
+ (int32_t)preconnectInflightForTest;

// 测试缝：读 current_http_backend() != nullptr。
+ (BOOL)preconnectBackendRegisteredForTest;
@end

NS_ASSUME_NONNULL_END
