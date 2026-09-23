#!/usr/bin/env bash
# 从官方 release tarball 下载 FFmpeg，校验 sha256，按五个 slice 交叉编译，
# 再打成单个 SYFFmpeg.xcframework，给 iOS 播放器的解封装/解码、以及 macOS
# headless 验证工具用。
#
# 产物全部落在仓库根下的 build-ffmpeg/（已被 .gitignore 的 build-*/ 覆盖），
# 源码和二进制都不要提交进仓库。
#
# 设计取舍（已定死，不要在本脚本里改）：
#   - 版本钉死 8.1.2（Hoare），官方 tarball，不 git clone、不 submodule。
#   - 默认不改源码；裁剪只靠 configure 的 --disable-everything + 按需
#     --enable-*（组件清单见下方 FF_COMPONENTS）。确需改源码时只能走
#     tools/ffmpeg-patches/*.patch（规则见该目录的 README.md）：解压后按文件名
#     顺序打上，任一失败即退出，NOTICE 自动列出打了哪些补丁、改了哪些文件。
#   - 只编一份「优化 + 带符号」：不关优化、不加 --disable-debug、加
#     --disable-stripping，相当于 RelWithDebInfo；Debug/Release 共用。
#     最终 App 出包再 strip。
#   - License 走 LGPL：--disable-gpl、不 --enable-nonfree，不带 x264 等
#     GPL-only 组件。
#   - 对外是**动态** SYFFmpeg.framework（见决策记录 D11.2）。LGPL 下
#     「动态链接」义务基本只剩标注与提供替换能力；静态链接则要求提供可重链接
#     的目标文件，是长期负担。
#
#     实现上是「内部静态编译 → 合成单个 dylib → 包成一个 framework」，
#     而不是 FFmpeg 自己的 --enable-shared。后者产出 libavcodec /
#     libavformat / libavutil / libswresample 四个互相依赖的 dylib，
#     而一个 xcframework 每个平台只能装一个库或一个 framework，
#     那就得打四个 xcframework、各自处理 install_name 与嵌入签名。
#     合成单个 framework 之后集成面只有一个，而 LGPL 要的
#     「使用者可以换掉这个库再跑起来」同样满足：framework 里只有 FFmpeg
#     的代码，使用者用本脚本重编、照同样方式合成、替换即可。
#
#   - 符号隔离：接入方的 App 可能自带另一份（任意版本的）FFmpeg。为了不和它
#     互相串用，合成 dylib 时把静态库里**每个已定义的全局符号**都用
#     ld -alias_list 起一个 syp_ 前缀的别名，再用 -exported_symbols_list
#     只导出这些别名：dylib 对外只有 _syp_*，原名留在库内（变成非导出的
#     本地符号）供 FFmpeg 自己互调，包括汇编。清单每次构建时用 nm 现生成，
#     不手写。framework 也因此改名 SYFFmpeg，不和接入方自带的 FFmpeg framework 撞名。
#     使用方编译时强制包含同源生成的 Headers/syp_ffmpeg_prefix.h
#     （#define 原名 syp_原名），业务代码照写原名。
#     tools/check-ffmpeg-symbols.sh 守住"导出表零未加前缀符号"。
#
# 用法：在 macOS + Xcode 环境下执行 ./tools/build-ffmpeg.sh
# 完整编一遍要几十分钟。
set -euo pipefail

P="$(cd "$(dirname "$0")/.." && pwd)"

# ---------------------------------------------------------------------------
# 版本钉死。换版本时只改这三处，并重新实测 sha256。
# ---------------------------------------------------------------------------
FFMPEG_VERSION="8.1.2"          # release 名 Hoare
FFMPEG_TARBALL_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_SHA256="464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"

BUILD_ROOT="$P/build-ffmpeg"
SRC_DIR="$BUILD_ROOT/src"
SLICES_DIR="$BUILD_ROOT/slices"
OUT_DIR="$BUILD_ROOT/out"
TARBALL="$SRC_DIR/ffmpeg-${FFMPEG_VERSION}.tar.xz"
SRC_UNPACK="$SRC_DIR/ffmpeg-${FFMPEG_VERSION}"

# 并行度可用环境变量压低，避免把机器占满：FFMPEG_BUILD_JOBS=4 ./tools/build-ffmpeg.sh
JOBS="${FFMPEG_BUILD_JOBS:-$(sysctl -n hw.ncpu)}"
HOST_ARCH="$(uname -m)"

IOS_MIN="13.0"
MACOS_MIN="11.0"
# Catalyst 的最低版本是 13.1，不是 13.0 —— 不能复用 IOS_MIN。
# 项目部署目标与 tools/check-deploy-target.sh 仍按 13.0，那条不动。
CATALYST_MIN="13.1"
# Catalyst framework 的 Info.plist 用 macOS 的键 LSMinimumSystemVersion，值必须
# 是 macOS 版本号，不能直接填 CATALYST_MIN（那是 iOS 版本号）。arm64 的 Catalyst
# 实际下限是 14.0（苹果芯片 Mac 从 macOS 11 起步，链接器把 minos 抬到 14.0，
# vtool -show-build 可核对），Catalyst 14 对应 macOS 11。
CATALYST_MACOS_MIN="11.0"

# ###########################################################################
# 组件清单，定稿后不要在本数组以外零散加 --enable-*；configure
# 若因内部依赖报错，把它要求的组件补进这个数组，并在提交信息里写清依据。
#
# 覆盖范围：本地 mp4/mov 文件（含 fMP4 分片）、HLS、老式 .ts 分片、
# H.264 视频、AAC 音频（含 mpegts 里常见的 LATM 封装）、file 协议。
#
# 只 --enable-protocol=http、不 --enable-protocol=https：
# hls.c:673 的 avio_find_protocol_name() 在调 io_open 之前先查编译进去的
# 协议表，要求协议名以 http 开头，所以 http 必须编进去——但纯粹为了过这个
# 名字检查，它的实现永不被使用（真正的连接由 HlsSession 的 io_open 发起，
# protocol_whitelist 只留 file，见 hls_session.cpp）。configure:4023 ——
# http_protocol_select="tcp_protocol"，只拉 tcp，不拉 TLS；而
# --enable-protocol=https 会拉 tls_protocol（configure:4027），后者必须绑一个
# TLS 后端（Apple 上是已废弃的 Secure Transport）。本项目在边界上把 https
# 改写成 http，绕开整套 TLS 依赖——tools/check-ffmpeg-no-tls.sh 就是用来
# 守住这一点的：证明产物里没有编进任何 TLS 后端的符号。
#
# 两个 VideoToolbox hwaccel 编进来。软解模式不挂 hw_device_ctx、不装 get_format，
# 不会进 hwaccel；硬解模式由 FFmpegVideoDecoder 的 get_format 闸门只接受硬件格式。
# ###########################################################################
FF_COMPONENTS=(
  --enable-demuxer=mov            # mp4 / mov / m4a / fMP4 分片都走这个 demuxer
  --enable-demuxer=hls            # HLS
  --enable-demuxer=mpegts         # 老式 .ts 分片；fMP4 分片复用上面的 mov
  --enable-decoder=h264
  --enable-decoder=aac
  --enable-parser=h264
  --enable-decoder=hevc
  --enable-parser=hevc
  --enable-hwaccel=h264_videotoolbox   # 硬解（Catalyst 行末 --disable-videotoolbox 覆盖掉，见 SLICES 表）
  --enable-hwaccel=hevc_videotoolbox
  --enable-parser=aac
  --enable-parser=aac_latm        # mpegts 里的 AAC 常用 LATM 封装
  --enable-protocol=file
  # http 只为让 hls.c:673 的 avio_find_protocol_name() 认得协议名。
  # 真正的连接由 HlsSession 的 io_open 发起，FFmpeg 自己的 http 实现
  # 永不使用（protocol_whitelist 只留 file，见 hls_session.cpp）。
  # configure:4023 —— http_protocol_select="tcp_protocol"，只拉 tcp，
  # 不拉 TLS；而 --enable-protocol=https 会拉 tls_protocol（configure:4027），
  # 后者必须绑一个 TLS 后端（Apple 上是已废弃的 Secure Transport）。
  # 所以本项目把 https 在边界上改写成 http，绕开整套 TLS 依赖。
  --enable-protocol=http
  --enable-bsf=h264_mp4toannexb
)

