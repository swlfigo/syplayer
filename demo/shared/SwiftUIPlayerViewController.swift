// SwiftUIPlayerViewController.swift — SwiftUI 接入页。存在的唯一目的是证明
// "SwiftUI 与 UIKit 两种接入都成立"，用的是同一个 SYPlayer、
// 同一个 SYPlayerLayer，没有第二套实现。
//
// 播放器由宿主控制器持有、用 @ObservedObject 传进视图，而不是 @StateObject：
// @StateObject 是 iOS 14 的 API，本仓库的部署目标是 iOS 13。
import SwiftUI
import SYPlayerKit
import UIKit

final class SwiftUIPlayerViewController: UIHostingController<SwiftUIPlayerScreen> {
    private let player: SYPlayer

    init() {
        // 先建播放器、赋给存储属性，再 super.init——Swift 不允许在 super.init
        // 之前读 self，所以这里必须经一个局部变量中转，两处用的是同一个对象。
        let player = SYPlayer()
        self.player = player
        super.init(rootView: SwiftUIPlayerScreen(player: player))
        title = "SwiftUI"
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) 不支持——这个页面只从代码构造")
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        player.pause()
    }
}

/// `@MainActor`：body 之外的计算属性与方法都要读 `player.state`（SYPlayer 全员
/// 主 actor 隔离），不标的话它们是非隔离上下文，编译期就过不去。SwiftUI 的
/// `body` 本身已经是 @MainActor，这里只是把同一条隔离铺满整个类型。
@MainActor
struct SwiftUIPlayerScreen: View {
    @ObservedObject var player: SYPlayer
    @State private var urlText = ""
    @State private var status = "尚未打开媒体。点「播放内置样片」或输入 URL。"

    private let rates: [Double] = [0.5, 1.0, 1.5, 2.0]

    var body: some View {
        VStack(spacing: 10) {
            // videoSize 到位前（未打开媒体、或没有视频轨）用 16:9 占位，与
            // UIKit 页 PlayerView.updateAspectRatio 同一口径。SwiftUI 这边
            // 没法用 sendActions 驱动自检（没有 UIControl），所以只保证编译
            // 通过与手工可用，不建自检入口。
            SYPlayerViewRepresentable(player: player)
                .aspectRatio(player.state.videoSize.map { $0.width / $0.height } ?? (16.0 / 9.0),
                             contentMode: .fit)

            Text(driftLine)
                .font(.system(size: 15, weight: .medium, design: .monospaced))
                .foregroundColor(driftColor)

            Text(PlayerViewController.statsLine(player.state))
                .font(.system(size: 11, design: .monospaced))
                .foregroundColor(.secondary)
                .lineLimit(2)

            HStack {
                Text(PlayerViewController.format(player.state.position))
                Slider(value: progressBinding, in: 0...1)
                    .disabled((player.state.duration ?? 0) <= 0)
                Text(PlayerViewController.format(player.state.duration ?? 0))
            }
            .font(.system(size: 13, design: .monospaced))

            HStack(spacing: 16) {
                // 与 UIKit 页同一个判据：读 `playback == .paused`（显式暂停），
                // 不读 `isPlaying`。缓冲的优先级高于暂停，用 isPlaying 的话
                // 一次网络卡顿就会把按钮翻成 "Play"，此时点它调到的是空操作的
                // play()，用户的暂停意图被静默丢掉（见 PlayerViewController 的
                // isExplicitlyPaused 注释）。
                Button(isExplicitlyPaused ? "Play" : "Pause") {
                    if isExplicitlyPaused {
                        player.play()
                    } else {
                        player.pause()
                    }
                }
                .disabled(!player.state.hasMedia)
                .font(.headline)

                Picker("倍速", selection: rateBinding) {
                    ForEach(0..<rates.count, id: \.self) { i in
                        Text(String(format: "%.1fx", rates[i])).tag(i)
                    }
                }
                .pickerStyle(SegmentedPickerStyle())
            }

            Button("播放内置样片") { openSample() }

            HStack(spacing: 8) {
                TextField("https://…", text: $urlText)
                    .textFieldStyle(RoundedBorderTextFieldStyle())
                    .autocapitalization(.none)
                    .disableAutocorrection(true)
                Button("播放 URL") { openTypedURL() }
            }

            Text(status)
                .font(.caption)
                .foregroundColor(.secondary)
                .lineLimit(2)

            Spacer(minLength: 0)
        }
        .padding(16)
        .onDisappear { player.pause() }
    }

    // MARK: - 绑定

    /// 进度条：读的是 position/duration，写的是 seek。SwiftUI 的 Slider 没有
    /// "拖动结束"回调，所以这里是边拖边 seek——对 demo 足够，也顺带演示了
    /// seek 在框架里是幂等、可高频调用的。
    private var progressBinding: Binding<Double> {
        Binding(
            get: {
                guard let duration = player.state.duration, duration > 0 else { return 0 }
                return min(1, max(0, player.state.position / duration))
            },
            set: { fraction in
                guard let duration = player.state.duration, duration > 0 else { return }
                player.seek(to: fraction * duration)
            })
    }

    private var rateBinding: Binding<Int> {
        Binding(
            get: { rates.firstIndex(where: { abs($0 - player.state.rate) < 0.01 }) ?? 1 },
            set: { index in
                do { try player.setRate(rates[index]) }
                catch { status = "倍速设置被拒绝（应在 [0.5, 2.0] 内）。" }
            })
    }

    // MARK: - 显示

    private var isExplicitlyPaused: Bool { player.state.playback == .paused }

    private var driftLine: String {
        let clockName = player.state.clock == .audio ? "Audio" : "System"
        guard player.state.hasMedia else { return "尚未打开媒体   时钟: —" }
        guard let drift = player.state.drift else {
            return player.state.hasVideo ? "漂移: 尚无已呈现帧   时钟: \(clockName)"
                                         : "无视频轨   时钟: \(clockName)"
        }
        return String(format: "漂移 %+.1f ms   时钟: %@", drift * 1000, clockName)
    }

    private var driftColor: Color {
        guard let drift = player.state.drift else { return .secondary }
        let absMs = abs(drift * 1000)
        return absMs <= 40 ? .green : (absMs <= 80 ? .orange : .red)
    }

    // MARK: - 打开

    private func openSample() {
        guard let url = Bundle.main.url(forResource: "sample", withExtension: "mp4") else {
            status = "bundle 里找不到内置样片 sample.mp4。"
            return
        }
        open(.file(url), label: "sample.mp4")
    }

    private func openTypedURL() {
        guard !urlText.isEmpty, let url = URL(string: urlText) else {
            status = "URL 解析失败，请检查输入。"
            return
        }
        open(.url(url), label: urlText)
    }

    private func open(_ source: SYPlayerSource, label: String) {
        status = "正在打开 \(label) …"
        // 硬解在 Catalyst 上不可用，SwiftUI 页不给开关，按平台能力自动选。
        let decoding: SYPlayerDecoding = SYPlayer.isHardwareDecodeAvailable ? .hardwarePreferred
                                                                           : .software
        Task {
            do {
                try await player.open(source, decoding: decoding)
                // open() 之后播放器是暂停的；这个页面替用户点一下 Play，
                // 与 UIKit 页刻意做成两种不同的接入风格。
                status = "已打开，开始播放。"
                player.play()
            } catch let error as SYPlayerError {
                status = "打开失败：\(error.localizedDescription)"
            } catch {
                status = "打开失败：\(error.localizedDescription)"
            }
        }
    }
}
