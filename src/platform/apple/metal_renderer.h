// metal_renderer.h — IVideoRenderer 的 Metal 实现。跟
// audio_unit_sink.{h,mm} 是同一批"第一次真正碰硬件"的平台组件，组织方式
// 刻意照抄那一份：纯算术/纯判定拆成 static 方法，用普通 test_metal_
// renderer.cpp（.cpp，不是 .mm）直接单测，不需要该 TU 本身编成
// Objective-C++、也不需要链 Metal.framework。
//
// 本头本身不 #include 任何 Apple 框架头（Metal/Foundation 都只在
// metal_renderer.mm 里出现）：全部 Metal 状态（id<MTLDevice> 等）都装在
// 一个只在 .mm 里定义的 Impl 里，本类只持有 std::unique_ptr<Impl>——跟
// audio_unit_sink.h 把 AudioComponentInstance 存成 void* 是同一个理由，
// 只是 Metal 这边资源更多（device/queue/pipeline/四张纹理），PIMPL 比
// 一堆 void* 成员更干净，而且 id<...> 是 ARC 托管对象，不能安全地塞进
// 裸 void*（ARC 的自动 retain/release 靠类型信息，void* 会绕过它）。
//
// —— 三条硬要求 ——
// 1. 只支持 yuv420p（另加 VideoToolbox NV12 硬解帧，见下）。遇到别的
//    pix_fmt 如实返回 SYP_ERR_NOT_IMPLEMENTED 并把格式名打进日志——静默画错比
//    报错更难查。
// 2. 三平面纹理必须按 stride 逐行拷，不能整块拷（AVFrame 的 buffer 带
//    对齐 padding，同 tools/syp_probe/frame_digest.cpp 处理平面数据的
//    手法）。
// 3. 色彩矩阵必须从 AVFrame 的 colorspace/color_range 取，取不到时按
//    分辨率推定，并把"这是推定的"打进日志。
//
// —— 第 3 条曾经的一个结构性落差，已修复 ——
// 早先 `Frame`（src/media/frame.h）只暴露 width()/height()/
// pix_fmt()/plane()/stride() 四个视频访问器，没有 colorspace()/
// color_range()，present() 因此结构性地只能恒以 AVCOL_SPC_UNSPECIFIED/
// AVCOL_RANGE_UNSPECIFIED 调用 select_color_matrix()——"取不到时按分辨
// 率推定"这条分支在那时的接线下恒为真。后来解禁了 frame.h 的这两个访问器，
// 给 Frame 加了
// colorspace()/color_range()（直接映射 AVFrame 对应字段，逐字照抄
// pix_fmt() 的写法），present() 现在真的把 f.colorspace()/
// f.color_range() 传给 select_color_matrix()——纯函数本身没有改动，
// 只是现在真的喂到了真值。回归覆盖见 test_metal_renderer.cpp 第 3 段
// 两条新用例（真实 colorspace 压过分辨率推定 / AVCOL_RANGE_JPEG 真的
// 走到 GPU 的 full-range 路径）。
//
// —— VideoToolbox NV12 零拷贝 ——
// AV_PIX_FMT_VIDEOTOOLBOX 帧的 hw_handle() 是 IOSurface 支撑的 CVPixelBuffer
// （NV12 420v/420f）。present() 经 CVMetalTextureCache 把两个平面直接映射成
// MTLTexture，编码 nv12_to_rgba 写同一张 out_tex——全程不经 CPU 拷贝；
// debug_cpu_upload_count() 只在 yuv420p 的 replaceRegion 路径上递增，测试据此
// 断言硬解帧零 CPU 上传。
#pragma once

#include "media/frame.h"
#include "media/video_geometry.h"
#include "media/video_renderer.h"

#include <syplayer/syp_types.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace syp::platform {

// present() 的 due_in_us 在渲染器内夹到 [0, kMaxPresentDueUs]（见 present() 注释）。
inline constexpr int64_t kMaxPresentDueUs = 1'000'000;

