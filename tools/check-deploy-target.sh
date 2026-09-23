#!/bin/bash
# 验证 dl 层实现源文件在 iOS 13 部署目标下能通过严格 availability 检查。
# 禁止 std::format / atomic wait-notify，
# 由编译器 -Werror=unguarded-availability-new 强制，不靠人记。
P="$(cd "$(dirname "$0")/.." && pwd)"
CXX=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++
SDK_FALLBACK=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS26.5.sdk

# xcode-select 可能指向 Command Line Tools（没有 iOS SDK），xcrun 要能退回 Xcode.app。
SDK_IOS=$(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null) || true
if [ -z "$SDK_IOS" ] || [ ! -d "$SDK_IOS" ]; then
  SDK_IOS=$(DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
            xcrun --sdk iphoneos --show-sdk-path 2>/dev/null) || true
fi
if [ -z "$SDK_IOS" ] || [ ! -d "$SDK_IOS" ]; then
  SDK_IOS="$SDK_FALLBACK"
fi

if [ ! -x "$CXX" ]; then
  echo "找不到 clang++：$CXX"
  echo "这是本机 Xcode 工具链环境问题，不是代码问题。"
  exit 1
fi
if [ ! -d "$SDK_IOS" ]; then
  echo "找不到 iPhoneOS SDK（本机 Xcode / 开发者目录环境问题，不是代码问题）。"
  echo "已尝试：xcrun --sdk iphoneos --show-sdk-path"
  echo "        DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun --sdk iphoneos --show-sdk-path"
  echo "        硬编码回退：$SDK_FALLBACK"
  echo "请安装 Xcode 并确保 iOS SDK 可用。若 xcode-select 指向 Command Line Tools，可设置"
  echo "  DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer"
  exit 1
fi

# src/media/ 挂了 FFmpeg 头文件（AvioBridge），产物不进仓库，与 CMakeLists.txt
# 探测同一份 xcframework——但要用 ios-arm64 那个 slice：本脚本的编译目标是
# arm64-apple-ios13.0，不是 macOS。src/media/ 该不该受这条纪律约束：该——
# 它会随 Demuxer 一起上 iOS，正是需要 iOS 13 availability 检查的代码，
# 所以是补 include 路径，不是把它从扫描里排除。
SYP_FFMPEG_XCFRAMEWORK="$P/build-ffmpeg/out/SYFFmpeg.xcframework"
SYP_FFMPEG_IOS_HEADERS="$SYP_FFMPEG_XCFRAMEWORK/ios-arm64/SYFFmpeg.framework/Headers"
SYP_HAVE_FFMPEG_IOS=0
if [ -f "$SYP_FFMPEG_IOS_HEADERS/libavformat/avio.h" ] \
   && [ -f "$SYP_FFMPEG_IOS_HEADERS/syp_ffmpeg_prefix.h" ]; then
  SYP_HAVE_FFMPEG_IOS=1
fi
# 与真实构建一致：包含 FFmpeg 头的编译单元先强制包含映射头（原名 → syp_ 名）。
# -isystem：与 CMakeLists.txt 里的处理一致，FFmpeg 头文件自身在
# -Wconversion/-Wsign-conversion 下会报警，这条纪律管的是自己的代码。
SYP_FFMPEG_ARGS=(-isystem "$SYP_FFMPEG_IOS_HEADERS"
                 -include "$SYP_FFMPEG_IOS_HEADERS/syp_ffmpeg_prefix.h")

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

fail=0
count=0
skipped=0
echo "== iOS 13 / arm64 部署目标（-fsyntax-only）=="

# 自动纳入 src/ 下全部实现，新增 .cpp / .mm 无需改脚本
check_one() {
  local f="$1"; shift
  count=$((count + 1))
  local rel="${f#"$P"/}"
  if "$CXX" -std=c++23 -target arm64-apple-ios13.0 -isysroot "$SDK_IOS" \
      -Werror=unguarded-availability -Werror=unguarded-availability-new \
      -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion \
      -I"$P/include" -I"$P/src" \
      "$@" \
      -fsyntax-only "$f" 2>"$T/err"; then
    printf "  ✅ %s\n" "$rel"
  else
    printf "  ❌ %s\n" "$rel"
    sed 's/^/       /' "$T/err"
    fail=1
  fi
}

while IFS= read -r f; do
  [ -z "$f" ] && continue
  case "$f" in
    "$P/src/media/"*)
      media_rel="${f#"$P"/}"
      if [ "$SYP_HAVE_FFMPEG_IOS" -eq 1 ]; then
        check_one "$f" "${SYP_FFMPEG_ARGS[@]}"
      else
        printf "  ⏭️  %s（跳过：未找到 FFmpeg ios-arm64 xcframework，运行\n" "$media_rel"
        printf "       tools/build-ffmpeg.sh 后可纳入检查）\n"
        skipped=$((skipped + 1))
      fi
      ;;
    *)
      check_one "$f"
      ;;
  esac
