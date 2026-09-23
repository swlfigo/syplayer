#!/usr/bin/env ruby
# generate_xcodeprojects.rb — 生成 demo/ios、demo/mac 两个 Xcode 工程。
#
# 手写 .pbxproj 出错代价很高（一个字符打错整个工程打不开），这个仓库也
# 没装 xcodegen；但装了 CocoaPods 的 xcodeproj gem（`gem list xcodeproj`
# 能看到 1.27.0），用它以编程方式生成两个工程，可重复执行、不依赖 Xcode
# GUI。
#
# **"diff 可审查"这句原话不成立**——把
# 这个脚本跑了两遍、diff 生成的 project.pbxproj，逐行改动铺满全文件：
# `xcodeproj` gem 默认给每个 PBXBuildFile/PBXFileReference 随机生成
# UUID，不是 CocoaPods 集成 Pod 时那种按内容哈希算出来的确定性 UUID，
# 同样的输入两次跑出来的 UUID 完全不同。后果是：以后往 demo/shared/
# 加一个文件、重跑这个脚本，产出的是一份几乎全文件改动的 diff，不是"只
# 多了几行"——**核对 project.pbxproj 时应该看 `git diff --stat`
# 的新增/删除文件列表，不要逐行比对内容**，逐行比对本身就会被 UUID
# 噪声淹没，找不出真正的改动点。
#
# 没有在这一轮里改成确定性 UUID（按文件相对路径算稳定哈希是可行的路子，
# 但要跟 xcodeproj gem 的内部 UUID 分配机制对齐，是一块独立的开发投入，
# 这次判断不值——demo 壳的 pbxproj 改动频率低，"新增哪些文件"这个信息
# `git diff --stat` 已经能看清楚）。
#
# **这个脚本本身不是可执行产物的一部分，只是生成器。** 以后往
# demo/shared/、或者本文件下面几个数组里列的 src/ 源文件集合增删文件，
# 改这里重新跑一遍（`ruby demo/generate_xcodeprojects.rb`）即可让两个工程
# 都同步感知到；不要手改生成出来的 project.pbxproj——下次重新生成会覆盖。
# 如果确实需要在 Xcode GUI 里调的设置（签名团队、能力开关……），GUI 存下来
# 的改动会保留在 pbxproj 里，但下次跑这个脚本会覆盖掉，这是已知取舍：
# 这个脚本管的是"源文件集合 + 编译期设置"，签名这类"每台开发机各不相同"
# 的设置不归它管，见下面 CODE_SIGNING 相关注释。
#
# ---------------------------------------------------------------------------
# 【--check：pbxproj 新鲜度闸门】
#
# 不带参数 = 老behaviour，生成并覆盖两个工程。带 `--check` 则**一个字节都不写**，
# 只把"现在生成出来会是什么样"和"磁盘上的 project.pbxproj 是什么样"逐个 target
# 比对文件集合，不一致就非零退出并把差异按 target 列出来。
#
# 【它为什么存在 —— 这是暴露出的最严重的流程漏洞】
# 两个 pbxproj 自 `afea771` 起没跟着本脚本重新生成，缺了 5 个 C++ 源
# （cache_store / m3u8_scan / media_info_provider / preloader / syp_preload_api），
# `SYPlayerKitTests` 链接失败，于是 `xcodebuild test` **一直红了很久**——
# 好几轮"全量验证"都没有任何人跑过一条 Swift 测试，
# 也没有任何人发现。CMake 侧一切正常，因为 CMake 有自己的源文件列表；
# 只有 demo 这两个壳是"手动同步"的。
#
# 【判据为什么是这个而不是 diff 或 glob】
#   · **不能逐行 diff pbxproj**：xcodeproj gem 给每个 PBXBuildFile 随机分配
#     UUID，同样的输入两次跑出来的文件内容完全不同（见本文件开头那段注释），
#     逐行比必然恒红。
#   · **不能用磁盘 glob 当源清单**（早期的一版原型就是）：`src/**/*.cpp`
#     里本来就有不该进 demo 壳的文件（tools/、测试用的），于是要加硬编码例外，
#     而例外清单一定会漂——第一个例外出现的那天，这道闸就开始走向"永久假红
#     ⇒ 被关掉"。
#   · 判据取**生成器自己的那几个常量数组**（CORE_CPP / CORE_MM / KIT_MM /
#     KIT_SWIFT / APP_SWIFT / TESTS_SWIFT / …）。它不可能与生成器漂移
#     ——它就是生成器；天然 per-target（早期原型丢了这一层，只比并集）；
#     天然全语言（早期原型只查 C++，而后来新增的恰恰是两个 .swift）；
#     天然双向（多一个文件和少一个文件一样报）。
#
# 【已知盲区，别指望它管】
#   1. **不比绝对路径前缀。** 两个 pbxproj 把 include/、src/、build-ffmpeg
#      写成绝对路径，在 git worktree 里它们指向**主工作树**。比前缀会让每个
#      worktree 都永久假红（而假红的下场就是闸被关掉）；真正的防护是
#      README 里那条"worktree 里先跑一遍本脚本"。这里只比路径里从
#      src//swift//demo//include//build-ffmpeg/ 起算的那一段。
#   2. **不比 build settings**，也不比 UUID、组结构、文件顺序。判据是集合。
#   3. **不比 scheme 的内容**，只比三个共享 scheme 文件在不在——"scheme not
#      found" 是这两个工程另一种真实发生过的失败形态。
#   4. **【结构性的已知盲区，尚未关闭】**
#      闸比的是"生成器常量数组 ↔ pbxproj"，**仓里没有任何东西比
#      "CMakeLists 源清单 ↔ 生成器常量数组"**。393f129 那次事故的形态恰好是
#      "生成器已更新、没重跑"（它核对过那 5 个源在生成器里都在），所以这道
#      闸兜住了；但**互补的那条路径——"往 CMake 加了源、忘了往生成器加"
#      ——仍然全裸**。它的表现与 393f129 一模一样：SYPlayerKitTests 链接失败，
#      而 ctest 侧一切正常。
#      为什么不关：两份清单**不是同一个集合**，CMake 的 syp_dl/syp_media/
#      syp_platform_apple 三个目标里有不该进 demo 壳的源（tools/、测试专用），
#      直接比会恒红；要比就得引入一份"例外清单"，而上面第二条判据说明里
#      已经论证过例外清单必然漂移、漂到第一次假红就会被关掉。真正的修法是
#      让 CMake 与生成器读**同一份**源清单文件（一份 .txt / .cmake，两边都
#      解析它），那是一次独立改造，不属于当前范围。
#      在那之前，这条盲区的唯一防线是那条约定："往 CMake 或生成器的
#      源清单里加文件之后，重跑生成器并把两个 pbxproj 一起提交"。
# ---------------------------------------------------------------------------
require 'xcodeproj'
require 'fileutils'
require 'set'