class MetalRenderer final : public syp::media::IVideoRenderer {
public:
    // 系统默认 Metal 设备在构造时就尝试创建（MTLCreateSystemDefaultDevice()
    // 只在 metal_renderer.mm 里调用）。创建失败（没有 GPU 的环境，比如
    // 某些无头虚拟机）不抛异常、不终止进程——failed() 会报 true，present()
    // 之后恒返回 SYP_ERR_IO，跟 AudioUnitSink::open() 失败后 write()
    // 恒返回 false 是同一条纪律：平台组件初始化失败要能被优雅地问出来，
    // 不是让调用方等到第一次真正呈现才发现。
    MetalRenderer();
    ~MetalRenderer() override;

    MetalRenderer(const MetalRenderer&)            = delete;
    MetalRenderer& operator=(const MetalRenderer&) = delete;

    // 呈现一帧。返回值语义见 IVideoRenderer::present()（video_renderer.h）：
    // 非 SYP_OK 表示该帧未被呈现，调用方计数、不终止播放。
    //
    // 不阻塞：编码后直接 commit，不等 GPU。在途名额（inflight_gate.h）：
    //   - 离屏：上限 kMaxInflightFrames，命令缓冲完成时归还；
    //   - 挂 layer：上限 min(kMaxInflightFrames, layer.maximumDrawableCount - 1)（默认 3 → 2，
    //     留一个给屏幕上的前缓冲），drawable 的 presentedHandler 里归还（drawable 回池的
    //     时机）；命令缓冲执行失败则在完成回调里归还；命令缓冲正常完成但 presentedHandler
    //     超过 提交时刻 + kMaxPresentDueUs + 200ms 仍未回调，由后续 present()/wait_until_idle()
    //     强制归还（兜底，仅挂 layer 的帧）。三处经每帧令牌 exactly-once。
    //   名额满时立即 SYP_ERR_BUSY，不调 nextDrawable。每帧用一组独立纹理槽，CPU
    //   永远不改写 GPU 正在读写的纹理；挂 layer 时名额可能先于槽归还，此时拿到名额也可能
    //   暂无空闲槽，同样返回 BUSY（瞬态）。SYP_OK 只表示"已提交"；GPU 执行错误**不再同步
    // 报告**，在完成回调里计数并打日志（见 debug_gpu_error_count()）。
    // 具体失败码：
    //   SYP_ERR_IO             — 设备/管线在构造时就没建起来
    //   SYP_ERR_INVALID_ARG    — Frame 本身无效，或宽高非正
    //   SYP_ERR_NOT_IMPLEMENTED — pix_fmt 既不是 yuv420p 也不是 VIDEOTOOLBOX（第 1 条
    //                             硬要求），或 VIDEOTOOLBOX 帧的 CVPixelBuffer 不是 NV12
    //                             420v/420f
    //   SYP_ERR_INVALID_ARG    — 也包括 VIDEOTOOLBOX 帧没有 hw_handle()，或其 CVPixelBuffer
    //                             亮度平面比 Frame 声明的宽高小
    //   SYP_ERR_BUSY           — 在途帧已满，或已 set_output_layer() 但 nextDrawable 返回
    //                             nil（本帧不提交）；瞬态，调用方保留帧稍后重试
    // due_in_us 见 IVideoRenderer::present()：先夹到 [0, kMaxPresentDueUs = 1s]（上游
    // TrackPlayer 给的是 ≤ 40ms/倍速；超过 1 秒只可能是调用方算错，持有 drawable 那么久
    // 会让 layer 路径长时间 BUSY）。有 layer 且 > 0 时用 presentDrawable:atTime:
    // （CACurrentMediaTime() + due）按时刻上屏，否则立即上屏。
    syp_status present(const syp::media::Frame& f, int64_t due_in_us) override;

    // 显示几何与填充方式，语义见 IVideoRenderer（video_renderer.h）。下一次
    // present() 起生效：present() 在挂 layer 的 blit 处现读这四个值，交给
    // syp::media::blit_transform() 算出 blit 参数。
    //
    // 线程契约【统一口径】：**与 present() 由调用方串行**。生产上由
    // 桥的 _core->mu_ 保证——-setVideoGravity: 与泵线程的 step()/present() 都持
    // mu_；测试缝 -debugSetSourceGeometryForTest 在首帧之后调用，安全也正是因为
    // 它持 mu_。与 present()/set_output_layer() 本身不加锁、靠调用方串行是同一个
    // 形状，这里不为几何单独引入一把锁。四个值存成各自的原子量只是防御，不是
    // 允许并发的依据：若将来出现不能与 present() 串行的调用方，须先把四个量打包
    // 成单个原子整体替换，否则 present()
    // 可能读到新旧混合的组合。
    void set_source_geometry(int32_t sar_num, int32_t sar_den, int32_t rotation_deg) noexcept override;
    void set_gravity(syp::media::Gravity g) noexcept override;