# 与宿主机路径无关、每个 slice 都一样的 configure 开关。configure 的实际参数是
# 「路径类参数（--prefix / --sysroot / --cc …）+ 本数组 + FF_COMPONENTS +
# slice 表里的额外参数」，顺序固定。--print-notice 原样列出本数组，所以分发包里
# NOTICE 写的开关与实际编译用的是同一份。
#
# 关掉 autodetect，避免链到本机 Homebrew 的 x264 / fdk-aac 等可选库。
# 不加 --disable-optimizations（软解会慢到不能用，时序问题也会变形）。
# 不加 --disable-debug（FFmpeg 默认带 -g）。
# --disable-programs：不需要 ffplay/ffprobe；iOS 也禁止 fork/system。
FF_COMMON_FLAGS=(
  # 静态只是中间产物：随后由 make_framework 用 -all_load 链成单个 dylib。
  # 对外交付的是动态 SYFFmpeg.framework，见文件头与 D11.2。
  --enable-static
  --disable-shared
  --enable-pic
  --disable-gpl
  --disable-autodetect
  --disable-stripping
  --disable-programs
  --disable-doc
  --disable-avdevice
  --disable-avfilter
  --disable-swscale
  # 不要加 --disable-postproc：libpostproc 是 GPL-only，8.x 已从上游移除，
  # 传了会直接 "Unknown option" 退出。实测于 8.1.2。
  --enable-videotoolbox
  --disable-audiotoolbox
  --enable-zlib
  --disable-everything
)

trap 'echo "失败：行 ${LINENO}（exit $?）" >&2' ERR

# ---------------------------------------------------------------------------
# xcode-select 可能指向 Command Line Tools（没有 iOS SDK），xcrun 要能退回
# Xcode.app。与 tools/check-abi.sh 同一套退回逻辑。
# ---------------------------------------------------------------------------
ensure_xcode_developer_dir() {
  local p
  p=$(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null) || true
  if [ -z "$p" ] || [ ! -d "$p" ]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
    echo "xcode-select 看不到 iOS SDK，改用 DEVELOPER_DIR=${DEVELOPER_DIR}"
    p=$(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null) || true
  fi
  if [ -z "$p" ] || [ ! -d "$p" ]; then
    echo "找不到 iPhoneOS SDK（本机 Xcode / 开发者目录环境问题，不是脚本问题）。" >&2
    echo "已尝试：xcrun --sdk iphoneos --show-sdk-path" >&2
    echo "        DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun --sdk iphoneos --show-sdk-path" >&2
    echo "请安装 Xcode。若 xcode-select 指向 Command Line Tools，可设置" >&2
    echo "  export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer" >&2
    exit 1
  fi
}

sdk_path() {
  local sdk="$1"
  local p
  p=$(xcrun --sdk "$sdk" --show-sdk-path 2>/dev/null) || true
  if [ -z "$p" ] || [ ! -d "$p" ]; then
    p=$(DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
        xcrun --sdk "$sdk" --show-sdk-path 2>/dev/null) || true
  fi
  if [ -z "$p" ] || [ ! -d "$p" ]; then
    echo "找不到 ${sdk} SDK。" >&2
    exit 1
  fi
  printf '%s\n' "$p"
}

xcrun_find() {
  local sdk="$1"
  local tool="$2"
  local p
  p=$(xcrun --sdk "$sdk" -f "$tool" 2>/dev/null) || true
  if [ -z "$p" ] || [ ! -x "$p" ]; then
    p=$(DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
        xcrun --sdk "$sdk" -f "$tool" 2>/dev/null) || true
  fi
  if [ -z "$p" ] || [ ! -x "$p" ]; then
    echo "找不到 ${tool}（sdk=${sdk}）。" >&2
    exit 1
  fi
  printf '%s\n' "$p"
}

# 校验失败立即删掉坏文件，避免下次误用缓存。返回 0/1，不直接 exit，
# 好让「缓存无效 → 重下」和「新下载仍坏 → 退出」走不同分支。
verify_sha256() {
  local file="$1"
  local expected="$2"
  local actual
  actual="$(shasum -a 256 "$file" | awk '{print $1}')"
  if [ "$actual" != "$expected" ]; then
    echo "sha256 校验失败：$file" >&2
    echo "  期望：$expected" >&2
    echo "  实际：$actual" >&2
    rm -f "$file"
    echo "已删除坏文件，避免下次误用缓存。" >&2
    return 1
  fi
  echo "sha256 通过：$actual"
  return 0
}

fetch_tarball() {
  mkdir -p "$SRC_DIR"
  if [ -f "$TARBALL" ]; then
    echo "== 发现已下载的 tarball，校验 sha256 =="
    if verify_sha256 "$TARBALL" "$FFMPEG_SHA256"; then
      echo "缓存有效，跳过下载。"
      return 0
    fi
    echo "缓存无效，重新下载。"
  fi
  echo "== 下载 FFmpeg ${FFMPEG_VERSION} =="
  echo "   $FFMPEG_TARBALL_URL"
  curl -L --fail --show-error -o "$TARBALL" "$FFMPEG_TARBALL_URL"
  if ! verify_sha256 "$TARBALL" "$FFMPEG_SHA256"; then
    exit 1
  fi
}

# 源码补丁目录。按文件名（C locale）排序逐个打；目录为空或不存在就是"未修改"。
PATCH_DIR="$P/tools/ffmpeg-patches"
# 解压出的源码树里记下"打过哪些补丁"（每行：sha256 两个空格 文件名）。
# 源码树是缓存，补丁集一变（增删改任一补丁）就必须重新解压，不能在旧树上叠补丁。
SRC_PATCH_STAMP="$SRC_UNPACK/.syp-applied-patches"
# 同一份清单随打包写进产物目录，--print-notice 以它为准，保证 NOTICE 描述的是
# 产物实际用的源码，而不是补丁目录此刻的样子。
OUT_PATCH_STAMP="$OUT_DIR/SYFFmpeg.applied-patches"

list_patches() {
  [ -d "$PATCH_DIR" ] || return 0
  find "$PATCH_DIR" -maxdepth 1 -type f -name '*.patch' -print | LC_ALL=C sort
}

# 当前补丁目录对应的清单正文（没有补丁时为空）。
patch_stamp() {
  local f
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    printf '%s  %s\n' "$(shasum -a 256 "$f" | awk '{print $1}')" "$(basename "$f")"
  done < <(list_patches)
}

apply_patches() {
  local f n=0
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    echo "   打补丁：$(basename "$f")"
    # --forward：补丁已经打过（或方向反了）时直接失败，不去猜"要不要反向打"。
    # --fuzz=0：上下文必须逐行对上，FFmpeg 升级后补丁错位就直接失败，不让
    # patch 凭模糊匹配把改动打到别处。
    if ! patch -p1 --forward --fuzz=0 -d "$SRC_UNPACK" -i "$f" </dev/null; then
      echo "补丁打不上：$f" >&2
      echo "源码树已处于半打补丁状态，删掉它，下次重新解压。" >&2
      rm -rf "$SRC_UNPACK"
      exit 1
    fi
    n=$((n + 1))
  done < <(list_patches)
  echo "   共打补丁 ${n} 个"
}

