#!/usr/bin/env bash
# check-ffmpeg-symbols.sh —— 证明 SYFFmpeg 对外零未加前缀符号，且映射头与导出表同源。
#
# 用法：
#   tools/check-ffmpeg-symbols.sh [选项] [xcframework] [消费方二进制或静态库 ...]
#   xcframework 缺省为 build-ffmpeg/out/SYFFmpeg.xcframework。
#
# 选项：
#   --expect-slices N      Info.plist 的 AvailableLibraries 必须恰好 N 项。
#   --dist                 检查的是分发件（build-spm/out 下那份）：每个 slice 都
#                          不得带 Headers（目录或符号链接），且 framework 结构
#                          经得起 codesign 签名与 --strict 校验（删 Headers 后
#                          不留悬空链接）。映射头改由 --prefix-header 提供。
#   --prefix-header FILE   用这份映射头做第 3–6 项比对（--dist 时必须给，通常是
#                          build-ffmpeg/out 那份构建产物里的）。
#
# 为什么需要这个：接入方的 App 可能自带另一份 FFmpeg。只要我们的 dylib 导出了
# 任何一个原名（av_read_frame 之类），接入方自己的调用就可能按链接顺序绑到我们
# 这份上，版本不符、无报错地崩或错。build-ffmpeg.sh 用 ld 的别名 + 导出清单把
# 导出表收窄到 _syp_*；这里从产物反查，防止构建脚本退化、或有人绕过映射头。
#
# 检查项（全部逐 slice、逐架构，任何一项不过都继续查完再统一报错）：
#   0. Info.plist 的 AvailableLibraries 每一项在盘上都有 framework 与二进制，
#      盘上也没有清单之外的 slice；给了 --expect-slices 时数目必须相等；
#   1. 每个 slice 都是 SYFFmpeg.framework，二进制名 SYFFmpeg；
#   2. 导出表（nm -gU 与 dyld_info -exports 两个视角）里不以 _syp_ 开头的项为 0，
#      白名单见 ALLOW_UNPREFIXED；
#   3. 构建产物：每个 slice 都带 Headers/syp_ffmpeg_prefix.h，且各 slice 内容一致；
#      分发件：每个 slice 都不带 Headers。映射头只允许生成器写出的那几种行形状：
#      注释/空行、include guard、"#define 名 syp_名"、末尾那段 extern "C" 补声明
#      （声明的函数名必须在映射里）——其他任何行（#undef、别的宏）都判失败；
#   4. 每个 slice、每个架构的导出都被映射头覆盖（否则使用方只能写原名去引用）；
#   5. 映射头里每个名字至少在一个 slice 里有 _syp_ 导出（映射不指向虚空）。
#      映射头是各 slice 的并集（理由见 build-ffmpeg.sh 的 write_union_prefix_headers），
#      某名字在个别 slice 缺席是预期的（Catalyst 没有 VideoToolbox、x86_64 模拟器
#      没有 NEON），逐 slice 打印缺席数以供核对；
#   6. 给了消费方文件时（逐架构，fat 文件每一片都查）：其未定义符号里不得有未加
#      前缀的 FFmpeg 名（命中映射头的名字，或属于 FFmpeg 前缀族），引用的 _syp_ 名
#      必须真有 slice 导出；自己定义的 _syp_ 名不得与导出重名。
#
# 退出码：0 通过；1 检查失败；64 参数错误；70 找不到输入。
set -euo pipefail

EXPECT_SLICES=""
DIST=0
PREFIX_HEADER=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --expect-slices) [ "$#" -ge 2 ] || { echo "--expect-slices 缺参数" >&2; exit 64; }
                     EXPECT_SLICES="$2"; shift 2 ;;
    --dist)          DIST=1; shift ;;
    --prefix-header) [ "$#" -ge 2 ] || { echo "--prefix-header 缺参数" >&2; exit 64; }
                     PREFIX_HEADER="$2"; shift 2 ;;
    --) shift; break ;;
    -*) echo "未知选项：$1" >&2; exit 64 ;;
    *) break ;;
  esac
