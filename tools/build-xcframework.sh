#!/usr/bin/env bash
# 构建分发用的 SYPlayerKit.xcframework（动态 framework，库演进模式，四个平台切片）。
#
# 流程：
#   1. 用 demo/generate_xcodeprojects.rb --dist 在 build-spm/project/ 下生成分发工程；
#   2. 逐平台 xcodebuild archive（iOS 真机、iOS 模拟器、Mac Catalyst、原生 macOS），
#      每次把 SYFFmpeg.xcframework 对应 slice 的 framework / 头文件搜索路径与映射头
#      syp_ffmpeg_prefix.h 从命令行传入；
#   3. xcodebuild -create-xcframework 合成 build-spm/out/SYPlayerKit.xcframework（带 dSYM）；
#   4. 把 SYFFmpeg.xcframework 拷到 build-spm/out/，两者一起作为 SwiftPM 的二进制目标；
#      拷贝件里每个 slice 删掉 Headers（FFmpeg 头与映射头只在构建 SYPlayerKit 时
#      用，接入方从不编译它们）、放进隐私清单，xcframework 根目录放 LGPL 许可证
#      正文与 NOTICE（build-ffmpeg/out 原件不动），再用 check-ffmpeg-symbols.sh
#      复核分发件；
#   5. 两个 xcframework 各打一个 zip（不带 AppleDouble / 扩展属性），
#      `swift package compute-checksum` 算校验和，
#      写 build-spm/out/checksums.txt；带 --version 时同步回填 Package.swift。
#
# 每一步都检查退出码与产物是否真的存在；xcodebuild 的完整输出落在
# build-spm/logs/，失败时打印日志路径与其中的 error: 行。
#
# 用法：tools/build-xcframework.sh [--version X.Y.Z]
#   不带 --version：只构建产物，Package.swift 不动，分发工程里的
#     MARKETING_VERSION 用默认值 0.1.0。
#   带 --version：额外把这次实跑出的两个 zip 的 checksum 与该版本号回填进
#     根目录 Package.swift 的远程模式分支。正式发版时，回填用的这两个 zip
#     必须原样传到 GitHub Release——checksum 是对文件内容算的，Release 上
#     传的 zip 只要有一个字节不同，SwiftPM 拉取时就会因 checksum 不匹配报错
#     拒绝，所以不能"回填完再重新打包"或"拿旧 zip 传新 checksum"。
#
# 退出码：0 成功；1 构建或校验失败；2 前置条件不满足（缺 SYFFmpeg.xcframework 等）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SPM_DIR="$ROOT/build-spm"
PROJECT_DIR="$SPM_DIR/project"
PROJECT="$PROJECT_DIR/SYPlayerKitDist.xcodeproj"
ARCHIVES="$SPM_DIR/archives"
DERIVED="$SPM_DIR/DerivedData"
LOGS="$SPM_DIR/logs"
OUT="$SPM_DIR/out"
FFMPEG_XCF="$ROOT/build-ffmpeg/out/SYFFmpeg.xcframework"
FFMPEG_PRIVACY="$ROOT/tools/dist/SYFFmpeg/PrivacyInfo.xcprivacy"
KIT_XCF="$OUT/SYPlayerKit.xcframework"
PACKAGE_SWIFT="$ROOT/Package.swift"

die() { echo "错误：$*" >&2; exit 1; }

VERSION="0.1.0"
DO_BACKFILL=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --version)
      [ "$#" -ge 2 ] || die "--version 需要一个参数，例如 --version 0.1.0"
      VERSION="$2"
      # 版本号会进 Release 下载地址与 git tag，只收纯 X.Y.Z。
      [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] \
        || die "--version 必须是 X.Y.Z 形式的纯数字版本号（收到：${VERSION}）"
      DO_BACKFILL=1
      shift 2
      ;;
    *)
      die "未知参数：$1（用法：$0 [--version X.Y.Z]）"
      ;;
  esac
done

if [ ! -f "$FFMPEG_XCF/Info.plist" ]; then
  echo "缺少 $FFMPEG_XCF" >&2
  echo "先跑 tools/build-ffmpeg.sh 产出 SYFFmpeg.xcframework。" >&2
  exit 2
fi
command -v xcodebuild >/dev/null || { echo "找不到 xcodebuild" >&2; exit 2; }
command -v ruby >/dev/null || { echo "找不到 ruby" >&2; exit 2; }
command -v swift >/dev/null || { echo "找不到 swift" >&2; exit 2; }
[ -f "$FFMPEG_PRIVACY" ] || { echo "缺少 FFmpeg 隐私清单：$FFMPEG_PRIVACY" >&2; exit 2; }
FFMPEG_LICENSE="$("$ROOT/tools/build-ffmpeg.sh" --print-license)" \
  || { echo "拿不到 FFmpeg 许可证正文：先跑 tools/build-ffmpeg.sh" >&2; exit 2; }