extract_tarball() {
  local want have=""
  want="$(patch_stamp)"
  if [ -f "$SRC_UNPACK/configure" ] && [ -f "$SRC_PATCH_STAMP" ]; then
    have="$(cat "$SRC_PATCH_STAMP")"
    if [ "$have" = "$want" ]; then
      echo "== 源码已解压且补丁集一致（${SRC_UNPACK}），跳过 =="
      return 0
    fi
    echo "== 源码树的补丁集与 ${PATCH_DIR} 不一致，重新解压 =="
  elif [ -d "$SRC_UNPACK" ]; then
    echo "== 源码树没有补丁记录（旧缓存或上次中断），重新解压 =="
  fi
  rm -rf "$SRC_UNPACK"
  # tar 解压会还原 tarball 里的旧 mtime。撤掉一个补丁后，被它改过的文件恢复成
  # 旧 mtime，比 work/ 里用补丁版编出的 .o 还旧，make 会认为不必重编，静默沿用
  # 补丁版目标文件。所以源码树一旦重建，各 slice 的 out-of-tree 构建目录一并清掉。
  rm -rf "$BUILD_ROOT/work"
  echo "== 解压 $TARBALL =="
  tar -xJf "$TARBALL" -C "$SRC_DIR"
  if [ ! -f "$SRC_UNPACK/configure" ]; then
    echo "解压后未找到 $SRC_UNPACK/configure（顶层目录名应对应 ffmpeg-${FFMPEG_VERSION}/）。" >&2
    exit 1
  fi
  apply_patches
  printf '%s' "$want" > "$SRC_PATCH_STAMP"
  if [ -n "$want" ]; then printf '\n' >> "$SRC_PATCH_STAMP"; fi
}

# 读 configure 生成的头文件（config.h 或 config_components.h）里一个 CONFIG_*
# 宏的值（0/1）。缺失视为 0。
config_value() {
  local cfg_h="$1" key="$2" v
  v="$(sed -n "s/^#define ${key} \([01]\)$/\1/p" "$cfg_h" | head -1)"
  echo "${v:-0}"
}

# 结构护栏：Catalyst 的 --disable-videotoolbox 排在 FF_COMPONENTS
# 之后、按"最后一次生效"覆盖 hwaccel——这是**明确**的平台差异，这里两个方向都钉死：
# Catalyst 必须为 0（不许悄悄变有），其余 slice 必须为 1（不许悄悄变没）。
check_hw_config() {
  local cfg_h="$1" name="$2" want key got
  if [ "$name" = "maccatalyst-arm64" ]; then want=0; else want=1; fi
  for key in CONFIG_H264_VIDEOTOOLBOX_HWACCEL CONFIG_HEVC_VIDEOTOOLBOX_HWACCEL; do
    got="$(config_value "$cfg_h" "$key")"
    if [ "$got" != "$want" ]; then
      echo "自检失败：${name} 的 ${key}=${got}，期望 ${want}" >&2
      exit 1
    fi
  done
  for key in CONFIG_HEVC_DECODER CONFIG_HEVC_PARSER; do
    got="$(config_value "$cfg_h" "$key")"
    if [ "$got" != "1" ]; then
      echo "自检失败：${name} 的 ${key}=${got}，期望 1" >&2
      exit 1
    fi
  done
  echo "  hw 自检通过：${name}（VT hwaccel=${want}）"
}

# 把两个 hwaccel 开关导出成对外头文件，平台代码据此判断本 slice 能不能硬解。
write_features_header() {
  local cfg_h="$1" out="$2"
  mkdir -p "$(dirname "$out")"
  {
    echo "#pragma once"
    echo "// 由 tools/build-ffmpeg.sh 从 configure 生成的 config_components.h 提取，勿手改。"
    echo "#define SYP_FFMPEG_H264_VIDEOTOOLBOX_HWACCEL $(config_value "$cfg_h" CONFIG_H264_VIDEOTOOLBOX_HWACCEL)"
    echo "#define SYP_FFMPEG_HEVC_VIDEOTOOLBOX_HWACCEL $(config_value "$cfg_h" CONFIG_HEVC_VIDEOTOOLBOX_HWACCEL)"
  } > "$out"
}

# 交叉编译一个 slice。组件裁剪全部靠 configure（源码改动只走补丁目录）。
# 额外的 configure 参数从第 5 个起传入（例如 x86_64 模拟器 --disable-x86asm）。
build_slice() {
  local name="$1"
  local triple="$2"
  local sdk="$3"
  local arch="$4"
  shift 4

  echo
  echo "== 编译 slice: ${name} =="
  echo "   triple=${triple}  sdk=${sdk}  arch=${arch}"

  local sdk_root cc cxx ar ranlib nm prefix work
  sdk_root="$(sdk_path "$sdk")"
  cc="$(xcrun_find "$sdk" clang)"
  cxx="$(xcrun_find "$sdk" clang++)"
  ar="$(xcrun_find "$sdk" ar)"
  ranlib="$(xcrun_find "$sdk" ranlib)"
  nm="$(xcrun_find "$sdk" nm)"
  prefix="$SLICES_DIR/$name"
  work="$BUILD_ROOT/work/$name"

  mkdir -p "$work"
  rm -rf "$prefix"

  # 路径类参数在前，其余开关见 FF_COMMON_FLAGS。
  local -a cfg=(
    --prefix="$prefix"
    --enable-cross-compile
    --target-os=darwin
    --arch="$arch"
    --sysroot="$sdk_root"
    --cc="$cc"
    --cxx="$cxx"
    --ar="$ar"
    --ranlib="$ranlib"
    --nm="$nm"
    --extra-cflags="-target ${triple} -isysroot ${sdk_root}"
    --extra-ldflags="-target ${triple} -isysroot ${sdk_root}"
  )
  cfg+=("${FF_COMMON_FLAGS[@]}")
  cfg+=("${FF_COMPONENTS[@]}")
  if [ "$#" -gt 0 ]; then
    cfg+=("$@")
  fi

  # PKG_CONFIG_LIBDIR 清空，防止 configure 捡到 brew 的 .pc。
  (
    cd "$work"
    PKG_CONFIG_LIBDIR="" PKG_CONFIG_PATH="" \
      "$SRC_UNPACK/configure" "${cfg[@]}"
    # 各组件的 CONFIG_* 开关不在 config.h 里，configure 把它们拆到同目录的
    # config_components.h（8.1.2 实测；config.h 只有 CONFIG_VIDEOTOOLBOX 这种
    # 大类开关，没有 xxx_HWACCEL / HEVC_DECODER 这一级）。
    check_hw_config "$work/config_components.h" "$name"
    make -j"$JOBS"
    make install
    write_features_header "$work/config_components.h" "$prefix/include/syp_ffmpeg_features.h"
  )
}