done <<EOF
$(find "$P/src" -type f -name '*.cpp' | LC_ALL=C sort)
EOF

while IFS= read -r f; do
  [ -z "$f" ] && continue
  case "$f" in
    "$P/src/platform/apple/audio_unit_sink.mm")
      # AudioUnitSink 间接 #include "media/audio_sink.h" ->
      # "media/frame.h" -> FFmpeg 头（libavutil/frame.h 等），还直接用
      # libswresample 做倍速重采样——跟上面 src/media/ 那个分支同一个
      # 理由，同一份 xcframework，只是这个文件不在 src/media/ 目录下，
      # 落不进那条 case 的路径前缀匹配，需要单独列一条。
      mm_rel="${f#"$P"/}"
      if [ "$SYP_HAVE_FFMPEG_IOS" -eq 1 ]; then
        check_one "$f" -x objective-c++ -fobjc-arc "${SYP_FFMPEG_ARGS[@]}"
      else
        printf "  ⏭️  %s（跳过：未找到 FFmpeg ios-arm64 xcframework，运行\n" "$mm_rel"
        printf "       tools/build-ffmpeg.sh 后可纳入检查）\n"
        skipped=$((skipped + 1))
      fi
      ;;
    "$P/src/platform/apple/vt_decode_backend.mm")
      # vt_decode_backend.mm 经 vt_decode_backend.h ->
      # media/hw/hw_decode_backend.h 直接 #include <libavcodec/avcodec.h>，
      # 跟上面 audio_unit_sink.mm 同一个理由，同一份 xcframework。
      mm_rel="${f#"$P"/}"
      if [ "$SYP_HAVE_FFMPEG_IOS" -eq 1 ]; then
        check_one "$f" -x objective-c++ -fobjc-arc "${SYP_FFMPEG_ARGS[@]}"
      else
        printf "  ⏭️  %s（跳过：未找到 FFmpeg ios-arm64 xcframework，运行\n" "$mm_rel"
        printf "       tools/build-ffmpeg.sh 后可纳入检查）\n"
        skipped=$((skipped + 1))
      fi
      ;;
    "$P/src/platform/apple/metal_renderer.mm")
      # MetalRenderer 同样间接 #include "media/frame.h" ->
      # FFmpeg 头，跟上面 audio_unit_sink.mm 同一个理由。额外还要能找到
      # video_shaders.metal 内嵌头——metal_renderer.mm 用 #include
      # "syp_video_shaders_metal_source.h"，这个头不进仓库，是构建期生成的
      # （顶层 CMakeLists.txt 走同一份 tools/gen-embedded-header.sh）。
      # standalone 语法检查不经过 CMake，这里现场生成一份到 $T 底下。
      mm_rel="${f#"$P"/}"
      gen_dir="$T/generated/syp"
      bash "$P/tools/gen-embedded-header.sh" \
           "$P/src/platform/apple/video_shaders.metal" \
           "$gen_dir/syp_video_shaders_metal_source.h" \
           "syp::platform" "kVideoShadersMetalSource"
      if [ "$SYP_HAVE_FFMPEG_IOS" -eq 1 ]; then
        check_one "$f" -x objective-c++ -fobjc-arc "${SYP_FFMPEG_ARGS[@]}" \
                   -I"$gen_dir"
      else
        printf "  ⏭️  %s（跳过：未找到 FFmpeg ios-arm64 xcframework，运行\n" "$mm_rel"
        printf "       tools/build-ffmpeg.sh 后可纳入检查）\n"
        skipped=$((skipped + 1))
      fi
      ;;
    *)
      check_one "$f" -x objective-c++ -fobjc-arc
      ;;
  esac
done <<EOF
$(find "$P/src" -type f -name '*.mm' | LC_ALL=C sort)
EOF

if [ "$count" -eq 0 ] && [ "$skipped" -eq 0 ]; then
  echo "  ❌ src/ 下没有找到 .cpp / .mm 文件"
  fail=1
fi

echo
if [ "$fail" -eq 0 ]; then
  if [ "$skipped" -eq 0 ]; then
    echo "全部通过（$count 个源文件，arm64-apple-ios13.0）。"
  else
    echo "全部通过（$count 个源文件，arm64-apple-ios13.0；另有 $skipped 个因缺 FFmpeg ios-arm64 xcframework 被跳过，不算失败）。"
  fi
else
  echo "有失败项。"
fi
exit "$fail"