ROOT = File.expand_path('..', __dir__)

CHECK_MODE = ARGV.include?('--check')
CHECK_FAILURES = []

# `--dist <dir>`：只生成分发用工程 <dir>/SYPlayerKitDist.xcodeproj（一个动态
# SYPlayerKit framework 目标，iOS / 模拟器 / Mac Catalyst / 原生 macOS 共用），
# 供 tools/build-xcframework.sh 逐平台 archive。它**不碰** demo 两个工程，也不
# 参与 `--check`：分发工程是构建期产物，内含绝对路径，不入库。
DIST_ARG_INDEX = ARGV.index('--dist')
DIST_DIR = DIST_ARG_INDEX.nil? ? nil : ARGV[DIST_ARG_INDEX + 1]
unless DIST_ARG_INDEX.nil?
  if DIST_DIR.nil? || DIST_DIR.start_with?('--')
    warn '用法：ruby demo/generate_xcodeprojects.rb --dist <输出目录>'
    exit 2
  end
  if CHECK_MODE
    warn '--dist 与 --check 不能同时使用：分发工程不参与新鲜度比对。'
    exit 2
  end
end

# `--dist-version X.Y.Z`：只在 `--dist` 模式下生效，写进分发工程的
# MARKETING_VERSION / CURRENT_PROJECT_VERSION。不传时默认 0.1.0。
DIST_VERSION_ARG_INDEX = ARGV.index('--dist-version')
DIST_VERSION = DIST_VERSION_ARG_INDEX.nil? ? '0.1.0' : ARGV[DIST_VERSION_ARG_INDEX + 1]
if DIST_VERSION.nil? || DIST_VERSION.start_with?('--')
  warn '用法：ruby demo/generate_xcodeprojects.rb --dist <输出目录> [--dist-version X.Y.Z]'
  exit 2
end

# 路径归一：截到仓库源根为止（见上面盲区 1）。
CHECK_PATH_ROOTS = %w[src swift demo include build-ffmpeg generated].freeze
def norm_ref_path(abs)
  s = abs.to_s
  best = nil
  CHECK_PATH_ROOTS.each do |r|
    i = s.index("/#{r}/")
    best = i if !i.nil? && (best.nil? || i < best)
  end
  best.nil? ? File.basename(s) : s[(best + 1)..]
end

# 一个工程的"文件集合"指纹：per-target、per-阶段。
def membership(project)
  out = {}
  project.targets.each do |t|
    phases = {
      'sources'   => t.respond_to?(:source_build_phase)    ? t.source_build_phase    : nil,
      'headers'   => t.respond_to?(:headers_build_phase)   ? t.headers_build_phase   : nil,
      'resources' => t.respond_to?(:resources_build_phase) ? t.resources_build_phase : nil,
      'frameworks' => t.respond_to?(:frameworks_build_phase) ? t.frameworks_build_phase : nil,
    }
    # 【第四个未声明的盲区，而且它漏掉的那类故障最难查】
    # `PBXCopyFilesBuildPhase`（Embed Frameworks）此前不在比对范围内。实测把
    # mac pbxproj 里两条 `SYFFmpeg.xcframework in Embed Frameworks` 删掉：
    # `--check` **exit 0**、`xcodebuild build` **BUILD SUCCEEDED**，而启动
    # 就是 `dyld: Library not loaded: @rpath/SYFFmpeg.framework/...`。
    # "构建全绿、App 启动即崩"正是这道闸存在的那一类故障。
    #
    # 一个 target 可以有多个 copy-files phase（Embed Frameworks / Embed App
    # Extensions / Embed Watch Content…），所以**按 phase 名各成一个 key**，
    # 不并成一个集合：把一个文件从 Embed Frameworks 挪到另一个 phase 是一次
    # 真实的行为改变，并集会把它看漏。同名 phase 有多份时（Xcode 允许）用
    # 序号区分，避免后一份把前一份覆盖掉。
    if t.respond_to?(:copy_files_build_phases)
      seen = Hash.new(0)
      t.copy_files_build_phases.each do |ph|
        base = "copyfiles:#{ph.name || ph.dst_subfolder_spec}"
        seen[base] += 1
        phases[seen[base] == 1 ? base : "#{base}##{seen[base]}"] = ph
      end
    end
    per = {}
    phases.each do |name, ph|
      next if ph.nil?
      acc = Set.new
      ph.files.each do |bf|
        fr = bf.file_ref
        # file_ref 为空的 PBXBuildFile 是坏条目；product_ref_group 里的产物
        # （.framework/.app/.xctest）不算源，跳过——它们的路径由 Xcode 决定。
        next if fr.nil?
        next if fr.respond_to?(:source_tree) && fr.source_tree == 'BUILT_PRODUCTS_DIR'
        acc << norm_ref_path(fr.respond_to?(:real_path) ? fr.real_path : fr.path)
      end
      per[name] = acc
    end
    out[t.name] = per
  end
  out
end