    // 构造时 Metal 设备/命令队列/计算管线是否**全部**建立成功。测试
    // 需要能区分"设备缺失"与"格式不支持"两类 present() 失败，不能靠
    // 猜返回值。
    //
    // 【ready() 曾经的一个缺陷】ready() 曾经是唯一的失败查询，
    // 把"没有 GPU 设备"（环境限制，测试该 skip）与"设备存在但着色器
    // 编译/管线创建失败"（真实缺陷，测试该 FAIL）合并成一个布尔——
    // 一次故意引入的故障（在 video_shaders.metal 里删掉一个分号，让着色器编译
    // 不过）证明了这个合并的代价：四条 GPU 用例统一按"ready()==false
    // 就 skip"处理，全部颜色逻辑所在的那个文件彻底编译不过时，
    // 23 条用例仍然全绿——守卫被它本该抓的那类缺陷自己静默关掉。
    // device_available() 是这次拆分出来的更细的查询：单独回答"设备
    // 本身找到了没有"，不掺进后续着色器/管线是否建立成功。测试的正确
    // 判定必须是"device_available()==false 才 skip；device_available()
    // ==true 但 ready()==false，是真实缺陷，必须 FAIL"，不能再用
    // ready() 一个布尔替两件事做判断。
    bool ready() const noexcept;

    // 构造时 MTLCreateSystemDefaultDevice() 是否成功找到了一块 Metal
    // 设备——不代表后续的命令队列/着色器编译/计算管线也建立成功了，那
    // 是 ready() 管的事。两者的差集（device_available()==true 但
    // ready()==false）恰好就是"这台机器明明有 GPU，但我们自己的着色器
    // 或管线代码有问题"这类真实缺陷，测试必须能单独查出这个差集，见
    // ready() 上面的注释。
    bool device_available() const noexcept;

    // ------------------------------------------------------------------
    // 可脱离 GPU 独立测试的纯函数/纯数据判定（对应发起方追加的要求：
    // 「能不靠硬件验证的部分抽成纯函数并写单测」）。
    // ------------------------------------------------------------------

    // 格式判定：yuv420p 与 VideoToolbox 硬解帧（AV_PIX_FMT_VIDEOTOOLBOX）判 true。
    // 零 GPU 依赖，纯查表。VIDEOTOOLBOX 帧内部的 CVPixelBuffer 格式（只接受
    // NV12 420v/420f）要到 present() 里才能看到，不在这里判。
    static bool is_pix_fmt_supported(int32_t pix_fmt) noexcept;

    enum class ColorMatrixKind : int32_t {
        BT601 = 0,
        BT709 = 1,
    };

    // 色彩矩阵选择的完整结果：用哪套矩阵、是 full range 还是 limited
    // range、这次选择里有没有任何一项是推定出来的（不是从 colorspace/
    // color_range 直接读到的）。presumed 是一个"任一项被推定就为 true"
    // 的合取标志，不细分是矩阵还是 range 被推定——调用方（present()）
    // 只需要知道"这次呈现的颜色有没有不确定的地方，要不要打日志"，细分
    // 到底哪一项不确定对日志的价值不大，反而让判定逻辑的测试矩阵翻倍。
    struct ColorMatrixChoice {
        ColorMatrixKind matrix     = ColorMatrixKind::BT601;
        bool            full_range = false;
        bool            presumed   = false;

        bool operator==(const ColorMatrixChoice& o) const noexcept {
            return matrix == o.matrix && full_range == o.full_range && presumed == o.presumed;
        }
    };

