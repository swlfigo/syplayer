#!/bin/bash
# gen-embedded-header.sh — 把一个文本文件的内容内嵌进一个 C++ 头，包成
# 一个 `inline constexpr char[]` raw string literal 常量。
#
# 目前唯一的用途：src/platform/apple/video_shaders.metal 的源码需要被
# metal_renderer.mm 用 -[MTLDevice newLibraryWithSource:] 在运行期编译
# （见该文件顶部注释：这台机器没装独立的 Metal Toolchain 组件，走不了
# `xcrun metal`/`metallib` 离线编译）。顶层 CMakeLists.txt（真正的构建）
# 与 tools/check-deploy-target.sh（standalone -fsyntax-only 语法检查，不
# 经过 CMake）两处都需要生成同一个内嵌头，抽成这一个脚本，两边共用同一
# 份生成逻辑——不然两处各写一份，漂移了没人发现（这个仓库不止一次因为
# "两份平行逻辑各自漂移"吃过亏，参见 tools/syp_probe/frame_digest.cpp
# 顶部注释引用的教训）。
#
# 用法：gen-embedded-header.sh <input-file> <output-header> <namespace> <const-name>
set -euo pipefail

if [ "$#" -ne 4 ]; then
  echo "用法: $0 <input-file> <output-header> <namespace> <const-name>" >&2
  exit 1
fi

IN="$1"
OUT="$2"
NS="$3"
NAME="$4"

if [ ! -f "$IN" ]; then
  echo "gen-embedded-header.sh: 找不到输入文件: $IN" >&2
  exit 1
fi

# 分隔符选一个不太可能出现在着色器源码里的串；万一真的冲突，raw string
# 会编译失败（一个明显的错误，不是静默生成错误内容），不需要在这里做
# 更复杂的冲突检测。
DELIM="SYP_EMBED_V1"

mkdir -p "$(dirname "$OUT")"

{
  echo "// 自动生成——由 tools/gen-embedded-header.sh 从"
  echo "// $IN 内嵌而来，不要手改本文件；改动前者、重新跑一次生成脚本"
  echo "// （或重新 configure，顶层 CMakeLists.txt 会自动重跑）即可同步。"
  echo "#pragma once"
  echo ""
  echo "namespace $NS {"
  echo ""
  echo "inline constexpr char ${NAME}[] = R\"${DELIM}("
  cat "$IN"
  echo ""
  echo ")${DELIM}\";"
  echo ""
  echo "}  // namespace $NS"
} > "$OUT"
