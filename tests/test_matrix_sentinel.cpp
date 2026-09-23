// tests/test_matrix_sentinel.cpp — 哨兵测试：验证矩阵子集缺失时必须变红。
//
// syp_media / syp_probe / probe_e2e（连同 packet_digest、avio_bridge）是
// 条件注册的——CMake 探测不到 FFmpeg xcframework（SYP_HAVE_FFMPEG）或
// 探测不到 ffmpeg CLI（SYP_HAVE_FIXTURES）时整批不注册。这是刻意设计
// （没装 FFmpeg 的机器 clone 下来构建照常绿），但代价是裸的 `ctest`
// 在那种机器上退出码仍是 0，整套验收证据可能无声消失。
// `--no-tests=error` 挡不住：dl 层自己的 5 个测试仍会正常注册跑绿，
// 这是"矩阵子集缺失"，不是"一个测试都没有"。
//
// 本文件必须永远注册进 tests/CMakeLists.txt，不放在任何 SYP_HAVE_* /
// SYP_REQUIRE_FULL_MATRIX 的 if() 条件块里——否则它自己也会跟着"无声
// 消失"，哨兵就白设了。
//
// 判据（五档）：
//   1. 完全没有 FFmpeg xcframework（SYP_HAVE_FFMPEG=0）
//        → 通过，但打印醒目的能力报告，让"矩阵未启用"在日志里可见。
//   2. 有 xcframework 但缺 ffmpeg CLI（SYP_HAVE_FIXTURES=0）
//        → 失败：packet_digest / probe_e2e 本该注册却没有注册，
//          正是"无声消失"的现实形态。
//   3. 有 macos-arm64 slice 但缺 ios-arm64 slice
//        → 失败：check-deploy-target.sh 对 src/media/ 的 iOS 13
//          availability 检查正在静默跳过。
//   4. 有 macos-arm64 slice 但缺 ios-arm64-maccatalyst slice
//        → 失败：macOS demo 壳要用 UIKit，只能走 Mac Catalyst，
//          缺这个 slice 就链不上。
//   5. -DSYP_REQUIRE_FULL_MATRIX=ON（CI 用）
//        → 判据 1 也变成失败：CI 机器必须把完整依赖装齐，不许"没装也算过"。
//
// 这五个宏由 tests/CMakeLists.txt 通过 target_compile_definitions 传入，
// 直接取自顶层 CMakeLists.txt 的探测结果，不在本文件里重新探测——
// 探测逻辑只应该有一份，重复一份迟早会漂移。

#include "tiny_test.h"

#include <cstdio>

namespace {

const char* bool_str(int b) { return b ? "有" : "无"; }

void print_capability_report() {
    std::printf("  ---- 验证矩阵能力报告 ----\n");
    std::printf("  SYP_HAVE_FFMPEG        (== SYP_HAVE_MACOS_SLICE，macos-arm64 slice) : %s\n",
                 bool_str(SYP_HAVE_FFMPEG));
    std::printf("  SYP_HAVE_FIXTURES      (ffmpeg CLI 探测，决定端到端场景是否注册)      : %s\n",
                 bool_str(SYP_HAVE_FIXTURES));
    std::printf("  SYP_HAVE_IOS_SLICE     (xcframework 里的 ios-arm64 slice)            : %s\n",
                 bool_str(SYP_HAVE_IOS_SLICE));
    std::printf("  SYP_HAVE_CATALYST_SLICE(xcframework 里的 ios-arm64-maccatalyst slice): %s\n",
                 bool_str(SYP_HAVE_CATALYST_SLICE));
    std::printf("  SYP_REQUIRE_FULL_MATRIX (CI 强制矩阵完整开关)                        : %s\n",
                 bool_str(SYP_REQUIRE_FULL_MATRIX));
    std::printf("  xcframework 路径: %s\n", SYP_FFMPEG_XCFRAMEWORK_PATH);
    std::printf("  --------------------------------------------------------\n");
}

}  // namespace

