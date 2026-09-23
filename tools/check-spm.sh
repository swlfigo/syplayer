#!/usr/bin/env bash
# check-spm.sh — SwiftPM 接入门禁：两个示例 App 经仓库根的 Package.swift 接入
# SYPlayerKit（本地产物模式），在 iOS 模拟器、Mac Catalyst（iOS 示例）与原生
# macOS 上各跑一遍测试。
#
# 前提：先跑过 tools/build-xcframework.sh，build-spm/out/ 下有
# SYPlayerKit.xcframework 与 SYFFmpeg.xcframework。缺产物 exit 2（与"检查失败"
# 的 exit 1 区分开：缺产物是没准备好，不是接入坏了）。
#
# 本地模式靠环境变量 SYPLAYER_LOCAL_BINARIES=1：xcodebuild 解析包时把自身的
# 环境传给清单求值，清单据此把两个 binaryTarget 指向 build-spm/out/。脚本在
# 每一路跑完后到解析结果（SourcePackages/workspace-state.json）里核对两个
# 产物确实来自本地路径——没有这一步，环境变量没生效时会悄悄去拉远程 zip，
# 而"拉到了旧版 Release 并且测试绿了"与"本地产物验证通过"在输出上无法区分。
#
# zip 形态：三路测试走的是 build-spm/out 下的 xcframework 目录，而发版上传的是
# 同目录下的两个 zip。脚本最后在 build-spm/zipcheck/ 生成一个一次性的包（不入库），
# 两个 binaryTarget 用 path 指向这两个 zip 的逐字节拷贝（先核对 checksum 与
# checksums.txt 一致），原生 macOS 上 swift build 并运行一个可执行目标、
# iOS 模拟器与 Mac Catalyst 上 xcodebuild build 一个库目标——压缩包本身坏了
# （漏文件、目录层级不对、带进 AppleDouble 垃圾）会在这里暴露，而不是等到发版后。
#
# 签名：三路都用 CODE_SIGNING_ALLOWED=NO。原生 macOS 与 Mac Catalyst 的 hosted
# 测试在这个设置下都能跑：arm64 上链接器会给可执行文件与 framework 自动打 ad-hoc
# 签名，够本机加载。
#
# 判定：逐路记 EXIT 与 "Executed N tests"，任一路 EXIT 非 0、拿不到 Executed 行、
# Executed 行带失败、或执行的用例数不等于这一路钉死的期望数，整体 exit 1——
# 期望数钉死是为了抓"用例没被编进测试目标、少跑了几条却照样全绿"。
# 日志存在 $LOG_DIR 下。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT="$ROOT/build-spm/out"
LOG_DIR="${LOG_DIR:-$ROOT/build-spm/check-spm}"

for fw in SYPlayerKit SYFFmpeg; do
  if [[ ! -d "$OUT/$fw.xcframework" || ! -f "$OUT/$fw.xcframework.zip" ]]; then
    echo "缺少 $OUT/$fw.xcframework 或其 zip：先跑 tools/build-xcframework.sh" >&2
    exit 2
  fi
done

rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"

FAILURES=()

# —— 静态检查 ——

# 示例工程与生成器逐字节一致（手改工程、改了生成器没重跑都会在这里红）。
if ! ruby examples/generate.rb --check; then
  FAILURES+=("示例工程与 examples/generate.rb 的产出不一致")
fi

# 示例工程只许有相对路径、只许经 SwiftPM 引用库：任何人 clone 之后要能直接打开。
# 正则与 examples/generate.rb 的 FORBIDDEN_IN_PROJECT 是同一个，改一处要同步另一处。
FORBIDDEN_IN_PROJECT='/Users/|/Volumes/|path = "?/|build-ffmpeg|swift/SYPlayerKit|\.xcframework'
while IFS= read -r f; do
  hits="$(grep -cE "$FORBIDDEN_IN_PROJECT" "$f" || true)"
  echo "工程路径检查 ${f}：命中 $hits 行"
  if [[ "$hits" != "0" ]]; then
    grep -nE "$FORBIDDEN_IN_PROJECT" "$f" >&2 || true
    FAILURES+=("$f 含绝对路径或绕过 SwiftPM 的库引用")
  fi
done < <(find examples \( -path '*.xcodeproj/project.pbxproj' \
                        -o -path '*.xcodeproj/xcshareddata/xcschemes/*.xcscheme' \) | sort)

