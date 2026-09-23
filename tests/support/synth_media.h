// synth_media.h —— 现场合成测试素材的公共脚手架：ffmpeg_cli_path() / TempDir /
// synth_audio_with_cover_art()。
//
// 这三样原先是 tests/test_track_player.cpp 匿名 namespace 里的普通函数/类型；
// PipelineConfig 硬解配置的封面图恒软解用例需要在
// tests/test_pipeline.cpp 里复用同一份"音频 + 封面图"素材构造逻辑，遂挪到
// 这里、给多个测试可执行文件共享（而不是各自复刻一遍）。
//
// 使用方必须在自己的 CMakeLists.txt 目标上定义 SYP_FFMPEG_CLI_PATH（编译期
// 烤进 configure 期 find_program(SYP_FFMPEG_CLI ffmpeg) 探测到的绝对路径——
// 不裸调 PATH 里的 "ffmpeg"，configure 期与运行期 PATH 不一定一致，见
// tests/test_decode_e2e.cpp 场景 G 的注释）。
#pragma once

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>

extern "C" {
#include <unistd.h>   // mkdtemp
}

namespace syp::test {

// 编译期烤进来的绝对路径，跟 tests/CMakeLists.txt 里 gen-fixtures.sh 用
// 的是同一个 find_program(SYP_FFMPEG_CLI ffmpeg) 结果。
inline std::string ffmpeg_cli_path() { return SYP_FFMPEG_CLI_PATH; }

// RAII 临时目录：素材现场生成、用完即删，不进仓库、不进 gen-fixtures.sh
// 的验证素材矩阵——只有现场合成素材的用例用得到。
struct TempDir {
    std::string path;
    TempDir() {
        char  tmpl[] = "/tmp/syp_synth_media_XXXXXX";
        char* p      = mkdtemp(tmpl);
        if (p != nullptr) path = p;
    }
    ~TempDir() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// 带封面图（AV_DISPOSITION_ATTACHED_PIC）的素材。
//
// 封面图在 mp4 里是一条 is_video 为真、只有一个采样的轨。历史上
// TrackPlayer::create() 曾经只认"第一条 is_video"，完全没查 attached_pic
// （已修复，见 test_track_player.cpp 对应用例）；
// PipelineConfig 硬解配置同样要求封面图恒软解——这份素材（音频 + 封面图）
// 是唯一的 is_video 轨，能同时钉住两件事。
//
// 关于素材构造的一条实测事实，写在这里免得下一个人再试一遍：**用 ffmpeg
// CLI 造不出"封面图流号更小"的 mp4**。mov 复用器在看到
// -disposition:...:attached_pic 时会把这条轨挪到最后。封面图必须是
// png/mjpeg（mp4 的 attached_pic 只接受这两种 tag）。
inline std::string synth_audio_with_cover_art(const std::string& dir) {
    const std::string path       = dir + "/synth_audio_cover.m4a";
    const std::string cover_path = dir + "/cover.png";
    const std::string audio_path = dir + "/cover_audio.m4a";

    std::ostringstream ccmd;
    ccmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
         << "-f lavfi -i \"color=c=red:size=32x16\" -frames:v 1 "
         << "\"" << cover_path << "\" > /dev/null 2>&1";
    if (std::system(ccmd.str().c_str()) != 0) return std::string();

    std::ostringstream acmd;
    acmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
         << "-f lavfi -i \"sine=frequency=440:sample_rate=44100:duration=1\" "
         << "-c:a aac -ar 44100 -ac 2 "
         << "\"" << audio_path << "\" > /dev/null 2>&1";
    if (std::system(acmd.str().c_str()) != 0) return std::string();

    std::ostringstream mux;
    mux << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
        << "-i \"" << cover_path << "\" -i \"" << audio_path << "\" "
        << "-map 0:v -map 1:a -c copy -disposition:v:0 attached_pic "
        << "\"" << path << "\" > /dev/null 2>&1";
    return (std::system(mux.str().c_str()) == 0) ? path : std::string();
}

// 带 displaymatrix 侧数据的旋转素材。rotation 取
// {0,90,180,270}；0 表示"不加任何旋转选项"，产出一份不带旋转信息的
// 普通对照素材。
//
// 实测踩坑记录：
// - `-display_rotation` 是 ffmpeg CLI 的**输入端**选项
//   （fftools/ffmpeg_opt.c：OPT_INPUT），必须写在 -i 之前；写在输出端
//   （-c:v 之后、输出路径之前）会被拒绝："input option applied to
//   output"。
// - 默认 autorotate 开着的话，这个选项会让 libavfilter 自动插入
//   transpose，把旋转直接烤进像素（宽高互换、640x360 变 360x640），
//   displaymatrix 侧数据反而不会出现在编码后的输出流上——必须搭配
//   `-noautorotate`，侧数据才会原样落进输出流的 coded_side_data，画面
//   本身保持未旋转（640x360）。
// - `-metadata:s:v:0 rotate=90` 这条路径在这份 FFmpeg 上完全不生效：
//   mov 复用器（libavformat/movenc.c）只认 AV_PKT_DATA_DISPLAYMATRIX
//   侧数据来写 tkhd matrix，不再从 "rotate" metadata tag 转换。
// 返回生成的文件路径；失败返回空串。
inline std::string synth_video_with_display_rotation(const TempDir& dir, int rotation) {
    const std::string path =
        dir.path + "/synth_rotate_" + std::to_string(rotation) + ".mp4";

    std::ostringstream cmd;
    cmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y ";
    if (rotation != 0) {
        cmd << "-noautorotate -display_rotation " << rotation << " ";
    }
    cmd << "-f lavfi -i \"testsrc2=size=640x360:rate=25:duration=1\" "
        << "-c:v libx264 -pix_fmt yuv420p "
        << "\"" << path << "\" > /dev/null 2>&1";
    return (std::system(cmd.str().c_str()) == 0) ? path : std::string();
}

// 指定 sample aspect ratio 的素材。用 setsar 滤镜直接写
// 编码流的 sample_aspect_ratio，不受 SAR/DAR 互相推导的干扰。
//
// 编码宽度按 640 * sar_den/sar_num 反推，使 SAR 校正后的显示宽度恒为
// 640（display_size() 的公式是 display_w = coded_w * sar_num/sar_den，
// 见 video_geometry.cpp）——这样调用方（用例）断言
// display_size(...).width == 640 时不用为每个 sar_num/sar_den 组合各算
// 一次期望值。
//
// 【运行期防呆】反推要求 sar_num 整除 640*sar_den（当前唯一调用点 (8,9)
// 满足：640*9/8=720，偶数，testsrc2 接受）；sar_num<=0 或整除不了就直接
// 返回空串，调用方的 REQUIRE(!path.empty()) 会挡住，不会静默拿一个宽度
// 算错的素材去跑用例。
inline std::string synth_video_with_sar(const TempDir& dir, int sar_num, int sar_den) {
    if (sar_num <= 0 || sar_den <= 0 || (640 * sar_den) % sar_num != 0) {
        return std::string();
    }
    const std::string path = dir.path + "/synth_sar_" + std::to_string(sar_num) + "_" +
                              std::to_string(sar_den) + ".mp4";
    const int coded_w = 640 * sar_den / sar_num;

    std::ostringstream cmd;
    cmd << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
        << "-f lavfi -i \"testsrc2=size=" << coded_w << "x360:rate=25:duration=1\" "
        << "-vf \"setsar=" << sar_num << "/" << sar_den << "\" "
        << "-c:v libx264 -pix_fmt yuv420p "
        << "\"" << path << "\" > /dev/null 2>&1";
    return (std::system(cmd.str().c_str()) == 0) ? path : std::string();
}

// 只有**容器层** SAR 的素材：先编一条 640x360、SAR 1:1 的
// x264 流，再 `-c copy -aspect 4:3` 重封装成 mp4——码流（SPS VUI）不动，只有
// mp4 的 pasp 盒写成 3:4（DAR 4:3 ÷ 640/360 = 3/4）。
//
// 与上面 synth_video_with_sar() 的区别正是要害：那个用 setsar 滤镜，
// 码流与容器**两层都写** SAR，所以"只读 codecpar"与"读流上"给出同一个数，
// 用例分辨不出来。这份素材两层不一致：
//   FFmpeg 8.1.2（本仓链接的那份）实测：
//     stream->sample_aspect_ratio          = 3:4   （mov.c 读 pasp 写到流上）
//     stream->codecpar->sample_aspect_ratio = 1:1   （h264 SPS 里的 Square）
//     av_guess_sample_aspect_ratio(...)     = 3:4
// 用例里另行断言 codecpar 那层确是 1:1（前提防呆：哪天生成方式变了、两层又
// 一致了，用例会先在前提上红，而不是悄悄失去判别力）。
inline std::string synth_video_with_container_sar(const TempDir& dir) {
    const std::string raw  = dir.path + "/synth_container_sar_src.mp4";
    const std::string path = dir.path + "/synth_container_sar_4x3.mp4";

    std::ostringstream enc;
    enc << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
        << "-f lavfi -i \"testsrc2=size=640x360:rate=25:duration=1\" "
        << "-c:v libx264 -pix_fmt yuv420p "
        << "\"" << raw << "\" > /dev/null 2>&1";
    if (std::system(enc.str().c_str()) != 0) return std::string();

    std::ostringstream remux;
    remux << "\"" << ffmpeg_cli_path() << "\" -hide_banner -loglevel error -y "
          << "-i \"" << raw << "\" -c copy -aspect 4:3 "
          << "\"" << path << "\" > /dev/null 2>&1";
    return (std::system(remux.str().c_str()) == 0) ? path : std::string();
}

}  // namespace syp::test
