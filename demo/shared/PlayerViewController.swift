// PlayerViewController.swift — UIKit 接入页。这里**一行 C 类型都没有**：
// 没有 ObjC 桥、没有 C 整数出参、没有微秒整数、没有自己起的 Timer、
// 没有 DispatchQueue.main.async 往回跳。数据来源是 SYPlayer，状态经
// SYPlayerDelegate 在主线程送上门。
//
// 布局与控件保持不变（非目标是"只把数据来源换成 SYPlayer"），
// 改的只有数据流。
//
// 漂移读数仍然是这个壳存在的主要理由：state.drift 就是
// 最近呈现帧 pts − 播放位置。正常播放时它不会贴死在 0，会在一个带宽内游走，
// 两个原因叠加：
//   1. 采样相位——两次状态刷新之间视频没有再呈现新帧，position 持续推进而
//      presentedTime 是阶梯状的；
//   2. 呈现窗口本身允许提前——track_player.h 的判定是
//      diff ∈ (-80ms, +40ms]，一帧只要没早到超过 40ms 就会被呈现。
// 正常信号是"这个带宽跨长时间播放不整体偏移、不单调增长"；整条曲线明显偏出
// ±40ms 附近还稳定不回落，才是设备延迟常数算错的信号。
import SYPlayerKit
import UIKit

final class PlayerViewController: UIViewController, UIDocumentPickerDelegate, SYPlayerDelegate {
    private let playerView = PlayerView()
    private let player = SYPlayer()
    private var isScrubbing = false
    private var lastKnownDuration: TimeInterval = 0

    override func loadView() {
        view = playerView
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "UIKit"
        wireActions()
        player.delegate = self
        player.attach(to: playerView.videoView)
        updateStatus("尚未打开媒体。选择本地文件 / 内置样片 / URL 均可。")
        if !SYPlayer.isHardwareDecodeAvailable {
            playerView.decodeModeControl.setEnabled(false, forSegmentAt: 1)
            updateStatus("当前平台不支持硬解（Mac Catalyst 或设备不支持），仅可软解。")
        }
        // 音量/静音/gravity 三个控件不是从 SYPlayerState 里渲染的（volume、
        // isMuted 跨 open 保留、独立于 state；videoGravity 只活在视图上），
        // 只在这里按当前值初始化一次，之后全靠用户操作与自检驱动。
        playerView.volumeSlider.value = Float(player.volume)
        updateMuteButtonTitle()
        playerView.gravityControl.selectedSegmentIndex =
            Self.gravities.firstIndex(of: playerView.videoView.videoGravity) ?? 0
        render(player.state)
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        // 两个页各有一个 SYPlayer；切走时暂停，免得两页同时出声。
        player.pause()
    }

    // MARK: - Wiring