# 示例源码只能用公开模块。
private_hits="$(grep -rln 'import SYPlayerKit_Private' examples --include='*.swift' || true)"
if [[ -n "$private_hits" ]]; then
  echo "示例源码引用了私有模块：" >&2
  echo "$private_hits" >&2
  FAILURES+=("示例源码 import SYPlayerKit_Private")
fi

# examples/ 下的每个 .swift 都要出现在某个示例工程里：生成器的清单漏了一个
# 文件时，工程照样能编过（那个文件只是没参与编译），这里兜住。
while IFS= read -r f; do
  base="$(basename "$f")"
  if ! grep -q "path = $base;" examples/*/*.xcodeproj/project.pbxproj; then
    FAILURES+=("$f 不在任何示例工程里（改 examples/generate.rb 并重跑）")
  fi
done < <(find examples -name '*.swift' -not -path '*/.build/*')

# 分发的 SYFFmpeg 与 SYPlayerKit 的符号隔离：SYFFmpeg 导出表全带 _syp_ 前缀、
# 不带头文件、删头后 framework 结构经得起 codesign；SYPlayerKit 四个 slice 对
# FFmpeg 的未定义引用全部带前缀（没有哪个编译单元绕过映射头）。映射头取
# build-ffmpeg 构建产物里那份——分发件里已经删掉了。
SYM_LOG="$LOG_DIR/check-ffmpeg-symbols.log"
sym_rc=0
tools/check-ffmpeg-symbols.sh --dist --expect-slices 4 \
  --prefix-header "$ROOT/build-ffmpeg/out/SYFFmpeg.xcframework/macos-arm64/SYFFmpeg.framework/Headers/syp_ffmpeg_prefix.h" \
  "$OUT/SYFFmpeg.xcframework" \
  "$OUT/SYPlayerKit.xcframework/ios-arm64/SYPlayerKit.framework/SYPlayerKit" \
  "$OUT/SYPlayerKit.xcframework/ios-arm64_x86_64-simulator/SYPlayerKit.framework/SYPlayerKit" \
  "$OUT/SYPlayerKit.xcframework/ios-arm64-maccatalyst/SYPlayerKit.framework/Versions/A/SYPlayerKit" \
  "$OUT/SYPlayerKit.xcframework/macos-arm64/SYPlayerKit.framework/Versions/A/SYPlayerKit" \
  > "$SYM_LOG" 2>&1 || sym_rc=$?
echo "符号隔离检查：EXIT=${sym_rc}，日志：$SYM_LOG"
if [[ "$sym_rc" != "0" ]]; then
  grep '✗' "$SYM_LOG" >&2 || true
  FAILURES+=("分发件符号隔离检查失败（tools/check-ffmpeg-symbols.sh，EXIT=${sym_rc}）")
fi

# —— 三路 xcodebuild test ——

export SYPLAYER_LOCAL_BINARIES=1

SIGN_ARGS=(CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="")

pick_iphone_simulator() {
  xcrun simctl list devices available -j | python3 -c '
import json, sys
devices = json.load(sys.stdin)["devices"]
for runtime in sorted(devices, reverse=True):
    if ".iOS-" not in runtime:
        continue
    for d in devices[runtime]:
        if d.get("isAvailable", True) and d["name"].startswith("iPhone"):
            print(d["udid"])
            sys.exit(0)
sys.exit(1)
'
}

# 核对这一路解析出来的两个 binaryTarget 都来自本地 build-spm/out。
check_local_artifacts() {
  local name="$1" dd="$2"
  local state="$dd/SourcePackages/workspace-state.json"
  if [[ ! -f "$state" ]]; then
    echo "[$name] 找不到 ${state}，无法确认包产物来源" >&2
    return 1
  fi
  python3 - "$state" "$OUT" <<'PY'
import json, sys
state, out = sys.argv[1], sys.argv[2]
arts = json.load(open(state))["object"]["artifacts"]
want = {"SYPlayerKit": f"{out}/SYPlayerKit.xcframework", "SYFFmpeg": f"{out}/SYFFmpeg.xcframework"}
got = {a["targetName"]: (a["source"]["type"], a["path"]) for a in arts}
ok = True
for t, p in want.items():
    src = got.get(t)
    print(f"  {t}: {src}")
    if src is None or src[0] != "local" or src[1] != p:
        ok = False
sys.exit(0 if ok else 1)
PY
}

run_one() {
  local name="$1" project="$2" scheme="$3" destination="$4" expected="$5"
  local dd="$LOG_DIR/dd-$name"
  local log="$LOG_DIR/$name.log"
  rm -rf "$dd"
  echo "== ${name}：xcodebuild test（${destination}）"
  local rc=0
  xcodebuild test -project "$project" -scheme "$scheme" \
    -destination "$destination" -derivedDataPath "$dd" \
    "${SIGN_ARGS[@]}" > "$log" 2>&1 || rc=$?
  local executed
  executed="$(grep -E 'Executed [0-9]+ tests?, with' "$log" | tail -n 1 || true)"
  echo "[$name] EXIT=$rc"
  echo "[$name] ${executed:-（没有 Executed 行）}"
  echo "[$name] 日志：$log"
  if [[ "$rc" != "0" ]]; then
    FAILURES+=("${name}：xcodebuild EXIT=$rc")
    grep -n 'error:\|failed' "$log" | head -n 20 >&2 || true
  fi
  if [[ -z "$executed" ]]; then
    FAILURES+=("${name}：没有跑出任何测试")
  else
    if ! grep -q 'with 0 failures' <<<"$executed"; then
      FAILURES+=("${name}：有用例失败")
    fi
    local count
    count="$(sed -E 's/.*Executed ([0-9]+) tests?,.*/\1/' <<<"$executed")"
    echo "[$name] 用例数 ${count}，期望 $expected"
    if [[ "$count" != "$expected" ]]; then
      FAILURES+=("${name}：执行了 $count 条用例，期望 $expected 条")
    fi
  fi
  echo "[$name] 包产物来源："
  if ! check_local_artifacts "$name" "$dd"; then
    FAILURES+=("${name}：包产物不是本地 build-spm/out（SYPLAYER_LOCAL_BINARIES 未生效？）")
  fi
}