def check_project(project_path, generated)
  rel = project_path.sub("#{ROOT}/", '')
  unless File.exist?(File.join(project_path, 'project.pbxproj'))
    CHECK_FAILURES << "#{rel}：project.pbxproj 不存在——从未生成过？"
    return
  end
  on_disk = membership(Xcodeproj::Project.open(project_path))
  want    = membership(generated)

  (want.keys.to_set | on_disk.keys.to_set).sort.each do |tname|
    w = want[tname]
    d = on_disk[tname]
    if w.nil?
      CHECK_FAILURES << "#{rel}：磁盘上多出一个 target「#{tname}」，生成器里没有"
      next
    end
    if d.nil?
      CHECK_FAILURES << "#{rel}：磁盘上缺 target「#{tname}」"
      next
    end
    (w.keys.to_set | d.keys.to_set).sort.each do |phase|
      wa = w[phase] || Set.new
      da = d[phase] || Set.new
      missing = (wa - da).to_a.sort
      extra   = (da - wa).to_a.sort
      next if missing.empty? && extra.empty?
      CHECK_FAILURES << "#{rel} · target #{tname} · #{phase}：" \
                        "磁盘上缺 #{missing.size} 个、多 #{extra.size} 个\n" +
                        missing.map { |x| "    缺: #{x}" }.join("\n") +
                        (missing.empty? || extra.empty? ? '' : "\n") +
                        extra.map { |x| "    多: #{x}" }.join("\n")
    end
  end
end

def check_schemes(project_path, names)
  rel = project_path.sub("#{ROOT}/", '')
  names.each do |n|
    f = File.join(project_path, 'xcshareddata', 'xcschemes', "#{n}.xcscheme")
    CHECK_FAILURES << "#{rel}：共享 scheme「#{n}」缺失（xcodebuild -scheme 会报 not found）" unless File.exist?(f)
  end
end

def p(*parts)
  File.join(ROOT, *parts)
end

FFMPEG_XCFRAMEWORK = p('build-ffmpeg', 'out', 'SYFFmpeg.xcframework')

# SYFFmpeg 只导出 syp_ 前缀的名字（接入方自带另一份 FFmpeg 时两边互不串用）。
# 凡是包含 FFmpeg 头的编译单元都要先强制包含这份映射头，把 av_read_frame 之类
# 的原名改写成 syp_ 版本；漏了链接期就是未定义符号。映射头由
# tools/build-ffmpeg.sh 生成，放在每个 slice 的 Headers/ 下，各 slice 内容一致。
FFMPEG_PREFIX_HEADER_NAME = 'syp_ffmpeg_prefix.h'

# OTHER_CFLAGS 里的强制包含。Xcode 的 OTHER_CPLUSPLUSFLAGS 缺省就是
# $(OTHER_CFLAGS)，C++ / ObjC++ 源也由这一条覆盖——所以任何目标都**不要**单独
# 设 OTHER_CPLUSPLUSFLAGS，设了就得把这一条抄过去。
def ffmpeg_prefix_cflags(prefix_header)
  ['$(inherited)', '-include', prefix_header]
end

# 两个壳共用的核心 C++/ObjC++ 源文件集合——跟顶层 CMakeLists.txt 里
# syp_dl / syp_media / syp_platform_apple 三个目标的源文件列表一一对应
# （URL 播放要走 dl 层 + NSURLSession HTTP 后端，所以 src/dl/ 全套也在列，
# 不只是 src/media/、src/platform/apple/）。**只读这些文件，不修改**——
# demo 壳是消费者。
CORE_CPP = %w[
  src/dl/hole_set.cpp
  src/dl/m3u8_scan.cpp
  src/dl/cache_index.cpp
  src/dl/cache_file.cpp
  src/dl/cache_store.cpp
  src/dl/dl_task.cpp
  src/dl/rate_limiter.cpp
  src/dl/health_ticker.cpp
  src/dl/preconnector.cpp
  src/dl/scheduler.cpp
  src/dl/preloader.cpp
  src/dl/source_bridge.cpp
  src/dl/syp_source_api.cpp
  src/dl/syp_preload_api.cpp
  src/dl/syp_net_api.cpp
  src/media/audio_ring.cpp
  src/media/avio_bridge.cpp
  src/media/demuxer.cpp
  src/media/ffmpeg_audio_decoder.cpp
  src/media/ffmpeg_video_decoder.cpp
  src/media/frame.cpp
  src/media/frame_queue.cpp
  src/media/hls/hls_session.cpp
  src/media/hls/playlist_fetcher.cpp
  src/media/hls/url_rewrite.cpp
  src/media/media_info_provider.cpp
  src/media/packet_queue.cpp
  src/media/pipeline.cpp
  src/media/time_source.cpp
  src/media/track_player.cpp
  src/media/video_geometry.cpp
].freeze

CORE_MM = %w[
  src/platform/apple/apple_http_backend.mm
  src/platform/apple/audio_unit_sink.mm
  src/platform/apple/metal_renderer.mm
  src/platform/apple/vt_decode_backend.mm
].freeze

# ---- SYPlayerKit framework 的源文件集合 ----
# 决策：C++ 内核 + ObjC++ 桥 + Swift 公开 API 收进一个静态 framework，
# 两个 App 只留自己的 UI 源并链接它。CORE_CPP / CORE_MM 从 App 挪到这里，
# App 的 Compile Sources 里从此不再出现任何 .cpp/.mm。
KIT_MM = %w[swift/SYPlayerKit/Internal/SYPBridge.mm].freeze
# `+AppKit` 两个文件整体包在 `#if os(macOS) && !targetEnvironment(macCatalyst)`
# 里，在 demo 的 iOS / Catalyst 目标里编成空文件；它们仍然列在这里，是因为
# 分发工程与 demo 共用这一份清单，原生 macOS 切片要靠它们提供视图类型。
KIT_SWIFT = %w[
  swift/SYPlayerKit/SYPlayerError.swift
  swift/SYPlayerKit/SYPlayerState.swift
  swift/SYPlayerKit/SYPlayerSnapshotMapping.swift
  swift/SYPlayerKit/SYPlayerLayer.swift
  swift/SYPlayerKit/SYPlayerView.swift
  swift/SYPlayerKit/SYPlayerView+AppKit.swift
  swift/SYPlayerKit/SYPlayer.swift
  swift/SYPlayerKit/SYPlayerPreloader.swift
  swift/SYPlayerKit/SYPlayerViewRepresentable.swift
  swift/SYPlayerKit/SYPlayerViewRepresentable+AppKit.swift
  swift/SYPlayerKit/SYPlayerNetwork.swift
].freeze
KIT_HEADERS_PUBLIC  = %w[swift/SYPlayerKit/SYPlayerKit.h].freeze
# Private 头进 framework 的 PrivateHeaders/，由下面这份私有 module map
# （MODULEMAP_PRIVATE_FILE，拷成 Modules/module.private.modulemap）包成独立
# 模块 SYPlayerKit_Private——公开伞头一个字都不提它，业务侧 `import SYPlayerKit`
# 看不见。
KIT_HEADERS_PRIVATE = %w[swift/SYPlayerKit/Internal/SYPBridge.h].freeze
KIT_MODULEMAP_PRIVATE = 'swift/SYPlayerKit/SYPlayerKit.private.modulemap'

