# 语言标准与工具链

**结论：C++23，最低部署目标 iOS 13.0。** 全新项目，没有历史包袱，C++17（2017 年）没必要背。

## 本机实测环境

| | |
|---|---|
| 编译器 | Apple clang 21.0.0 (clang-2100.1.1.101) |
| iOS SDK | iPhoneOS26.5.sdk |
| 可用标准 | c++17 / c++20 / c++23 / c++26 全部可解析 |

⚠️ 注意 `xcode-select -p` 当前指向 `/Library/Developer/CommandLineTools`，
里面没有 iOS SDK。做 iOS 编译测试要显式指定 Xcode 的工具链和 sysroot，
或者 `sudo xcode-select -s /Applications/Xcode.app`。

## 实测：部署目标 iOS 15.0，开启严格 availability 检查

命令加了 `-Werror=unguarded-availability -Werror=unguarded-availability-new`
（**不加这两个 flag 会漏检 availability 错误，测试结果是假的**）。

| 特性 | 标准 | iOS 15.0 |
|---|---|---|
| `std::span` | C++20 | ✅ |
| concepts / `<bit>` | C++20 | ✅ |
| ranges | C++20 | ✅ |
| `std::atomic` wait/notify | C++20 | ✅ |
| coroutines | C++20 | ✅ |
| `std::filesystem` | C++17 | ✅ |
| **`std::expected`** | **C++23** | ✅ |
| `std::format`（单独用） | C++20 | ✅ |

### 踩到的坑：`<format>` + `<filesystem>` 同时 include

| 组合 | iOS 15.0 | iOS 16.3 |
|---|---|---|
| 只 `<filesystem>` | ✅ | ✅ |
| 只 `<format>` | ✅ | ✅ |
| **两个一起** | ❌ `'to_chars' is unavailable: introduced in iOS 16.3` | ✅ |

原因：`std::formatter<std::filesystem::path>` 特化会拉到 dylib 里的
`to_chars`，那个符号 iOS 16.3 才有。

**操作规则**：
1. 定死 deployment target，CI 里**始终**开 `-Werror=unguarded-availability-new`，
   让编译器替你兜底，不要靠记忆
2. 想用 `std::format` → deployment target 定 **iOS 16.3+ / macOS 13.3+**
3. 想支持更低版本 → 用 fmt 库或自己写格式化，别用 `std::format`

## C++23 对 dl 层的实际收益

| 特性 | 用在哪 |
|---|---|
| **`std::expected<T,E>`** | **最大收益。**这层全是可失败操作（read / seek / 网络 / 落盘 / 索引解析），替掉 errno 风格的 int 返回码 |
| `std::span<uint8_t>` | 替掉所有 `ptr + len` 参数对，buffer 传递直接受益 |
| ranges | `HoleSet` 的区间集合运算（查洞 / 合并 / 求交） |
| `<bit>` | 位图与对齐计算 |
| designated initializers | 配置结构体，可读性大幅提升 |
| `std::atomic` wait/notify | 替掉手写 condvar |
| concepts | 模板错误信息可读 |

## 明确不用的

**C++26** —— 不是包袱问题，是标准还在定稿、libc++ 实现不完整。
会踩到"编译器支持但标准库没有"的坑。C++23 是当前的甜点。

**协程（暂缓）** —— C++20 协程是低层设施，没有内置执行器。
用来写异步下载任务听着美好，但要引入 asio / cppcoro 之类，
而且移动端调试时协程栈非常难看。M1 先用传统线程 + 回调跑通，
架构稳定后再评估是否值得换。

## 两条与标准版本无关的纪律

1. **C ABI 边界不受影响。** 内部用 C++23 还是 C++26，公开头文件永远只能是
   C 基本类型 + 不透明指针 + POD + 函数指针。见 `architecture.md`。

2. **Android NDK 的 libc++ 滞后于 Xcode（待验证）。**
   dl 层未来要接 Android，而 NDK 的 libc++ 版本通常落后。
   本机没装 NDK，**无法验证当前状态**。
   处理方式：现在按 C++23 写（不为未承诺的平台提前妥协），
   真要接 Android 时再实测一次；那时 NDK 也更新了。
   如果届时发现不兼容，受影响的只是 dl 层，改动面可控。


---

## 部署目标 iOS 13.0：实测矩阵与绕法

项目最低支持 iOS 13。实测下来 C++23 的主力特性全部可用，只有两处要绕：

| | iOS 13 | 绕法 |
|---|---|---|
| `std::expected` / `span` / ranges / concepts / `<bit>` | ✅ | — |
| `filesystem` / `variant` / `optional` / `string_view` / `shared_mutex` | ✅ | — |
| `to_chars(int)`、`atomic` 的 load/store/CAS | ✅ | — |
| `std::atomic::wait/notify`（需 iOS 14） | ❌ | 用 `std::condition_variable` |
| `std::format`、`to_chars(double)`（需 iOS 16.3） | ❌ | 用 **fmt 库**（header-only）或 `snprintf` |