SIM_ID=""
if SIM_ID="$(pick_iphone_simulator)"; then
  run_one ios examples/iOSExample/iOSExample.xcodeproj iOSExample "platform=iOS Simulator,id=$SIM_ID" 2
else
  FAILURES+=("ios：本机没有可用的 iPhone 模拟器（xcrun simctl list devices available 里找不到 iPhone）")
fi

run_one catalyst examples/iOSExample/iOSExample.xcodeproj iOSExample \
  "platform=macOS,variant=Mac Catalyst,arch=arm64" 2

run_one macos examples/macOSExample/macOSExample.xcodeproj macOSExample "platform=macOS,arch=arm64" 3

# —— zip 形态：发版要上传的那两个 zip 本身能不能被 SwiftPM 解出来、编过、跑起来 ——

ZIPCHECK="$ROOT/build-spm/zipcheck"
run_zipcheck() {
  rm -rf "$ZIPCHECK"
  mkdir -p "$ZIPCHECK/Sources/ZipConsumer" "$ZIPCHECK/Sources/zipcheck"
  local fw sum want
  for fw in SYPlayerKit SYFFmpeg; do
    cp "$OUT/$fw.xcframework.zip" "$ZIPCHECK/$fw.xcframework.zip"
    sum="$(swift package compute-checksum "$ZIPCHECK/$fw.xcframework.zip")"
    want="$(awk -v f="$fw.xcframework.zip" '$1 == f { print $2 }' "$OUT/checksums.txt" 2>/dev/null || true)"
    echo "[zip] $fw.xcframework.zip checksum ${sum}（checksums.txt：${want:-<无>}）"
    if [[ -z "$want" || "$sum" != "$want" ]]; then
      FAILURES+=("zip：$fw.xcframework.zip 的 checksum 与 build-spm/out/checksums.txt 不一致")
    fi
  done
  cat > "$ZIPCHECK/Package.swift" <<'SWIFT'
// swift-tools-version:5.9
// check-spm.sh 生成的一次性包：两个 binaryTarget 直接吃发版用的 zip。
import PackageDescription

let package = Package(
    name: "ZipCheck",
    platforms: [.iOS(.v13), .macCatalyst(.v14), .macOS(.v11)],
    products: [
        .library(name: "ZipConsumer", targets: ["ZipConsumer"]),
        .executable(name: "zipcheck", targets: ["zipcheck"]),
    ],
    targets: [
        .binaryTarget(name: "SYPlayerKit", path: "SYPlayerKit.xcframework.zip"),
        .binaryTarget(name: "SYFFmpeg", path: "SYFFmpeg.xcframework.zip"),
        .target(name: "ZipConsumer", dependencies: ["SYPlayerKit", "SYFFmpeg"]),
        .executableTarget(name: "zipcheck", dependencies: ["ZipConsumer"]),
    ]
)
SWIFT
  cat > "$ZIPCHECK/Sources/ZipConsumer/ZipConsumer.swift" <<'SWIFT'
import SYPlayerKit

// 走一遍公开 API 的往返：构造错误码再读回来，确保真的链接到 SYPlayerKit。
public func zipCheckRoundTrip() -> Bool {
    let code = SYPlayerError.noSpace.statusCode
    return SYPlayerError(statusCode: code) == .noSpace
}
SWIFT
  cat > "$ZIPCHECK/Sources/zipcheck/main.swift" <<'SWIFT'
import Foundation
import ZipConsumer

if zipCheckRoundTrip() {
    print("zipcheck: SYPlayerKit loaded from zip, round trip ok")
} else {
    print("zipcheck: round trip mismatch")
    exit(1)
}
SWIFT

  # 原生 macOS：swift build 后直接运行，动态库加载失败会在这里崩。
  local log="$LOG_DIR/zip-macos.log" rc=0
  echo "== zip-macos：swift build + run"
  ( cd "$ZIPCHECK" && swift build -c release --product zipcheck && "$(swift build -c release --show-bin-path)/zipcheck" ) \
    > "$log" 2>&1 || rc=$?
  echo "[zip-macos] EXIT=${rc}，日志：$log"
  if [[ "$rc" != "0" ]] || ! grep -q 'round trip ok' "$log"; then
    FAILURES+=("zip-macos：从 zip 接入后 swift build / 运行失败（EXIT=${rc}）")
    grep -n 'error:' "$log" | head -n 20 >&2 || true
  fi
  # 解出来的 SYFFmpeg 里要有随包附带的许可证与 NOTICE：证明用的是这次打出的 zip。
  local extracted
  extracted="$(find "$ZIPCHECK/.build/artifacts" -type d -name SYFFmpeg.xcframework -print -quit 2>/dev/null || true)"
  if [[ -z "$extracted" || ! -f "$extracted/NOTICE" || ! -f "$extracted/COPYING.LGPLv2.1" ]]; then
    FAILURES+=("zip-macos：解出的 SYFFmpeg.xcframework 里没有 NOTICE / COPYING.LGPLv2.1（${extracted:-未解出}）")
  else
    echo "[zip-macos] 解出的 SYFFmpeg.xcframework 带 NOTICE 与 COPYING.LGPLv2.1"
  fi
  # 从 zip 解出来的那份同样不许带头文件（zip 是发版真正上传的东西）。
  if [[ -n "$extracted" ]]; then
    local zip_headers
    zip_headers="$(find "$extracted" \( -name Headers -o -name '*.h' \) 2>/dev/null || true)"
    if [[ -n "$zip_headers" ]]; then
      FAILURES+=("zip-macos：解出的 SYFFmpeg.xcframework 带头文件")
      printf '%s\n' "$zip_headers" | sed -n '1,5p' >&2
    else
      echo "[zip-macos] 解出的 SYFFmpeg.xcframework 不带头文件"
    fi
  fi

  # iOS 模拟器与 Mac Catalyst：xcodebuild 直接构建包里的库目标。
  local name destination extra
  for entry in "ios-simulator|generic/platform=iOS Simulator|" \
               "catalyst|generic/platform=macOS,variant=Mac Catalyst|ARCHS=arm64"; do
    IFS='|' read -r name destination extra <<<"$entry"
    log="$LOG_DIR/zip-$name.log"; rc=0
    echo "== zip-${name}：xcodebuild build（${destination}）"
    local extra_args=()
    [[ -n "$extra" ]] && extra_args+=("$extra")
    ( cd "$ZIPCHECK" && xcodebuild build -scheme ZipConsumer -destination "$destination" \
        -derivedDataPath "$LOG_DIR/dd-zip-$name" "${SIGN_ARGS[@]}" \
        ${extra_args[@]+"${extra_args[@]}"} ) > "$log" 2>&1 || rc=$?
    echo "[zip-$name] EXIT=${rc}，日志：$log"
    if [[ "$rc" != "0" ]] || ! grep -q '\*\* BUILD SUCCEEDED \*\*' "$log"; then
      FAILURES+=("zip-${name}：从 zip 接入后 xcodebuild build 失败（EXIT=${rc}）")
      grep -n 'error:' "$log" | head -n 20 >&2 || true
    fi
  done
}
# 一次性包的清单不读 SYPLAYER_LOCAL_BINARIES，两个 binaryTarget 写死指向 zip。
run_zipcheck

if (( ${#FAILURES[@]} > 0 )); then
  echo "check-spm：失败" >&2
  printf '  - %s\n' "${FAILURES[@]}" >&2
  exit 1
fi
echo "check-spm：通过"