# App 自己的源：只有 UI。这里的每个文件都不许出现 Syp* / int32_t /
# UnsafeMutablePointer（验收 grep 会检查）。
APP_SWIFT = %w[
  demo/shared/AppDelegate.swift
  demo/shared/PlayerView.swift
  demo/shared/PlayerViewController.swift
  demo/shared/SwiftUIPlayerViewController.swift
  demo/shared/PreloadViewController.swift
  demo/shared/PreloadSampleServer.swift
].freeze
APP_HEADERS = %w[].freeze   # 桥接头已随桥迁移一并删除

TESTS_SWIFT = %w[
  swift/SYPlayerKitTests/SYPlayerErrorTests.swift
  swift/SYPlayerKitTests/SYPlayerStateMappingTests.swift
  swift/SYPlayerKitTests/SYPlayerLayerTests.swift
  swift/SYPlayerKitTests/SYPlayerLifecycleTests.swift
  swift/SYPlayerKitTests/SYPlayerPreloaderTests.swift
  swift/SYPlayerKitTests/SYPlayerVolumeTests.swift
  swift/SYPlayerKitTests/SYPlayerNetworkTests.swift
].freeze

# 【记录在案】sample.mp4 是这个仓库目前唯一被允许提交
# 进 git 历史的二进制素材，例外原因是它要进两个 .app bundle 让 demo 开箱
# 即跑（UIDocumentPickerViewController 挑文件之外，"点一下就能看到东西"
# 这条路径不能依赖外部网络或者本机是否装了 ffmpeg CLI）。1.4MB，可接受。
# 早先定的纪律——"随机素材测试、不进 git"（`tools/gen-fixtures.sh` 产出的
# 那些）——不受这条例外影响，仍然一律不进 git：那条纪律冲着的是"历史
# 不可变，二进制一旦进去删分支也删不掉"，跟这里"一份小体积、内容固定、
# 明确需要跟 app 一起分发的样片"是两件事，不要拿这条当以后往仓库塞更多
# 二进制素材的先例。
SHARED_RESOURCES = %w[demo/shared/Resources/sample.mp4].freeze

def add_sources(project, target, group, root, rel_paths)
  refs = rel_paths.map { |rel| group.new_reference(File.join(root, rel)) }
  target.add_file_references(refs)
  refs
end

# SYPlayerKit：静态 framework。
#
# 为什么是 framework 而不是 static library：混合 ObjC++/Swift 的目标要让
# 消费者 `import SYPlayerKit` 就能拿到 Swift API，必须有一个模块；static
# library 目标不产出 .swiftmodule 的可分发布局，得自己拼 modulemap 与
# 头文件目录，比 framework 麻烦且更容易错。
#
# 为什么是静态（MACH_O_TYPE = staticlib）而不是动态：动态 framework 要签名
# 与内嵌，App 启动多一次 dyld 加载；而且这个仓库已经在为 SYFFmpeg.xcframework
# 做“链接 + 内嵌 + CodeSignOnCopy”，再加一份纯属重复成本。静态 framework
# 的产物是 .framework 壳里放一个静态归档，Xcode 原生支持，App 只需要链接。
def add_kit_target(project:, platform_kind:, deployment_target:, ffmpeg_headers:, ffmpeg_slice_dir:)
  kit = project.new_target(:framework, 'SYPlayerKit', :ios, deployment_target, nil, :swift, 'SYPlayerKit')
  add_kit_sources(project, kit)

  common = kit_compile_settings.merge(
    'MACH_O_TYPE'                    => 'staticlib',
    'SYSTEM_HEADER_SEARCH_PATHS'     => ['$(inherited)', ffmpeg_headers],
    'OTHER_CFLAGS'                   => ffmpeg_prefix_cflags(File.join(ffmpeg_headers, FFMPEG_PREFIX_HEADER_NAME)),
    'FRAMEWORK_SEARCH_PATHS'         => ['$(inherited)', '$(BUILT_PRODUCTS_DIR)', ffmpeg_slice_dir],
    'TARGETED_DEVICE_FAMILY'         => platform_kind == :mac_catalyst ? '2,6' : '1,2',
    'SKIP_INSTALL'                   => 'YES',
    # 静态 framework 没有可签名的 Mach-O，打开签名只会在 CI/无证书机器上
    # 平白失败。App 自己的签名不受这条影响（它有独立的 build settings）。
    'CODE_SIGNING_ALLOWED'           => 'NO',
    'SUPPORTS_MACCATALYST'           => platform_kind == :mac_catalyst ? 'YES' : 'NO',
  )
  common['DERIVE_MACCATALYST_PRODUCT_BUNDLE_IDENTIFIER'] = 'NO' if platform_kind == :mac_catalyst

  kit.build_configurations.each { |config| config.build_settings.merge!(common) }

  # 【`ENABLE_TESTABILITY` 只给 Debug】
  #
  # 它做的事是把 internal 符号导出、并**关掉跨模块的一部分优化**（Swift 侧
  # 等价于 `-enable-testing`），好让 `@testable import SYPlayerKit` 看得见
  # SYPlayer 的那几道 internal 测试缝。上一版对所有 configuration 一律打开，
  # 于是 Release 产物里也带着这些符号、也少做那部分优化——这是给发布构建
  # 无谓地掏成本。测试目标（SYPlayerKitTests）只在 Debug 下跑，Debug 一档
  # 就够。
  kit.build_configurations.each do |config|
    config.build_settings['ENABLE_TESTABILITY'] = config.name == 'Debug' ? 'YES' : 'NO'
  end

  kit
end