    private func wireActions() {
        playerView.playPauseButton.addTarget(self, action: #selector(togglePlayPause), for: .touchUpInside)
        playerView.speedControl.addTarget(self, action: #selector(speedChanged), for: .valueChanged)
        playerView.openFileButton.addTarget(self, action: #selector(presentDocumentPicker), for: .touchUpInside)
        playerView.openSampleButton.addTarget(self, action: #selector(openBundledSample), for: .touchUpInside)
        playerView.openURLButton.addTarget(self, action: #selector(openTypedURL), for: .touchUpInside)

        playerView.progressSlider.addTarget(self, action: #selector(scrubBegan), for: .touchDown)
        playerView.progressSlider.addTarget(self, action: #selector(scrubEnded),
                                            for: [.touchUpInside, .touchUpOutside, .touchCancel])

        playerView.volumeSlider.addTarget(self, action: #selector(volumeChanged), for: .valueChanged)
        playerView.muteButton.addTarget(self, action: #selector(toggleMute), for: .touchUpInside)
        playerView.gravityControl.addTarget(self, action: #selector(gravityChanged), for: .valueChanged)
    }

    // MARK: - SYPlayerDelegate（全部在主线程被调用，框架保证）

    func player(_ player: SYPlayer, didChange state: SYPlayerState) {
        render(state)
    }

    func playerDidReachEnd(_ player: SYPlayer) {
        updateStatus("播放到达结尾（Eof）。可以拖动进度条回退重放。")
    }

    func player(_ player: SYPlayer, didFailWith error: SYPlayerError) {
        updateStatus("播放中出错：\(error.localizedDescription)")
    }

    // MARK: - Opening media

    private var selectedDecoding: SYPlayerDecoding {
        playerView.decodeModeControl.selectedSegmentIndex == 1 ? .hardwarePreferred : .software
    }

    private func open(_ source: SYPlayerSource, label: String) {
        let decoding = selectedDecoding
        updateStatus("正在打开 \(label) …")
        Task { [weak self] in
            guard let self = self else { return }
            do {
                try await self.player.open(source, decoding: decoding)
                self.lastKnownDuration = 0
                // open() 之后播放器是暂停的（SYPlayer 的契约，与 AVPlayer 一致），
                // 起播由用户点 Play——这个页面刻意不自动播放。
                self.updateStatus("已打开，点 Play 开始播放。")
            } catch let error as SYPlayerError {
                self.updateStatus(self.failureMessage(error, decoding: decoding))
            } catch {
                self.updateStatus("打开失败：\(error.localizedDescription)")
            }
        }
    }

    /// `.notImplemented` 在硬解与软解下是两类不同的"不支持"：硬解模式下几乎总是
    /// "这个平台/编码/位深硬解不了"；软解模式下走到它的路径完全不同（比如渲染器
    /// 只认 yuv420p，遇到别的 pix_fmt 一样报这个），跟"硬解不支持"没有关系，
    /// 继续说"硬解不支持"是在编造一个没发生过的原因。其余错误直接用框架文案。
    private func failureMessage(_ error: SYPlayerError, decoding: SYPlayerDecoding) -> String {
        if error == .notImplemented && decoding == .hardwarePreferred {
            return "所选解码方式不支持这个文件（硬解不支持该编码/位深），可改用软解。"
        }
        return "打开失败：\(error.localizedDescription)"
    }

    @objc private func presentDocumentPicker() {
        let picker = UIDocumentPickerViewController(
            documentTypes: ["public.movie", "public.mpeg-4", "public.avi", "public.audiovisual-content"],
            in: .import)
        picker.delegate = self
        present(picker, animated: true)
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        guard let url = urls.first else { return }
        let didStartScope = url.startAccessingSecurityScopedResource()
        defer { if didStartScope { url.stopAccessingSecurityScopedResource() } }

        // 复制到 app 的 tmp 目录再打开：避免要管理"整段播放期间都持有
        // security-scoped 访问权限"这件事——document picker 给的权限严格说
        // 只在这次回调期间保证有效。
        let dest = FileManager.default.temporaryDirectory.appendingPathComponent(url.lastPathComponent)
        try? FileManager.default.removeItem(at: dest)
        do {
            try FileManager.default.copyItem(at: url, to: dest)
            open(.file(dest), label: dest.lastPathComponent)
        } catch {
            updateStatus("拷贝所选文件失败：\(error.localizedDescription)")
        }
    }

    @objc private func openBundledSample() {
        guard let url = Bundle.main.url(forResource: "sample", withExtension: "mp4") else {
            updateStatus("bundle 里找不到内置样片 sample.mp4。")
            return
        }
        open(.file(url), label: "sample.mp4")
    }

    @objc private func openTypedURL() {
        guard let text = playerView.urlField.text, !text.isEmpty,
              let url = URL(string: text) else {
            updateStatus("URL 解析失败，请检查输入。")
            return
        }
        open(.url(url), label: text)
    }

    // MARK: - Transport

    /// 按钮读的是 **`playback == .paused`（用户显式按过暂停）**，不是
    /// `isPlaying`（严格等于 `.playing`）。这两者在一处真实场景里会分叉：
    /// 状态映射里**缓冲的优先级高于暂停**（SYPlayerSnapshotMapping），所以播放
    /// 途中一次普通的网络卡顿就会让 `isPlaying` 变 false——按钮翻成 "Play"，
    /// 用户这时想暂停、点下去调到的却是 `play()`（空操作），暂停意图在整段卡顿
    /// 期间被静默吃掉。
    private var isExplicitlyPaused: Bool { player.state.playback == .paused }

    @objc private func togglePlayPause() {
        if isExplicitlyPaused {
            player.play()
        } else {
            player.pause()
        }
    }

    @objc private func speedChanged() {
        let rates: [Double] = [0.5, 1.0, 1.5, 2.0]
        let idx = playerView.speedControl.selectedSegmentIndex
        guard idx >= 0, idx < rates.count else { return }
        do {
            try player.setRate(rates[idx])
        } catch {
            updateStatus("倍速设置被拒绝（应在 [0.5, 2.0] 内）。")
        }
    }

    @objc private func scrubBegan() {
        isScrubbing = true
    }

    @objc private func scrubEnded() {
        defer { isScrubbing = false }
        guard lastKnownDuration > 0 else { return }
        player.seek(to: Double(playerView.progressSlider.value) * lastKnownDuration)
    }

    // MARK: - 画面区参数（音量 / 静音 / 填充方式）

    /// gravity 分段控件的索引与这个数组的顺序对齐（也与 PlayerView.gravityControl
    /// 的 items ["Fit", "Fill", "Resize"] 对齐）。改任何一处顺序都要同步改另外两处。
    private static let gravities: [SYPlayerVideoGravity] = [.aspectFit, .aspectFill, .resize]

    @objc private func volumeChanged() {
        player.volume = Double(playerView.volumeSlider.value)
    }

    /// 静音与音量相互独立（SYPlayer.isMuted 的契约）：切换静音不擦 volume 的值，
    /// 所以这里只翻 isMuted，不动 volumeSlider。
    @objc private func toggleMute() {
        player.isMuted.toggle()
        updateMuteButtonTitle()
    }

    private func updateMuteButtonTitle() {
        playerView.muteButton.setTitle(player.isMuted ? "取消静音" : "静音", for: .normal)
    }

    @objc private func gravityChanged() {
        let idx = playerView.gravityControl.selectedSegmentIndex
        guard idx >= 0, idx < Self.gravities.count else { return }
        playerView.videoView.videoGravity = Self.gravities[idx]
    }

    // MARK: - 渲染

    private func render(_ state: SYPlayerState) {
        lastKnownDuration = state.duration ?? 0
        // 画面区宽高比按真实 videoSize 重建；无视频轨/未打开时该方法自己回落
        // 到 16:9 占位（PlayerView.updateAspectRatio 的契约）。
        playerView.updateAspectRatio(for: state.videoSize)

        // hasMedia 为 false 时其余字段没有意义（SYPlayerState 顶部的契约）：
        // 显示 "—"，不编造状态。
        playerView.playPauseButton.isEnabled = state.hasMedia
        // 标题与 togglePlayPause 用同一个判据（见 isExplicitlyPaused 的注释）：
        // 缓冲期间照样显示 "Pause"，点下去也确实是暂停。
        playerView.playPauseButton.setTitle(
            state.hasMedia ? (state.playback == .paused ? "Play" : "Pause") : "—", for: .normal)

        if let duration = state.duration, duration > 0 {
            playerView.progressSlider.isEnabled = true
            if !isScrubbing {
                playerView.progressSlider.value = Float(state.position / duration)
            }
        } else {
            playerView.progressSlider.isEnabled = false
        }

        playerView.positionLabel.text =
            "\(Self.format(state.position)) / \(Self.format(state.duration ?? 0))"

        let clockName = state.clock == .audio ? "Audio" : "System"
        if !state.hasMedia {
            playerView.driftLabel.text = "尚未打开媒体   时钟: —"
            playerView.driftLabel.textColor = .secondaryLabel
        } else if let drift = state.drift {
            let driftMs = drift * 1000.0
            playerView.driftLabel.text = String(format: "漂移 %+.1f ms   时钟: %@", driftMs, clockName)
            let absMs = abs(driftMs)
            playerView.driftLabel.textColor =
                absMs <= 40 ? .systemGreen : (absMs <= 80 ? .systemOrange : .systemRed)
        } else if state.hasVideo {
            playerView.driftLabel.text = "漂移: 尚无已呈现帧   时钟: \(clockName)"
            playerView.driftLabel.textColor = .secondaryLabel
        } else {
            playerView.driftLabel.text = "无视频轨   时钟: \(clockName)"
            playerView.driftLabel.textColor = .secondaryLabel
        }

        playerView.statsLabel.text = Self.statsLine(state)
    }

    private func updateStatus(_ text: String) {
        playerView.statusLabel.text = text
    }

    // MARK: - Helpers

    static func format(_ seconds: TimeInterval) -> String {
        guard seconds.isFinite, seconds > 0 else { return "00:00" }
        let total = Int(seconds)
        return String(format: "%02d:%02d", total / 60, total % 60)
    }

    /// 统计行。两个页共用（SwiftUI 页直接调），免得两份文案漂移。
    static func statsLine(_ state: SYPlayerState) -> String {
        guard state.hasMedia else { return "倍速 —   —" }
        let stats = state.statistics
        let rate = String(format: "%.2f", state.rate) + "x"
        let decode = state.isHardwareDecoding ? "硬解" : "软解"
        // renderBusyRetries 是 BUSY 重试次数（帧不丢、下一轮重试），不是丢帧数。
        var line = "倍速 \(rate)   \(decode)   丢帧 \(stats.droppedFrames)"
            + "   呈现失败 \(stats.presentFailures)   忙重试 \(stats.renderBusyRetries)"
        if stats.isCatchingUp { line += "  追帧中" }
        switch state.clockSwitchReason {
        case .audioFailed: line += "  (音频失败→系统时钟)"
        case .audioEnded:  line += "  (音频播完→系统时钟)"
        case .none:        break
        }
        let bufferedText = state.buffered.map { String(format: "%.1fs", $0) } ?? "∞"
        if case .buffering(let reason) = state.playback {
            let name: String
            switch reason {
            case .startup: name = "起播"
            case .seek:    name = "seek"
            case .stall:   name = "卡顿"
            }
            line += "  缓冲中…(\(name)) 已缓冲 \(bufferedText)  卡顿 \(stats.rebufferCount)"
        } else {
            line += "  已缓冲 \(bufferedText)  卡顿 \(stats.rebufferCount)"
        }
        if let startup = stats.startupDuration {
            line += String(format: "  起播 %.0fms", startup * 1000)
        }
        line += "  欠载 \(stats.audioUnderruns)"
        // videoSize 含 SAR 与旋转折算后的显示尺寸；无视频轨/未打开为 nil。
        if let size = state.videoSize {
            line += String(format: "  画面 %.0fx%.0f", size.width, size.height)
        } else {
            line += "  画面 —"
        }
        return line
    }

    // MARK: - 无人值守自检（仅 DEBUG）

    #if DEBUG
    /// 播放页此前没有任何自检入口——预加载页有（AppDelegate.swift:46-70，
    /// `SYPLAYER_DEMO_PRELOAD_AUTORUN=1`），照它的形状新建一个：控件用
    /// `sendActions(for:)` 驱动（不直接调 @objc 方法：
    /// 直接调 handler 测不出没接线的控件）、判据用轮询谓词而不是固定睡眠、
    /// 失败即打印是哪一步、结果经 `completion` 回调交给 `AppDelegate` 转成
    /// 退出码。
    ///
    ///   SYPLAYER_DEMO_PLAYER_AUTORUN=1 <path>/syplayer-mac.app/Contents/MacOS/syplayer-mac
    ///
    /// 覆盖：（毒值预置）→ 打开内置样片 → videoSize 到位（轮询，上限 10 秒）→
    /// 画面区宽高比约束已按 videoSize 重建（且是经 `render(_:)` 回调重建的，
    /// 不是自检自己调的）→ 音量滑块接线 → 静音按钮接线（且不擦音量值）
    /// → gravity 分段控件接线。**不覆盖旋转**：sample.mp4 大概率没有旋转
    /// 元数据，旋转校验是真机人工清单的事，不为此造素材。
    func startAutorunProbe(completion: @escaping (_ ok: Bool) -> Void) {
        Task { [weak self] in
            guard let self = self else {
                completion(false)
                return
            }
            var ok = true
            func fail(_ step: String) {
                ok = false
                print("[player-autorun] ❌ 失败于：\(step)")
            }

            // 【回调接线判据】上一版只在打开样片之后读一次
            // `aspectRatioConstraint.multiplier`，测不出 `render(_:)` 里那行
            // `playerView.updateAspectRatio(for: state.videoSize)`
            // （PlayerViewController.swift）到底有没有被状态回调真的调用——
            // sample.mp4 恰好 16:9，跟 `setUp()` 建的 16:9 占位约束逐位相同，
            // 把那一行整个删掉，旧判据照样能读到 0.5625，实测确认过是假绿。
            // 打开样片**之前**先用一个毒值把约束打到一个绝不会跟真实结果
            // 撞车的数（999/999=1.0，≠ 0.5625），并确认毒值真的生效；随后
            // 走正常流程，如果 `render(_:)` 没调 `updateAspectRatio`，约束会
            // 停在毒值 1.0 上，就会跟下面"应等于 videoSize 算出的比例"这条
            // 判据对不上。
            let poisonSize = CGSize(width: 999, height: 999)
            self.playerView.updateAspectRatio(for: poisonSize)
            let poisonActual = self.playerView.aspectRatioConstraint.multiplier
            if abs(poisonActual - 1.0) >= 1e-3 {
                fail("毒值预置失败：打了 999x999 之后 multiplier = \(poisonActual)，期望 1.0——"
                     + "updateAspectRatio 本身没生效，后面的判据无法成立")
            } else {
                print("[player-autorun] 毒值预置生效：multiplier=\(poisonActual)（≠ 0.5625，"
                      + "接下来只有 render(_:) 真的调了 updateAspectRatio 才会变回真实比例）")
            }

            print("[player-autorun] 开始。点「播放内置样片」（经 openSampleButton.sendActions）")
            self.playerView.openSampleButton.sendActions(for: .touchUpInside)

            let gotSize = await self.waitUntil(timeout: 10.0) {
                self.player.state.videoSize != nil
            }
            guard gotSize, let videoSize = self.player.state.videoSize else {
                fail("等待 player.state.videoSize 到位（10 秒超时）——"
                     + "可能是 bundle 里缺 sample.mp4，或 openSampleButton 的 addTarget 断了")
                completion(false)
                return
            }
            print("[player-autorun] videoSize = \(videoSize)")

            // 画面区宽高比约束应已按 videoSize 重建——这一次不是自检自己调的，
            // 是 render(_:) 收到状态变化之后自己调的（毒值预置的用意见上）。
            let expected = videoSize.height / videoSize.width
            let actual = self.playerView.aspectRatioConstraint.multiplier
            if abs(actual - expected) >= 1e-3 {
                if abs(actual - poisonActual) < 1e-9 {
                    fail("宽高比约束还停在毒值 \(actual) 上——render(_:) 没有调用 "
                         + "updateAspectRatio(for:)，回调接线断了？")
                } else {
                    fail("宽高比约束未按 videoSize 重建：期望 multiplier≈\(expected)，实际 \(actual)")
                }
            } else {
                print("[player-autorun] 宽高比约束已重建（经 render(_:) 回调）：multiplier=\(actual)")
            }
            // 【方法本体】sample.mp4 实测是 640x360、SAR 1:1，
            // 恰好等于 16:9（360/640 = 0.5625 = 9/16，逐位相同）。上面那条
            // 判据现在能抓住"回调没接线"（靠毒值），但还是抓不住
            // "updateAspectRatio 方法本体写死 16:9、忽略传入的 videoSize"——
            // 因为传真实 videoSize 进去算出来的数，跟写死 16:9 的数逐位相同。
            // 额外用一个非 16:9 的合成尺寸直接调 `updateAspectRatio`
            // （不是 UIControl，不需要 sendActions）校验方法本体：换个真会让
            // multiplier 不同的输入，两种实现才分得开。跑完立刻用真实
            // videoSize 换回来，不留下与 state 不一致的约束。
            let syntheticSize = CGSize(width: 400, height: 300)  // 4:3，0.75 ≠ 0.5625
            self.playerView.updateAspectRatio(for: syntheticSize)
            let syntheticExpected = syntheticSize.height / syntheticSize.width
            let syntheticActual = self.playerView.aspectRatioConstraint.multiplier
            if abs(syntheticActual - syntheticExpected) >= 1e-3 {
                fail("合成尺寸 400x300 直接调 updateAspectRatio 之后 multiplier = "
                     + "\(syntheticActual)，期望 \(syntheticExpected)（=300/400）—— "
                     + "宽高比约束没有按传入的 videoSize 重建，疑似写死了 16:9")
            } else {
                print("[player-autorun] 合成尺寸校验通过：multiplier=\(syntheticActual)")
            }
            self.playerView.updateAspectRatio(for: videoSize)

            // 音量滑块。
            self.playerView.volumeSlider.value = 0.3
            self.playerView.volumeSlider.sendActions(for: .valueChanged)
            if abs(self.player.volume - 0.3) >= 1e-6 {
                fail("volumeSlider.sendActions(.valueChanged) 之后 player.volume = "
                     + "\(self.player.volume)，期望 0.3 —— volumeSlider 的 addTarget 断了？")
            } else {
                print("[player-autorun] 音量滑块接线正常：player.volume = \(self.player.volume)")
            }

            // 静音按钮：切到静音，音量值不应被擦掉。
            self.playerView.muteButton.sendActions(for: .touchUpInside)
            if self.player.isMuted != true {
                fail("muteButton.sendActions(.touchUpInside) 之后 player.isMuted = "
                     + "\(self.player.isMuted)，期望 true —— muteButton 的 addTarget 断了？")
            } else if abs(self.player.volume - 0.3) >= 1e-6 {
                fail("静音之后 player.volume 被改动为 \(self.player.volume)，期望仍是 0.3")
            } else {
                print("[player-autorun] 静音按钮接线正常：isMuted=true，volume 仍是 0.3")
            }

            // gravity 分段控件：先核对段数、每段标题与 Self.gravities 的顺序
            // 是否真的对齐——这个对齐只靠注释维护（PlayerView.gravityControl
            // 的 items 与这里的 Self.gravities），改动任何一处顺序而忘了同步
            // 另一处，光看"接线正常"这条判据是发现不了的（值照样对得上，只是
            // 巧合），加一条运行期断言兜底。
            let expectedTitles: [SYPlayerVideoGravity: String] =
                [.aspectFit: "Fit", .aspectFill: "Fill", .resize: "Resize"]
            if self.playerView.gravityControl.numberOfSegments != Self.gravities.count {
                fail("gravityControl 段数 \(self.playerView.gravityControl.numberOfSegments) "
                     + "≠ Self.gravities.count \(Self.gravities.count) —— 两处顺序没同步")
            } else {
                for (idx, gravity) in Self.gravities.enumerated() {
                    let title = self.playerView.gravityControl.titleForSegment(at: idx)
                    if title != expectedTitles[gravity] {
                        fail("gravityControl 第 \(idx) 段标题=\(title ?? "nil")，"
                             + "与 Self.gravities[\(idx)]=\(gravity) 期望的 "
                             + "\(expectedTitles[gravity] ?? "?") 不一致 —— 顺序没同步")
                    }
                }
            }

            // gravity 分段控件：切到 Fill（索引与 Self.gravities 对齐 = 1 = .aspectFill）。
            let aspectFillIndex = Self.gravities.firstIndex(of: .aspectFill) ?? 1
            self.playerView.gravityControl.selectedSegmentIndex = aspectFillIndex
            self.playerView.gravityControl.sendActions(for: .valueChanged)
            if self.playerView.videoView.videoGravity != .aspectFill {
                fail("gravityControl 切到 Fill 之后 videoView.videoGravity = "
                     + "\(self.playerView.videoView.videoGravity)，期望 .aspectFill —— "
                     + "gravityControl 的 addTarget 断了？")
            } else {
                print("[player-autorun] gravity 分段控件接线正常：videoGravity = .aspectFill")
            }

            completion(ok)
        }
    }

    /// 轮询一个谓词直到成立或超时。不睡固定时长——`open()` 是异步的，
    /// 睡固定时长要么慢要么脆（与 PreloadViewController.waitUntil 同一理由）。
    private func waitUntil(timeout: TimeInterval, _ predicate: () -> Bool) async -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if predicate() { return true }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        return predicate()
    }
    #endif
}
