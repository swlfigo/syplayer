// PlayerScreen.swift — 示例 App 的播放页，iOS 与原生 macOS 共用。
//
// 只用 SYPlayerKit 的公开 API（经 SwiftPM 接入，`import SYPlayerKit`）：
// `SYPlayer` 是 ObservableObject，状态直接从 `player.state` 读；画面由
// `SYPlayerViewRepresentable` 显示（iOS 上是 UIViewRepresentable，原生 macOS
// 上是 NSViewRepresentable，接入方写法一致）。
import SwiftUI
import SYPlayerKit

/// App bundle 里的内置样片（12 秒，640×360 H.264 + AAC）。
@MainActor
enum ExampleSample {
    static var url: URL? {
        Bundle.main.url(forResource: "sample", withExtension: "mp4")
    }

    /// 本平台支持硬解就用硬解，否则软解。模拟器与部分平台没有 H.264 硬解，
    /// 直接要 `.hardwarePreferred` 会被 open 拒绝。
    static var decoding: SYPlayerDecoding {
        SYPlayer.isHardwareDecodeAvailable ? .hardwarePreferred : .software
    }
}

/// "分:秒" 形式的时间文本；未知时长显示 "--:--"。
func exampleTimeText(_ seconds: TimeInterval?) -> String {
    guard let s = seconds, s.isFinite, s >= 0 else { return "--:--" }
    let total = Int(s.rounded(.down))
    return String(format: "%02d:%02d", total / 60, total % 60)
}

struct PlayerScreen: View {
    @ObservedObject var player: SYPlayer
    @State private var errorText: String?
    @State private var didOpen = false

    var body: some View {
        VStack(spacing: 12) {
            SYPlayerViewRepresentable(player: player)
                .aspectRatio(aspectRatio, contentMode: .fit)
                .frame(minWidth: 160, minHeight: 90)
                .background(Color.black)

            Text("\(exampleTimeText(player.state.position)) / \(exampleTimeText(player.state.duration))")
                .font(.system(.body, design: .monospaced))

            Text(sizeText)
                .font(.system(.footnote, design: .monospaced))

            Button(player.state.isPlaying ? "暂停" : "播放", action: togglePlayback)
                .disabled(!player.state.hasMedia)

            if let errorText = errorText {
                Text(errorText).foregroundColor(.red)
            }
        }
        .padding()
        .onAppear(perform: openSampleOnce)
    }

    private var aspectRatio: CGFloat? {
        guard let size = player.state.videoSize, size.height > 0 else { return 16.0 / 9.0 }
        return size.width / size.height
    }

    private var sizeText: String {
        guard let size = player.state.videoSize else { return "视频尺寸：未知" }
        return "视频尺寸：\(Int(size.width))×\(Int(size.height))"
    }

    private func openSampleOnce() {
        guard !didOpen else { return }
        didOpen = true
        guard let url = ExampleSample.url else {
            errorText = "App bundle 里没有 sample.mp4"
            return
        }
        let player = self.player
        Task { @MainActor in
            do {
                try await player.open(.file(url), decoding: ExampleSample.decoding)
            } catch {
                errorText = "打开失败：\(error)"
            }
        }
    }

    private func togglePlayback() {
        if player.state.isPlaying {
            player.pause()
        } else {
            // 播完之后再按播放：回到开头重播。
            if player.state.isEnded { player.seek(to: 0) }
            player.play()
        }
    }
}