[ -f "$FFMPEG_LICENSE" ] || { echo "FFmpeg 许可证正文不存在：$FFMPEG_LICENSE" >&2; exit 2; }
if [ "$DO_BACKFILL" -eq 1 ]; then
  [ -f "$PACKAGE_SWIFT" ] || die "要回填但找不到 $PACKAGE_SWIFT"
fi

# 四个平台：名字 | archive 目标 | FFmpeg slice | 额外 build setting。
# Catalyst 与原生 macOS 只出 arm64：FFmpeg 这两个 slice 只有 arm64，
# 不限定 ARCHS 的话 xcodebuild 会连 x86_64 一起编，链接时找不到 FFmpeg 符号。
PLATFORMS=(
  "ios|generic/platform=iOS|ios-arm64|"
  "ios-simulator|generic/platform=iOS Simulator|ios-arm64_x86_64-simulator|"
  "maccatalyst|generic/platform=macOS,variant=Mac Catalyst|ios-arm64-maccatalyst|ARCHS=arm64"
  "macos|generic/platform=macOS|macos-arm64|ARCHS=arm64"
)

# 重复运行从干净状态开始：旧工程、旧归档、旧产物全部删掉。
rm -rf "$SPM_DIR"
mkdir -p "$PROJECT_DIR" "$ARCHIVES" "$DERIVED" "$LOGS" "$OUT"

echo "==> 生成分发工程"
ruby "$ROOT/demo/generate_xcodeprojects.rb" --dist "$PROJECT_DIR" --dist-version "$VERSION"
[ -f "$PROJECT/project.pbxproj" ] || die "分发工程未生成：$PROJECT"
[ -f "$PROJECT/xcshareddata/xcschemes/SYPlayerKit.xcscheme" ] || die "分发工程缺共享 scheme SYPlayerKit"