# 把一个 slice 前缀里的多个 .a 合成 libffmpeg.a。
# xcframework 一次只能装一个 library，所以走方案 b。
merge_slice_libs() {
  local prefix="$1"
  local libdir="$prefix/lib"
  local out="$libdir/libffmpeg.a"
  local -a libs=()
  local f

  for f in "$libdir"/*.a; do
    [ -f "$f" ] || continue
    [ "$(basename "$f")" = "libffmpeg.a" ] && continue
    libs+=("$f")
  done
  if [ "${#libs[@]}" -eq 0 ]; then
    echo "未找到静态库：$libdir" >&2
    exit 1
  fi
  echo "  合并 ${#libs[@]} 个静态库 -> $out"
  xcrun libtool -static -o "$out" "${libs[@]}"
}

# 从一个 slice 的 libffmpeg.a 取"要加前缀的符号"清单：每行一个名字，不带
# Mach-O 的前导下划线，C locale 排序去重。
#
# 取的是已定义的外部符号（nm -gU），但**排除 private external**：那是 FFmpeg
# 用 hidden 可见性标出的内部符号（ff_aac_* 表之类），合成 dylib 后本来就导不
# 出去，给它起导出别名没有意义。nm -m 的输出里还夹着 "xxx.o:" 成员名行和
# 空行，只认"地址 (段,节) ..."开头的符号行，名字是最后一列（-m 会把
# [cold func] 之类的属性写在名字前面）。
#
# 不以 _ 开头的全局符号没法用 C 的 #define 映射，出现就停下来查，不静默跳过。
gen_symbol_list() {
  local lib="$1" out="$2" dump bad
  dump="$(xcrun nm -gUm "$lib")"
  printf '%s\n' "$dump" \
    | grep -E '^[0-9a-f]+ \(' \
    | grep -v 'private external' \
    | awk '{print $NF}' > "$out.raw"
  bad="$(grep -v '^_' "$out.raw" || true)"
  if [ -n "$bad" ]; then
    echo "发现不以 _ 开头的全局符号，无法映射：" >&2
    printf '%s\n' "$bad" | head -5 >&2
    exit 1
  fi
  # 同名定义出现两次说明合并出的静态库本身有问题（链接也会报重复定义）。
  bad="$(LC_ALL=C sort "$out.raw" | uniq -d)"
  if [ -n "$bad" ]; then
    echo "静态库里有重复定义的全局符号：" >&2
    printf '%s\n' "$bad" | head -5 >&2
    exit 1
  fi
  sed 's/^_//' "$out.raw" | LC_ALL=C sort -u > "$out"
  rm -f "$out.raw"
  if [ ! -s "$out" ]; then
    echo "符号清单为空：$lib" >&2
    exit 1
  fi
}

# FFmpeg 公开头里有少数函数的声明包在"#ifndef 函数名"里（libavutil/common.h 的
# av_log2 / av_log2_16bit：内部实现可能先把它 #define 成别的名字）。映射头一旦把
# 这个名字 #define 成 syp_ 版本，FFmpeg 头就以为"已经有了"而跳过声明，后面的内联
# 函数调用它时报"未声明的函数"。
#
# 这里找出这类名字（符号清单 ∩ 公开头里被 #ifdef/#ifndef/defined() 探测的名字），
# 把紧跟在 "#ifndef 名字" 后面的那条声明原样抄进映射头——此时名字已被映射，
# 抄出来的声明声明的正是 syp_ 版本。抄不出来（不是"#ifndef 名 / 声明 / #endif"
# 这种形状）就停下来人工看，不猜。
guarded_names() {
  local list="$1" incdir="$2"
  grep -rhoE '(#[[:space:]]*if(n)?def[[:space:]]+[A-Za-z_][A-Za-z0-9_]*|defined[[:space:]]*\(?[[:space:]]*[A-Za-z_][A-Za-z0-9_]*)' \
       "$incdir" --include='*.h' --exclude='syp_ffmpeg_*.h' \
    | sed -E 's/^#[[:space:]]*if(n)?def[[:space:]]+//; s/^defined[[:space:]]*\(?[[:space:]]*//' \
    | LC_ALL=C sort -u \
    | LC_ALL=C comm -12 - "$list"
}

guarded_decls() {
  local list="$1" incdir="$2" g decl
  while IFS= read -r g; do
    [ -n "$g" ] || continue
    decl="$(find "$incdir" -name '*.h' ! -name 'syp_ffmpeg_*.h' -print0 | LC_ALL=C sort -z \
            | xargs -0 awk -v g="$g" '
                capture == 0 && $0 ~ "^#[ \t]*ifndef[ \t]+" g "[ \t]*$" { capture = 1; buf = ""; next }
                capture == 1 && /^[ \t]*#/ { capture = 0; next }
                capture == 1 {
                  buf = buf $0 "\n"
                  if ($0 ~ /;/) { printf "%s", buf; exit }
                }')"
    if ! printf '%s\n' "$decl" | grep -Eq "[^A-Za-z0-9_]${g}[[:space:]]*\\("; then
      echo "FFmpeg 公开头用预处理条件探测了符号名 ${g}，但找不到" >&2
      echo "\"#ifndef ${g} / 声明 / #endif\" 形状的声明可抄，映射头无法正确处理，请人工检查。" >&2
      exit 1
    fi
    printf '%s\n' "$decl"
  done < <(guarded_names "$list" "$incdir")
}

# 由一个或多个符号清单生成映射头：#define 原名 syp_原名。
# 多个清单时取并集（见 write_union_prefix_headers 的说明）。incdir 用来找上面
# 那类被条件探测的声明。
write_prefix_header() {
  local out="$1" incdir="$2"
  shift 2
  local all decls
  mkdir -p "$(dirname "$out")"
  incdir="$(cd -P "$incdir" && pwd)"
  all="$(dirname "$out")/.syp_prefix_names.tmp"
  cat "$@" | LC_ALL=C sort -u > "$all"
  decls="$(guarded_decls "$all" "$incdir")"
  {
    echo "// 由 tools/build-ffmpeg.sh 从静态库符号表生成，勿手改。"
    echo "//"
    echo "// SYFFmpeg 对外只导出 syp_ 前缀的名字。凡是包含 FFmpeg 头文件的编译单元，"
    echo "// 都要在任何 FFmpeg 头之前强制包含本文件（-include），源码里照写原名即可。"
    echo "#pragma once"
    echo "#ifndef SYP_FFMPEG_PREFIX_H"
    echo "#define SYP_FFMPEG_PREFIX_H"
    awk '{print "#define " $0 " syp_" $0}' "$all"
    if [ -n "$decls" ]; then
      echo
      echo "// 下面这些函数的声明在 FFmpeg 公开头里包在 \"#ifndef 函数名\" 中，上面的"
      echo "// #define 会让 FFmpeg 头跳过它们，所以在这里补上（声明的是 syp_ 版本）。"
      echo "#include \"libavutil/attributes.h\""
      echo "#ifdef __cplusplus"
      echo "extern \"C\" {"
      echo "#endif"
      printf '%s\n' "$decls"
      echo "#ifdef __cplusplus"
      echo "}"
      echo "#endif"
    fi
    echo "#endif"
  } > "$out"
  rm -f "$all"
}