**编码规则（写进 CI）：**

1. **dl 层禁止使用 `std::format` 和 `std::atomic::wait/notify`。**
2. CI 始终带 `-target arm64-apple-ios13.0 -Werror=unguarded-availability-new`，
   让编译器强制这条规则，不靠人记。
3. `tools/check-abi.sh` 的编译矩阵已包含 iOS 13 目标。

Apple 侧 API 在 iOS 13 均可用，渲染/解码/网络方案不受影响：
`CVMetalTextureCache`、`VideoToolbox`、`CAMetalLayer`、`NSURLSession`。

---

## headless 验证工具的依赖

- **FFmpeg xcframework**：`tools/build-ffmpeg.sh` 产出，落 `build-ffmpeg/out/`，不进仓库。
  CMake 探测不到就跳过 `syp_media` / `syp_probe` / `probe_e2e`，构建照常绿。
- **系统 ffmpeg CLI**：`brew install ffmpeg`。只用于 `tools/gen-fixtures.sh` 生成验证素材。
  缺它则跳过端到端测试（`packet_digest` 与 `probe_e2e`），其余测试不受影响。
- 素材落 `build/fixtures/<seed>/`，seed 在 CMake 配置阶段随机抽取并打印。
  同一个 build 目录复现同一份素材；失败要复现别的种子，用
  `bash tools/gen-fixtures.sh --seed <N> --out <DIR>` 手工生成。

### ⚠️ CI 门禁提醒：缺依赖时验证矩阵会无声消失（已用哨兵测试补上）

`SYP_HAVE_FFMPEG` 或 `SYP_HAVE_FIXTURES` 探测不到时，`syp_media` /
`syp_probe` / `probe_e2e`（连同 `packet_digest`）整批不会被注册进
`add_subdirectory(tools)`。这本身是刻意设计——保证没装 FFmpeg
的机器上 clone 下来构建照常绿——**但代价是裸的 `ctest` 命令在这种
机器上退出码仍然是 0**，不会提示矩阵少跑了。M1 的验收矩阵一旦这样
被无声摘掉，整套「逐 packet 差分比对 8 条场景」的验收证据就悄悄消失，
而 CI 日志上完全看不出区别。

**处理方式**：CI 里跑 `ctest` 时一律加 `--no-tests=error`
（一个测试都没找到就非零退出），**外加一个始终注册的哨兵测试**
`tests/test_matrix_sentinel.cpp`——不放在任何 `SYP_HAVE_*` 的 `if()`
块里，所以它自己不会跟着矩阵一起消失。`README.md` 的构建说明已经带了
`--no-tests=error`。

⚠️ **`--no-tests=error` 单独用时的局限**：它只在 ctest **一个
测试都没找到**时才生效。本仓库缺 FFmpeg 时，`syp_dl` 层的 5 个测试
（`hole_set`/`cache_index`/`dl_task`/`scheduler`/`source_bridge`）仍会
正常注册并跑绿，所以裸的 `--no-tests=error` **抓不住"矩阵少了 4 个"
这种子集缺失**，只能防住"CMake 配置彻底坏掉、一个测试都没注册"这种
更极端的情况。

✅ **哨兵测试补上了这一块**（`tests/test_matrix_sentinel.cpp`，
`docs/tech-debt.md` #1/#2 已解决）：`matrix_sentinel` 这一个 ctest
目标始终存在，读取 CMake 探测到的 `SYP_HAVE_FFMPEG`/`SYP_HAVE_FIXTURES`/
`SYP_HAVE_IOS_SLICE`/`SYP_REQUIRE_FULL_MATRIX` 四个编译期定义，按四档
判据判定：

| 状态 | 哨兵行为 |
|---|---|
| 完全没装 FFmpeg xcframework | 通过，但打印醒目的能力报告（矩阵未启用，日志里可见） |
| 有 xcframework 但缺 ffmpeg CLI（`SYP_HAVE_FIXTURES` 假） | 失败——`packet_digest`/`probe_e2e` 本该注册却没注册 |
| 有 `macos-arm64` slice 但缺 `ios-arm64` slice | 失败——`check-deploy-target.sh` 对 `src/media/` 的 iOS 13 availability 检查正在静默跳过 |
| `-DSYP_REQUIRE_FULL_MATRIX=ON`（CI 用） | 缺任何一项都失败，包括"完全没装 FFmpeg" |

失败时的输出直接给出修复命令（`brew install ffmpeg` /
`bash tools/build-ffmpeg.sh` 等），不用去翻源码猜。**CI 应当带
`-DSYP_REQUIRE_FULL_MATRIX=ON` configure**，这样"CI 机器缺依赖"这种
本不该发生的情况也会被挡住，而不是降级成"矩阵悄悄没跑但通过"。

本地开发机不需要这个开关——`SYP_REQUIRE_FULL_MATRIX` 默认 `OFF`，
"没装 FFmpeg 时构建照常绿"这条既有取舍没有被打破。
