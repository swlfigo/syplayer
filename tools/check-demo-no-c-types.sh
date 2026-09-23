#!/bin/bash
# 验证核心验收判据：**业务侧源码里不出现任何 C / ObjC 桥类型**。
#
# 这条判据原先只以一行 grep 的形式存在于计划文档的验证步骤里，没有人会
# 重跑——往 demo/shared/ 里加一个 `import SYPlayerKit_Private`
# 不会让任何构建变红，判据就这么静默失效了。所以把它落成脚本，与
# tools/check-abi.sh / tools/check-deploy-target.sh 同一档。
#
# 扫描口径（把范围从 demo/shared/*.swift 扩到
# 整个 demo/ 下的 Swift 源）：
#   · \bSyp[A-Z]           —— 桥的 ObjC 类型（SypPlayerBridge/SypPlayerSnapshot/
#                             SypStatusCode…）。刻意不写成 \bSYP：公开的 Swift
#                             类型全都叫 SYPlayer*，两者只差大小写。
#   · int32_t              —— C 的定宽整数（桥的 errorOut 出参）
#   · UnsafeMutablePointer —— 指针出参
#   · SYPlayerKit_Private  —— 私有模块本身（过渡期 import）
#
# 反向自检：脚本同时确认业务代码里**确实**大量出现 SYPlayer——否则"零命中"
# 可能只是因为路径写错、扫了个空目录（这正是那条反向 grep 的
# 用意，一并固化进来）。
set -u
P="$(cd "$(dirname "$0")/.." && pwd)"

BAD='\bSyp[A-Z]|int32_t|UnsafeMutablePointer|SYPlayerKit_Private'
fail=0

echo "== 业务侧（demo/）Swift 源码禁止出现 C / 桥类型 =="

files=$(find "$P/demo" -type f -name '*.swift' | LC_ALL=C sort)
if [ -z "$files" ]; then
  echo "  ❌ demo/ 下没有找到任何 .swift 文件（扫描范围出错，不是“通过”）"
  exit 1
fi

count=0
while IFS= read -r f; do
  [ -z "$f" ] && continue
  count=$((count + 1))
  rel="${f#"$P"/}"
  hits=$(grep -nE "$BAD" "$f" || true)
  if [ -z "$hits" ]; then
    printf "  ✅ %s\n" "$rel"
  else
    printf "  ❌ %s\n" "$rel"
    printf '%s\n' "$hits" | sed 's/^/       /'
    fail=1
  fi
done <<EOF
$files
EOF

echo "== 反向自检：业务代码确实在用 SYPlayer 公开 API =="
# 随便一条命中都不够——要的是"这个文件真的是在用这套 API"，所以设了下限。
probe="$P/demo/shared/PlayerViewController.swift"
if [ ! -f "$probe" ]; then
  echo "  ❌ 找不到 $probe（目录结构变了，扫描口径需要跟着更新）"
  fail=1
else
  n=$(grep -cE 'SYPlayer' "$probe")
  if [ "$n" -ge 10 ]; then
    printf "  ✅ %s 命中 SYPlayer %s 处（≥ 10）\n" "${probe#"$P"/}" "$n"
  else
    printf "  ❌ %s 只命中 SYPlayer %s 处——上面的“零命中”可能是扫了个空\n" \
           "${probe#"$P"/}" "$n"
    fail=1
  fi
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "全部通过（$count 个 Swift 源文件）。"
else
  echo "有失败项。"
fi
exit "$fail"