# 映射头的编译冒烟：公开头里凡是本身能独立编过的（hwcontext_vulkan.h 之类依赖
# 别的 SDK 的除外），加上 -include 映射头之后在 C 与 C++ 下都必须零告警编过；
# 被条件探测的那几个函数在 C++ 下必须解析到未改编（extern "C"）的 _syp_ 名。
smoke_prefix_header() {
  local cc="$1" triple="$2" sdk_root="$3" hdrdir="$4" tag="$5"
  local tmp="$BUILD_ROOT/work/prefix-smoke/$tag" h rel n_all=0 n_ok=0 g
  # macOS 布局下 Headers 是符号链接，find / grep -r 默认不跟进起点链接。
  hdrdir="$(cd -P "$hdrdir" && pwd)"
  local -a base=(-target "$triple" -isysroot "$sdk_root" -I "$hdrdir" -Wall -Werror)
  rm -rf "$tmp"
  mkdir -p "$tmp"
  : > "$tmp/all.c"
  while IFS= read -r h; do
    rel="${h#"$hdrdir"/}"
    n_all=$((n_all + 1))
    printf '#include <%s>\n' "$rel" > "$tmp/one.c"
    if "$cc" -fsyntax-only -x c "${base[@]}" "$tmp/one.c" >/dev/null 2>&1; then
      printf '#include <%s>\n' "$rel" >> "$tmp/all.c"
      n_ok=$((n_ok + 1))
    fi
  done < <(find "$hdrdir" -mindepth 2 -name '*.h' | LC_ALL=C sort)
  if [ "$n_ok" -lt 50 ]; then
    echo "映射头冒烟：能独立编过的公开头只有 ${n_ok}/${n_all} 个，环境不对？" >&2
    exit 1
  fi
  if ! "$cc" -fsyntax-only -x c "${base[@]}" "$tmp/all.c"; then
    echo "映射头冒烟：不带映射头时公开头合在一起就编不过（${tag}），先查环境" >&2
    exit 1
  fi
  if ! "$cc" -fsyntax-only -x c "${base[@]}" -include "$hdrdir/syp_ffmpeg_prefix.h" "$tmp/all.c"; then
    echo "映射头冒烟失败（C，${tag}）：带上映射头后公开头编不过" >&2
    exit 1
  fi
  # 这里不用进程替换 <(...)：bash 3.2 下把它传进函数再接管道会报 Bad file descriptor。
  sed -n 's/^#define \([A-Za-z_][A-Za-z0-9_]*\) syp_\1$/\1/p' "$hdrdir/syp_ffmpeg_prefix.h" \
    | LC_ALL=C sort -u > "$tmp/names.txt"
  guarded_names "$tmp/names.txt" "$hdrdir" > "$tmp/guarded.txt"
  {
    echo 'extern "C" {'
    cat "$tmp/all.c"
    echo '}'
    echo 'void *syp_smoke_refs[] = {'
    awk '{print "  (void *)&" $0 ","}' "$tmp/guarded.txt"
    echo '  nullptr };'
  } > "$tmp/all.cpp"
  if ! "$cc" -c -x c++ "${base[@]}" -include "$hdrdir/syp_ffmpeg_prefix.h" "$tmp/all.cpp" -o "$tmp/all.o"; then
    echo "映射头冒烟失败（C++，${tag}）：带上映射头后公开头编不过" >&2
    exit 1
  fi
  xcrun nm -u "$tmp/all.o" > "$tmp/undef.txt"
  while IFS= read -r g; do
    [ -n "$g" ] || continue
    if [ "$(grep -cx "_syp_${g}" "$tmp/undef.txt" || true)" -eq 0 ]; then
      echo "映射头冒烟失败（C++，${tag}）：${g} 没有解析到 _syp_${g}（被 C++ 改编了？）" >&2
      cat "$tmp/undef.txt" >&2
      exit 1
    fi
  done < "$tmp/guarded.txt"
  echo "   映射头冒烟通过（${tag}）：${n_ok}/${n_all} 个可独立编译的公开头，C 与 C++"
}

# 映射头会被 -include 进使用方的每个编译单元。若某个符号名恰好也是 FFmpeg
# 公开头里的宏名，两边的 #define 会互相覆盖（-Werror 下直接报错，或者更糟：
# 静默映射到错的名字）。这里在生成时就把这种撞名拦下。
check_prefix_macro_clash() {
  local list="$1" incdir="$2" clash
  clash="$(grep -rhoE '^[[:space:]]*#[[:space:]]*define[[:space:]]+[A-Za-z_][A-Za-z0-9_]*' "$incdir" \
             | awk '{print $NF}' | sed 's/.*define//' | LC_ALL=C sort -u \
             | LC_ALL=C comm -12 - "$list")"
  if [ -n "$clash" ]; then
    echo "以下符号名同时是 FFmpeg 公开头里的宏名，映射头会与之冲突：" >&2
    printf '%s\n' "$clash" | head -10 >&2
    exit 1
  fi
}

# 把 slice 里的 libffmpeg.a 链成单个动态库，并组装成 SYFFmpeg.framework。
# 这是「对外动态」的落地点，理由见文件头与决策记录 D11.2。
#
# 系统依赖不手写清单，从 FFmpeg 自己生成的 .pc 里取——组件清单一改，
# 依赖跟着变，手写的迟早对不上。
#
# 注意取的是 Libs: 而不是 Libs.private:。实测 8.1.2 四个 .pc 的
# Libs.private 全是空的，真正的清单（-lz / -framework VideoToolbox /
# CoreFoundation / CoreMedia / CoreVideo / CoreServices）在 Libs: 里。
# 要滤掉 -L${libdir}（未展开的 pkg-config 变量，留着会被 set -u 打死）
# 和 -lavcodec 之类自家的库（已经在 libffmpeg.a 里了）。
make_framework() {
  local name="$1" triple="$2" sdk="$3" kind="$4"
  local prefix="$SLICES_DIR/$name"
  local fw="$prefix/SYFFmpeg.framework"
  local work="$BUILD_ROOT/work/$name"
  local symlist="$prefix/syp_symbols.txt"
  local alias_txt="$work/syp_alias.txt"
  local exports_txt="$work/syp_exports.txt"
  local sdk_root cc extra_libs binpath hdrdir plistdir install_name

  sdk_root="$(sdk_path "$sdk")"
  cc="$(xcrun_find "$sdk" clang)"
  extra_libs="$(cat "$prefix"/lib/pkgconfig/*.pc 2>/dev/null \
                | sed -n 's/^Libs: *//p' \
                | tr ' ' '\n' \
                | grep -v '^$' \
                | grep -v '^-L' \
                | grep -vE '^-l(avcodec|avformat|avutil|avfilter|avdevice|swresample|swscale|postproc)$' \
                | awk '$0=="-framework"{getline n; k=$0" "n; if(!seen[k]++){print; print n} next} !seen[$0]++' \
                | tr '\n' ' ')"

  echo "== 组装 framework: ${name} =="
  echo "   系统依赖（取自 .pc 的 Libs:）：${extra_libs:-<空>}"

  # 改名清单：本 slice 自己的静态库现生成。模拟器两个架构各自生成、各自链接，
  # 之后才 lipo，所以每个架构的导出表都只含它自己真有的符号。
  mkdir -p "$work"
  gen_symbol_list "$prefix/lib/libffmpeg.a" "$symlist"
  check_prefix_macro_clash "$symlist" "$prefix/include"
  awk '{print "_" $0 " _syp_" $0}' "$symlist" > "$alias_txt"
  awk '{print "_syp_" $0}' "$symlist" > "$exports_txt"
  echo "   加前缀的符号数：$(wc -l < "$symlist" | tr -d ' ')"

  rm -rf "$fw"
  if [ "$kind" = "macos" ] || [ "$kind" = "maccatalyst" ]; then
    # macOS 要 versioned bundle，iOS 是 flat bundle，两者结构不同。
    # Catalyst 与 macOS 一样用 versioned bundle 结构。
    mkdir -p "$fw/Versions/A/Headers" "$fw/Versions/A/Resources"
    binpath="$fw/Versions/A/SYFFmpeg"
    hdrdir="$fw/Versions/A/Headers"
    plistdir="$fw/Versions/A/Resources"
    install_name="@rpath/SYFFmpeg.framework/Versions/A/SYFFmpeg"
  else
    mkdir -p "$fw/Headers"
    binpath="$fw/SYFFmpeg"
    hdrdir="$fw/Headers"
    plistdir="$fw"
    install_name="@rpath/SYFFmpeg.framework/SYFFmpeg"
  fi

  # -all_load：静态库里的符号默认按需拉取，不加会丢掉大量未被直接引用的
  # 注册型符号（demuxer/decoder 的注册表），链出来的 dylib 是空壳。
  # -alias_list：每个全局符号再起一个 _syp_ 名字，指向同一地址。
  # -exported_symbols_list：只导出 _syp_*；原名降为本地符号，库内互调不受影响。
  # shellcheck disable=SC2086
  "$cc" -dynamiclib \
    -target "$triple" -isysroot "$sdk_root" \
    -install_name "$install_name" \
    -compatibility_version "${FFMPEG_VERSION}" \
    -current_version "${FFMPEG_VERSION}" \
    -Wl,-all_load "$prefix/lib/libffmpeg.a" \
    -Wl,-alias_list,"$alias_txt" \
    -Wl,-exported_symbols_list,"$exports_txt" \
    $extra_libs \
    -o "$binpath"

  cp -R "$prefix/include/." "$hdrdir/"
  # 先放本 slice 自己的映射头，保证 --only 单独编出的 framework 自洽；
  # 全量构建在打包前会换成所有 slice 的并集（write_union_prefix_headers）。
  write_prefix_header "$hdrdir/syp_ffmpeg_prefix.h" "$hdrdir" "$symlist"
  smoke_prefix_header "$cc" "$triple" "$sdk_root" "$hdrdir" "$name"

  local plat minkey minver
  case "$kind" in
    ios)         plat="iPhoneOS";       minkey="MinimumOSVersion";      minver="$IOS_MIN" ;;
    iossim)      plat="iPhoneSimulator"; minkey="MinimumOSVersion";      minver="$IOS_MIN" ;;
    macos)       plat="MacOSX";          minkey="LSMinimumSystemVersion"; minver="$MACOS_MIN" ;;
    maccatalyst) plat="MacOSX";          minkey="LSMinimumSystemVersion"; minver="$CATALYST_MACOS_MIN" ;;
  esac

  cat > "$plistdir/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>SYFFmpeg</string>
  <key>CFBundleIdentifier</key><string>com.syplayer.SYFFmpeg</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>SYFFmpeg</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>${FFMPEG_VERSION}</string>
  <key>CFBundleVersion</key><string>${FFMPEG_VERSION}</string>
  <key>CFBundleSupportedPlatforms</key><array><string>${plat}</string></array>
  <key>${minkey}</key><string>${minver}</string>