# SYPlayerKit 目标的源文件、头文件阶段、私有 module map 与着色器脚本阶段。
# demo 的静态 kit 与分发用的动态 kit 共用这一份，两者的源集合不可能漂移。
def add_kit_sources(project, kit)
  group_kit      = project.main_group.new_group('SYPlayerKit', p('swift', 'SYPlayerKit'))
  group_kit_int  = group_kit.new_group('Internal', p('swift', 'SYPlayerKit', 'Internal'))
  group_dl       = project.main_group.new_group('src-dl', p('src', 'dl'))
  group_media    = project.main_group.new_group('src-media', p('src', 'media'))
  group_platform = project.main_group.new_group('src-platform-apple', p('src', 'platform', 'apple'))

  add_sources(project, kit, group_dl, ROOT, CORE_CPP.grep(%r{^src/dl/}))
  add_sources(project, kit, group_media, ROOT, CORE_CPP.grep(%r{^src/media/}))
  add_sources(project, kit, group_platform, ROOT, CORE_MM)
  add_sources(project, kit, group_kit_int, ROOT, KIT_MM)
  add_sources(project, kit, group_kit, ROOT, KIT_SWIFT)

  # Headers 构建阶段：Public 进 Headers/（伞头），Private 进 PrivateHeaders/。
  # 注意 src/ 下那一大堆 .h **不进**这个阶段——它们靠 HEADER_SEARCH_PATHS
  # 找到就够了，进了 Headers 阶段会被拷进 framework 壳里当公开头。
  KIT_HEADERS_PUBLIC.each do |rel|
    ref = group_kit.new_reference(File.join(ROOT, rel))
    kit.headers_build_phase.add_file_reference(ref).settings = { 'ATTRIBUTES' => ['Public'] }
  end
  KIT_HEADERS_PRIVATE.each do |rel|
    ref = group_kit_int.new_reference(File.join(ROOT, rel))
    kit.headers_build_phase.add_file_reference(ref).settings = { 'ATTRIBUTES' => ['Private'] }
  end
  # 私有 module map 只入组（让它在 Xcode 里能被看到、被编辑），不进任何构建
  # 阶段——它是被 MODULEMAP_PRIVATE_FILE 这个 build setting 拷贝的，不是源文件。
  group_kit.new_reference(p(*KIT_MODULEMAP_PRIVATE.split('/')))

  add_shader_script_phase(kit)
end

# 两种 kit（demo 静态、分发动态）共同的编译设置：模块、语言标准、告警、头搜索路径。
# 链接形态、FFmpeg 搜索路径、平台与签名相关的设置各自在调用方补。
def kit_compile_settings
  {
    'PRODUCT_NAME'                   => 'SYPlayerKit',
    'PRODUCT_BUNDLE_IDENTIFIER'      => 'com.syplayer.SYPlayerKit',
    'DEFINES_MODULE'                 => 'YES',
    'MODULEMAP_PRIVATE_FILE'         => p(*KIT_MODULEMAP_PRIVATE.split('/')),
    'SWIFT_VERSION'                  => '5.0',
    'CLANG_ENABLE_OBJC_ARC'          => 'YES',
    'CLANG_CXX_LANGUAGE_STANDARD'    => 'c++23',
    'CLANG_CXX_LIBRARY'              => 'libc++',
    'WARNING_CFLAGS'                 => '-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion',
    'GCC_WARN_INHIBIT_ALL_WARNINGS'  => 'NO',
    'HEADER_SEARCH_PATHS'            => ['$(inherited)', p('include'), p('src'), '$(SRCROOT)/generated'],
    'ENABLE_USER_SCRIPT_SANDBOXING'  => 'NO',
  }
end

# 着色器内嵌头：present() 之前必须存在，Run Script 放在 Compile Sources 之前
# （顶层 CMakeLists.txt 用 execute_process 在 configure 期生成，这里是 Xcode
# 构建期的等价物，同一份 tools/gen-embedded-header.sh，不重复一份生成逻辑）。
# metal_renderer.mm 已经挪进 SYPlayerKit，这个阶段跟着挪过来——App 不再
# 编译任何 .mm，留在 App 上只会白跑一遍。
def add_shader_script_phase(target)
  metal_src  = p('src', 'platform', 'apple', 'video_shaders.metal')
  gen_script = p('tools', 'gen-embedded-header.sh')
  gen_header = '$(SRCROOT)/generated/syp_video_shaders_metal_source.h'
  phase = target.new_shell_script_build_phase('Generate Metal Shader Header')
  phase.shell_script = <<~SH
    set -e
    mkdir -p "$SRCROOT/generated"
    bash "#{gen_script}" "#{metal_src}" "#{gen_header.sub('$(SRCROOT)', '$SRCROOT')}" "syp::platform" "kVideoShadersMetalSource"
  SH
  phase.input_paths  = [metal_src, gen_script]
  phase.output_paths = [gen_header]
  # 必须排在 Compile Sources 之前：build_phases 数组顺序即执行顺序。
  target.build_phases.delete(phase)
  target.build_phases.insert(0, phase)
end