CREATE_ARGS=()
for entry in "${PLATFORMS[@]}"; do
  IFS='|' read -r name destination slice extra <<<"$entry"
  slice_dir="$FFMPEG_XCF/$slice"
  headers="$slice_dir/SYFFmpeg.framework/Headers"
  prefix_header="$headers/syp_ffmpeg_prefix.h"
  [ -d "$slice_dir/SYFFmpeg.framework" ] || die "SYFFmpeg.xcframework 缺 slice：${slice}"
  # macOS / Catalyst slice 是版本化布局，Headers 是指向 Versions/Current/Headers
  # 的符号链接；这里按"能解析到目录"判定，两种布局都成立。
  [ -d "$headers" ] || die "SYFFmpeg slice 没有可用的 Headers 目录：${headers}"
  [ -f "$prefix_header" ] || die "SYFFmpeg slice 缺映射头：${prefix_header}"

  archive="$ARCHIVES/$name.xcarchive"
  log="$LOGS/archive-$name.log"
  extra_args=()
  [ -n "$extra" ] && extra_args+=("$extra")

  echo "==> archive ${name}（${destination}，FFmpeg slice ${slice}）"
  rc=0
  xcodebuild archive \
    -project "$PROJECT" \
    -scheme SYPlayerKit \
    -configuration Release \
    -destination "$destination" \
    -archivePath "$archive" \
    -derivedDataPath "$DERIVED/$name" \
    SKIP_INSTALL=NO \
    BUILD_LIBRARY_FOR_DISTRIBUTION=YES \
    FRAMEWORK_SEARCH_PATHS="$slice_dir" \
    SYSTEM_HEADER_SEARCH_PATHS="$headers" \
    SYP_FFMPEG_PREFIX_HEADER="$prefix_header" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" \
    ${extra_args[@]+"${extra_args[@]}"} \
    >"$log" 2>&1 || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "xcodebuild archive $name 失败（exit ${rc}），日志：$log" >&2
    grep -n "error:" "$log" | head -40 >&2 || true
    exit 1
  fi
  grep -q "\*\* ARCHIVE SUCCEEDED \*\*" "$log" || die "archive $name 退出码为 0 但日志里没有 ARCHIVE SUCCEEDED：$log"

  fw="$archive/Products/Library/Frameworks/SYPlayerKit.framework"
  dsym="$archive/dSYMs/SYPlayerKit.framework.dSYM"
  [ -d "$fw" ] || die "archive $name 里没有 SYPlayerKit.framework：$fw"
  [ -d "$dsym" ] || die "archive $name 里没有 dSYM：$dsym"
  shopt -s nullglob
  ifaces=("$fw"/Modules/SYPlayerKit.swiftmodule/*.swiftinterface)
  shopt -u nullglob
  [ "${#ifaces[@]}" -gt 0 ] || die "archive $name 的 SYPlayerKit.framework 没有 .swiftinterface（库演进模式没生效？）"
  echo "    framework 与 ${#ifaces[@]} 个 .swiftinterface 就位"

  CREATE_ARGS+=(-framework "$fw" -debug-symbols "$dsym")
done

echo "==> 合成 SYPlayerKit.xcframework"
log="$LOGS/create-xcframework.log"
rc=0
xcodebuild -create-xcframework "${CREATE_ARGS[@]}" -output "$KIT_XCF" >"$log" 2>&1 || rc=$?
if [ "$rc" -ne 0 ]; then
  echo "xcodebuild -create-xcframework 失败（exit ${rc}），日志：$log" >&2
  cat "$log" >&2
  exit 1
fi
[ -f "$KIT_XCF/Info.plist" ] || die "xcframework 没有 Info.plist：$KIT_XCF"

# 合成结果逐个 slice 复核：四个 LibraryIdentifier 都在，每个都带 framework 与 .swiftinterface。
# Info.plist 先整段读进变量再匹配：`plutil | grep -q` 在 pipefail 下可能因
# grep 提前退出让 plutil 吃 SIGPIPE，把"匹配成功"误判成失败。
libs_xml="$(plutil -extract AvailableLibraries xml1 -o - "$KIT_XCF/Info.plist")"
for id in ios-arm64 ios-arm64_x86_64-simulator ios-arm64-maccatalyst macos-arm64; do
  fw="$KIT_XCF/$id/SYPlayerKit.framework"
  [ -d "$fw" ] || die "xcframework 缺 slice：$id"
  shopt -s nullglob
  ifaces=("$fw"/Modules/SYPlayerKit.swiftmodule/*.swiftinterface)
  shopt -u nullglob
  [ "${#ifaces[@]}" -gt 0 ] || die "xcframework slice $id 没有 .swiftinterface"
  [[ "$libs_xml" == *"<string>$id</string>"* ]] \
    || die "xcframework Info.plist 里没有 LibraryIdentifier $id"
done

# 隐私清单与最低系统版本逐 slice 核对。iOS / 模拟器是平铺 bundle，资源在
# framework 根目录；Catalyst / macOS 是版本化 bundle，资源在 Versions/A/Resources。
# Catalyst 的 Info.plist 用 macOS 的键，值必须是 macOS 版本号（arm64 Catalyst 14
# 对应 macOS 11）。
# 参数：xcframework 路径、framework 名、要核对的隐私清单源文件。
verify_slices() {
  local xcf="$1" name="$2" privacy_src="$3" id fw res key want got
  for id in ios-arm64 ios-arm64_x86_64-simulator ios-arm64-maccatalyst macos-arm64; do
    fw="$xcf/$id/$name.framework"
    case "$id" in
      ios-arm64|ios-arm64_x86_64-simulator)
        res="$fw"; key="MinimumOSVersion"; want="13.0" ;;
      *)
        res="$fw/Versions/A/Resources"; key="LSMinimumSystemVersion"; want="11.0" ;;
    esac
    [ -f "$res/PrivacyInfo.xcprivacy" ] || die "$name slice $id 缺隐私清单：$res/PrivacyInfo.xcprivacy"
    cmp -s "$res/PrivacyInfo.xcprivacy" "$privacy_src" \
      || plutil -convert xml1 -o - "$res/PrivacyInfo.xcprivacy" | cmp -s - <(plutil -convert xml1 -o - "$privacy_src") \
      || die "$name slice $id 的隐私清单与 $privacy_src 内容不一致"
    got="$(plutil -extract "$key" raw -o - "$res/Info.plist" 2>/dev/null || true)"
    [ "$got" = "$want" ] || die "$name slice $id 的 Info.plist $key=${got:-<无>}，期望 $want"
    echo "    $name ${id}：隐私清单就位，${key}=${got}"
  done
}
verify_slices "$KIT_XCF" SYPlayerKit "$ROOT/swift/SYPlayerKit/PrivacyInfo.xcprivacy"

echo "==> 拷贝 SYFFmpeg.xcframework（去头文件，补隐私清单、许可证与 NOTICE）"
OUT_FFMPEG_XCF="$OUT/SYFFmpeg.xcframework"
ditto "$FFMPEG_XCF" "$OUT_FFMPEG_XCF"
[ -f "$OUT_FFMPEG_XCF/Info.plist" ] || die "SYFFmpeg.xcframework 拷贝失败"
# 只改拷贝件。SYFFmpeg 的 framework 没有 _CodeSignature 封条（二进制只有链接器
# 打的 ad-hoc 签名），增删 bundle 里的资源与头文件不会让签名失效；接入方 App
# 内嵌时由 Xcode 重新签名。
#
# 分发件不带 Headers：FFmpeg 头与映射头 syp_ffmpeg_prefix.h 只在构建 SYPlayerKit
# 时用；接入方从不编译 FFmpeg 头，带着它们只会让人误以为可以直接调我们这份
# FFmpeg（导出名全是 syp_ 前缀，照原名调用链接不上），还可能和接入方自带的
# FFmpeg 头在搜索路径里撞车。版本化布局（Catalyst / macOS）要把
# Versions/A/Headers 与顶层的 Headers 符号链接一起删，只删一头会留下悬空链接，
# 签名时报 bundle 结构不合法。
for id in ios-arm64 ios-arm64_x86_64-simulator; do
  fw="$OUT_FFMPEG_XCF/$id/SYFFmpeg.framework"
  rm -rf "$fw/Headers"
  cp "$FFMPEG_PRIVACY" "$fw/PrivacyInfo.xcprivacy"
done
for id in ios-arm64-maccatalyst macos-arm64; do
  fw="$OUT_FFMPEG_XCF/$id/SYFFmpeg.framework"
  [ -L "$fw/Headers" ] || die "SYFFmpeg slice $id 的顶层 Headers 不是符号链接，布局与预期不符：$fw"
  rm -f "$fw/Headers"
  rm -rf "$fw/Versions/A/Headers"
  cp "$FFMPEG_PRIVACY" "$fw/Versions/A/Resources/PrivacyInfo.xcprivacy"
done
verify_slices "$OUT_FFMPEG_XCF" SYFFmpeg "$FFMPEG_PRIVACY"
# LGPL 分发义务：随二进制附许可证正文与 NOTICE（版本、源码地址、configure 开关）。
cp "$FFMPEG_LICENSE" "$OUT_FFMPEG_XCF/$(basename "$FFMPEG_LICENSE")"
"$ROOT/tools/build-ffmpeg.sh" --print-notice >"$OUT_FFMPEG_XCF/NOTICE"
grep -q "GNU LESSER GENERAL PUBLIC LICENSE" "$OUT_FFMPEG_XCF/$(basename "$FFMPEG_LICENSE")" \
  || die "FFmpeg 许可证正文不是 LGPL：$OUT_FFMPEG_XCF/$(basename "$FFMPEG_LICENSE")"
grep -q "releases/ffmpeg-" "$OUT_FFMPEG_XCF/NOTICE" || die "NOTICE 缺源码地址：$OUT_FFMPEG_XCF/NOTICE"
echo "    许可证与 NOTICE 就位：$(basename "$FFMPEG_LICENSE")、NOTICE"

# 分发件复核：导出表全带 _syp_ 前缀、不带头文件、删头后结构经得起 codesign；
# SYPlayerKit 四个 slice 对 FFmpeg 的未定义引用全部带前缀（没有绕过映射头）。
# 映射头用构建产物里那份（分发件里已经删了）。
KIT_BINS=()
for id in ios-arm64 ios-arm64_x86_64-simulator; do
  KIT_BINS+=("$KIT_XCF/$id/SYPlayerKit.framework/SYPlayerKit")
done
for id in ios-arm64-maccatalyst macos-arm64; do
  KIT_BINS+=("$KIT_XCF/$id/SYPlayerKit.framework/Versions/A/SYPlayerKit")
done
log="$LOGS/check-ffmpeg-symbols.log"
rc=0
"$ROOT/tools/check-ffmpeg-symbols.sh" --dist --expect-slices 4 \
  --prefix-header "$FFMPEG_XCF/macos-arm64/SYFFmpeg.framework/Headers/syp_ffmpeg_prefix.h" \
  "$OUT_FFMPEG_XCF" "${KIT_BINS[@]}" >"$log" 2>&1 || rc=$?
if [ "$rc" -ne 0 ]; then
  echo "分发件的符号 / 结构复核失败（exit ${rc}），日志：$log" >&2
  cat "$log" >&2
  exit 1
fi
echo "    符号与结构复核通过（日志：${log}）"

echo "==> 打包 zip"
KIT_ZIP="$OUT/SYPlayerKit.xcframework.zip"
FFMPEG_ZIP="$OUT/SYFFmpeg.xcframework.zip"
rm -f "$KIT_ZIP" "$FFMPEG_ZIP"
# -k --keepParent：zip 内顶层是 xcframework 目录本身，跟 GitHub Release 惯例
# 与 SwiftPM 解压后找目录名的方式一致。cd 到 $OUT 再打包，zip 里就不会带上
# 宿主机的绝对路径前缀。
# --norsrc --noextattr --noacl：不打资源分支、扩展属性与 ACL。ditto 默认会把
# 它们写成 __MACOSX/ 下的 ._* AppleDouble 条目，解压到非 APFS 或被别的工具
# 解压时会变成一堆多余文件。
for name in SYPlayerKit SYFFmpeg; do
  ( cd "$OUT" && ditto -c -k --norsrc --noextattr --noacl --keepParent \
      "$name.xcframework" "$name.xcframework.zip" )
  [ -f "$OUT/$name.xcframework.zip" ] || die "$name.xcframework.zip 打包失败"
  # 读完整份清单再数，不用 grep -q（pipefail 下提前退出会让 zipinfo 吃 SIGPIPE）。
  listing="$(zipinfo -1 "$OUT/$name.xcframework.zip")"
  appledouble="$(grep -c '/\._\|^\._\|__MACOSX' <<<"$listing" || true)"
  [ "$appledouble" = "0" ] || die "$name.xcframework.zip 里有 ${appledouble} 个 AppleDouble 条目"
  echo "    $name.xcframework.zip：$(wc -l <<<"$listing" | tr -d ' ') 个条目，AppleDouble 0"
done

echo "==> 计算 checksum"
KIT_CHECKSUM="$(swift package compute-checksum "$KIT_ZIP")"
FFMPEG_CHECKSUM="$(swift package compute-checksum "$FFMPEG_ZIP")"
[ -n "$KIT_CHECKSUM" ] || die "SYPlayerKit.xcframework.zip 的 checksum 为空"
[ -n "$FFMPEG_CHECKSUM" ] || die "SYFFmpeg.xcframework.zip 的 checksum 为空"

CHECKSUMS_FILE="$OUT/checksums.txt"
{
  echo "SYPlayerKit.xcframework.zip $KIT_CHECKSUM"
  echo "SYFFmpeg.xcframework.zip $FFMPEG_CHECKSUM"
} >"$CHECKSUMS_FILE"
[ -f "$CHECKSUMS_FILE" ] || die "checksums.txt 没写出来：$CHECKSUMS_FILE"

if [ "$DO_BACKFILL" -eq 1 ]; then
  echo "==> 回填 Package.swift（version=${VERSION}）"
  ruby -e '
content = File.read(ARGV[0])
replacements = {
  /^let version = "[^"]*"$/          => %(let version = "#{ARGV[1]}"),
  /^let kitChecksum = "[^"]*"$/      => %(let kitChecksum = "#{ARGV[2]}"),
  /^let ffmpegChecksum = "[^"]*"$/   => %(let ffmpegChecksum = "#{ARGV[3]}"),
}
replacements.each do |pattern, replacement|
  abort "找不到匹配行：#{pattern.source}" unless content =~ pattern
  content = content.sub(pattern, replacement)
end
File.write(ARGV[0], content)
' "$PACKAGE_SWIFT" "$VERSION" "$KIT_CHECKSUM" "$FFMPEG_CHECKSUM"
  # 改完必须真的落进文件再往下走，不能只信 ruby 的退出码。
  grep -qF "let version = \"$VERSION\"" "$PACKAGE_SWIFT" || die "version 回填没有落进 Package.swift"
  grep -qF "let kitChecksum = \"$KIT_CHECKSUM\"" "$PACKAGE_SWIFT" || die "kitChecksum 回填没有落进 Package.swift"
  grep -qF "let ffmpegChecksum = \"$FFMPEG_CHECKSUM\"" "$PACKAGE_SWIFT" || die "ffmpegChecksum 回填没有落进 Package.swift"
  echo "    version / kitChecksum / ffmpegChecksum 已回填并核对"
fi

echo "完成："
echo "  $KIT_XCF"
echo "  $OUT/SYFFmpeg.xcframework"
echo "  $KIT_ZIP"
echo "  $FFMPEG_ZIP"
echo "  $CHECKSUMS_FILE"