done
case "$EXPECT_SLICES" in
  ""|*[!0-9]*) [ -z "$EXPECT_SLICES" ] || { echo "--expect-slices 要一个整数" >&2; exit 64; } ;;
esac
if [ "$DIST" -eq 1 ] && [ -z "$PREFIX_HEADER" ]; then
  echo "--dist 需要同时给 --prefix-header（分发件自己不带映射头）" >&2
  exit 64
fi
if [ -n "$PREFIX_HEADER" ] && [ ! -f "$PREFIX_HEADER" ]; then
  echo "找不到映射头：$PREFIX_HEADER" >&2
  exit 70
fi

XCF="${1:-build-ffmpeg/out/SYFFmpeg.xcframework}"
[ "$#" -gt 0 ] && shift
[ -d "$XCF" ] || { echo "找不到 xcframework：$XCF" >&2; exit 70; }
[ -f "$XCF/Info.plist" ] || { echo "xcframework 缺 Info.plist：$XCF" >&2; exit 70; }

# 导出表里允许出现的非 _syp_ 名字。目前为空：实测 ld 合成的 dylib 在
# -exported_symbols_list 下不会额外导出任何系统符号（__mh_dylib_header 之类都是
# 本地符号）。以后若确有 dyld/链接器必需的项，逐个写进来并注明理由。
ALLOW_UNPREFIXED=()

# 消费方未定义符号里算作"FFmpeg 原名"的前缀族（映射头之外的兜底：哪怕映射头
# 本身漏了某个名字，只要长得像 FFmpeg 的也要拦下）。
FFMPEG_FAMILY_RE='^_(av|avcodec|avformat|avio|avutil|avfilter|avpriv|avsubtitle|sws|swr|swscale|swresample|swri|ff|ffio|ffurl)_'

TMP="$(mktemp -d "${TMPDIR:-/tmp}/check-ffmpeg-symbols.XXXXXX")"
# 中途中止一律按失败处理。实测 macOS 自带的 bash 3.2 只要设了 EXIT trap，
# set -u 碰到未定义变量而中止时，trap 里的 $? 是 0、脚本整体也以 0 退出——
# 门禁会假绿。所以不信 $?：没走到脚本末尾的正常出口就强制非 0。
finished=0
trap 'rc=$?; rm -rf "$TMP"; if [ "$finished" != 1 ] && [ "$rc" -eq 0 ]; then rc=1; fi; exit "$rc"' EXIT

fail=0
err() { echo "✗ $*" >&2; fail=1; }

is_allowed() {
  local s="$1" a
  for a in ${ALLOW_UNPREFIXED[@]+"${ALLOW_UNPREFIXED[@]}"}; do
    [ "$s" = "$a" ] && return 0
  done
  return 1
}

# 一律先把工具输出整体写进文件再处理，不在管道里用会提前退出的消费者
# （grep -q / head），避免 pipefail 下的 SIGPIPE 误判。

: > "$TMP/union_exports"
nslices=0
first_header=""

# ---- 0. Info.plist 清单与盘上内容逐项对上 ----
# 以 Info.plist 为准逐项查，而不是只看盘上有什么：xcframework 被消费时 Xcode/
# SwiftPM 读的是清单，清单里有、盘上缺的 slice 只会在某个平台链接时才爆出来。
nlibs="$(plutil -extract AvailableLibraries raw -o - "$XCF/Info.plist" 2>/dev/null || true)"
case "$nlibs" in
  ""|*[!0-9]*) err "读不出 $XCF/Info.plist 的 AvailableLibraries"; nlibs=0 ;;
esac
if [ -n "$EXPECT_SLICES" ] && [ "$nlibs" -ne "$EXPECT_SLICES" ]; then
  err "Info.plist 的 AvailableLibraries 有 ${nlibs} 项，期望 ${EXPECT_SLICES} 项"