def add_app_target(project:, kit:, name:, bundle_id:, platform_kind:, deployment_target:,
                   ffmpeg_ref:, ffmpeg_slice_dir:, ffmpeg_headers:)
  app = project.new_target(:application, name, :ios, deployment_target, nil, :swift, name)

  group_shared = project.main_group['shared'] || project.main_group.new_group('shared', p('demo', 'shared'))
  add_sources(project, app, group_shared, ROOT, APP_SWIFT)
  APP_HEADERS.each { |rel| group_shared.new_reference(File.join(ROOT, rel)) }

  res_ref = group_shared.new_reference(File.join(ROOT, SHARED_RESOURCES.first))
  app.resources_build_phase.add_file_reference(res_ref)

  # SYPlayerKit：加依赖 + 链接，**不内嵌**——静态 framework 里没有 dylib 可嵌，
  # 内嵌只会把一份静态归档拷进 .app/Frameworks 白占体积。
  app.add_dependency(kit)
  app.frameworks_build_phase.add_file_reference(kit.product_reference)

  # SYFFmpeg.xcframework：链接 + 内嵌（Xcode 原生 xcframework 支持会按目标平台/
  # 变体自动挑正确的 slice）。SYPlayerKit 是静态的，FFmpeg 的符号在最终可执行
  # 文件这一层才解析，所以这一份链接留在 App 上，不挪进 framework。
  app.frameworks_build_phase.add_file_reference(ffmpeg_ref)
  embed_phase = app.new_copy_files_build_phase('Embed Frameworks')
  embed_phase.symbol_dst_subfolder_spec = :frameworks
  embed_phase.add_file_reference(ffmpeg_ref).settings =
    { 'ATTRIBUTES' => %w[CodeSignOnCopy RemoveHeadersOnCopy] }

  app.add_system_framework(%w[UIKit AudioToolbox CoreAudio Metal VideoToolbox CoreVideo
                              CoreMedia QuartzCore AVFoundation])

  common = {
    'PRODUCT_BUNDLE_IDENTIFIER'      => bundle_id,
    'PRODUCT_NAME'                   => name,
    'SWIFT_VERSION'                  => '5.0',
    'CLANG_ENABLE_OBJC_ARC'          => 'YES',
    'WARNING_CFLAGS'                 => '-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion',
    'GCC_WARN_INHIBIT_ALL_WARNINGS'  => 'NO',
    # App 目前只有 Swift 源；映射头照样挂上，以后加进来的 C/ObjC 源一旦包含
    # FFmpeg 头也不会绕过映射。
    'OTHER_CFLAGS'                   => ffmpeg_prefix_cflags(File.join(ffmpeg_headers, FFMPEG_PREFIX_HEADER_NAME)),
    'FRAMEWORK_SEARCH_PATHS'         => ['$(inherited)', '$(BUILT_PRODUCTS_DIR)', ffmpeg_slice_dir],
    'LD_RUNPATH_SEARCH_PATHS'        => ['$(inherited)', '@executable_path/Frameworks'],
    'ENABLE_USER_SCRIPT_SANDBOXING'  => 'NO',
    'TARGETED_DEVICE_FAMILY'         => platform_kind == :mac_catalyst ? '2,6' : '1,2',
    'INFOPLIST_KEY_UILaunchScreen_Generation' => 'YES',
    'INFOPLIST_KEY_UIApplicationSupportsIndirectInputEvents' => 'YES',
    'GENERATE_INFOPLIST_FILE'        => 'YES',
    'CODE_SIGN_STYLE'                => 'Automatic',
    'DEVELOPMENT_TEAM'               => '',
    'SUPPORTS_MACCATALYST'           => platform_kind == :mac_catalyst ? 'YES' : 'NO',
  }
  common['DERIVE_MACCATALYST_PRODUCT_BUNDLE_IDENTIFIER'] = 'NO' if platform_kind == :mac_catalyst

  app.build_configurations.each { |config| config.build_settings.merge!(common) }
  app
end

# SYPlayerKitTests：**无 host 的逻辑测试 bundle**。
#
# 不挂 TEST_HOST 的理由：挂了就得先把 App 启起来，Mac Catalyst 上这要求 App
# 被（哪怕 ad-hoc）签名，而本仓库的验证一律 CODE_SIGNING_ALLOWED=NO。无 host
# 的 bundle 由 xctest 运行器加载，测的又正是纯映射与离屏可验证的部分，
# 不需要一个真 App 壳。
#
# SYPlayerKit 是静态的，所以测试 bundle 这一层要自己把 FFmpeg 与系统 framework
# 链齐、并内嵌 FFmpeg（bundle 加载时按 @loader_path/Frameworks 找 dylib）。
def add_tests_target(project:, kit:, deployment_target:, ffmpeg_ref:, ffmpeg_slice_dir:, ffmpeg_headers:)
  t = project.new_target(:unit_test_bundle, 'SYPlayerKitTests', :ios, deployment_target,
                         nil, :swift, 'SYPlayerKitTests')
  group = project.main_group.new_group('SYPlayerKitTests', p('swift', 'SYPlayerKitTests'))
  add_sources(project, t, group, ROOT, TESTS_SWIFT)

  # 生命周期用例要一个真素材：复用 App 那份内置样片（12 秒，640x360 h264 + aac）。
  res_ref = group.new_reference(File.join(ROOT, SHARED_RESOURCES.first))
  t.resources_build_phase.add_file_reference(res_ref)

  t.add_dependency(kit)
  t.frameworks_build_phase.add_file_reference(kit.product_reference)
  t.frameworks_build_phase.add_file_reference(ffmpeg_ref)
  embed_phase = t.new_copy_files_build_phase('Embed Frameworks')
  embed_phase.symbol_dst_subfolder_spec = :frameworks
  embed_phase.add_file_reference(ffmpeg_ref).settings =
    { 'ATTRIBUTES' => %w[CodeSignOnCopy RemoveHeadersOnCopy] }

  t.add_system_framework(%w[UIKit AudioToolbox CoreAudio Metal VideoToolbox CoreVideo
                            CoreMedia QuartzCore AVFoundation])

  common = {
    'PRODUCT_NAME'                   => 'SYPlayerKitTests',
    'PRODUCT_BUNDLE_IDENTIFIER'      => 'com.syplayer.SYPlayerKitTests',
    'SWIFT_VERSION'                  => '5.0',
    'GENERATE_INFOPLIST_FILE'        => 'YES',
    # 同 App：目前只有 Swift 源，映射头为以后的 C/ObjC 源预先挂上。
    'OTHER_CFLAGS'                   => ffmpeg_prefix_cflags(File.join(ffmpeg_headers, FFMPEG_PREFIX_HEADER_NAME)),
    'FRAMEWORK_SEARCH_PATHS'         => ['$(inherited)', '$(BUILT_PRODUCTS_DIR)', ffmpeg_slice_dir],
    'LD_RUNPATH_SEARCH_PATHS'        => ['$(inherited)', '@executable_path/Frameworks',
                                         '@loader_path/Frameworks'],
    'TARGETED_DEVICE_FAMILY'         => '2,6',
    'SUPPORTS_MACCATALYST'           => 'YES',
    'DERIVE_MACCATALYST_PRODUCT_BUNDLE_IDENTIFIER' => 'NO',
    'CODE_SIGN_STYLE'                => 'Automatic',
    'DEVELOPMENT_TEAM'               => '',
    'ENABLE_USER_SCRIPT_SANDBOXING'  => 'NO',
  }
  t.build_configurations.each { |config| config.build_settings.merge!(common) }
  t
end