</dict>
</plist>
PLIST

  if [ "$kind" = "macos" ] || [ "$kind" = "maccatalyst" ]; then
    ( cd "$fw/Versions" && ln -sfn A Current )
    ( cd "$fw" && ln -sfn Versions/Current/SYFFmpeg  SYFFmpeg
                  ln -sfn Versions/Current/Headers   Headers
                  ln -sfn Versions/Current/Resources Resources )
  fi

  # 自检：dylib 装出来是不是真带着我们要的符号，而不是空壳；并且导出表里
  # 只有 _syp_*。
  # ⚠️ 本脚本开了 pipefail，管道里**任何会提前退出的消费者**都是雷：
  # grep -q / grep -m1 / head 一命中就退出，上游还没写完就吃 SIGPIPE(141)，
  # 整条管道被判失败。而是否触发取决于匹配位置落在管道缓冲区（约 64KB）
  # 的哪一段，所以它是随机的——同一份输入，符号在前面就误报、在后面就正常。
  # 这里一律用 grep -c（读完全部输入，不提前退出）再比数量。
  local nsym nexp nbad symdump
  symdump="$(xcrun nm -gU "$binpath" 2>/dev/null || true)"
  nsym="$(printf '%s\n' "$symdump" | grep -c ' T _syp_av' || true)"
  echo "   导出的 syp_av* 函数数：${nsym}"
  if [ "$nsym" -lt 100 ]; then
    echo "导出符号太少（${nsym}），-all_load 或 -alias_list 可能没生效。" >&2
    exit 1
  fi
  nexp="$(printf '%s\n' "$symdump" | grep -c '^[0-9a-f]* [A-Z] ' || true)"
  nbad="$(printf '%s\n' "$symdump" | grep '^[0-9a-f]* [A-Z] ' | grep -vc ' _syp_' || true)"
  echo "   导出符号总数：${nexp}，其中未加前缀：${nbad}"
  if [ "$nbad" -ne 0 ]; then
    echo "导出表里有未加 syp_ 前缀的符号：" >&2
    printf '%s\n' "$symdump" | grep '^[0-9a-f]* [A-Z] ' | grep -v ' _syp_' | head -5 >&2
    exit 1
  fi
  if [ "$nexp" -ne "$(wc -l < "$symlist" | tr -d ' ')" ]; then
    echo "导出符号数（${nexp}）与改名清单行数不一致，别名没有全部生效。" >&2
    exit 1
  fi
  # 只数数量不够：libavutil 一个库就能凑出几百个 av_*。逐个点名几个
  # 真正要用的入口，确认四个库都合进来了。
  local missing=0 sym
  for sym in _syp_avformat_open_input _syp_av_read_frame _syp_av_seek_frame \
             _syp_avio_alloc_context _syp_avcodec_find_decoder _syp_swr_init; do
    if [ "$(printf '%s\n' "$symdump" | grep -c " T ${sym}$" || true)" -eq 0 ]; then
      echo "缺少导出符号：${sym}" >&2
      missing=1
    fi
  done
  if [ "$missing" -ne 0 ]; then
    echo "framework 缺关键入口，合并或链接有问题。" >&2
    exit 1
  fi
  xcrun otool -L "$binpath" | head -3
}

# xcframework 不允许两个 platform+variant 相同的目录，所以两个模拟器
# slice 必须先 lipo 成一个 fat，再和真机、macOS 一起打包。
# 头文件以 arm64 模拟器为准（avconfig.h 的 HAVE_* 与 x86_64 可能不完全一致；
# x86_64 模拟器只作兼容，Intel Mac 上跑 iOS 模拟器已越来越少）。
# 两个架构的二进制在 lipo 之前已各自按自己的符号表加好前缀。
lipo_simulator() {
  local arm64_p="$SLICES_DIR/ios-simulator-arm64"
  local x86_p="$SLICES_DIR/ios-simulator-x86_64"
  local fat_p="$SLICES_DIR/ios-simulator"

  echo "== lipo 模拟器 arm64 + x86_64 =="
  rm -rf "$fat_p"
  mkdir -p "$fat_p"
  # 以 arm64 模拟器的 framework 为骨架（头文件、Info.plist 都用它的），
  # 只把可执行二进制换成两架构的 fat。
  cp -R "$arm64_p/SYFFmpeg.framework" "$fat_p/SYFFmpeg.framework"
  xcrun lipo -create \
    "$arm64_p/SYFFmpeg.framework/SYFFmpeg" \
    "$x86_p/SYFFmpeg.framework/SYFFmpeg" \
    -output "$fat_p/SYFFmpeg.framework/SYFFmpeg"
  xcrun lipo -info "$fat_p/SYFFmpeg.framework/SYFFmpeg"
}

