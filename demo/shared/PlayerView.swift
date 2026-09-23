// PlayerView.swift — 两个壳共用的 UIKit 视图层。纯哑视图：只布局、不碰
// 播放器，所有播放逻辑都在 PlayerViewController 里。
//
// 壳里只有四样东西：文件/URL 选择、播放暂停、进度与
// 倍速控件、漂移读数。driftLabel 是这四样里最重要的一个——见它下面的
// 注释。
import SYPlayerKit
import UIKit

final class PlayerView: UIView {
    // 真上屏：SYPlayerView 的 layerClass 是 SYPlayerLayer（CAMetalLayer 子类），
    // MetalRenderer 的 blit pass 直接画进它的 drawable（GPU 到 GPU，零 CPU 拷贝）。
    //
    // 此前这里有一个自建的 MetalVideoView，自己配 device/pixelFormat、自己在
    // layoutSubviews 里算 drawableSize —— 那两条约束现在都在框架里
    // （SYPlayerLayer），壳不再需要知道它们的存在。接入方摆一个 View 就够了。
    let videoView: SYPlayerView = {
        let v = SYPlayerView()
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    let decodeModeControl: UISegmentedControl = {
        let v = UISegmentedControl(items: ["软解", "硬解"])
        v.selectedSegmentIndex = 0
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    // 漂移读数：最近呈现帧 pts − 播放位置，外加当前主时钟种类。
    // 这是这个壳存在的主要理由，见 SYPlayerState.drift 的注释——单调字体
    // 是为了让数字对齐、肉眼一眼就能看出"稳不稳、贴不贴 0"，不是审美选择。
    let driftLabel: UILabel = {
        let v = UILabel()
        v.font = UIFont.monospacedDigitSystemFont(ofSize: 15, weight: .medium)
        v.textAlignment = .center
        v.numberOfLines = 1
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    let statsLabel: UILabel = {
        let v = UILabel()
        v.font = UIFont.monospacedDigitSystemFont(ofSize: 12, weight: .regular)
        v.textColor = .secondaryLabel
        v.textAlignment = .center
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    let positionLabel: UILabel = {
        let v = UILabel()
        v.font = UIFont.monospacedDigitSystemFont(ofSize: 13, weight: .regular)
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    let progressSlider: UISlider = {
        let v = UISlider()
        v.minimumValue = 0
        v.maximumValue = 1
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    let playPauseButton: UIButton = {
        let v = UIButton(type: .system)
        v.setTitle("Play", for: .normal)
        v.titleLabel?.font = .boldSystemFont(ofSize: 17)
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    let speedControl: UISegmentedControl = {
        let v = UISegmentedControl(items: ["0.5x", "1.0x", "1.5x", "2.0x"])
        v.selectedSegmentIndex = 1
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    // 音量 / 静音 / 填充方式三个控件。放同一行是因为它们都是"画面区
    // 的展示参数"，与倍速那类"播放参数"分开摆。
    let volumeSlider: UISlider = {
        let v = UISlider()
        v.minimumValue = 0
        v.maximumValue = 1
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    let muteButton: UIButton = {
        let v = UIButton(type: .system)
        v.setTitle("静音", for: .normal)
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    /// 三态：Fit / Fill / Resize，索引与 SYPlayerVideoGravity 的
    /// [.aspectFit, .aspectFill, .resize] 顺序对齐——PlayerViewController
    /// 与自检都按这个顺序取值，改这里的 items 顺序要同步改那两处。
    let gravityControl: UISegmentedControl = {
        let v = UISegmentedControl(items: ["Fit", "Fill", "Resize"])
        v.selectedSegmentIndex = 0
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    let openFileButton: UIButton = {
        let v = UIButton(type: .system)
        v.setTitle("打开本地文件…", for: .normal)
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    let openSampleButton: UIButton = {
        let v = UIButton(type: .system)
        v.setTitle("播放内置样片", for: .normal)
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    let urlField: UITextField = {
        let v = UITextField()
        v.placeholder = "https://…"
        v.borderStyle = .roundedRect
        v.autocapitalizationType = .none
        v.autocorrectionType = .no
        v.keyboardType = .URL
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    let openURLButton: UIButton = {
        let v = UIButton(type: .system)
        v.setTitle("播放 URL", for: .normal)
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    let statusLabel: UILabel = {
        let v = UILabel()
        v.font = .systemFont(ofSize: 12)
        v.textColor = .secondaryLabel
        v.numberOfLines = 2
        v.translatesAutoresizingMaskIntoConstraints = false
        return v
    }()

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .systemBackground
        setUp()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setUp()
    }

    private func setUp() {
        // urlField 和 openURLButton 的水平抗压缩优先级默认都是 750，.fill
        // 分布在平手时按 arrangedSubviews 顺序解歧义 —— 排在前面的
        // urlField 赢，按钮被压成零宽（UITextField 的固有宽度随文本增长，
        // URL 越长挤得越死，长 m3u8 地址下按钮完全看不见）。
        // 显式定优先级：文本框可压可拉，按钮始终保住自己的固有宽度。
        urlField.setContentHuggingPriority(.defaultLow, for: .horizontal)
        urlField.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        openURLButton.setContentHuggingPriority(.required, for: .horizontal)
        openURLButton.setContentCompressionResistancePriority(.required, for: .horizontal)

        let urlRow = UIStackView(arrangedSubviews: [urlField, openURLButton])
        urlRow.axis = .horizontal
        urlRow.alignment = .center
        urlRow.spacing = 8
        urlRow.translatesAutoresizingMaskIntoConstraints = false

        let openRow = UIStackView(arrangedSubviews: [openFileButton, openSampleButton])
        openRow.axis = .horizontal
        openRow.distribution = .fillEqually
        openRow.spacing = 8
        openRow.translatesAutoresizingMaskIntoConstraints = false

        let transportRow = UIStackView(arrangedSubviews: [playPauseButton, speedControl])
        transportRow.axis = .horizontal
        transportRow.spacing = 12
        transportRow.translatesAutoresizingMaskIntoConstraints = false

        let mediaControlRow = UIStackView(arrangedSubviews: [volumeSlider, muteButton, gravityControl])
        mediaControlRow.axis = .horizontal
        mediaControlRow.alignment = .center
        mediaControlRow.spacing = 8
        mediaControlRow.translatesAutoresizingMaskIntoConstraints = false

        let stack = UIStackView(arrangedSubviews: [
            videoView, driftLabel, statsLabel, positionLabel, progressSlider,
            transportRow, mediaControlRow, openRow, decodeModeControl, urlRow, statusLabel,
        ])
        stack.axis = .vertical
        stack.spacing = 8
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)

        // 16:9 占位——`videoSize` 到位之前（尚未打开媒体，或没有视频轨）用它。
        // 保存引用是因为 `NSLayoutConstraint.multiplier` 只读，`updateAspectRatio`
        // 改比例时要靠这个引用先停用旧的、再换新的。
        let placeholderAspect = videoView.heightAnchor.constraint(
            equalTo: videoView.widthAnchor, multiplier: 9.0 / 16.0)
        aspectRatioConstraint = placeholderAspect

        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: safeAreaLayoutGuide.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: safeAreaLayoutGuide.trailingAnchor, constant: -16),
            stack.topAnchor.constraint(equalTo: safeAreaLayoutGuide.topAnchor, constant: 12),
            stack.bottomAnchor.constraint(lessThanOrEqualTo: safeAreaLayoutGuide.bottomAnchor, constant: -12),
            placeholderAspect,
        ])
    }

    // MARK: - 画面区宽高比

    /// 画面区宽高比约束——当前生效的那一条。`setUp()` 里建好 16:9 占位后填入，
    /// 此后只由 `updateAspectRatio(for:)` 替换，外部（自检）只读它的
    /// `multiplier` 核对，不应该自己改它的 `isActive`。
    private(set) var aspectRatioConstraint: NSLayoutConstraint!

    /// 按 `videoSize` 重建画面区宽高比约束；`videoSize == nil`（未打开媒体、
    /// 或没有视频轨）时保持 16:9 占位。
    ///
    /// `NSLayoutConstraint.multiplier` 是只读属性——改比例只能整条替换：
    /// 先停用旧约束，再建一条新的并激活，最后更新保存的引用。
    func updateAspectRatio(for videoSize: CGSize?) {
        let multiplier: CGFloat
        if let size = videoSize, size.width > 0, size.height > 0 {
            multiplier = size.height / size.width
        } else {
            multiplier = 9.0 / 16.0
        }
        // 没变就不换：state 在播放中会因 position 等字段高频更新，
        // 每次都重建约束是纯浪费，还会在自检轮询窗口里制造无意义的中间态。
        if let current = aspectRatioConstraint, abs(current.multiplier - multiplier) < 1e-9 {
            return
        }
        aspectRatioConstraint?.isActive = false
        let next = videoView.heightAnchor.constraint(equalTo: videoView.widthAnchor, multiplier: multiplier)
        next.isActive = true
        aspectRatioConstraint = next
    }
}
