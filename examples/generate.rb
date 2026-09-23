#!/usr/bin/env ruby
# generate.rb — 生成 examples/iOSExample、examples/macOSExample 两个 Xcode 工程。
#
# 两个示例 App 演示"接入方怎样经 SwiftPM 用 SYPlayerKit"：唯一依赖是仓库根的
# SwiftPM 包（本地包引用，相对路径 ../..），源码里只写 `import SYPlayerKit`。
#
# 与 demo/generate_xcodeprojects.rb 生成的两个 demo 工程不同，这里的工程**只含
# 相对路径**：任何人 clone 之后直接打开就能用，不需要先重跑生成器。
# 生成器把所有文件引用都挂在以相对路径声明的组下面，最后逐字扫一遍产物，
# 出现绝对路径（以 / 开头的 path、或本机仓库根的前缀）就报错退出。
#
# 用法：
#   ruby examples/generate.rb          # 生成并覆盖两个工程
#   ruby examples/generate.rb --check  # 一个字节都不写，只比对仓库里的工程与生成器产出
#
# 往 examples/ 下增删源文件之后，改下面的清单、重跑本脚本，并把两个
# project.pbxproj 与 scheme 一起提交。tools/check-spm.sh 会核对 examples/ 下的
# 每个 .swift 都出现在某个工程里。
require 'xcodeproj'
require 'fileutils'
require 'tmpdir'

EXAMPLES = File.expand_path(__dir__)
ROOT = File.expand_path('..', EXAMPLES)

CHECK_MODE = ARGV.include?('--check')
CHECK_FAILURES = []
CHECK_DIR = CHECK_MODE ? Dir.mktmpdir('examples-check') : nil

# 本地包引用需要 Xcode 15 的 XCLocalSwiftPackageReference，工程格式因此取 60。
OBJECT_VERSION = 60

SAMPLE = File.join(ROOT, 'demo', 'shared', 'Resources', 'sample.mp4')
SHARED_SWIFT = %w[Shared/PlayerScreen.swift].freeze
SHARED_TESTS_SWIFT = %w[SharedTests/SPMConsumerTests.swift].freeze

PLATFORMS = {
  ios: {
    name: 'iOSExample',
    sdk: :ios,
    deployment_target: '13.0',
    app_swift: %w[iOSExample/iOSExample/AppDelegate.swift],
    # iOS 13 没有 Info.plist 里的 UILaunchScreen 字典（iOS 14 起），部署目标 13 必须
    # 给一个启动 storyboard，否则 Xcode 警告、iOS 13 上按兼容模式留黑边启动。
    app_resources: %w[iOSExample/iOSExample/LaunchScreen.storyboard],
    tests_swift: [],
    bundle_id: 'com.syplayer.examples.ios',
  },
  macos: {
    name: 'macOSExample',
    sdk: :osx,
    deployment_target: '11.0',
    app_swift: %w[macOSExample/macOSExample/ExampleApp.swift],
    app_resources: [],
    tests_swift: %w[macOSExample/macOSExampleTests/AppKitPlayerViewTests.swift],
    bundle_id: 'com.syplayer.examples.macos',
  },
}.freeze

def add_files(project, target, group, rel_paths)
  rel_paths.each do |rel|
    abs = File.join(EXAMPLES, rel)
    raise "文件不存在：#{abs}" unless File.exist?(abs)
    ref = group.new_reference(abs)
    target.source_build_phase.add_file_reference(ref)
  end
end

def link_package_product(project, target, package_ref)
  dep = project.new(Xcodeproj::Project::Object::XCSwiftPackageProductDependency)
  dep.package = package_ref
  dep.product_name = 'SYPlayerKit'
  target.package_product_dependencies << dep
  bf = project.new(Xcodeproj::Project::Object::PBXBuildFile)
  bf.product_ref = dep
  target.frameworks_build_phase.files << bf
end