    // 输入 colorspace/color_range 取 AVColorSpace/AVColorRange 的枚举值
    // （AVCOL_SPC_UNSPECIFIED == 2、AVCOL_RANGE_UNSPECIFIED == 0 是"取
    // 不到"的哨兵值，见 libavutil/pixfmt.h）。宽高用于"取不到 colorspace
    // 时按分辨率推定"这条规则——≥720p（约定按高度，height >= 720）用
    // BT.709，否则 BT.601。color_range 取不到时
    // 推定为 limited/tv range（行业最常见的默认值，比"猜 full range"更
    // 保守——错误地把 full range 内容当 limited 处理只会轻微压缩对比度，
    // 反过来把 limited 当 full 处理会明显削波，选更安全的一侧）。
    //
    // 零 GPU 依赖，纯查表 + 一次比较，可以拿任意 (colorspace, color_range,
    // width, height) 组合直接单测，不需要真实 Frame/AVFrame。
    static ColorMatrixChoice select_color_matrix(int32_t colorspace, int32_t color_range,
                                                  int32_t width, int32_t height) noexcept;

    // 三平面纹理上传前的"剥 padding"步骤（第 2 条硬要求）：把 src 里
    // 按 src_stride 隔开的 rows 行、每行 row_bytes 字节有效数据，逐行拷
    // 进 dst 里一段没有行间 padding 的连续缓冲（dst 至少要有
    // row_bytes * rows 字节）——跟 tools/syp_probe/frame_digest.cpp 里
    // av_image_copy 对每个分量做的事是同一件事，只是这里手写成一个可以
    // 独立单测的小函数（frame_digest.cpp 直接复用了 libavutil 的
    // av_image_copy，本函数服务于一个更窄的场景：单个平面、tight
    // linesize==row_bytes 的目标，不需要 pix_fmt 描述符那一整套）。
    //
    // 参数非法（任一指针为空、row_bytes<=0、rows<=0、src_stride <
    // row_bytes——stride 比一行有效数据还窄，说明调用方传错了，逐行拷会
    // 读到下一行本该属于别处的数据，必须拒绝而不是照常拷）时返回 false
    // 且不碰 dst 一个字节；否则做完全部 rows 行拷贝后返回 true。
    static bool pack_plane_rows(uint8_t* dst, const uint8_t* src, int32_t src_stride,
                                 int32_t row_bytes, int32_t rows) noexcept;

    // ------------------------------------------------------------------
    // 测试专用只读回读——真正碰 GPU，用来验证 present() 的接线（发起方
    // 追加要求的第 2 条：纯函数之外也要测"present() 有没有真的把 Frame
    // 的参数传给这些纯函数、再用返回值配置渲染"这件事本身）。
    // ------------------------------------------------------------------

    // 取出当前输出纹理的内容，RGBA8、行主序、无 padding（out.size() ==
    // width*height*4）。从未有过一次成功的 present()，或设备不可用时
    // 返回 false、不改动 out/width/height。
    //
    // 这不是给生产路径用的（生产路径要把纹理喂给 CAMetalLayer 的
    // drawable，零拷贝、GPU 到 GPU；不在本任务范围——见 video_renderer.h
    // 顶部注释"平台实现只做上传纹理 + 一个着色器"）。
    // 【订正】原注释这里写的是"那是
    // demo 壳的事"——这句话已经失实：demo 壳早就落地了
    // （demo/ios、demo/mac），而且正是靠这个方法跑起来的——它没有走
    // CAMetalLayer，是把 present() 每次成功之后的输出纹理经这个口子读回
    // CPU、拼成 CGImage 显示在 UIImageView 上（当时在 demo/shared/bridge.mm
    // 的 DemoVideoRenderer 里；那个类后来随桥搬进
    // swift/SYPlayerKit/Internal/SYPBridge.mm，并已改名为
    // PresentTrackingRenderer）。CAMetalLayer 生产路径现在已由
    // set_output_layer() 实现（同一命令缓冲里 compute 之后追加 blit
    // pass 画到 drawable），这个方法本身继续留给
    // tests/test_metal_renderer.cpp 当测试专用回读口。这个方法唯一的
    // 消费者除了测试之外，现在还多了 demo 壳：没有它，present() 的
    // GPU 端结果就只能"相信它对了"，不能真正断言，demo 壳也没有画面
    // 可看。
    bool debug_copy_output_rgba(std::vector<uint8_t>& out, int32_t& width,
                                 int32_t& height) const;