fi
: > "$TMP/slices"
: > "$TMP/listed"
i=0
while [ "$i" -lt "$nlibs" ]; do
  lid="$(plutil -extract "AvailableLibraries.$i.LibraryIdentifier" raw -o - "$XCF/Info.plist" 2>/dev/null || true)"
  lpath="$(plutil -extract "AvailableLibraries.$i.LibraryPath" raw -o - "$XCF/Info.plist" 2>/dev/null || true)"
  bpath="$(plutil -extract "AvailableLibraries.$i.BinaryPath" raw -o - "$XCF/Info.plist" 2>/dev/null || true)"
  i=$((i + 1))
  echo "$lid" >> "$TMP/listed"
  if [ -z "$lid" ] || [ -z "$lpath" ] || [ -z "$bpath" ]; then
    err "Info.plist 第 ${i} 项缺 LibraryIdentifier / LibraryPath / BinaryPath"
    continue
  fi
  if [ ! -d "$XCF/$lid/$lpath" ]; then
    err "${lid}：Info.plist 列了 ${lpath}，盘上没有"
    continue
  fi
  if [ ! -f "$XCF/$lid/$bpath" ]; then
    err "${lid}：Info.plist 列的二进制 $bpath 盘上没有"
    continue
  fi
  printf '%s|%s|%s\n' "$lid" "$lpath" "$bpath" >> "$TMP/slices"