def build(cfg)
  name = cfg[:name]
  project_dir = File.join(EXAMPLES, name)
  project_path = File.join(project_dir, "#{name}.xcodeproj")
  FileUtils.rm_rf(project_path) unless CHECK_MODE
  project = Xcodeproj::Project.new(project_path, false, OBJECT_VERSION)

  package_ref = project.new(Xcodeproj::Project::Object::XCLocalSwiftPackageReference)
  package_ref.relative_path = '../..'
  project.root_object.package_references << package_ref

  # 组：全部以相对工程目录的路径声明，文件引用挂在组下面、自动算成相对组的路径。
  shared_group = project.main_group.new_group('Shared', File.join(EXAMPLES, 'Shared'))
  shared_tests_group = project.main_group.new_group('SharedTests', File.join(EXAMPLES, 'SharedTests'))
  app_group = project.main_group.new_group(name, File.join(project_dir, name))
  tests_group = project.main_group.new_group("#{name}Tests", File.join(project_dir, "#{name}Tests"))
  resources_group = project.main_group.new_group('Resources', File.dirname(SAMPLE))

  app = project.new_target(:application, name, cfg[:sdk], cfg[:deployment_target], nil, :swift, name)
  add_files(project, app, shared_group, SHARED_SWIFT)
  cfg[:app_swift].each do |rel|
    ref = app_group.new_reference(File.join(EXAMPLES, rel))
    app.source_build_phase.add_file_reference(ref)
  end
  cfg[:app_resources].each do |rel|
    ref = app_group.new_reference(File.join(EXAMPLES, rel))
    app.resources_build_phase.add_file_reference(ref)
  end
  sample_ref = resources_group.new_reference(SAMPLE)
  app.resources_build_phase.add_file_reference(sample_ref)
  link_package_product(project, app, package_ref)

  tests = project.new_target(:unit_test_bundle, "#{name}Tests", cfg[:sdk], cfg[:deployment_target],
                             nil, :swift, "#{name}Tests")
  add_files(project, tests, shared_tests_group, SHARED_TESTS_SWIFT)
  cfg[:tests_swift].each do |rel|
    ref = tests_group.new_reference(File.join(EXAMPLES, rel))
    tests.source_build_phase.add_file_reference(ref)
  end
  # 测试目标要编译 `import SYPlayerKit`，所以同样依赖包产物；动态 framework 由
  # SwiftPM 内嵌在宿主 App 里，测试 bundle 运行时用的就是 App 里那一份。
  link_package_product(project, tests, package_ref)
  tests.add_dependency(app)

  app_settings = {
    'PRODUCT_BUNDLE_IDENTIFIER' => cfg[:bundle_id],
    'PRODUCT_NAME' => name,
    'SWIFT_VERSION' => '5.0',
    'GENERATE_INFOPLIST_FILE' => 'YES',
    'MARKETING_VERSION' => '1.0',
    'CURRENT_PROJECT_VERSION' => '1',
    'CODE_SIGN_STYLE' => 'Automatic',
    'DEVELOPMENT_TEAM' => '',
  }
  tests_settings = {
    'PRODUCT_BUNDLE_IDENTIFIER' => "#{cfg[:bundle_id]}.tests",
    'PRODUCT_NAME' => "#{name}Tests",
    'SWIFT_VERSION' => '5.0',
    'GENERATE_INFOPLIST_FILE' => 'YES',
    'CODE_SIGN_STYLE' => 'Automatic',
    'DEVELOPMENT_TEAM' => '',
    'BUNDLE_LOADER' => '$(TEST_HOST)',
  }

  if cfg[:sdk] == :ios
    # 同一个 iOS 示例也跑 Mac Catalyst：SYPlayerKit 的 Catalyst 切片只有 arm64、
    # 最低 14.0（包清单声明 .macCatalyst(.v14)；不写的话 iOS 13.0 会被映射成
    # Catalyst 13.1，低于包的下限，解析阶段就被拒绝）。Catalyst 上的 App 是 macOS
    # 布局（Contents/MacOS、Contents/Frameworks），测试宿主与 rpath 按 SDK 分开写。
    catalyst = {
      'SUPPORTS_MACCATALYST' => 'YES',
      'DERIVE_MACCATALYST_PRODUCT_BUNDLE_IDENTIFIER' => 'NO',
      'IPHONEOS_DEPLOYMENT_TARGET[sdk=macosx*]' => '14.0',
      'ARCHS[sdk=macosx*]' => 'arm64',
    }
    app_settings.merge!(catalyst)
    tests_settings.merge!(catalyst)
    app_settings.merge!(
      'TARGETED_DEVICE_FAMILY' => '1,2',
      'INFOPLIST_KEY_UILaunchStoryboardName' => 'LaunchScreen',
      'INFOPLIST_KEY_UIApplicationSupportsIndirectInputEvents' => 'YES',
      'INFOPLIST_KEY_UISupportedInterfaceOrientations_iPhone' =>
        'UIInterfaceOrientationPortrait UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight',
      'INFOPLIST_KEY_UISupportedInterfaceOrientations_iPad' =>
        'UIInterfaceOrientationPortrait UIInterfaceOrientationPortraitUpsideDown ' \
        'UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight',
      'LD_RUNPATH_SEARCH_PATHS' => ['$(inherited)', '@executable_path/Frameworks'],
      'LD_RUNPATH_SEARCH_PATHS[sdk=macosx*]' => ['$(inherited)', '@executable_path/../Frameworks'],
    )
    tests_settings.merge!(
      'TARGETED_DEVICE_FAMILY' => '1,2',
      'TEST_HOST' => "$(BUILT_PRODUCTS_DIR)/#{name}.app/#{name}",
      'TEST_HOST[sdk=macosx*]' => "$(BUILT_PRODUCTS_DIR)/#{name}.app/Contents/MacOS/#{name}",
      'LD_RUNPATH_SEARCH_PATHS' => ['$(inherited)', '@executable_path/Frameworks',
                                    '@loader_path/Frameworks'],
      'LD_RUNPATH_SEARCH_PATHS[sdk=macosx*]' => ['$(inherited)', '@executable_path/../Frameworks',
                                                 '@loader_path/../Frameworks'],
    )
  else
    # SYPlayerKit 的原生 macOS 切片只有 arm64（不支持 Intel Mac），两个目标都钉死
    # arm64；不钉的话 Release 默认连 x86_64 一起编，链接时找不到 x86_64 切片。
    app_settings.merge!(
      'ARCHS' => 'arm64',
      'COMBINE_HIDPI_IMAGES' => 'YES',
      'INFOPLIST_KEY_NSPrincipalClass' => 'NSApplication',
      'LD_RUNPATH_SEARCH_PATHS' => ['$(inherited)', '@executable_path/../Frameworks'],
    )
    tests_settings.merge!(
      'ARCHS' => 'arm64',
      'TEST_HOST' => "$(BUILT_PRODUCTS_DIR)/#{name}.app/Contents/MacOS/#{name}",
      'LD_RUNPATH_SEARCH_PATHS' => ['$(inherited)', '@executable_path/../Frameworks',
                                    '@loader_path/../Frameworks'],
    )
  end

  # new_target 会自动链一份 Foundation/Cocoa，路径写死某个 SDK 版本号
  # （DEVELOPER_DIR 下的 iPhoneOS18.0.sdk 之类）。Swift 目标按 import 自动链接，
  # 这份引用既多余、又会随 Xcode 升级失效，删掉。
  [app, tests].each do |t|
    t.frameworks_build_phase.files.select { |bf| bf.file_ref && bf.file_ref.source_tree == 'DEVELOPER_DIR' }
     .each do |bf|
       ref = bf.file_ref
       t.frameworks_build_phase.remove_build_file(bf)
       ref.remove_from_project if ref.build_files.empty?
     end
  end

  app.build_configurations.each { |c| c.build_settings.merge!(app_settings) }
  tests.build_configurations.each { |c| c.build_settings.merge!(tests_settings) }

  # 确定性 UUID：按对象在工程树里的位置算哈希，同样的输入每次生成逐字节相同，
  # `--check` 才能直接比文件内容。必须在建 scheme 之前做——scheme 里引用的是
  # target 的 UUID。
  #
  # 要做两遍：PBXContainerItemProxy 的哈希里含它的 remoteGlobalIDString（被依赖
  # target 的 UUID），第一遍算它时那个 UUID 还是随机的，于是代理与依赖对象的新
  # UUID 每次不同；第一遍结束后 remoteGlobalIDString 已换成确定值，第二遍全部稳定。
  project.predictabilize_uuids
  project.predictabilize_uuids

  scheme = Xcodeproj::XCScheme.new
  scheme.add_build_target(app, true)
  scheme.add_test_target(tests)
  scheme.set_launch_target(app)

  out_path = CHECK_MODE ? File.join(CHECK_DIR, "#{name}.xcodeproj") : project_path
  project.save(out_path)
  scheme.save_as(out_path, name, true)
  assert_relative_only(out_path)

  if CHECK_MODE
    compare_project(project_path, out_path)
  else
    puts "生成完成: #{project_path.sub("#{ROOT}/", '')}（目标：#{project.targets.map(&:name).join(', ')}）"
  end
