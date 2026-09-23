#!/usr/bin/env bash
# check-ffmpeg-no-tls.sh —— 证明 SYFFmpeg 产物里没有 TLS 后端。
#
# 为什么需要这个：本项目为了过 hls.c 的协议名检查而 --enable-protocol=http，
# 而 https/tls 是**刻意不要**的（见 build-ffmpeg.sh 的组件表注释）。
# configure 的输出会变、命令行会被改，唯一可靠的是查产物符号。
#
# 用法：tools/check-ffmpeg-no-tls.sh [--expect-slices N] [xcframework]
#   --expect-slices N：Info.plist 的 AvailableLibraries 必须恰好 N 项。
set -euo pipefail

EXPECT_SLICES=""
if [ "${1:-}" = "--expect-slices" ]; then
  [ "$#" -ge 2 ] || { echo "--expect-slices 缺参数" >&2; exit 64; }
  EXPECT_SLICES="$2"
  shift 2
  case "$EXPECT_SLICES" in
    ""|*[!0-9]*) echo "--expect-slices 要一个整数" >&2; exit 64 ;;
  esac
fi

XCF="${1:-build-ffmpeg/out/SYFFmpeg.xcframework}"
[ -d "$XCF" ] || { echo "找不到 xcframework：$XCF" >&2; exit 70; }
[ -f "$XCF/Info.plist" ] || { echo "xcframework 缺 Info.plist：$XCF" >&2; exit 70; }

# 这些符号只要出现，就说明某个 TLS 后端被编了进来。
#
# 注意：不能用裸的 `ff_tls_` 前缀。libavformat/network.c 里的
# ff_tls_init()/ff_tls_deinit() 是通用桩函数，只要 CONFIG_NETWORK=1
# （本项目 --enable-protocol=http 必然拉上）就会被无条件编译进产物、
# 无条件被 avformat_network_init()/_deinit() 调用——跟有没有链 TLS 后端
# 无关，只有函数体内 `#if CONFIG_TLS_PROTOCOL` 那几行才受后端开关影响。
# 真正只在链了某个 TLS 后端时才会出现的，是各后端文件
# （tls_openssl.c / tls_gnutls.c / tls_schannel.c / tls_securetransport.c /
# tls_mbedtls.c / tls_libtls.c）统一导出的 `ff_tls_protocol`
# 这个 URLProtocol 符号——用它替代裸前缀。
BAD_SYMS='SSLCreateContext|SSLHandshake|tls_open|ff_tls_protocol|SSL_CTX_new|mbedtls_ssl_'

fail=0
nslices=0

# 正向确认用的符号：hls 与 http 真的在里面——不然"没有 TLS"可能只是因为
# 整个构建是空的。
GOOD_SYMS='ff_hls_demuxer ff_http_protocol ff_mpegts_demuxer'

# 【看的是全部符号表，不是导出表】SYFFmpeg 只导出 _syp_ 前缀的别名，原名降成了
# 非导出的本地符号（见 build-ffmpeg.sh 的"符号隔离"）。nm -g 只列外部符号，拿它
# 查原名会"一个都查不到"，负向因此恒过——所以这里一律用 nm -a（含本地符号），
# 正向也按**原名整名**匹配（_ff_hls_demuxer），不接受只在 _syp_ 别名里出现，
# 以证明确实看到了库内的完整符号表。负向按子串匹配，原名或别名命中都算。
#
# 【正负两个方向都必须逐 slice、逐架构查，不能只挑一个】实测：正向检查原来
# 只看 `find | head -1` 挑出的那**一个** slice，造一个两 slice 的假
# xcframework（A 真、B 空），脚本照样打印 ✓、rc=0。而 slice 之间**已经在
# 分叉**——build-ffmpeg.sh 的 Catalyst slice 带 --disable-videotoolbox
# ——且脚本支持 `--only <slice>` 单独重编，"只重编了一个 slice、另一个是
# 旧的/空的"正是现实中会发生的事。模拟器 slice 是 arm64 + x86_64 的 fat，
# 两个架构各自编出，也要分开查。
#
# 【以 Info.plist 为准逐项查】不是 find 盘上有什么：清单列了、盘上缺的 slice
# 用 find 根本看不见，照样打印 ✓。清单每一项都必须在盘上有二进制。
libs=()
nlibs="$(plutil -extract AvailableLibraries raw -o - "$XCF/Info.plist" 2>/dev/null || true)"
case "$nlibs" in
  ""|*[!0-9]*) echo "✗ 读不出 $XCF/Info.plist 的 AvailableLibraries" >&2; fail=1; nlibs=0 ;;