# 映射头取五个构建（四个 slice，模拟器两个架构）符号清单的**并集**，每个
# slice 放同一份。
#
# 为什么不按 slice 各放各的：各清单确实不同——Catalyst 关了 VideoToolbox，
# 少了 av_videotoolbox_* 等；x86_64 模拟器关了汇编，少了 *_neon。而使用方
# 的构建系统未必按 slice 选头文件路径（例如拿 ios-arm64 的 Headers 去编
# Catalyst）。并集头在任何 slice 上用都不会漏映射。
#
# 为什么并集是安全的：映射只是改名，不会凭空制造引用。某个名字在 slice S
# 里不存在时，使用方在 S 上引用它本来就链接不过（有没有前缀都一样）；存在时，
# S 的导出表里必然有它的 _syp_ 版本（清单就是从 S 的静态库生成的）。
# tools/check-ffmpeg-symbols.sh 校验：各 slice 头文件一致；每个 slice
# 的导出表都被头文件覆盖；头文件里每个名字至少在一个 slice 里有导出。
write_union_prefix_headers() {
  local -a lists=()
  local e n fw
  for e in "${SLICES[@]}"; do
    n="${e%%|*}"
    [ -s "$SLICES_DIR/$n/syp_symbols.txt" ] || { echo "缺符号清单：$n" >&2; exit 1; }
    lists+=("$SLICES_DIR/$n/syp_symbols.txt")
  done
  for n in ios-arm64 ios-simulator "macos-${HOST_ARCH}" maccatalyst-arm64; do
    fw="$SLICES_DIR/$n/SYFFmpeg.framework"
    write_prefix_header "$fw/Headers/syp_ffmpeg_prefix.h" "$fw/Headers" "${lists[@]}"
  done
  echo "   映射头条目数（并集）：$(grep -c '^#define .* syp_' "$SLICES_DIR/ios-arm64/SYFFmpeg.framework/Headers/syp_ffmpeg_prefix.h" || true)"

  # 并集头换进去之后，按每个构建自己的 triple 再做一遍编译冒烟；
  # fat 模拟器的两个架构共用一份 Headers，两个 triple 都编。
  local s_name s_triple s_sdk fwdir
  for e in "${SLICES[@]}"; do
    IFS='|' read -r s_name s_triple s_sdk _ _ _ <<<"$e"
    case "$s_name" in
      ios-simulator-*) fwdir="$SLICES_DIR/ios-simulator/SYFFmpeg.framework" ;;
      *)               fwdir="$SLICES_DIR/$s_name/SYFFmpeg.framework" ;;
    esac
    smoke_prefix_header "$(xcrun_find "$s_sdk" clang)" "$s_triple" "$(sdk_path "$s_sdk")" \
      "$fwdir/Headers" "union-${s_name}"
  done
}