end

# `--check`：生成到临时目录，与仓库里那份逐文件、逐字节比对（pbxproj 与共享
# scheme）。UUID 是确定性的，所以任何差异都是真实漂移：手改了工程、改了生成器
# 没重跑、或加了文件没登记进清单。
def compare_project(committed, generated)
  rel = committed.sub("#{ROOT}/", '')
  files = %w[project.pbxproj] +
          Dir.glob(File.join(generated, 'xcshareddata', 'xcschemes', '*.xcscheme'))
             .map { |f| f.sub("#{generated}/", '') }
  on_disk = Dir.glob(File.join(committed, 'xcshareddata', 'xcschemes', '*.xcscheme'))
               .map { |f| f.sub("#{committed}/", '') }
  (on_disk - files).each { |f| CHECK_FAILURES << "#{rel}/#{f}：磁盘上多出来，生成器不产出它" }
  files.each do |f|
    a = File.join(committed, f)
    b = File.join(generated, f)
    if !File.exist?(a)
      CHECK_FAILURES << "#{rel}/#{f}：磁盘上缺失"
    elsif File.binread(a) != File.binread(b)
      CHECK_FAILURES << "#{rel}/#{f}：与生成器产出不一致"
    end
  end
end

# 产物里不许出现的东西：绝对路径（/Users、/Volumes、以 / 开头的 path 属性、本机
# 仓库根前缀），以及绕过 SwiftPM 直接引用库的痕迹（build-ffmpeg、swift/SYPlayerKit、
# 任何 .xcframework）。tools/check-spm.sh 用**同一个正则**（FORBIDDEN_IN_PROJECT）
# 对入库的 pbxproj 与 xcscheme 再查一遍，两处改一处要同步改另一处。
FORBIDDEN_IN_PROJECT = %r{/Users/|/Volumes/|path = "?/|build-ffmpeg|swift/SYPlayerKit|\.xcframework}

def assert_relative_only(project_path)
  Dir.glob(File.join(project_path, '**', '*')).select { |f| File.file?(f) }.each do |f|
    text = File.read(f)
    bad = text.lines.each_with_index.select do |line, _|
      line.include?(ROOT) || line =~ FORBIDDEN_IN_PROJECT
    end
    next if bad.empty?
    bad.each { |line, i| warn "#{f.sub("#{ROOT}/", '')}:#{i + 1}: #{line.strip}" }
    abort "生成的工程里出现了绝对路径：#{f}"
  end
end

PLATFORMS.each_value { |cfg| build(cfg) }

if CHECK_MODE
  FileUtils.rm_rf(CHECK_DIR)
  if CHECK_FAILURES.empty?
    puts '全部通过（两个示例工程与生成器产出逐字节一致）。'
  else
    warn "示例工程与生成器不一致（重跑 ruby examples/generate.rb 并提交）："
    CHECK_FAILURES.each { |f| warn "  - #{f}" }
    exit 1
  end
end