done
for d in "$XCF"/*/; do
  d="$(basename "${d%/}")"
  if ! awk -v d="$d" '$0 == d { found = 1 } END { exit !found }' "$TMP/listed"; then
    for f in "$XCF/$d"/*.framework; do
      [ -d "$f" ] && err "${d}：盘上有 slice，Info.plist 里没有"
    done
  fi
done

while IFS='|' read -r slice lpath bpath; do
  nslices=$((nslices + 1))
  fw="$XCF/$slice/$lpath"
  bin="$XCF/$slice/$bpath"
  if [ "$lpath" != "SYFFmpeg.framework" ] || [ "$(basename "$bpath")" != "SYFFmpeg" ]; then
    err "${slice}：framework 应当是 SYFFmpeg.framework、二进制名 SYFFmpeg，实际：$lpath / $bpath"
  fi
  nfw=0
  for f in "$XCF/$slice"/*.framework; do [ -d "$f" ] && nfw=$((nfw + 1)); done
  [ "$nfw" -eq 1 ] || err "${slice}：slice 里应当恰好一个 framework，实际 ${nfw} 个"

  # ---- 映射头 / 分发件不带头文件 ----
  if [ "$DIST" -eq 1 ]; then
    find "$fw" -name Headers \( -type d -o -type l \) > "$TMP/headers.found" 2>/dev/null || true
    find "$fw" \( -name '*.h' -o -name '*.modulemap' \) >> "$TMP/headers.found" 2>/dev/null || true
    if [ -s "$TMP/headers.found" ]; then
      err "${slice}：分发件不应带头文件，却有："
      sed -n '1,5p' "$TMP/headers.found" | sed 's/^/    /' >&2
    fi
    # 悬空符号链接（例如删了 Versions/A/Headers 却留着顶层 Headers 链接）。
    # 不用 find -L：它会顺着指向包外的链接一路走进去。逐个链接判断目标在不在。
    : > "$TMP/dangling.links"
    while IFS= read -r l; do
      [ -e "$l" ] || echo "$l" >> "$TMP/dangling.links"
    done < <(find "$fw" -type l 2>/dev/null)
    if [ -s "$TMP/dangling.links" ]; then
      err "${slice}：有悬空符号链接："
      sed -n '1,5p' "$TMP/dangling.links" | sed 's/^/    /' >&2
    fi
    # 结构校验：在副本上 ad-hoc 签名再 --strict 验证。bundle 结构不合法
    # （悬空/越界链接、versioned 布局缺项）时 codesign 会拒签或验不过。
    rm -rf "$TMP/cs"; mkdir -p "$TMP/cs"
    cp -R "$fw" "$TMP/cs/"
    if ! codesign --force --sign - "$TMP/cs/$lpath" > "$TMP/cs.log" 2>&1; then
      err "${slice}：framework 副本 ad-hoc 签名失败："
      sed -n '1,5p' "$TMP/cs.log" | sed 's/^/    /' >&2
    elif ! codesign --verify --strict --verbose=2 "$TMP/cs/$lpath" > "$TMP/cs.log" 2>&1; then
      err "${slice}：framework 副本 codesign --verify --strict 不过："
      sed -n '1,5p' "$TMP/cs.log" | sed 's/^/    /' >&2
    fi
  else
    hdr="$fw/Headers/syp_ffmpeg_prefix.h"
    if [ ! -f "$hdr" ]; then
      err "${slice}：缺映射头 Headers/syp_ffmpeg_prefix.h"
    elif [ -z "$first_header" ]; then
      first_header="$hdr"
    elif ! cmp -s "$first_header" "$hdr"; then
      err "${slice}：映射头与第一个 slice 的不一致：$hdr"
    fi
  fi

  # ---- 导出表，逐架构 ----
  if ! archs="$(xcrun lipo -archs "$bin" 2>/dev/null)" || [ -z "$archs" ]; then
    err "${slice}：lipo 读不出架构：$bin"
    continue
  fi
  for arch in $archs; do
    tag="$slice/$arch"
    xcrun nm -gU -arch "$arch" "$bin" > "$TMP/nm.out" 2>/dev/null || { err "${tag}：nm 失败"; continue; }
    awk 'NF >= 3 { print $NF }' "$TMP/nm.out" | LC_ALL=C sort -u > "$TMP/exp.nm"
    xcrun dyld_info -arch "$arch" -exports "$bin" > "$TMP/dyld.out" 2>/dev/null || { err "${tag}：dyld_info 失败"; continue; }
    awk '$1 ~ /^0x[0-9A-Fa-f]+$/ { print $2 }' "$TMP/dyld.out" | LC_ALL=C sort -u > "$TMP/exp.dyld"

    nexp_nm="$(wc -l < "$TMP/exp.nm" | tr -d ' ')"
    nexp_dyld="$(wc -l < "$TMP/exp.dyld" | tr -d ' ')"
    if [ "$nexp_nm" -eq 0 ]; then
      err "${tag}：导出表为空"
      continue
    fi

    nbad=0
    for view in nm dyld; do
      grep -v '^_syp_' "$TMP/exp.$view" > "$TMP/bad.$view" || true
      : > "$TMP/bad.$view.real"
      while IFS= read -r s; do
        [ -n "$s" ] || continue
        is_allowed "$s" || echo "$s" >> "$TMP/bad.$view.real"
      done < "$TMP/bad.$view"
      n="$(wc -l < "$TMP/bad.$view.real" | tr -d ' ')"
      [ "$n" -gt "$nbad" ] && nbad="$n"
      if [ "$n" -ne 0 ]; then
        err "${tag}：导出表（$view 视角）里有 $n 个未加 _syp_ 前缀的符号，例如："
        sed -n '1,5p' "$TMP/bad.$view.real" | sed 's/^/    /' >&2
      fi
    done
    if ! cmp -s "$TMP/exp.nm" "$TMP/exp.dyld"; then
      err "${tag}：nm -gU（${nexp_nm}）与 dyld_info -exports（${nexp_dyld}）看到的导出集合不同"
    fi

    # 去掉前缀后的名字集合，给映射头比对用。
    sed -n 's/^_syp_//p' "$TMP/exp.nm" | LC_ALL=C sort -u > "$TMP/exp.$slice.$arch"
    cat "$TMP/exp.$slice.$arch" >> "$TMP/union_exports"
    echo "  ${tag}：导出 ${nexp_nm} 个，未加前缀 ${nbad} 个"
  done
done < "$TMP/slices"

[ -n "$PREFIX_HEADER" ] && first_header="$PREFIX_HEADER"

if [ "$nslices" -eq 0 ]; then
  err "$XCF 里一个 framework slice 都没有"
fi

LC_ALL=C sort -u "$TMP/union_exports" -o "$TMP/union_exports"

# ---- 映射头内容与导出表比对 ----
if [ -z "$first_header" ]; then
  err "没有可比对的映射头"
else
  # 整个文件只允许生成器（build-ffmpeg.sh 的 write_prefix_header）写出的几种
  # 行形状，按顺序：
  #   开头：注释/空行 → #pragma once → #ifndef/#define SYP_FFMPEG_PREFIX_H
  #   映射：#define 名 syp_名（其间可有注释/空行）
  #   可选：#include "libavutil/attributes.h" / #ifdef __cplusplus / extern "C" {
  #         / #endif / 若干 "…名(…);" 声明 / #ifdef __cplusplus / } / #endif
  #   结尾：#endif，其后只许空行
  # 其他任何行（#undef、别的宏、多余的 #include）都判失败——映射头被 -include
  # 进每个编译单元，夹带的东西会悄悄改变所有代码的语义。
  awk '
    function bad(msg) { printf "第 %d 行（%s）：%s\n", NR, msg, $0; nbad++ }
    BEGIN { st = "pre"; ndef = 0 }
    {
      line = $0
      if (st != "end" && (line ~ /^\/\// || line ~ /^[ \t]*$/)) {
        if (st == "decl") bad("声明块里不该有注释/空行")
        next
      }
      if (st == "pre")    { if (line == "#pragma once") st = "guard1"; else bad("开头只许注释与 #pragma once"); next }
      if (st == "guard1") { if (line == "#ifndef SYP_FFMPEG_PREFIX_H") st = "guard2"; else bad("缺 include guard"); next }
      if (st == "guard2") { if (line == "#define SYP_FFMPEG_PREFIX_H") st = "defs"; else bad("缺 include guard"); next }
      if (st == "defs") {
        if (line ~ /^#define [A-Za-z_][A-Za-z0-9_]* syp_[A-Za-z_][A-Za-z0-9_]*$/) {
          split(line, f, " ")
          if (f[3] != "syp_" f[2]) bad("映射目标不是 syp_+原名")
          else { print f[2] > names; defd[f[2]] = 1; ndef++ }
          next
        }
        if (line == "#include \"libavutil/attributes.h\"") { st = "ifcpp1"; next }
        if (line == "#endif") { st = "end"; next }
        bad("映射区只许 \"#define 名 syp_名\""); next
      }
      if (st == "ifcpp1") { if (line == "#ifdef __cplusplus") st = "extc"; else bad("补声明块形状不对"); next }
      if (st == "extc")   { if (line == "extern \"C\" {") st = "endif1"; else bad("补声明块形状不对"); next }
      if (st == "endif1") { if (line == "#endif") { st = "decl"; ndecl = 0 } else bad("补声明块形状不对"); next }
      if (st == "decl") {
        if (line == "#ifdef __cplusplus") { if (ndecl == 0) bad("补声明块是空的"); st = "close"; next }
        if (line ~ /^[A-Za-z_][A-Za-z0-9_ *]*[ *][A-Za-z_][A-Za-z0-9_]*\([^;{}#]*\);$/) {
          fn = line; sub(/\(.*/, "", fn); sub(/.*[ *]/, "", fn)
          if (!(fn in defd)) bad("补声明的函数不在映射里")
          ndecl++
          next
        }
        bad("补声明块只许函数声明"); next
      }
      if (st == "close")  { if (line == "}") st = "endif2"; else bad("补声明块形状不对"); next }
      if (st == "endif2") { if (line == "#endif") st = "tail"; else bad("补声明块形状不对"); next }
      if (st == "tail")   { if (line == "#endif") st = "end"; else bad("补声明块之后只许结尾 #endif"); next }
      if (st == "end")    { if (line !~ /^[ \t]*$/) bad("结尾 #endif 之后还有内容"); next }
    }
    END {
      if (st != "end") { printf "文件在状态 %s 结束，缺结尾 #endif 或结构不完整\n", st; nbad++ }
      if (ndef == 0)   { print "一条映射都没有"; nbad++ }
    }
  ' names="$TMP/hdr.raw_names" "$first_header" > "$TMP/hdr.malformed" || true
  if [ -s "$TMP/hdr.malformed" ]; then
    err "映射头 $first_header 里有生成器不会写出的行："
    sed -n '1,5p' "$TMP/hdr.malformed" | sed 's/^/    /' >&2
  fi
  touch "$TMP/hdr.raw_names"
  LC_ALL=C sort -u "$TMP/hdr.raw_names" > "$TMP/hdr.names"
  nhdr="$(wc -l < "$TMP/hdr.names" | tr -d ' ')"
  echo "  映射头：${nhdr} 条"
  [ "$nhdr" -gt 0 ] || err "映射头是空的"

  for f in "$TMP"/exp.*.*; do
    [ -f "$f" ] || continue
    tag="${f#"$TMP"/exp.}"
    case "$tag" in nm|dyld) continue ;; esac
    tag="${tag%.*}/${tag##*.}"
    LC_ALL=C comm -23 "$f" "$TMP/hdr.names" > "$TMP/unmapped"
    if [ -s "$TMP/unmapped" ]; then
      err "${tag}：有 $(wc -l < "$TMP/unmapped" | tr -d ' ') 个导出没被映射头覆盖，例如："
      sed -n '1,5p' "$TMP/unmapped" | sed 's/^/    /' >&2
    fi
    nabsent="$(LC_ALL=C comm -23 "$TMP/hdr.names" "$f" | wc -l | tr -d ' ')"
    echo "  ${tag}：映射头里本 slice 不导出的名字 ${nabsent} 个（并集头，按平台差异预期）"
  done

  LC_ALL=C comm -23 "$TMP/hdr.names" "$TMP/union_exports" > "$TMP/dangling"
  if [ -s "$TMP/dangling" ]; then
    err "映射头里有 $(wc -l < "$TMP/dangling" | tr -d ' ') 个名字在任何 slice 都没有 _syp_ 导出，例如："
    sed -n '1,5p' "$TMP/dangling" | sed 's/^/    /' >&2
  fi