esac
if [ -n "$EXPECT_SLICES" ] && [ "$nlibs" -ne "$EXPECT_SLICES" ]; then
  echo "✗ Info.plist 的 AvailableLibraries 有 ${nlibs} 项，期望 ${EXPECT_SLICES} 项" >&2
  fail=1
fi
i=0
while [ "$i" -lt "$nlibs" ]; do
  lid="$(plutil -extract "AvailableLibraries.$i.LibraryIdentifier" raw -o - "$XCF/Info.plist" 2>/dev/null || true)"
  bpath="$(plutil -extract "AvailableLibraries.$i.BinaryPath" raw -o - "$XCF/Info.plist" 2>/dev/null || true)"
  i=$((i + 1))
  if [ -z "$lid" ] || [ -z "$bpath" ]; then
    echo "✗ Info.plist 第 ${i} 项缺 LibraryIdentifier / BinaryPath" >&2
    fail=1
  elif [ "$(basename "$bpath")" != "SYFFmpeg" ]; then
    echo "✗ ${lid}：二进制应当是 SYFFmpeg，Info.plist 写的是 ${bpath}" >&2
    fail=1
  elif [ ! -f "$XCF/$lid/$bpath" ]; then
    echo "✗ ${lid}：Info.plist 列的二进制 $bpath 盘上没有" >&2
    fail=1
  else
    libs+=("$XCF/$lid/$bpath")
  fi
done

for lib in ${libs[@]+"${libs[@]}"}; do
  # 先把架构取出来再循环：写成 for arch in $(lipo …) 时 lipo 失败只会让循环
  # 空转一次都不进，这个 slice 就被静默跳过了。
  if ! archs="$(xcrun lipo -archs "$lib" 2>/dev/null)" || [ -z "$archs" ]; then
    echo "✗ ${lib#"$XCF"/}：lipo 读不出架构" >&2
    fail=1
    continue
  fi
  for arch in $archs; do
    nslices=$((nslices + 1))
    tag="${lib#"$XCF"/} [${arch}]"
    # 一次 nm 同时供正负两个方向用，不重复读符号表。
    #
    # 注意：不能写成 `nm -a "$lib" | grep -q "$sym"`。符号表很大，grep -q
    # 一旦在中途找到第一个匹配就会关闭读端，nm 仍在写、收到 SIGPIPE 退出非零；
    # 在 set -o pipefail 下，这个非零会盖过 grep 其实已经匹配成功的事实，
    # 导致该符号"总是报告找不到"——跟符号是否真的存在无关，纯粹是管道时序问题。
    # 改成先用命令替换把 nm 输出整体读完（不会有读端提前关闭），再用 bash
    # 自带的字符串匹配判断，避免这个陷阱。
    syms=$(xcrun nm -a -arch "$arch" "$lib" 2>/dev/null || true)
    if [ -z "$syms" ]; then
      echo "✗ ${tag} 读不出符号表" >&2
      fail=1
      continue
    fi

    # 负向：这些符号只要出现，就说明某个 TLS 后端被编了进来。
    hits=$(printf '%s\n' "$syms" | grep -E "$BAD_SYMS" || true)
    if [ -n "$hits" ]; then
      echo "✗ ${tag} 含 TLS 符号：" >&2
      echo "$hits" | head -5 >&2
      fail=1
    fi

    # 正向：组件真的编进去了，而且看到的是库内原名（本地符号）。
    for sym in $GOOD_SYMS; do
      case "$syms" in
        *" _${sym}"$'\n'*|*" _${sym}") ;;
        *) echo "✗ ${tag} 的完整符号表里找不到 _${sym} —— 该 slice 的组件没编进去，或没看到本地符号" >&2; fail=1 ;;
      esac
    done
  done
done

# 一个 slice 都没有的话，上面那个循环一次也不进，两个方向都"没发现问题"。
if [ "$nslices" -eq 0 ]; then
  echo "✗ $XCF 里一个 SYFFmpeg slice 都没有" >&2
  fail=1
fi

[ "$fail" -eq 0 ] && echo "✓ SYFFmpeg 产物（${nslices} 个 slice×架构）：有 hls/http/mpegts，无 TLS 后端"
exit "$fail"
