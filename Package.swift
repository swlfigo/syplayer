// swift-tools-version:5.9
// SYPlayerKit 的 SwiftPM 分发清单。默认从 GitHub Release 拉两个预编译
// xcframework（SYPlayerKit 本体、它依赖的 SYFFmpeg），接入方只需要
// `import SYPlayerKit`，不用手工拷 framework 或补链接设置。
//
// SYFFmpeg 是我们私有的一份 FFmpeg：framework 名、SwiftPM target 名都带 SY
// 前缀，导出符号全部是 syp_ 前缀、也不带头文件。接入方自己再带任意版本的
// FFmpeg（静态或动态、任意链接顺序）都不会撞名、不会串用。
//
// 设置环境变量 SYPLAYER_LOCAL_BINARIES=1 时改用本地
// tools/build-xcframework.sh 的产物（build-spm/out/*.xcframework），不发版
// 也能验证接入能不能跑通。
import Foundation
import PackageDescription

// version / kitChecksum / ffmpegChecksum 三行由
// `tools/build-xcframework.sh --version X.Y.Z` 在打包对应版本的 zip 后原地
// 回填，回填后 grep 校验过确实落进了文件。发布新版本前必须重新跑一遍脚本
// 拿到新 checksum；上传到 GitHub Release 的两个 zip 必须是算出这份 checksum
// 的那两个文件本身，不能事后换成"内容一样但重新打的"zip
// ——checksum 是对文件字节算的，差一个字节 SwiftPM 拉取时就会报错拒绝。
let version = "0.1.0"
let kitChecksum = "57a9006804a4a9aff5a92f95917e79ae98aedde3d118f5ec7019b8e3496e913d"
let ffmpegChecksum = "9972b95027cd6bc9c41f15b5f87b5a1c3f9c9e9da3c5967b233aefba9c01b93a"

let useLocalBinaries = ProcessInfo.processInfo.environment["SYPLAYER_LOCAL_BINARIES"] == "1"

let releaseBase = "https://github.com/swlfigo/syplayer/releases/download/\(version)"

let kitTarget: Target = useLocalBinaries
    ? .binaryTarget(name: "SYPlayerKit", path: "build-spm/out/SYPlayerKit.xcframework")
    : .binaryTarget(
        name: "SYPlayerKit",
        url: "\(releaseBase)/SYPlayerKit.xcframework.zip",
        checksum: kitChecksum
      )

let ffmpegTarget: Target = useLocalBinaries
    ? .binaryTarget(name: "SYFFmpeg", path: "build-spm/out/SYFFmpeg.xcframework")
    : .binaryTarget(
        name: "SYFFmpeg",
        url: "\(releaseBase)/SYFFmpeg.xcframework.zip",
        checksum: ffmpegChecksum
      )

let package = Package(
    name: "SYPlayerKit",
    platforms: [
        .iOS(.v13),
        // arm64 Mac Catalyst 的最低版本是 14.0：苹果芯片 Mac 从 macOS 11 起步，
        // 对应 Catalyst 14，链接器把 Catalyst 切片的 minos 定在 14.0（otool 可核对）。
        // 声明 .v13 会让接入方以为 13.x 能用，实际启动时报最低系统版本不满足。
        .macCatalyst(.v14),
        .macOS(.v11),
    ],
    products: [
        .library(name: "SYPlayerKit", targets: ["SYPlayerKit", "SYFFmpeg"]),
    ],
    targets: [
        kitTarget,
        ffmpegTarget,
    ]
)