pack_xcframework() {
  local ios_p="$SLICES_DIR/ios-arm64"
  local sim_p="$SLICES_DIR/ios-simulator"
  local mac_p="$SLICES_DIR/macos-${HOST_ARCH}"
  local cat_p="$SLICES_DIR/maccatalyst-arm64"
  local out="$OUT_DIR/SYFFmpeg.xcframework"

  echo "== 打包 SYFFmpeg.xcframework =="
  rm -rf "$out"
  mkdir -p "$OUT_DIR"
  xcrun xcodebuild -create-xcframework \
    -framework "$ios_p/SYFFmpeg.framework" \
    -framework "$sim_p/SYFFmpeg.framework" \
    -framework "$mac_p/SYFFmpeg.framework" \
    -framework "$cat_p/SYFFmpeg.framework" \
    -output "$out"
  # 产物实际用的补丁清单，供 --print-notice 生成 NOTICE。
  cp "$SRC_PATCH_STAMP" "$OUT_PATCH_STAMP"
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
if [ "$(uname -s)" != "Darwin" ]; then
  echo "本脚本只在 macOS 上跑（需要 Xcode / iOS SDK）。" >&2
  exit 1
fi

# slice 表：名字|triple|sdk|arch|kind|额外 configure 参数
# kind 决定 framework 的 bundle 结构（macOS 要 versioned，iOS 是 flat）。
SLICES=(
  "ios-arm64|arm64-apple-ios${IOS_MIN}|iphoneos|arm64|ios|"
  "ios-simulator-arm64|arm64-apple-ios${IOS_MIN}-simulator|iphonesimulator|arm64|iossim|"
  "ios-simulator-x86_64|x86_64-apple-ios${IOS_MIN}-simulator|iphonesimulator|x86_64|iossim|--disable-x86asm"
  "macos-${HOST_ARCH}|${HOST_ARCH}-apple-macos${MACOS_MIN}|macosx|${HOST_ARCH}|macos|"
  # --disable-videotoolbox：libavcodec/videotoolbox.c 引用的
  # kCVPixelBufferOpenGLESCompatibilityKey 在 macCatalyst SDK 头文件里标了
  # API_UNAVAILABLE(macCatalyst)，直接编译失败。这里选择用 configure 关掉，
  # 而不是往 tools/ffmpeg-patches/ 加补丁绕过；Catalyst 因此没有 FFmpeg 的
  # VideoToolbox 硬解——这是明确的平台差异，不是遗漏。check_hw_config() 断言本 slice 两个 hwaccel 为 0，
  # vt_decode_backend.mm 据 syp_ffmpeg_features.h 让 supports() 恒 false。
  "maccatalyst-arm64|arm64-apple-ios${CATALYST_MIN}-macabi|macosx|arm64|maccatalyst|--disable-videotoolbox"
)

# 本构建是 LGPL v2.1 or later：没有 --enable-version3（那会升到 LGPLv3），
# 也没有 --enable-gpl / --enable-nonfree。许可证正文取上游源码包里的这份。
LICENSE_FILE_NAME="COPYING.LGPLv2.1"

# 分发 FFmpeg 二进制时随包附带的 NOTICE 正文。版本、源码地址、configure 开关
# 全部从本脚本的变量与数组展开，分发包里写的就是实际编译用的那一份。
# 列出一个补丁改动的文件（-p1 口径，去掉 a/ b/ 前缀）。新增文件取 +++ 一侧，
# 删除文件（+++ /dev/null）取 --- 一侧。
patch_touched_files() {
  awk '
    /^--- / { old = $2; next }
    /^\+\+\+ / {
      f = $2
      if (f == "/dev/null") f = old
      sub(/^[^\/]*\//, "", f)
      if (!seen[f]++) print f
    }
  ' "$1"
}

# 分发 FFmpeg 二进制时随包附带的 NOTICE 正文。版本、源码地址、configure 开关
# 全部从本脚本的变量与数组展开，分发包里写的就是实际编译用的那一份。
#
# 源码改动部分以产物目录里的补丁清单为准（打包时从源码树拷出），不是补丁目录
# 此刻的样子：二者不一致说明产物过期，拒绝生成，免得 NOTICE 与二进制对不上。
print_notice() {
  local e name triple extra stamp="" cur sha pname
  if [ ! -f "$OUT_PATCH_STAMP" ]; then
    echo "找不到 ${OUT_PATCH_STAMP}：先完整跑一遍本脚本产出 SYFFmpeg.xcframework" >&2
    exit 1
  fi
  stamp="$(cat "$OUT_PATCH_STAMP")"
  cur="$(patch_stamp)"
  if [ "$stamp" != "$cur" ]; then
    echo "产物用的补丁集与 ${PATCH_DIR} 当前内容不一致：产物过期，先重跑本脚本。" >&2
    echo "产物：" >&2; printf '%s\n' "${stamp:-<无补丁>}" >&2
    echo "目录：" >&2; printf '%s\n' "${cur:-<无补丁>}" >&2
    exit 1
  fi

  echo "FFmpeg ${FFMPEG_VERSION}"
  echo
  if [ -z "$stamp" ]; then
    cat <<NOTICE
This SYFFmpeg.xcframework contains an unmodified build of FFmpeg ${FFMPEG_VERSION}
(https://ffmpeg.org/), licensed under the GNU Lesser General Public License
version 2.1 or later. The full license text is in ${LICENSE_FILE_NAME}
next to this file.
NOTICE
  else
    cat <<NOTICE
This SYFFmpeg.xcframework contains a MODIFIED build of FFmpeg ${FFMPEG_VERSION}
(https://ffmpeg.org/), licensed under the GNU Lesser General Public License
version 2.1 or later. The full license text is in ${LICENSE_FILE_NAME}
next to this file. The modifications are listed below.
NOTICE
  fi
  cat <<NOTICE

Corresponding source:
  ${FFMPEG_TARBALL_URL}
  sha256 ${FFMPEG_SHA256}

NOTICE
  if [ -z "$stamp" ]; then
    echo "No source file was modified or patched."
  else
    echo "The source above was modified by applying these patches, in this order"
    echo "(patch -p1; the patch files are in tools/ffmpeg-patches/ of the syplayer"
    echo "repository):"
    while read -r sha pname; do
      [ -n "$pname" ] || continue
      echo "  ${pname} (sha256 ${sha}), changing:"
      patch_touched_files "$PATCH_DIR/$pname" | sed 's/^/    /'
    done <<<"$stamp"
  fi
  cat <<NOTICE

Symbol prefix: every global symbol exported by this library carries an
added "syp_" prefix (for example av_read_frame is exported as
syp_av_read_frame), so that it cannot clash with another copy of FFmpeg in the
same application. The framework is named SYFFmpeg.framework. This build
produces Headers/syp_ffmpeg_prefix.h, which maps each original name to its
prefixed name; it is used only when compiling SYPlayerKit against this build
and is not included in the distributed package (the SYFFmpeg.xcframework
shipped to consumers has no Headers directory). The prefixing is done at link
time only (ld -alias_list and -exported_symbols_list); it does not change any
source.

The binary was built by tools/build-ffmpeg.sh in the syplayer repository
(https://github.com/swlfigo/syplayer), which downloads the tarball above,
verifies its sha256, applies the patches in tools/ffmpeg-patches/ (if any),
cross-compiles each platform slice with configure, links the static libraries
into one dynamic library per slice with the symbol prefix described above and
packages them as SYFFmpeg.framework inside this xcframework. To replace this
library, rebuild FFmpeg with the same (or modified) sources using that script
(put your own changes into tools/ffmpeg-patches/). A replacement must stay
ABI-compatible with the FFmpeg 8.1.x major library versions SYPlayerKit was
compiled against (the major sonames of libavcodec/libavformat/libavutil/...
at the time of that build), and must be produced by this script so it exports
the same syp_-prefixed names under the same SYFFmpeg.framework name. Swap
SYFFmpeg.framework in the application bundle; it is dynamically linked, so
the app must be re-signed after replacing it.

configure flags common to every slice, in addition to
--enable-cross-compile --target-os=darwin --arch=<slice arch> and
"-target <slice triple>" in --extra-cflags/--extra-ldflags (host paths such
as --prefix, --sysroot, --cc omitted):
NOTICE
  printf '  %s\n' "${FF_COMMON_FLAGS[@]}" "${FF_COMPONENTS[@]}"
  echo
  echo "per-slice extra configure flags:"
  for e in "${SLICES[@]}"; do
    IFS='|' read -r name _ _ _ _ extra <<<"$e"
    IFS='|' read -r _ triple _ _ _ _ <<<"$e"
    echo "  ${name} (${triple}): ${extra:-(none)}"
  done
  echo
  echo "The PrivacyInfo.xcprivacy files inside SYFFmpeg.framework are added by the"
  echo "syplayer packaging step; they are not part of FFmpeg."
}

usage() {
  cat <<USAGE
用法：
  $0                     编全部 5 个 slice 并打包 xcframework
  $0 --only <slice>      只编一个 slice（编完到 framework 为止，不 lipo、不打包）
  $0 --list              列出 slice 名字
  $0 --print-notice      打印随分发包附带的 NOTICE 正文（版本、源码地址、补丁、configure 开关；需先完整构建）
  $0 --print-license     打印 LGPL 许可证正文文件的路径（源码需已解压）

slice 名字：
$(for e in "${SLICES[@]}"; do echo "  ${e%%|*}"; done)

环境变量：
  FFMPEG_BUILD_JOBS=N    压低 make 并行度，默认取 hw.ncpu（当前 ${JOBS}）
USAGE
}

ONLY=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --only) ONLY="${2:-}"; [ -n "$ONLY" ] || { echo "--only 需要 slice 名字" >&2; exit 1; }; shift 2 ;;
    --list) for e in "${SLICES[@]}"; do echo "${e%%|*}"; done; exit 0 ;;
    --print-notice) print_notice; exit 0 ;;
    --print-license)
      if [ ! -f "$SRC_UNPACK/$LICENSE_FILE_NAME" ]; then
        echo "找不到 $SRC_UNPACK/${LICENSE_FILE_NAME}：先完整跑一遍本脚本解压源码" >&2
        exit 1
      fi
      echo "$SRC_UNPACK/$LICENSE_FILE_NAME"; exit 0 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数：$1" >&2; usage >&2; exit 1 ;;
  esac
done

if [ -n "$ONLY" ]; then
  found=0
  for e in "${SLICES[@]}"; do [ "${e%%|*}" = "$ONLY" ] && found=1; done
  if [ "$found" -eq 0 ]; then
    echo "没有这个 slice：$ONLY" >&2
    echo "可用：$(for e in "${SLICES[@]}"; do printf '%s ' "${e%%|*}"; done)" >&2
    exit 1
  fi
fi

echo "== FFmpeg ${FFMPEG_VERSION}（Hoare）交叉编译 =="
echo "   仓库：$P"
echo "   产物：$BUILD_ROOT"
echo "   make -j${JOBS}"
echo "   macOS slice 架构：${HOST_ARCH}"
[ -n "$ONLY" ] && echo "   只编：${ONLY}（不 lipo、不打包）"

ensure_xcode_developer_dir

echo
echo "== [1/5] 下载并校验 tarball =="
fetch_tarball

echo
echo "== [2/5] 解压源码 =="
extract_tarball

echo
echo "== [3/5] 交叉编译并组装各 slice 的 framework =="
for entry in "${SLICES[@]}"; do
  IFS='|' read -r s_name s_triple s_sdk s_arch s_kind s_extra <<<"$entry"
  if [ -n "$ONLY" ] && [ "$s_name" != "$ONLY" ]; then
    continue
  fi
  if [ -n "$s_extra" ]; then
    build_slice "$s_name" "$s_triple" "$s_sdk" "$s_arch" "$s_extra"
  else
    build_slice "$s_name" "$s_triple" "$s_sdk" "$s_arch"
  fi
  merge_slice_libs "$SLICES_DIR/$s_name"
  make_framework "$s_name" "$s_triple" "$s_sdk" "$s_kind"
done

if [ -n "$ONLY" ]; then
  echo
  echo "完成（只编了 ${ONLY}）：$SLICES_DIR/${ONLY}/SYFFmpeg.framework"
  echo "全量打包请不带 --only 再跑一次。"
  exit 0
fi

echo
echo "== [4/5] lipo 模拟器 fat，写入映射头（各 slice 符号并集） =="
lipo_simulator
write_union_prefix_headers

echo
echo "== [5/5] 打包 xcframework =="
pack_xcframework

echo
echo "完成：$OUT_DIR/SYFFmpeg.xcframework"
echo "这是动态 framework（见文件头与 D11.2）：App 侧要 Embed & Sign，"
echo "不是 Do Not Embed。"
echo "中间产物在 ${BUILD_ROOT}（.gitignore 的 build-*/ 已覆盖，不要提交）。"