# 共享 scheme：一旦工程里有了共享 scheme，xcodebuild 就不再给其余目标自动
# 补 scheme——所以**每个需要用 -scheme 点名的目标都必须在这里生成一个**，
# 漏一个就会变成 "scheme not found"。
def write_scheme(project_path:, name:, build_targets:, launch_target: nil, test_targets: [])
  scheme = Xcodeproj::XCScheme.new
  build_targets.each { |t| scheme.add_build_target(t, t == launch_target) }
  test_targets.each  { |t| scheme.add_test_target(t) }
  scheme.set_launch_target(launch_target) unless launch_target.nil?
  scheme.save_as(project_path, name, true)
end

# platform_kind:
#   :ios_device    — demo/ios，真机/generic iOS device，SUPPORTS_MACCATALYST=NO
#   :mac_catalyst  — demo/mac，Mac Catalyst，SUPPORTS_MACCATALYST=YES
def build_project(name:, dir_name:, bundle_id:, platform_kind:)
  project_dir  = p('demo', dir_name)
  project_path = File.join(project_dir, "#{name}.xcodeproj")
  FileUtils.mkdir_p(project_dir)

  project = Xcodeproj::Project.new(project_path)

  deployment_target = platform_kind == :mac_catalyst ? '13.1' : '13.0'
  slice_dir = platform_kind == :mac_catalyst ? 'ios-arm64-maccatalyst' : 'ios-arm64'
  ffmpeg_slice_dir = File.join(FFMPEG_XCFRAMEWORK, slice_dir)
  ffmpeg_headers   = File.join(ffmpeg_slice_dir, 'SYFFmpeg.framework', 'Headers')

  # 一个工程里只建一份 xcframework 引用，多个目标共用它——每个目标各建一份
  # 会在 pbxproj 里留下同路径的多个 PBXFileReference，Xcode 打开时显示成重复项。
  ffmpeg_ref = project.frameworks_group.new_reference(FFMPEG_XCFRAMEWORK)

  kit = add_kit_target(project: project, platform_kind: platform_kind,
                       deployment_target: deployment_target,
                       ffmpeg_headers: ffmpeg_headers, ffmpeg_slice_dir: ffmpeg_slice_dir)
  app = add_app_target(project: project, kit: kit, name: name, bundle_id: bundle_id,
                       platform_kind: platform_kind, deployment_target: deployment_target,
                       ffmpeg_ref: ffmpeg_ref, ffmpeg_slice_dir: ffmpeg_slice_dir,
                       ffmpeg_headers: ffmpeg_headers)

  # 测试目标只建在 Catalyst 工程里：XCTest 要在本机跑起来，只有 Mac Catalyst
  # 这一条路（iOS 目标要么真机、要么模拟器，都不在本仓库的验证口径内）。
  tests = platform_kind == :mac_catalyst ? add_tests_target(
    project: project, kit: kit, deployment_target: deployment_target,
    ffmpeg_ref: ffmpeg_ref, ffmpeg_slice_dir: ffmpeg_slice_dir,
    ffmpeg_headers: ffmpeg_headers) : nil

  project.build_configurations.each do |config|
    config.build_settings['ENABLE_USER_SCRIPT_SANDBOXING'] = 'NO'
  end

  scheme_names = [name, 'SYPlayerKit'] + (tests.nil? ? [] : ['SYPlayerKitTests'])

  if CHECK_MODE
    # 一个字节都不写：只拿刚在内存里装好的这份工程去对磁盘上那份。
    check_project(project_path, project)
    check_schemes(project_path, scheme_names)
    return [project, project_path, kit, app, tests]
  end

  project.save
  write_scheme(project_path: project_path, name: name,
               build_targets: [kit, app], launch_target: app,
               test_targets: tests.nil? ? [] : [tests])
  write_scheme(project_path: project_path, name: 'SYPlayerKit', build_targets: [kit])
  unless tests.nil?
    write_scheme(project_path: project_path, name: 'SYPlayerKitTests',
                 build_targets: [kit, tests], test_targets: [tests])
  end
  puts "生成完成: #{project_path}（目标：#{project.targets.map(&:name).join(', ')}）"
  [project, project_path, kit, app, tests]
end

# 分发工程：一个**动态** SYPlayerKit framework 目标，单目标覆盖 iOS 真机、iOS
# 模拟器、Mac Catalyst 与原生 macOS 四个平台，由 tools/build-xcframework.sh 逐平台
# archive 后合成 xcframework。
#
# 为什么是动态：作为 SwiftPM binaryTarget 分发时，接入方没有地方声明链接设置；
# 静态 framework 对 FFmpeg、VideoToolbox、Metal、libc++ 等的链接需求会全部
# 甩给接入方。动态 framework 在这里把它们链好，SwiftPM 负责内嵌与签名。
#
# 与 demo 静态 kit 的差别只在链接形态与平台设置，源集合与着色器脚本阶段走
# 同一个 add_kit_sources。FFmpeg 的 framework / 头文件搜索路径**不写进工程**：
# 四个平台各用 SYFFmpeg.xcframework 的一个 slice，由构建脚本在命令行按 slice 传入。
# 映射头同理：工程里只写 `-include $(SYP_FFMPEG_PREFIX_HEADER)`，这个变量由构建
# 脚本按 slice 在命令行给出；忘了给时 -include 拿到空参数，编译直接失败，不会
# 静默编出引用原名的产物。
DIST_PRIVACY_MANIFEST = 'swift/SYPlayerKit/PrivacyInfo.xcprivacy'
DIST_SYSTEM_FRAMEWORKS = %w[AudioToolbox CoreAudio Metal VideoToolbox CoreVideo
                            CoreMedia QuartzCore AVFoundation].freeze