TEST_CASE(matrix_capability_report_and_gate) {
    print_capability_report();

    if (!SYP_HAVE_FFMPEG) {
        std::printf(
            "  [判据 1] 未探测到 FFmpeg xcframework —— 本轮验证矩阵"
            "（syp_media / syp_probe / probe_e2e / packet_digest / avio_bridge）"
            "整批不会注册。这是刻意设计（没装 FFmpeg 的机器 clone 下来构建"
            "照常绿），本机默认视为通过，但要求这份能力报告必须出现在日志里。\n"
            "  要启用完整矩阵：bash tools/build-ffmpeg.sh\n");
        if (SYP_REQUIRE_FULL_MATRIX) {
            std::printf(
                "  但本次带了 -DSYP_REQUIRE_FULL_MATRIX=ON（CI 专用开关）——"
                "CI 机器必须把依赖装齐，不允许缺 FFmpeg 也算过。判哨兵失败。\n"
                "  修复：在 CI 镜像里运行 bash tools/build-ffmpeg.sh 生成"
                "xcframework，再重新 cmake configure。\n");
            CHECK(SYP_HAVE_FFMPEG);
        }
        return;  // 判据 1 通过时，判据 2/3 没有意义（xcframework 都不存在）。
    }

    // 走到这里说明至少探测到 macos-arm64 slice，矩阵理论上应该已经就绪。
    // 下面逐项核对；任何一项缺失都是"这台机器本来能跑却没跑"，
    // 不区分 SYP_REQUIRE_FULL_MATRIX——判据 2/3 无条件失败。
    bool matrix_complete = true;

    if (!SYP_HAVE_FIXTURES) {
        std::printf(
            "  [判据 2] 探测到 FFmpeg xcframework，但没找到 ffmpeg CLI —— "
            "packet_digest / probe_e2e 这两个端到端场景本该注册却没有注册。\n"
            "  修复：brew install ffmpeg ，然后重新 cmake configure"
            "（重新生成 build 目录或重新 configure 现有 build 目录均可）。\n");
        matrix_complete = false;
    }

    if (!SYP_HAVE_IOS_SLICE) {
        std::printf(
            "  [判据 3] xcframework 里有 macos-arm64 slice，但缺 ios-arm64 slice —— "
            "tools/check-deploy-target.sh 对 src/media/ 的 iOS 13 严格 "
            "availability 检查（纪律 ③）会静默跳过（打 ⏭️ 但 exit 0）。\n"
            "  修复：重新运行 bash tools/build-ffmpeg.sh —— 它应当同时产出 "
            "macos-arm64 与 ios-arm64 两个 slice；只手动拷贝过 macos-arm64 的 "
            "话，重新跑一遍完整脚本。\n");
        matrix_complete = false;
    }

    if (!SYP_HAVE_CATALYST_SLICE) {
        std::printf(
            "  [判据 4] xcframework 里有 macos-arm64 slice，但缺 "
            "ios-arm64-maccatalyst slice —— macOS demo 壳要用 UIKit，"
            "在 macOS 上只能走 Mac Catalyst，缺这个 slice 就链不上。\n"
            "  修复：重新运行 bash tools/build-ffmpeg.sh —— 它应当同时产出 "
            "macos-arm64 与 ios-arm64-maccatalyst 两个 slice；只手动拷贝过 "
            "macos-arm64 的话，重新跑一遍完整脚本。\n");
        matrix_complete = false;
    }

    if (!matrix_complete) {
        std::printf(
            "  验证矩阵子集缺失（详见上方逐项报告）——这正是本哨兵存在的意义："
            "让这种情况在 CI 日志上一眼可见，而不是让 ctest 悄悄退出 0。\n");
    }
    CHECK(matrix_complete);
}

int main() { return tiny_test_main(); }