fi

# ---- 消费方 ----
for consumer in "$@"; do
  if [ ! -f "$consumer" ]; then
    err "找不到消费方文件：$consumer"
    continue
  fi
  # 逐架构查。不带 -arch 的 nm 对 fat 文件只看宿主架构那一片——模拟器 slice 的
  # x86_64 那一片就整片漏检（实测：x86_64 片引用裸 _av_malloc 的 fat .o 照样 ✓）。
  if ! carchs="$(xcrun lipo -archs "$consumer" 2>/dev/null)" || [ -z "$carchs" ]; then
    err "${consumer}：lipo 读不出架构"
    continue
  fi
  for arch in $carchs; do
    ctag="$(basename "$consumer") [${arch}]"
    if ! xcrun nm -u -arch "$arch" "$consumer" > "$TMP/undef.raw" 2>/dev/null; then
      err "${consumer} [${arch}]：nm -u 失败"
      continue
    fi
    # nm -u 对静态库会夹着 "xxx.o:" 成员名行；只取符号名。
    grep -E '^_' "$TMP/undef.raw" | LC_ALL=C sort -u > "$TMP/undef" || true
    : > "$TMP/undef.bad"
    if [ -f "$TMP/hdr.names" ]; then
      sed 's/^/_/' "$TMP/hdr.names" | LC_ALL=C comm -12 - "$TMP/undef" >> "$TMP/undef.bad"
    fi
    grep -E "$FFMPEG_FAMILY_RE" "$TMP/undef" >> "$TMP/undef.bad" || true
    LC_ALL=C sort -u "$TMP/undef.bad" -o "$TMP/undef.bad"
    # _syp_ 也是本项目自己公开 C 接口的前缀（syp_source_open 之类），所以只看
    # "去掉 syp_ 后是 FFmpeg 名"的那部分：在映射头里，或属于 FFmpeg 前缀族。
    sed -n 's/^_syp_//p' "$TMP/undef" | LC_ALL=C sort -u > "$TMP/undef.syp.all"
    : > "$TMP/undef.syp"
    if [ -f "$TMP/hdr.names" ]; then
      LC_ALL=C comm -12 "$TMP/undef.syp.all" "$TMP/hdr.names" >> "$TMP/undef.syp"
    fi
    sed 's/^/_/' "$TMP/undef.syp.all" | grep -E "$FFMPEG_FAMILY_RE" | sed 's/^_//' >> "$TMP/undef.syp" || true
    LC_ALL=C sort -u "$TMP/undef.syp" -o "$TMP/undef.syp"
    LC_ALL=C comm -23 "$TMP/undef.syp" "$TMP/union_exports" > "$TMP/undef.missing"
    # 反过来：消费方自己定义的 _syp_ 名不得与 SYFFmpeg 的导出重名，否则两级命名
    # 空间之外（静态链进同一镜像时）会撞符号。
    if ! xcrun nm -gU -arch "$arch" "$consumer" > "$TMP/def.raw" 2>/dev/null; then
      err "${consumer} [${arch}]：nm -gU 失败"
      continue
    fi
    awk 'NF >= 3 && $NF ~ /^_syp_/ { sub(/^_syp_/, "", $NF); print $NF }' "$TMP/def.raw" \
      | LC_ALL=C sort -u > "$TMP/def.syp"
    LC_ALL=C comm -12 "$TMP/def.syp" "$TMP/union_exports" > "$TMP/def.clash"
    if [ -s "$TMP/def.clash" ]; then
      err "${consumer} [${arch}] 自己定义的 _syp_ 符号与 SYFFmpeg 的导出重名，例如："
      sed -n '1,5p' "$TMP/def.clash" | sed 's/^/    _syp_/' >&2
    fi
    nsyp="$(wc -l < "$TMP/undef.syp" | tr -d ' ')"
    nbad="$(wc -l < "$TMP/undef.bad" | tr -d ' ')"
    echo "  消费方 ${ctag}：引用 SYFFmpeg 的 _syp_ 名 ${nsyp} 个，未加前缀的 FFmpeg 名 ${nbad} 个"
    if [ "$nbad" -ne 0 ]; then
      err "${consumer} [${arch}] 引用了未加前缀的 FFmpeg 符号（没有经过映射头？），例如："
      sed -n '1,5p' "$TMP/undef.bad" | sed 's/^/    /' >&2
    fi
    if [ -s "$TMP/undef.missing" ]; then
      err "${consumer} [${arch}] 引用的 _syp_ 名在任何 slice 都没有导出，例如："
      sed -n '1,5p' "$TMP/undef.missing" | sed 's/^/    _syp_/' >&2
    fi
  done
done

if [ "$fail" -eq 0 ]; then
  if [ "$DIST" -eq 1 ]; then
    echo "✓ SYFFmpeg（分发件）：${nslices} 个 slice 导出表全部带 _syp_ 前缀，不带头文件，结构经得起 codesign 校验"
  else
    echo "✓ SYFFmpeg：${nslices} 个 slice 导出表全部带 _syp_ 前缀，映射头与导出表一致"
  fi
fi
finished=1
exit "$fail"