def build_dist_project(dir)
  out_dir      = File.expand_path(dir)
  project_path = File.join(out_dir, 'SYPlayerKitDist.xcodeproj')
  # 重复运行要从干净状态开始：旧工程与旧的着色器生成头一并删掉。
  FileUtils.rm_rf(project_path)
  FileUtils.rm_rf(File.join(out_dir, 'generated'))
  FileUtils.mkdir_p(out_dir)

  project = Xcodeproj::Project.new(project_path)
  kit = project.new_target(:framework, 'SYPlayerKit', :ios, '13.0', nil, :swift, 'SYPlayerKit')
  add_kit_sources(project, kit)

  # 系统框架用 -framework 链，而不是往 Frameworks 阶段加 SDK 内的引用：后者
  # 按单一 SDK 写路径，多平台目标换到 macosx SDK 时会指错。demo App 链的那份
  # 清单里只有 UIKit 在原生 macOS 上不存在，所以不在这里显式链；UIKit / AppKit
  # 只被 Swift 源 import，由 Swift 的自动链接按平台各自带上。
  ldflags = ['$(inherited)', '-framework', 'SYFFmpeg'] +
            DIST_SYSTEM_FRAMEWORKS.flat_map { |fw| ['-framework', fw] }

  settings = kit_compile_settings.merge(
    'MACH_O_TYPE'                    => 'mh_dylib',
    'BUILD_LIBRARY_FOR_DISTRIBUTION' => 'YES',
    'SKIP_INSTALL'                   => 'NO',
    'SDKROOT'                        => 'auto',
    'SUPPORTED_PLATFORMS'            => 'iphoneos iphonesimulator macosx',
    'SUPPORTS_MACCATALYST'           => 'YES',
    'DERIVE_MACCATALYST_PRODUCT_BUNDLE_IDENTIFIER' => 'NO',
    # IPHONEOS_DEPLOYMENT_TARGET 管 iOS 真机与模拟器；Catalyst 在 macosx SDK 下
    # 构建，单独按 sdk=macosx* 写成 14.0。arm64 的 Mac Catalyst 没有 13.x 这一档，
    # 苹果芯片 Mac 从 macOS 11 起步，链接器本来就把 Catalyst 切片的 minos 抬到
    # 14.0；不单独写的话，Xcode 按 13.0 推出的 Info.plist LSMinimumSystemVersion
    # 是 10.15，与二进制实际要求的 macOS 11 对不上。原生 macOS 不读这个设置，
    # 只看 MACOSX_DEPLOYMENT_TARGET。
    'IPHONEOS_DEPLOYMENT_TARGET'     => '13.0',
    'IPHONEOS_DEPLOYMENT_TARGET[sdk=macosx*]' => '14.0',
    'MACOSX_DEPLOYMENT_TARGET'       => '11.0',
    'TARGETED_DEVICE_FAMILY'         => '1,2',
    'ENABLE_TESTABILITY'             => 'NO',
    'GENERATE_INFOPLIST_FILE'        => 'YES',
    'MARKETING_VERSION'              => DIST_VERSION,
    'CURRENT_PROJECT_VERSION'        => DIST_VERSION,
    'DEBUG_INFORMATION_FORMAT'       => 'dwarf-with-dsym',
    'OTHER_LDFLAGS'                  => ldflags,
    # 四条各服务一种壳层：iOS/模拟器的 App 把 framework 平铺在
    # Frameworks/ 下，用 @executable_path/Frameworks；Catalyst/macOS 的 App
    # 是 .app/Contents/Frameworks/，用 @executable_path/../Frameworks 才能
    # 找到同目录下的 SYFFmpeg.framework。@loader_path 那一对是本 framework
    # 被别的 framework（而不是可执行文件）直接加载时的兜底。
    'LD_RUNPATH_SEARCH_PATHS'        => ['$(inherited)', '@loader_path/Frameworks',
                                         '@executable_path/Frameworks', '@loader_path/../Frameworks',
                                         '@executable_path/../Frameworks'],
    'DYLIB_INSTALL_NAME_BASE'        => '@rpath',
    'OTHER_CFLAGS'                   => ffmpeg_prefix_cflags('$(SYP_FFMPEG_PREFIX_HEADER)'),
  )
  kit.build_configurations.each { |config| config.build_settings.merge!(settings) }
  project.build_configurations.each do |config|
    config.build_settings['ENABLE_USER_SCRIPT_SANDBOXING'] = 'NO'
  end

  # 隐私清单作为资源进 framework：iOS / 模拟器落在 framework 根目录，
  # Catalyst / macOS 落在 Versions/A/Resources/，位置由 Xcode 按平台决定。
  # 上架 App Store 时 Xcode 汇总各 SDK 的清单，缺了会被 ITMS-91053 拒收。
  privacy_ref = project.main_group.new_reference(p(*DIST_PRIVACY_MANIFEST.split('/')))
  kit.resources_build_phase.add_file_reference(privacy_ref)

  project.save
  # 不走 write_scheme：xcodeproj gem 只给 App 目标默认打开 buildForArchiving，
  # framework 目标的条目默认不参与 archive，xcodebuild archive 会直接报
  # "Scheme SYPlayerKit is not currently configured for the archive action"。
  scheme = Xcodeproj::XCScheme.new
  scheme.add_build_target(kit, true)
  scheme.build_action.entries.each do |entry|
    entry.build_for_archiving = true
    entry.build_for_profiling = true
    entry.build_for_analyzing = true
  end
  scheme.archive_action.build_configuration = 'Release'
  scheme.save_as(project_path, 'SYPlayerKit', true)
  puts "生成完成: #{project_path}（分发用动态 framework，目标：#{project.targets.map(&:name).join(', ')}）"
end

unless DIST_DIR.nil?
  build_dist_project(DIST_DIR)
  exit 0
end

build_project(name: 'syplayer-ios', dir_name: 'ios', bundle_id: 'com.syplayer.demo.ios',
              platform_kind: :ios_device)
build_project(name: 'syplayer-mac', dir_name: 'mac', bundle_id: 'com.syplayer.demo.mac',
              platform_kind: :mac_catalyst)

if CHECK_MODE
  if CHECK_FAILURES.empty?
    puts '全部通过（两个工程的 per-target 文件集合与生成器一致，三个共享 scheme 齐全）。'
    exit 0
  end
  warn 'pbxproj 与生成器不一致——两个工程没跟着 demo/generate_xcodeprojects.rb 重新生成：'
  CHECK_FAILURES.each { |m| warn "  · #{m}" }
  warn ''
  warn '修法：ruby demo/generate_xcodeprojects.rb  然后把两个 project.pbxproj 一起提交。'
  warn '（pbxproj 的 UUID 是随机的，重新生成必然是一份满屏 diff，看 git diff --stat 即可。）'
  exit 1
end
