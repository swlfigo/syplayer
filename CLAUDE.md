# syplayer —— 给在这个仓库里干活的人（和 agent）的须知

> 这份文件很短，是**刻意的**：它每次会话都会被读一遍。
> 详细版在 `README.md`（构建/测试）、`docs/tech-debt.md`（已登记的债）、
> `docs/known-gaps.md`（已知缺口，带编号）。

## 约定

- 注释与文档用**中文**。提交信息用中文，格式 `模块：内容`，**不加任何署名行**。
- 测试框架是 `tests/tiny_test.h`：`TEST_CASE` / `CHECK` / `CHECK_EQ` / `REQUIRE`。
  **没有 `REQUIRE_EQ`**。
- 编译带 `-Werror -Wconversion -Wsign-conversion -Wshadow`，零警告是硬要求。
- `.claude/` 是并行 agent 的一次性工作树，已 gitignore。**提交前先看
  `git status`，不要 `git add -A`**（2026-09-20 真的把另一个 agent 的整个
  工作树扫进过索引）。
- **新增用例没做变异验证 = 没写。** 本仓的纪律，不是建议。

## ⚠️ 六种让测试给出**假结果**的机制

下面每一条在 M5 这一个里程碑里**都真实发生过**，其中三条当场制造了错误结论。
它们的共同点是：你看到的那个"绿"或"红"**不是你以为的那次运行的结果**。

| # | 机制 | 防法 |
|---|---|---|
| 1 | `make` 的 mtime 粒度是 **1 秒**，改完立刻重编会跑**没重编的二进制** | 改完 `rm` 掉对应的 `.o`，别只靠 `make` 判新旧 |
| 2 | **构建失败但测试照跑上一个二进制并报它的结果** | 显式检查构建退出码，打一个 `BUILD_OK` 闸再跑 ctest |
| 3 | **变异根本没落进文件**（`perl -0pi` 里 `$0` 在 `\Q…\E` 内被当变量插值，替换静默不发生） | 改完 **`grep` 一遍确认改动真的在文件里** |
| 4 | **批量跑变异会假绿**（5 个放一个 shell 循环 ⇒ 3 个报绿；单独重跑全红） | **一个变异一次工具调用** |
| 5 | 用 `cp` / `git checkout` **还原头文件**时 mtime 被盖成当前秒，可能与已编好的 `.o` 同秒 | 删**所有 include 了它的 TU** 的 `.o`，不只是被变异的那个 |
| 6 | **负载假红把存活变异伪装成"被杀掉"** —— 最阴的一条，因为"红了"看起来正是你想要的结果 | 看到红必须确认**红的是你预期的那一条用例**，不是"有红就算数" |

补两条同源的：
- `git checkout` 还原变异**不会触发重建**（复审被咬过），还原之后照样要删 `.o`。
- 直接跑测试二进制时 **cwd 不对**会让读 fixture 的用例集体 FAIL，与变异毫无因果。

长版（每一条的现场、测到的数字、当时得出的错误结论）在
`docs/tech-debt.md` 的「M5 收尾登记」一节。

## 门禁，改完都要跑

```bash
tools/check-abi.sh            # 公开头的 ABI 约定
tools/check-deploy-target.sh  # 部署目标
tools/check-demo-no-c-types.sh# 业务侧 Swift 不出现 C 类型
tools/check-ffmpeg-no-tls.sh  # FFmpeg 不带 TLS
tools/check-ffmpeg-symbols.sh --expect-slices 4 build-ffmpeg/out/SYFFmpeg.xcframework   # SYFFmpeg 只导出 syp_ 前缀符号（CMake 构建与 ctest ffmpeg_symbols 也会自动跑）
cmake --build build --target syp_dl_purity_check   # dl 层零 FFmpeg / 零平台符号
ruby demo/generate_xcodeprojects.rb --check        # demo 两个 pbxproj 与生成器同步
tools/check-spm.sh            # SwiftPM 接入：示例 App 三路测试 + zip 形式验证（先跑 tools/build-xcframework.sh，缺产物 exit 2）
```

**`--check` 那一道为什么存在**：`demo/{ios,mac}` 两个 `project.pbxproj` 是
`demo/generate_xcodeprojects.rb` 生成的，**手动同步**。它们自 `afea771` 起漏了
5 个 C++ 源，`SYPlayerKitTests` 链接失败，于是 `xcodebuild test`
**从 M5 Task 1 一直红到 Task 8**——整整七个 task、七轮"全量验证"，
没有任何人跑过一条 Swift 测试，也没有任何人发现。
**往 CMake 或生成器的源清单里加文件之后，重跑生成器并把两个 pbxproj 一起提交。**
**改了 Swift 层（`swift/SYPlayerKit/`）或它编进去的源之后，分发产物也要重编**
（`tools/build-xcframework.sh`）**并跑 `tools/check-spm.sh`**：demo 是静态源码直编，
分发是另一套动态 framework 配置，demo 全绿不代表 SwiftPM 接入方能用。

## FFmpeg 是隔离的私有副本（SYFFmpeg）

`tools/build-ffmpeg.sh` 产出的 `SYFFmpeg` 只导出 `syp_` 前缀的符号，我们的代码靠强制包含
`syp_ffmpeg_prefix.h` 把 `av_*` 等名字映射过去——**不要绕过映射头直接引用 FFmpeg 原名**，
门禁会红。"接入方自带 FFmpeg"的冲突测试是 ctest `ffmpeg_coexist_*`
（`tests/test_ffmpeg_coexist.cpp`，假库在 `tests/support/fake_ffmpeg/`）；SYFFmpeg 若退化成导出
原名，`static_syffmpeg_first` 与 `dylib_syffmpeg_first` 会红。
**假静态库必须保持一函数一成员、记账单独一个成员**：静态库里的定义只在其成员被加载时才参与，
成员一旦被别的引用（例如记账接口）强制加载，里面的假定义会无视链接顺序胜过动态库，测试会假绿。
改 FFmpeg 源码一律放 `tools/ffmpeg-patches/*.patch`，不直接改 `build-ffmpeg/src`。

## 跑测试时的三个已知坑

- **`xcodebuild` 的 Release 必须显式加 `ARCHS=arm64`**。不加会连 x86_64 一起编，
  而 `SYFFmpeg.xcframework` 没有 x86_64-maccatalyst 切片 ⇒ `ld: symbol(s) not found`、
  exit 65。失败清单点名的是 `Objects-normal/x86_64/`。
- **在 `git worktree` 里必须先跑一遍 `ruby demo/generate_xcodeprojects.rb`**：
  两个 pbxproj 内嵌 `include/`、`src/`、`build-ffmpeg` 的**绝对路径**，不重新生成
  的话 `xcodebuild` 会去编**主工作树的源码**，整轮结果作废。
- **`xcodebuild test` 不带签名参数会 `EXIT=65`，那是签名错误，不是用例失败**，
  极易被当成"红了"（机制 #6 同类）。一律用 known-gaps #68 验证过的写法：
  ```bash
  xcodebuild test -project demo/mac/syplayer-mac.xcodeproj -scheme SYPlayerKitTests \
    -destination 'platform=macOS,variant=Mac Catalyst' -derivedDataPath <全新目录> \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY=""
  ```
  看到 `EXIT=65` 先 `grep -c "error:"` 与 `grep -n "Signing\|signing"`，区分编译
  错误、签名错误、用例失败。
