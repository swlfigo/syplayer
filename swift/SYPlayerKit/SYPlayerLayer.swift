// SYPlayerLayer.swift — 真上屏用的 CAMetalLayer 子类，**自己管 drawableSize**。
//
// 为什么这件事必须收进框架：CAMetalLayer 不像普通 CALayer 那样自动跟 bounds
// 同步 drawableSize，留着默认值（构造时的 0×0）会让 MetalRenderer::present()
// 里的 nextDrawable 返回 nil，一帧都呈现不出来（表现为 SYP_ERR_BUSY，
// TrackPlayer 保留帧重试直到迟到被丢）。此前这条隐式约束写在 demo 的
// PlayerView.MetalVideoView 里，等于每个接入方都要自己踩一遍。
//
// MetalRenderer 每次 present() 都现读 layer.drawableSize、不缓存 attach 那一刻
// 的值，所以 attach 顺序不敏感：只要真正开始播放之前这里设置过一次非零尺寸就够。
import Metal
import QuartzCore

public final class SYPlayerLayer: CAMetalLayer {
    public override init() {
        super.init()
        configure()
    }

    /// CALayer 在做呈现副本（presentation copy）时走这个初始化器。必须实现，
    /// 否则副本会丢掉 device/pixelFormat 配置。
    public override init(layer: Any) {
        super.init(layer: layer)
        configure()
    }

    public required init?(coder: NSCoder) {
        super.init(coder: coder)
        configure()
    }

    private func configure() {
        // CALayer 属性只在主线程改。视图构造（主线程）时一次配好
        // device / pixelFormat / framebufferOnly；MetalRenderer::set_output_layer()
        // 会在后台 open 队列上被调用，那里只校验、不改（见 metal_renderer.h）。
        if device == nil { device = MTLCreateSystemDefaultDevice() }
        pixelFormat = .bgra8Unorm
        framebufferOnly = true
    }

    public override func layoutSublayers() {
        super.layoutSublayers()
        syncDrawableSize()
    }

    /// bounds × contentsScale 同步到 drawableSize。**0×0 不下发**——保持上一次
    /// 有效值，避免 nextDrawable 恒 nil（对应"view 尺寸为 0"的情形）。
    /// internal 而非 private：单测直接调它，不必依赖 layoutIfNeeded 的时机。
    func syncDrawableSize() {
        let scale = contentsScale > 0 ? contentsScale : 1.0
        let width  = bounds.width * scale
        let height = bounds.height * scale
        guard width >= 1, height >= 1 else { return }
        let size = CGSize(width: width.rounded(), height: height.rounded())
        if drawableSize != size { drawableSize = size }
    }
}