    // 测试专用：用当前的 set_source_geometry()/set_gravity() 状态，把最近一次
    // present() 的 out_tex 经**与生产 blit 同一个编码函数**画进一张 dst_w×dst_h 的离屏
    // BGRA8 纹理并回读（out.size() == dst_w*dst_h*4，字节序 B,G,R,A，行主序、无 padding）。
    // 无帧可画、dst 非正或设备不可用返回 false、不改动 out。
    //
    // 为什么需要它：旋转与 gravity 做在 blit pass 里，blit 只在挂 layer 时才执行，而
    // drawable 恒为 framebufferOnly、不可回读；debug_copy_output_rgba() 读的是 blit
    // **之前**的 out_tex，看不到几何变换。blit 参数在两条路径上都由
    // syp::media::blit_transform() 算（这里以 dst_w×dst_h 当目标尺寸），编码都走
    // metal_renderer.mm 里同一个 encode_blit()——测试缝测的就是生产编码路径本身。
    // 与 present() 必须由调用方串行（同 debug_copy_output_rgba）。
    bool debug_blit_to_bgra(int32_t dst_w, int32_t dst_h, std::vector<uint8_t>& out) const;

    // 结构性验收用：CPU 上传路径（yuv420p replaceRegion）累计帧数。零拷贝路径不计。
    int64_t debug_cpu_upload_count() const noexcept;

    // 生产上屏入口。ca_metal_layer 是 (__bridge void*) 的 CAMetalLayer*；
    // 非空：此后每次 present() 在同一命令缓冲里追加 blit pass 画到它的 drawable 并 present；
    // nullptr：回到仅离屏。渲染器强引用 layer。与 present() 必须由调用方串行。
    //
    // 线程契约：CALayer 属性只在主线程改。调用方应在主线程预先把 layer
    // 配成 device=系统默认 Metal 设备、pixelFormat=BGRA8Unorm、framebufferOnly=YES；
    // 本方法可以在任意线程调用，但只**校验** device（按 registryID）与 pixelFormat：
    //   - 已配好 → 挂上；
    //   - 未配好且当前在主线程 → 就地补配置后挂上（打 WARN）；
    //   - 未配好且不在主线程 → 拒绝（打 ERROR、保持仅离屏），绝不跨线程改 layer。
    // drawableSize 由视图在主线程布局时写、present() 在调用方线程现读——这一读是
    // 容忍的非同步读（CGSize 两个 double，最坏读到一次撕裂的中间尺寸，只影响本帧
    // aspect-fit 视口，下一帧自愈）。
    // nextDrawable 超时（返回 nil）→ present() 返回 SYP_ERR_BUSY。
    // drawableSize 为 0×0 时同样会让 nextDrawable 返回 nil → present() 返回
    // SYP_ERR_BUSY——但这是 present() 时刻的要求，不是 attach 时刻的：
    // drawableSize 是 layer 自身的属性，每次 present() 都现读，不会在
    // set_output_layer() 里缓存下来，所以 attach 可以早于 layer 拿到非零
    // drawableSize（demo 壳就是这么做的：viewDidLoad 里先 attach，
    // MetalVideoView.layoutSubviews 稍后才把 drawableSize 设成非零）——只要
    // **第一次真正 present() 的时候** drawableSize 已经非零即可。
    void set_output_layer(void* ca_metal_layer) noexcept;
    // 结构性验收用：经 layer 实际 present 的 drawable 累计数（离屏 present 不计）。
    // 在命令缓冲完成回调里计数（异步），断言前先 wait_until_idle()。
    int64_t debug_presented_drawable_count() const noexcept;

    // 最近一次安排的 presentDrawable 目标时刻（秒，CACurrentMediaTime 时基）；
    // 从未经 layer 上屏则为 0。
    double debug_last_present_host_time() const noexcept;
    // 完成回调里观察到的命令缓冲执行失败累计数（present() 不再同步报告）。
    int64_t debug_gpu_error_count() const noexcept;
    // 阻塞到所有在途帧完成：名额全部归还、所有命令缓冲完成回调跑完。析构函数也用它
    // （回调捕获裸 Impl*）；若 debug_hold_gate_releases(true) 仍生效，先放掉扣住的名额。
    // 有界：命令缓冲由 Metal 保证完成；丢失的 presentedHandler 最迟在最晚一次提交后约
    // 1.3 秒内被兜底回收，到期仍不归零则打日志返回。与 present() 必须由调用方串行。
    void wait_until_idle() noexcept;

