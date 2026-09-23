#!/bin/bash
# 验证公开头文件守住 C ABI 纪律：
#   1. 严格 C11 能编（保证 Swift / Rust / Android JNI 都能绑）
#   2. C++23 能编（内部实现用）
#   3. Objective-C 能编（iOS bridging header 路径）
#   4. iOS 部署目标下能编
#   5. 没有泄漏 C++ / ObjC / 平台专有类型
P="$(cd "$(dirname "$0")/.." && pwd)"
INC="$P/include"
XC=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
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
if [ ! -d "$SDK_IOS" ]; then
  echo "找不到 iPhoneOS SDK（本机 Xcode / 开发者目录环境问题，不是代码问题）。"
  echo "已尝试：xcrun --sdk iphoneos --show-sdk-path"
  echo "        DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun --sdk iphoneos --show-sdk-path"
  echo "        硬编码回退：$SDK_FALLBACK"
  echo "请安装 Xcode 并确保 iOS SDK 可用。若 xcode-select 指向 Command Line Tools，可设置"
  echo "  DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer"
  exit 1
fi

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0

cat > "$T/all.h" <<'H'
#include <syplayer/syp_types.h>
#include <syplayer/syp_config.h>
#include <syplayer/syp_http.h>
#include <syplayer/syp_source.h>
#include <syplayer/syp_preload.h>
#include <syplayer/syp_net.h>
H
# 编两个 TU 并链接，顺带验证没有 ODR / 重复定义问题
printf '#include "all.h"\nint main(void){ syp_config c; syp_config_init(&c); (void)c; return 0; }\n' > "$T/a.c"
printf '#include "all.h"\n' > "$T/b.c"

t() { # 名称 编译器 flags...
  local name="$1"; shift
  if "$@" 2>"$T/err"; then printf "  ✅ %s\n" "$name"
  else printf "  ❌ %s\n" "$name"; sed 's/^/       /' "$T/err" | head -8; fail=1; fi
}

echo "== 编译矩阵 =="
t "C11 严格 (-pedantic -Wall -Wextra)" \
  $XC -std=c11 -pedantic -Wall -Wextra -Werror -I"$INC" -I"$T" -fsyntax-only "$T/a.c"
t "C17" \
  $XC -std=c17 -Wall -Wextra -Werror -I"$INC" -I"$T" -fsyntax-only "$T/a.c"
t "C++23" \
  $XC -x c++ -std=c++23 -Wall -Wextra -Werror -I"$INC" -I"$T" -fsyntax-only "$T/a.c"
t "Objective-C" \
  $XC -x objective-c -std=c11 -Wall -Wextra -Werror -I"$INC" -I"$T" -fsyntax-only "$T/a.c"
t "Objective-C++" \
  $XC -x objective-c++ -std=c++23 -Wall -Wextra -Werror -I"$INC" -I"$T" -fsyntax-only "$T/a.c"
t "iOS 13 / arm64 (严格 availability)" \
  $XC -std=c11 -target arm64-apple-ios13.0 -isysroot "$SDK_IOS" \
     -Werror=unguarded-availability -Werror=unguarded-availability-new \
     -I"$INC" -I"$T" -fsyntax-only "$T/a.c"
t "多 TU 无重复符号" \
  $XC -std=c11 -I"$INC" -I"$T" -fsyntax-only "$T/a.c" "$T/b.c"

echo "== 禁止类型扫描 =="
# 先剥掉注释（注释里提到 std:: / NSURLSession 是正常的），再用词边界匹配
BAD='\bstd::|\bNS[A-Z][A-Za-z]*|@interface|@protocol|\bCF[A-Z][A-Za-z]*Ref|\bCV[A-Z][A-Za-z]*Ref|\bMTL[A-Z]|\btemplate[[:space:]]*<|\bnamespace[[:space:]]|\bclass[[:space:]]+[A-Za-z]|\bshared_ptr|\bunique_ptr|\bdispatch_[a-z]*_t'
hits=""
for f in "$INC"/syplayer/*.h; do
  stripped=$(sed -e 's|//.*||' -e 's|/\*.*\*/||' "$f")
  h=$(printf '%s\n' "$stripped" | grep -nE "$BAD" || true)
  [ -n "$h" ] && hits="$hits\n$(basename $f): $h"
done
if [ -z "$hits" ]; then echo "  ✅ 未发现 C++/ObjC/平台专有类型（已剥注释 + 词边界匹配）"
else echo "  ❌ 发现泄漏："; printf "%b\n" "$hits" | sed 's/^/       /'; fail=1; fi

echo "== extern \"C\" 覆盖 =="
for f in "$INC"/syplayer/*.h; do
  o=$(grep -c 'extern "C" {' "$f"); c=$(grep -c '^}  *// extern "C"\|^}$' "$f")
  [ "$o" -ge 1 ] && printf "  ✅ %s\n" "$(basename $f)" || { printf "  ❌ %s 缺 extern \"C\"\n" "$(basename $f)"; fail=1; }
done

echo "== Swift 原生绑定 =="
SWIFTC=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/swiftc
MAC_SDK=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
cat > "$T/probe.swift" <<'SW'
import SYPlayerC
func probe() {
    var cfg = syp_config(); syp_config_init(&cfg)
    cfg.max_concurrent_tasks = 3
    var src: OpaquePointer? = nil; _ = src
    let r = syp_range(start: 0, end: 1024); _ = r.end - r.start
    var cb = syp_source_callbacks()
    cb.on_buffering = { _, b in _ = b }
    var backend = syp_http_backend()
    backend.cancel = { h in _ = h }
    _ = (cb, backend)
    var pc = syp_preload_config(); syp_preload_config_init(&pc)
    var mip = syp_media_info_provider()
    mip.estimate_range_for_ms = { _, _, _, s, e in _ = (s, e); return syp_status(SYP_ERR_NOT_IMPLEMENTED) }
    _ = (pc, mip)
}
SW
if [ -x "$SWIFTC" ] && $SWIFTC -typecheck -sdk "$MAC_SDK" -I "$INC/syplayer" "$T/probe.swift" 2>"$T/sw"; then
  echo "  ✅ Swift import SYPlayerC 成功（结构体/枚举/闭包转 C 函数指针/不透明指针）"
else
  echo "  ❌ Swift 绑定失败"; grep -m3 "error:" "$T/sw" 2>/dev/null | sed 's/^/       /'; fail=1
fi

echo
[ $fail -eq 0 ] && echo "全部通过。" || echo "有失败项。"
exit $fail