    // 测试用：on=true 期间提交的帧，其 presentedHandler 不归还名额（模拟回调丢失），
    // 用来验证兜底回收。
    void debug_suppress_presented_release(bool on) noexcept;
    // 测试用：此后提交的帧的兜底期限（毫秒）；ms <= 0 恢复默认（kMaxPresentDueUs + 200ms）。
    void debug_set_permit_reclaim_after_ms(int32_t ms) noexcept;
    // 测试用：兜底回收的名额累计数。
    int64_t debug_reclaimed_permit_count() const noexcept;

    // 测试用：hold=true 时本该归还的在途名额先扣住计数（槽照常释放），hold=false 时一次性
    // 归还——用来确定性地把门填满、测 BUSY 与"满了不取 drawable"。
    void debug_hold_gate_releases(bool hold) noexcept;
    // 测试用：当前占用的在途名额数。
    int32_t debug_inflight_count() const noexcept;
    // 测试用：最近一次成功提交所用的纹理槽下标；从未提交或重建后未提交为 -1。
    int32_t debug_last_slot() const noexcept;

private:
    struct Impl;

    // 构造/present() 里尺寸变化时（重新）建每个槽的四张纹理 + 对应暂存缓冲。
    // 声明在这里（而不是留在 metal_renderer.mm 的匿名命名空间里）纯粹
    // 是因为它需要碰 Impl 的私有字段——Impl 对本类是可见的（同一个类
    // 的嵌套类型），对匿名命名空间里的自由函数不是。
    static void rebuild_textures(Impl& impl, int32_t width, int32_t height);

    // present() 按 pix_fmt 分叉出的两段编码。都是成员函数而不是
    // .mm 匿名命名空间里的自由函数，同样是为了碰 impl_。ObjC 对象不能出现在本头
    // （它被普通 .cpp 测试 include），cmd 是 id<MTLCommandBuffer>、keep_alive 是
    // NSMutableArray*，都以 __bridge 的 void* 过界，只在 .mm 里转回去。
    //
    // encode_nv12：VideoToolbox NV12 CVPixelBuffer 经 CVMetalTextureCache 取两张
    // 纹理（零 CPU 拷贝）并编码 nv12_to_rgba；需要活到命令缓冲完成的对象
    // （CVMetalTextureRef、CVPixelBuffer）塞进 keep_alive。返回 SYP_OK /
    // SYP_ERR_INVALID_ARG / SYP_ERR_NOT_IMPLEMENTED / SYP_ERR_IO，非 OK 时调用方
    // 不得提交 cmd。
    //
    // slot 是本帧独占的纹理槽下标（Impl::slots），两条路径都只写该槽的纹理。
    syp_status encode_nv12(const syp::media::Frame& f, void* cmd, void* keep_alive, int32_t slot);
    // encode_yuv420p：把 present() 已经按 stride 剥好 padding 的三块暂存区
    // replaceRegion 上传（计入 debug_cpu_upload_count）并编码 yuv420p_to_rgba。
    // 不会失败——可能失败的逐行打包（pack_plane_rows）在 present() 里、建命令
    // 缓冲之前就做完了。
    void encode_yuv420p(const syp::media::Frame& f, void* cmd, int32_t slot);

    // 用当前几何状态为 src_w×src_h 的 out_tex、dst_w×dst_h 的目标算 blit 参数。
    // present() 的 layer 路径与 debug_blit_to_bgra() 共用，保证两边读的是同一组状态。
    syp::media::BlitParams current_blit_params(int32_t src_w, int32_t src_h, double dst_w,
                                               double dst_h) const noexcept;

    std::unique_ptr<Impl> impl_;

    // 显示几何（set_source_geometry/set_gravity 写，present()/debug_blit_to_bgra 读）。
    // 默认值 = 1:1（0/0 在 blit_transform 里按 1:1 处理）、不旋转、AspectFit——与此前
    // 的 aspect_fit_scale() 行为一致。线程说明见 set_source_geometry() 注释。
    std::atomic<int32_t>             sar_num_{0};
    std::atomic<int32_t>             sar_den_{0};
    std::atomic<int32_t>             rotation_{0};
    std::atomic<syp::media::Gravity> gravity_{syp::media::Gravity::AspectFit};
};

}  // namespace syp::platform
