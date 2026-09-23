// test_pipeline.cpp — Pipeline：把 Demuxer/PacketQueue/Decoder/FrameQueue
// 串起来的编排层，只暴露 step()。
//
// 五条用例覆盖核心承诺：
//   1. 确定性——同一份输入同一串 step() 得同一串输出
//   2. Blocked 是返回值不是内部阻塞——FrameQueue 满了必须停在 Blocked
//   3. Blocked 之后排空要能继续，且帧不丢不重
//   4. seek() 的四步顺序都要生效：队列清空、demuxer 真的移动、解码器
//      flush、EOF 标志复位
//   5. 排空的前提下有限次 step() 必定推进到 Eof
//
// 两条回归用例覆盖一个真实缺陷：
// 上面五条用例全部跑在 gen-fixtures.sh 产出的 faststart.mp4/moovend.mp4
// 上——那两份素材用 -preset ultrafast 编码，libx264 在这个 preset 下不产
// B 帧，全程验证矩阵因此对"带 B 帧的真实素材"是结构性盲区：Pipeline
// 在这种素材上会在 flush 信号发出、解码器还压着重排序延迟帧时把这条轨
// 判成"没有更多能做的事"而永久跳过它，表现为永久 Blocked（活锁），且
// 丢帧数 = 解码器重排延迟 - 1。
//   6. bframes.mp4（真的带 B 帧，见 gen-fixtures.sh 里现场的 ffprobe 断言）
//      上必须能正常推进到 Eof，且帧数与 FFmpeg 参照完全一致——不是"跑到
//      Blocked 就算数"。
//   7. 裸 step() + 即时 pop_frame 的消费节奏（真实消费者的写法，不
//      经过 decode_pipeline() 那层"Blocked 时兜底轮询排空"的安全网）不会
//      陷入连续 Blocked——用一个远小于"永久卡死"量级、但远大于任何正常
//      背压抖动的上限即时 fail，不靠 ctest TIMEOUT 撞看门狗。
//
// 第 8 条覆盖同族的第二个缺陷：seek() 复位
// ts.eof_sent 这一步（pipeline.cpp 里 seek() 末尾那个 for 循环）此前没有
// 任何回归保护——删掉那一行，完整 ctest 16/16 依然全绿。「解到 Eof →
// seek → 再解到 Eof」（循环播放/重播，最普通的操作）会复现跟 #6/#7
// 逐字同一个活锁模式：flush 信号从没针对第二轮重发，receive() 永远
// NeedInput，decoder_eof 永不置位。场景 C（test_decode_e2e.cpp）抓不到
// 它——那条用例每次 seek 后只解固定 80 帧，从不把管线推进到真正的 Eof。
//   8. 解到 Eof → seek → 再解到 Eof：第二轮必须还能正常推进到 Eof，不能
//      陷入连续 Blocked。
//
// 第 6/7/8 三条用例开头都会先用 material_has_b_frames() 做一次运行期
// 守护（把素材换成零 B 帧的同名副本，这三条用例会在没有这层
// 守护时全绿——缺陷完全逃逸，因为它们的断言本身不依赖"素材真的带 B 帧"
// 这个前提，只依赖"帧数/Eof 是否达到"这类间接后果）。gen-fixtures.sh 里
// 生成期的 ffprobe 断言只挡"有人改坏生成脚本"，挡不住"素材来自别处/被
// 替换/manifest 目录复用"——material_has_b_frames() 独立于生成脚本，
// 直接对当前测试实际打开的这份文件用 avformat_find_stream_info() 读
// AVCodecParameters::video_delay（"Number of delayed frames"）。
//
// 位置纪律：这个字段**不是**"H.264/HEVC
// 通用"的探测手段——它是 MOV demuxer（libavformat/mov.c）里
// mov_estimate_video_delay() 依 ctts（composition time to sample）表
// 现场估算出来的，而那个函数的门槛写死是
// `codec_id == AV_CODEC_ID_H264`（见 mov.c 里
// `mov_read_trak`/`mov_estimate_video_delay` 附近对 `st->codecpar->
// video_delay` 的赋值），HEVC 完全走不到这条路径。也就是说
// material_has_b_frames() 目前**只对 H.264 素材可靠**——现状无风险，
// 因为 tools/gen-fixtures.sh 至今只产 H.264 素材，这里用到它的三条用例
// 与 test_decode_e2e.cpp 场景 A~D 用的都是 H.264。将来素材矩阵加 HEVC
// 带 B 帧内容时，这个判据要重新验证，不能想当然地照搬——不复用
// gen-fixtures.sh 那条 ffprobe 断言用的 pts!=dts 判据，是两个独立来源
// 的交叉验证，跟这条"只对 H.264 可靠"的限制是两件不同的事，别混在一起。
#include "frame_digest.h"
#include "scenarios.h"
#include "media/pipeline.h"
#include "tiny_test.h"
#include "support/fake_hw_backend.h"
#include "support/synth_media.h"
#include "support/watchdog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

using namespace syp::media;
using namespace syp::probe;

namespace {
std::string fixture(const char* name) {
    const char* d = std::getenv("SYP_FIXTURE_DIR");
    return std::string(d ? d : "") + "/" + name;
}

// 运行期守护（见文件顶部长注释）：path 指向的文件真的带 B 帧——视频轨
// AVCodecParameters::video_delay > 0。打不开/找不到流信息/没有视频轨
// 一律按「没有 B 帧」处理（返回 false），交给调用方的 REQUIRE 报失败，
// 不在这里静默吞掉。
bool material_has_b_frames(const std::string& path) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }
    bool has_delay = false;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVCodecParameters* par = fmt->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && par->video_delay > 0) {
            has_delay = true;
            break;
        }
    }
    avformat_close_input(&fmt);
    return has_delay;
}

// ---------------------------------------------------------------------
// pipeline_seek_failure_still_completes_steps_3_and_4 的脚手架：造一份
// "demuxer_->seek() 会真的失败" 的素材。
//
// 原先设想的构造思路（经不报 Content-Length 的 LoopbackServer 让
// AvioBridge 判定不可 seek）经查证不成立——写了独立探测小程序验证：
// mov 解码器的 mov_read_seek()/mov_seek_stream() 对非分片 mp4 纯粹基于
// 已经建好的内存采样索引工作，从不真正碰 AVIOContext 的 seek 回调；
// 就算把回调改成"无条件返回错误"、把 ctx->seekable 显式清零，
// av_seek_frame() 依然返回 0（成功）——因为它压根没被调用。分片 mp4
// （fMP4）路径也试过：mov_seek_fragment() 在 frag_index 不完整时直接
// `return 0`（不去抓新分片），照样落回已知范围内的采样、不失败。
//
// 真正能让 av_seek_frame() 在这个容器格式上失败的唯一途径（穿了源码才
// 找到）：被选中的默认轨的采样索引本身是空的——mov_seek_stream() 对
// nb_index_entries==0 的轨直接 `return AVERROR_INVALIDDATA`。构造方法：
// 拿一份正常素材，把视频轨 stbl 里 stsz/stts/stco/stsc 四个 box 的
// entry_count/sample_count 字段原地清零（这四个字段都在各自 box payload
// 的固定偏移上，清零不改变任何 box 的大小，不需要挪动后面的任何字节，
// 也不需要重算任何父 box 的 size 字段）。av_find_default_stream_index()
// 在两条轨都存在时仍然会因为 codecpar 里 width/height 已经从 SPS 解出
// （即使 pix_fmt 未知）而把视频判成默认轨，seek(-1, ...) 因此落在这条
// 空索引的轨上失败。音频轨完全不受影响，继续正常携带样本——这正是
// 用例断言"失败之后仍能继续 step() 出帧"所需要的那条活轨。
//
// 不是"生产上可达"的场景（最初设想的那个场景已被证伪），是
// 已知范围内唯一能安全（不碰 UB/不崩）复现 av_seek_frame() < 0 的构造。

std::vector<uint8_t> read_file_bytes(const std::string& path) {
    // 一次 resize + 一次 read，不用 istreambuf_iterator 逐字节 push_back：
    // 这两份素材都是 30MB 量级，逐字节路径在 TSan 构建下是三千万次被插桩的
    // 内存写，实测能吃掉好几秒的用例预算（这条读文件本身跟被测契约无关，
    // 不值得占那份预算）。
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> out(static_cast<std::size_t>(n));
    f.read(reinterpret_cast<char*>(out.data()), n);
    if (!f) return {};
    return out;
}

bool write_file_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

struct MovBox {
    std::string fourcc;
    std::size_t start   = 0;
    std::size_t end     = 0;
    std::size_t payload = 0;   // box 头（8 或 16 字节）之后的第一个字节
};

// 只解顶层 box 头，不递归——递归由调用方对 payload 区间再次调用实现。
// 只需要正确处理我们自己 gen-fixtures.sh 产出的素材（单个 moov，box 都
// 远小于 4GB），但仍然按标准 largesize（size==1）与 size==0（占到区间
// 结尾）两种边界写，防止将来素材生成方式变了就在这里读出垂悬偏移。
std::vector<MovBox> read_boxes(const std::vector<uint8_t>& d, std::size_t start, std::size_t end) {
    std::vector<MovBox> out;
    std::size_t pos = start;
    while (pos + 8 <= end) {
        const uint32_t size32 = (uint32_t(d[pos]) << 24) | (uint32_t(d[pos + 1]) << 16) |
                                 (uint32_t(d[pos + 2]) << 8) | uint32_t(d[pos + 3]);
        const std::string fourcc(reinterpret_cast<const char*>(&d[pos + 4]), 4);
        std::size_t hdr  = 8;
        uint64_t    size = size32;
        if (size32 == 1) {
            if (pos + 16 > end) break;
            uint64_t s = 0;
            for (int i = 0; i < 8; ++i) s = (s << 8) | d[pos + 8 + static_cast<std::size_t>(i)];
            size = s;
            hdr  = 16;
        } else if (size32 == 0) {
            size = end - pos;
        }
        if (size < hdr || pos + size > end) break;   // 格式不对：停止而不是继续读越界
        out.push_back(MovBox{fourcc, pos, pos + static_cast<std::size_t>(size),
                              pos + hdr});
        pos += static_cast<std::size_t>(size);
    }
    return out;
}

// 按 path（如 {"moov","trak"}）在 [start,end) 区间递归找所有匹配 box，
// 返回它们各自的 payload 区间——跟上面 tools/gen-fixtures.sh 里那份
// Python 原型逐字对应（本函数最初就是照那份验证脚本翻译过来的）。
void find_path(const std::vector<uint8_t>& d, const std::vector<std::string>& path,
               std::size_t start, std::size_t end, std::vector<MovBox>* out) {
    const std::vector<MovBox> boxes = read_boxes(d, start, end);
    for (const MovBox& b : boxes) {
        if (b.fourcc != path[0]) continue;
        if (path.size() == 1) {
            out->push_back(b);
        } else {
            const std::vector<std::string> rest(path.begin() + 1, path.end());
            find_path(d, rest, b.payload, b.end, out);
        }
    }
}

// 把 faststart.mp4 的字节原样拷一份，找到视频轨（mdia/hdlr 的
// handler_type == "vide"）后清零它 stbl 里 stsz/stts/stco/stsc 四个 box
// 的计数字段。返回 false 表示没找到视频轨或没找到 stsz（素材结构跟
// 预期不符）——调用方按 REQUIRE 处理，不能静默把一份没被真正改写过
// 的素材当成"已损坏"送进 Pipeline，那样这条用例会变成测一个假前提。
bool zero_out_video_track_index(std::vector<uint8_t>* data) {
    std::vector<MovBox> moovs;
    find_path(*data, {"moov"}, 0, data->size(), &moovs);
    if (moovs.empty()) return false;

    std::vector<MovBox> traks;
    find_path(*data, {"trak"}, moovs[0].payload, moovs[0].end, &traks);

    bool patched_stsz = false;
    for (const MovBox& trak : traks) {
        std::vector<MovBox> hdlrs;
        find_path(*data, {"mdia", "hdlr"}, trak.payload, trak.end, &hdlrs);
        bool is_video = false;
        for (const MovBox& h : hdlrs) {
            // hdlr payload：version(1)+flags(3)+pre_defined(4)+handler_type(4)。
            if (h.payload + 12 > h.end) continue;
            const std::string handler_type(reinterpret_cast<const char*>(&(*data)[h.payload + 8]), 4);
            if (handler_type == "vide") is_video = true;
        }
        if (!is_video) continue;

        // stsz：version(1)+flags(3)+sample_size(4)+sample_count(4)+[...]——
        // 计数字段在 payload 偏移 8。stts/stco/stsc 三者都是
        // version(1)+flags(3)+entry_count(4)+[...]——计数字段在偏移 4。
        std::vector<MovBox> stszs, sttses, stcos, stscs;
        find_path(*data, {"mdia", "minf", "stbl", "stsz"}, trak.payload, trak.end, &stszs);
        find_path(*data, {"mdia", "minf", "stbl", "stts"}, trak.payload, trak.end, &sttses);
        find_path(*data, {"mdia", "minf", "stbl", "stco"}, trak.payload, trak.end, &stcos);
        find_path(*data, {"mdia", "minf", "stbl", "stsc"}, trak.payload, trak.end, &stscs);

        for (const MovBox& b : stszs) {
            if (b.payload + 12 > b.end) continue;
            for (int i = 0; i < 4; ++i) (*data)[b.payload + 8 + static_cast<std::size_t>(i)] = 0;
            patched_stsz = true;
        }
        for (const MovBox& b : sttses) {
            if (b.payload + 8 > b.end) continue;
            for (int i = 0; i < 4; ++i) (*data)[b.payload + 4 + static_cast<std::size_t>(i)] = 0;
        }
        for (const MovBox& b : stcos) {
            if (b.payload + 8 > b.end) continue;
            for (int i = 0; i < 4; ++i) (*data)[b.payload + 4 + static_cast<std::size_t>(i)] = 0;
        }
        for (const MovBox& b : stscs) {
            if (b.payload + 8 > b.end) continue;
            for (int i = 0; i < 4; ++i) (*data)[b.payload + 4 + static_cast<std::size_t>(i)] = 0;
        }
    }
    return patched_stsz;
}

// ---------------------------------------------------------------------
// 第二份「demuxer_->seek() 会真的失败」的素材 + 一个由用例完全掌控的
// AVIOContext。跟上面那份走的是完全不同的失败路径，副作用也完全不同——
// 为什么需要第二条路，见 pipeline_failed_seek_still_flushes_decoders
// 上方的长注释。
// ---------------------------------------------------------------------
void put_be32(std::vector<uint8_t>* d, std::size_t off, uint32_t v) {
    (*d)[off]     = static_cast<uint8_t>(v >> 24);
    (*d)[off + 1] = static_cast<uint8_t>(v >> 16);
    (*d)[off + 2] = static_cast<uint8_t>(v >> 8);
    (*d)[off + 3] = static_cast<uint8_t>(v);
}

// 把视频轨 stss（sync sample table）改写成 entry_count=1、唯一那条记录
// 指向一个根本不存在的样本号。效果：keyframe_absent 仍然是 0（box 非空，
// 走不到 mov.c 里「stss 为空 ⇒ 所有样本都是关键帧」那条兜底），而
// mov_build_index() 里 `current_sample + key_off == sc->keyframes[stss_index]`
// 永远不成立 ⇒ 整条轨的 AVIndexEntry 一个 AVINDEX_KEYFRAME 都没有。
bool blank_out_video_keyframe_table(std::vector<uint8_t>* data) {
    std::vector<MovBox> moovs;
    find_path(*data, {"moov"}, 0, data->size(), &moovs);
    if (moovs.empty()) return false;
    std::vector<MovBox> traks;
    find_path(*data, {"trak"}, moovs[0].payload, moovs[0].end, &traks);
    bool patched = false;
    for (const MovBox& trak : traks) {
        std::vector<MovBox> hdlrs;
        find_path(*data, {"mdia", "hdlr"}, trak.payload, trak.end, &hdlrs);
        bool is_video = false;
        for (const MovBox& h : hdlrs) {
            if (h.payload + 12 > h.end) continue;
            if (std::string(reinterpret_cast<const char*>(&(*data)[h.payload + 8]), 4) == "vide")
                is_video = true;
        }
        if (!is_video) continue;
        std::vector<MovBox> stsses;
        find_path(*data, {"mdia", "minf", "stbl", "stss"}, trak.payload, trak.end, &stsses);
        for (const MovBox& b : stsses) {
            if (b.payload + 12 > b.end) continue;
            put_be32(data, b.payload + 4, 1);
            put_be32(data, b.payload + 8, 0x0FFFFFFFu);
            patched = true;
        }
    }
    return patched;
}

struct MemSource {
    const std::vector<uint8_t>* buf        = nullptr;
    int64_t                     pos        = 0;
    bool                        fail_seek  = false;
    int64_t                     seek_calls = 0;
    // >= 0 时：读到这个偏移就报 AVERROR_EOF，等价于"这份流被截断了"。
    // seek 回调仍然照常工作（截断的是可读数据，不是可寻址范围）。
    // 用途见 TEST_CASE 里 B) 那一半——只是为了让"demux 到底、解码器
    // 正在 drain"这个状态在整条流的 1/8 处就出现，省掉两趟整片解码。
    int64_t                     cutoff     = -1;
};

int mem_read(void* opaque, uint8_t* out, int n) {
    MemSource* s = static_cast<MemSource*>(opaque);
    int64_t size = static_cast<int64_t>(s->buf->size());
    if (s->cutoff >= 0 && s->cutoff < size) size = s->cutoff;
    if (s->pos >= size) return AVERROR_EOF;
    const int64_t left = size - s->pos;
    const int     m    = static_cast<int>(left < n ? left : n);
    std::memcpy(out, s->buf->data() + s->pos, static_cast<std::size_t>(m));
    s->pos += m;
    return m;
}

int64_t mem_seek(void* opaque, int64_t off, int whence) {
    MemSource* s = static_cast<MemSource*>(opaque);
    if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) return static_cast<int64_t>(s->buf->size());
    ++s->seek_calls;
    if (s->fail_seek) return AVERROR(EIO);
    const int w = whence & ~AVSEEK_FORCE;
    int64_t np = (w == SEEK_CUR)   ? s->pos + off
               : (w == SEEK_END)   ? static_cast<int64_t>(s->buf->size()) + off
                                   : off;
    if (np < 0) return AVERROR(EINVAL);
    s->pos = np;
    return np;
}

// 一次完整的"跑到 Eof"运行的可观测量。seek_at_step >= 0 时在第
// seek_at_step 次 step() *之前* 做一次 seek()，并且只在这一次 seek()
// 期间打开 MemSource::fail_seek——也就是说底层源在正常读包时完全健康，
// 只有 libavformat 在 seek 路径上真正去调 AVIOContext::seek 的那一刻会
// 拿到 EIO。
struct SeekFailRun {
    int64_t    steps             = 0;
    int64_t    video_frames      = 0;
    int64_t    audio_frames      = 0;
    int64_t    last_demux_step   = -1;
    syp_status seek_rc           = SYP_OK;
    int64_t    video_after_seek  = 0;
    int64_t    audio_after_seek  = 0;
    int64_t    last_video_pts_before_seek = -1;
    int64_t    first_video_pts_after_seek = -1;
    bool       reached_eof       = false;
    bool       saw_error         = false;
    bool       created           = false;
};

// stop_after_seek >= 0：seek 之后最多再 step 这么多次就收工（不跑到 Eof）。
// 只是为了控制用例耗时——流中段那一半的断言不需要跑完整条流。
// seek_after_video >= 0：等这条流吐出这么多帧视频之后再 seek（跟
// seek_at_step 二选一）。用"第几帧视频"而不是"第几次 step"来定位，是因为
// gen-fixtures.sh 的 GOP 长度由 seed 决定（1~4 秒一个 I 帧），固定步数会
// 让 seek 点落在 GOP 里的随机位置——本用例 A) 那一半要的是"离下一个 IDR
// 尽量远"，只有把 seek 点钉在 GOP 开头才对所有 seed 都成立。
SeekFailRun run_to_eof(const std::vector<uint8_t>& data, int64_t seek_at_step, int64_t seek_us,
                        int64_t stop_after_seek = -1, int64_t cutoff = -1,
                        int64_t seek_after_video = -1) {
    SeekFailRun r;
    MemSource   src{&data, 0, false, 0, cutoff};
    const int   kBuf  = 32768;
    unsigned char* iobuf = static_cast<unsigned char*>(av_malloc(static_cast<std::size_t>(kBuf)));
    AVIOContext*   pb    = avio_alloc_context(iobuf, kBuf, 0, &src, mem_read, nullptr, mem_seek);
    pb->seekable = AVIO_SEEKABLE_NORMAL;

    syp_status err = SYP_OK;
    auto       p   = Pipeline::create_avio(pb, PipelineConfig{}, &err);
    if (p == nullptr) {
        av_free(pb->buffer);
        avio_context_free(&pb);
        return r;
    }
    r.created = true;

    std::vector<bool> is_video;
    for (const TrackInfo& t : p->tracks()) is_video.push_back(t.is_video);

    bool    seeked     = false;
    int64_t seek_step_i = -1;
    for (int64_t i = 0; i < 2000000; ++i) {
        const bool due = (seek_at_step >= 0 && i == seek_at_step) ||
                          (seek_after_video >= 0 && !seeked && r.video_frames >= seek_after_video);
        if (due && !seeked) {
            src.fail_seek = true;
            r.seek_rc     = p->seek(seek_us);
            src.fail_seek = false;
            seeked        = true;
            seek_step_i   = i;
        }
        if (seeked && stop_after_seek >= 0 && i - seek_step_i > stop_after_seek) break;
        const auto o = p->step();
        ++r.steps;
        if (o.kind == StepOutcome::Kind::DemuxedPacket) {
            r.last_demux_step = i;
        } else if (o.kind == StepOutcome::Kind::DecodedFrame) {
            auto       f = p->pop_frame(o.track_index);
            const bool v = o.track_index >= 0 &&
                            static_cast<std::size_t>(o.track_index) < is_video.size() &&
                            is_video[static_cast<std::size_t>(o.track_index)];
            if (v) {
                ++r.video_frames;
                if (seeked) {
                    ++r.video_after_seek;
                    if (r.first_video_pts_after_seek < 0 && f.has_value())
                        r.first_video_pts_after_seek = f->pts_us();
                } else if (f.has_value()) {
                    r.last_video_pts_before_seek = f->pts_us();
                }
            } else {
                ++r.audio_frames;
                if (seeked) ++r.audio_after_seek;
            }
        } else if (o.kind == StepOutcome::Kind::Eof) {
            r.reached_eof = true;
            break;
        } else if (o.kind == StepOutcome::Kind::Error) {
            r.saw_error = true;
            break;
        }
    }
    p.reset();
    av_free(pb->buffer);
    avio_context_free(&pb);
    return r;
}

}  // namespace

TEST_CASE(pipeline_step_is_deterministic) {
    // 同一份输入 + 同一串 step() 调用 = 同一串输出。
    //
    // 原先这条用例几乎没有鉴别力：只比较 Kind、只跑固定 200 步。
    // 实测那 200 步是 P/F（DemuxedPacket/DecodedFrame）严格交替，
    // Blocked=0、Eof=0、没有排空、没有 seek——任何会在 demux/decode 间
    // 交替的实现都能凑出同一串 Kind，根本没检验"谁的 packet 先被谁的
    // 帧"这种交织顺序信息，也没检验 seek 会不会破坏确定性。补两处：
    //   1. 把 (kind, track_index) 一起编码进序列（用
    //      kind*100 + (track_index+1)，track_index 可能是 -1，+1 后落在
    //      [0, N] 不会跟 kind 的百位数混叠），而不是只有 kind 一个维度。
    //   2. 步数从固定 200 改成跑到 Eof 为止（本素材约 1.2~1.3 万步），
    //      中途在同一个确定的步数插一次 seek()——seek 本身也是确定性
    //      操作，只要两次 run() 在同样的步数调用同样的 seek()，序列理应
    //      仍然逐项相等；这同时验证了"调度顺序被打乱"一类变异不再能靠
    //      "反正没测到 seek 场景"侥幸逃过。
    auto run = [](bool* seek_ok) {
        syp_status err = SYP_OK;
        auto p = Pipeline::create_file(fixture("faststart.mp4"), PipelineConfig{}, &err);
        std::vector<int> kinds;
        if (p == nullptr) return kinds;
        for (int i = 0; i < 50000; ++i) {
            if (i == 500) {
                if (p->seek(3000000) != SYP_OK) *seek_ok = false;
            }
            auto o = p->step();
            kinds.push_back(static_cast<int>(o.kind) * 100 + (o.track_index + 1));
            if (o.kind == StepOutcome::Kind::DecodedFrame) p->pop_frame(o.track_index);
            if (o.kind == StepOutcome::Kind::Eof || o.kind == StepOutcome::Kind::Error) break;
        }
        return kinds;
    };
    bool a_seek_ok = true;
    bool b_seek_ok = true;
    auto a = run(&a_seek_ok);
    auto b = run(&b_seek_ok);
    REQUIRE(!a.empty());
    CHECK(a_seek_ok);
    CHECK(b_seek_ok);
    CHECK(a == b);

    // 判别力自检：确认序列里真的同时出现过两条轨（不是只测到视频或只测
    // 到音频），且真的推进到了 Eof（不是提前因为 Error 中断）——否则上面
    // 的相等比较可能只是巧合地比对了两段空/退化的序列。
    const int last_kind = a.back() / 100;
    CHECK_EQ(last_kind, static_cast<int>(StepOutcome::Kind::Eof));
    bool saw_video = false;
    bool saw_audio = false;
    for (int k : a) {
        const int track_plus1 = k % 100;
        if (track_plus1 == 1) saw_video = true;
        else if (track_plus1 == 2) saw_audio = true;
    }
    CHECK(saw_video);
    CHECK(saw_audio);
}

TEST_CASE(pipeline_reports_blocked_when_frame_queue_full) {
    // 背压是返回值，不是内部阻塞。不排空就必须停在 Blocked。
    PipelineConfig cfg;
    cfg.max_frames_per_track = 2;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), cfg, &err);
    REQUIRE(p != nullptr);
    bool saw_blocked = false;
    for (int i = 0; i < 5000; ++i) {          // 不排空
        auto o = p->step();
        if (o.kind == StepOutcome::Kind::Blocked) { saw_blocked = true; break; }
        if (o.kind == StepOutcome::Kind::Eof) break;
        REQUIRE(o.kind != StepOutcome::Kind::Error);
    }
    CHECK(saw_blocked);
}

TEST_CASE(pipeline_resumes_after_drain_without_losing_frames) {
    // Blocked 之后排空，帧序列不丢不重。
    //
    // 用极小的 max_frames_per_track 逼出频繁的 Blocked；两种排空节奏
    // （每帧立刻 pop / 攒到 Blocked 才一次性排空）跑同一份素材，按轨分别
    // 比较各自的 pts_us 序列——用 map<track_index, vector<pts_us>> 而不是
    // 一条全局序列，因为不同的排空节奏会改变两轨之间的交织顺序（这是
    // 消费节奏的正常差异，不是 bug），但同一条轨内部的先后顺序、总数、
    // 每一个 pts 值必须完全一致：这才是「不丢不重」真正要钉住的东西。
    using PerTrack = std::map<int32_t, std::vector<int64_t>>;

    auto run = [](bool defer_drain) {
        PipelineConfig cfg;
        cfg.max_frames_per_track = 2;   // 很小，逼近频繁触发 Blocked
        syp_status err = SYP_OK;
        auto p = Pipeline::create_file(fixture("faststart.mp4"), cfg, &err);
        PerTrack out;
        if (p == nullptr) return out;

        bool saw_blocked = false;
        for (int i = 0; i < 200000; ++i) {
            auto o = p->step();
            if (o.kind == StepOutcome::Kind::DecodedFrame) {
                if (!defer_drain) {
                    auto f = p->pop_frame(o.track_index);
                    if (f.has_value()) out[o.track_index].push_back(f->pts_us());
                }
                // defer_drain 模式故意不立刻取——攒着直到 Blocked 才一次性
                // 排空所有轨，制造真实的背压/排空循环。
            } else if (o.kind == StepOutcome::Kind::Blocked) {
                saw_blocked = true;
                for (const TrackInfo& t : p->tracks()) {
                    for (;;) {
                        auto f = p->pop_frame(t.index);
                        if (!f.has_value()) break;
                        out[t.index].push_back(f->pts_us());
                    }
                }
            } else if (o.kind == StepOutcome::Kind::Eof) {
                break;
            } else if (o.kind == StepOutcome::Kind::Error) {
                CHECK(false);   // 见下方 lambda 返回类型说明，这里不能用 REQUIRE
                break;
            }
        }
        // 收尾：Eof 之后 FrameQueue 里可能还剩没被 Blocked 分支排空的帧。
        for (const TrackInfo& t : p->tracks()) {
            for (;;) {
                auto f = p->pop_frame(t.index);
                if (!f.has_value()) break;
                out[t.index].push_back(f->pts_us());
            }
        }
        if (defer_drain) CHECK(saw_blocked);   // 确认真的触发了背压路径
        return out;
    };

    auto with_backpressure    = run(/*defer_drain=*/true);
    auto without_backpressure = run(/*defer_drain=*/false);

    REQUIRE(!with_backpressure.empty());
    REQUIRE(!without_backpressure.empty());
    CHECK_EQ(with_backpressure.size(), without_backpressure.size());   // 轨数一致
    CHECK(with_backpressure == without_backpressure);                  // 每条轨的帧序列都一致
}

TEST_CASE(pipeline_seek_flushes_everything) {
    // seek 后队列为空、解码器已 flush、EOF 标志复位。
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), PipelineConfig{}, &err);
    REQUIRE(p != nullptr);

    // 先跑一段、故意不 pop——让 FrameQueue 真的攒着帧，PacketQueue 也攒着
    // 包，这样"seek 后队列为空"这条断言才不是"从没跑过所以本来就是空"的
    // 假绿。
    bool saw_frame_before_seek = false;
    for (int i = 0; i < 50; ++i) {
        auto o = p->step();
        if (o.kind == StepOutcome::Kind::DecodedFrame) saw_frame_before_seek = true;
        if (o.kind == StepOutcome::Kind::Eof || o.kind == StepOutcome::Kind::Error) break;
        // 故意不调用 pop_frame：帧留在 FrameQueue 里。
    }
    REQUIRE(saw_frame_before_seek);

    REQUIRE(p->seek(5000000) == SYP_OK);   // 5 秒

    // 1) 队列必须清空：seek 前攒的旧帧不能还在队列里等着被取出。
    //    （去掉 seek() 里的 clear() 时，这里会直接拿到 seek 前的残留帧。）
    for (const TrackInfo& t : p->tracks()) {
        CHECK(!p->pop_frame(t.index).has_value());
    }

    // 2) 解码器已 flush + EOF 标志复位：继续 step() 必须能推进到 5s 附近的
    //    新帧，而不是卡死、也不是吐出 seek 前遗留的早期帧（pts 接近 0）。
    //    AVSEEK_FLAG_BACKWARD 落在最近的前一个关键帧，容差给 2 秒足够覆盖
    //    素材的 GOP 间隔，同时窄到能抓住"seek 完全不生效"这类数量级错误。
    int64_t first_pts_after_seek  = -1;
    int32_t first_track_after_seek = -1;
    for (int i = 0; i < 500; ++i) {
        auto o = p->step();
        if (o.kind == StepOutcome::Kind::DecodedFrame) {
            auto f = p->pop_frame(o.track_index);
            REQUIRE(f.has_value());
            first_pts_after_seek   = f->pts_us();
            first_track_after_seek = o.track_index;
            break;
        }
        REQUIRE(o.kind != StepOutcome::Kind::Error);
        if (o.kind == StepOutcome::Kind::Eof) break;
    }
    REQUIRE(first_track_after_seek >= 0);
    CHECK(first_pts_after_seek > 2000000);
    CHECK(first_pts_after_seek < 7000000);

    // 存活变异体：忘记复位 EOF 相关标志（demux_eof_/eof_sent/
    // decoder_eof）在上面这段测不出来，因为跑到 seek() 那一刻 demux 才
    // 刚开始 50 步，demux_eof_ 本来就还是 false，"复位"是空操作也照样
    // 通过。只有先真的跑到 Eof、再 seek，复位与不复位才会产生可观察的
    // 差异：忘记复位的话，decode 分支会因为每条轨的 decoder_eof 仍是
    // true 而永远跳过，demux 分支也会因为 demux_eof_ 仍是 true 而永远
    // 不再读——第一次 step() 就直接落回 Eof 判定，一帧都不会吐出来。
    //
    // 先排空跑到真正的 Eof（复用 pipeline_step_never_blocks_forever 已经
    // 验证过的性质：排空前提下有限步必达 Eof）。
    bool reached_real_eof = false;
    for (int i = 0; i < 20000; ++i) {
        auto o = p->step();
        if (o.kind == StepOutcome::Kind::DecodedFrame) {
            p->pop_frame(o.track_index);   // 排空，保证真的能推进到 Eof
        } else if (o.kind == StepOutcome::Kind::Eof) {
            reached_real_eof = true;
            break;
        }
        REQUIRE(o.kind != StepOutcome::Kind::Error);
    }
    REQUIRE(reached_real_eof);

    REQUIRE(p->seek(1000000) == SYP_OK);   // 1 秒——EOF 之后再 seek 一次

    int64_t first_pts_after_second_seek = -1;
    int64_t frames_after_second_seek    = 0;
    for (int i = 0; i < 20000; ++i) {
        auto o = p->step();
        if (o.kind == StepOutcome::Kind::DecodedFrame) {
            auto f = p->pop_frame(o.track_index);
            REQUIRE(f.has_value());
            if (first_pts_after_second_seek < 0) first_pts_after_second_seek = f->pts_us();
            ++frames_after_second_seek;
        } else if (o.kind == StepOutcome::Kind::Eof) {
            break;
        }
        REQUIRE(o.kind != StepOutcome::Kind::Error);
    }
    // 忘记复位 EOF 标志的变异体：这里 frames_after_second_seek 恒为 0，
    // 第一次 step() 就直接是 Eof。
    CHECK(frames_after_second_seek > 0);
    CHECK(first_pts_after_second_seek >= 0);
    CHECK(first_pts_after_second_seek < 2000000);   // 落在 1s 前最近关键帧附近
}

TEST_CASE(pipeline_step_never_blocks_forever) {
    // 排空的前提下，有限次 step() 必定推进到 Eof。
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), PipelineConfig{}, &err);
    REQUIRE(p != nullptr);

    bool reached_eof = false;
    int64_t frames = 0;
    for (int i = 0; i < 1000000; ++i) {
        auto o = p->step();
        if (o.kind == StepOutcome::Kind::DecodedFrame) {
            auto f = p->pop_frame(o.track_index);   // 排空
            REQUIRE(f.has_value());
            ++frames;
        } else if (o.kind == StepOutcome::Kind::Error) {
            REQUIRE(false);
        } else if (o.kind == StepOutcome::Kind::Eof) {
            reached_eof = true;
            break;
        }
        // Blocked/DemuxedPacket：继续下一轮 step()。
    }
    CHECK(reached_eof);
    CHECK(frames > 0);
}

TEST_CASE(pipeline_reaches_eof_on_b_frame_material) {
    // 6：B 帧素材必须能解到 Eof，帧数与参照完全一致——不是"跑到 Blocked
    // 就算数"。fixture 名字取自 gen-fixtures.sh：bframes.mp4 是唯一
    // 保证带 B 帧的产物（脚本里现场用 ffprobe 断言过 pts!=dts 包数 > 0）。
    // 这里仍然用独立来源（video_delay，见文件顶部 material_has_b_frames()
    // 注释）在运行期再核一次——生成期断言挡不住"这条用例实际打开的文件
    // 不是刚生成的那份"（换成零 B 帧同名副本，没有这层守护
    // 时缺陷完全逃逸）。
    const std::string path = fixture("bframes.mp4");
    REQUIRE(material_has_b_frames(path));

    DecodeOptions opt;
    DecodeResult  ref = decode_reference(path, opt);
    REQUIRE(ref.error_stage.empty());
    // 空转硬底线（同 test_decode_e2e.cpp 场景 A/D）：先证明参照路径真的
    // 解出了东西，且给一个宽松但非零的下限。
    REQUIRE(ref.frames.size() > 500);

    syp_status err = SYP_OK;
    auto       p   = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(p != nullptr);

    DecodeResult got = decode_pipeline(*p, opt);

    std::printf("  [bframe-eof] ref_frames=%zu got_frames=%zu skipped=%lld err=[%s]\n",
                ref.frames.size(), got.frames.size(),
                static_cast<long long>(got.skipped_packets), got.error_stage.c_str());

    // 缺陷修复前，这里会看到 got.error_stage == "pipeline blocked with
    // nothing to drain"（decode_pipeline() 的兜底诊断：Blocked 时排空
    // 所有轨仍然一无所获），got.frames.size() 比 ref 少
    // "解码器重排延迟 - 1" 帧。
    CHECK_EQ(got.error_stage, std::string());
    CHECK_EQ(got.frames.size(), ref.frames.size());
    const std::string diff = diff_frames(ref, got);
    CHECK_EQ(diff, std::string());
}

TEST_CASE(pipeline_bare_step_loop_does_not_stall_on_b_frame_material) {
    // 7：裸 step() + 即时 pop_frame，不经过 decode_pipeline() 的 Blocked
    // 兜底排空——这是真实消费者最朴素的写法，也是缺陷复现时真正
    // 卡死的那条路径（实测：100000 次连续 Blocked 仍未到 Eof）。
    // 用一个远小于"永久卡死"、远大于任何正常背压抖动的上限，命中就立刻
    // REQUIRE 失败退出，不靠 ctest TIMEOUT 撞看门狗——TIMEOUT 只会告诉
    // 你"这条用例挂了"，命中不了就直接看到失败点在哪一步。
    const std::string path = fixture("bframes.mp4");
    REQUIRE(material_has_b_frames(path));   // 运行期守护，见文件顶部注释

    syp_status err = SYP_OK;
    auto       p   = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(p != nullptr);

    constexpr int kMaxConsecutiveBlocked = 2000;
    int           consecutive_blocked    = 0;
    bool          reached_eof            = false;
    int64_t       frames                 = 0;

    for (int i = 0; i < 2000000; ++i) {
        auto o = p->step();
        if (o.kind == StepOutcome::Kind::DecodedFrame) {
            consecutive_blocked = 0;
            auto f = p->pop_frame(o.track_index);
            REQUIRE(f.has_value());
            ++frames;
        } else if (o.kind == StepOutcome::Kind::DemuxedPacket) {
            consecutive_blocked = 0;
        } else if (o.kind == StepOutcome::Kind::Blocked) {
            ++consecutive_blocked;
            if (consecutive_blocked > kMaxConsecutiveBlocked) {
                std::printf("  [bframe-bare-step] STUCK: %d consecutive Blocked, "
                            "frames_so_far=%lld\n",
                            consecutive_blocked, static_cast<long long>(frames));
            }
            REQUIRE(consecutive_blocked <= kMaxConsecutiveBlocked);
        } else if (o.kind == StepOutcome::Kind::Error) {
            REQUIRE(false);
        } else if (o.kind == StepOutcome::Kind::Eof) {
            reached_eof = true;
            break;
        }
    }
    CHECK(reached_eof);
    CHECK(frames > 0);
}

TEST_CASE(pipeline_reaches_eof_again_after_seek_from_eof) {
    // 8（同族第二个缺陷的回归）：解到 Eof → seek → 再解到 Eof——
    // 循环播放/重播是最普通的操作。守的是 seek() 里
    // `ts.eof_sent = false;`（pipeline.cpp 约 395 行）这一句复位：
    // 不复位的话，第二轮到达真正的文件尾时，`ts.eof_sent` 仍然是第一轮
    // 就置过的 true，`has_pending_flush` 恒假、`draining` 也恒假（它的
    // 定义是 `demux_eof_ && ts.eof_sent`，eof_sent 不复位的话这个值跟
    // 复位与否无关都是 true——但 flush 信号本身从未针对这一轮重发），
    // flush 从没真正发给这一轮的解码器，receive() 永远停在 NeedInput，
    // decoder_eof 永不置位——跟前面那个缺陷逐字同一个
    // 活锁模式，只是触发条件从"带 B 帧"换成了"seek 之后二次到达 Eof"。
    //
    // 场景 C（test_decode_e2e.cpp）抓不到这个缺陷：它每次 seek 后只解
    // 固定的 80 帧，从不把管线真正推进到 Eof，状态机的这一段永远没被
    // 走到。
    const std::string path = fixture("bframes.mp4");
    REQUIRE(material_has_b_frames(path));   // 运行期守护，见文件顶部注释
    syp_status        err  = SYP_OK;
    auto              p    = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(p != nullptr);

    // 第一轮：正常解到 Eof（复用 pipeline_step_never_blocks_forever 已经
    // 验证过的性质）。
    bool first_eof = false;
    for (int i = 0; i < 2000000; ++i) {
        auto o = p->step();
        if (o.kind == StepOutcome::Kind::DecodedFrame) {
            p->pop_frame(o.track_index);
        } else if (o.kind == StepOutcome::Kind::Eof) {
            first_eof = true;
            break;
        }
        REQUIRE(o.kind != StepOutcome::Kind::Error);
    }
    REQUIRE(first_eof);

    REQUIRE(p->seek(1000000) == SYP_OK);   // 1 秒——从 Eof 状态往回 seek

    // 第二轮：必须还能推进到 Eof，不能陷入连续 Blocked——跟
    // pipeline_bare_step_loop_does_not_stall_on_b_frame_material 同一条
    // 纪律：设明确上限即时 fail，不靠 ctest TIMEOUT 撞看门狗。
    constexpr int kMaxConsecutiveBlocked = 2000;
    int           consecutive_blocked    = 0;
    bool          second_eof             = false;
    int64_t       frames_after_seek      = 0;
    for (int i = 0; i < 2000000; ++i) {
        auto o = p->step();
        if (o.kind == StepOutcome::Kind::DecodedFrame) {
            consecutive_blocked = 0;
            auto f = p->pop_frame(o.track_index);
            REQUIRE(f.has_value());
            ++frames_after_seek;
        } else if (o.kind == StepOutcome::Kind::DemuxedPacket) {
            consecutive_blocked = 0;
        } else if (o.kind == StepOutcome::Kind::Blocked) {
            ++consecutive_blocked;
            if (consecutive_blocked > kMaxConsecutiveBlocked) {
                std::printf("  [seek-then-eof] STUCK: %d consecutive Blocked, "
                            "frames_after_seek=%lld\n",
                            consecutive_blocked, static_cast<long long>(frames_after_seek));
            }
            REQUIRE(consecutive_blocked <= kMaxConsecutiveBlocked);
        } else if (o.kind == StepOutcome::Kind::Error) {
            REQUIRE(false);
        } else if (o.kind == StepOutcome::Kind::Eof) {
            second_eof = true;
            break;
        }
    }
    CHECK(second_eof);
    CHECK(frames_after_seek > 0);
}

TEST_CASE(pipeline_seek_failure_still_completes_steps_3_and_4) {
    // 条件 1：demuxer_->seek() 失败
    // 时，Pipeline::seek() 仍必须跑完第 3、4 步（各解码器 flush + EOF
    // 相关标志复位）——这条契约写进了 pipeline.h:116-124 +
    // pipeline.cpp seek() 本身的注释，此前没有任何回归测试覆盖：
    // 变异（av_seek_frame 失败时提前 return rc）16/16 全绿地漏过去了。
    //
    // 素材构造思路见本文件顶部 zero_out_video_track_index() 上方的长
    // 注释：让视频轨的采样索引本身是空的，是已知范围内唯一能安全复现
    // av_seek_frame() < 0 的办法。
    //
    // 诚实的局限性：这条用例能
    // 断言、且实测过会随那个变异变红/变绿的，只有 seek_rc 本身的
    // 返回值——其它三条断言（队列已清 / 不卡死不报错 / 干净推进到
    // Eof）都无法用这份素材区分"变异（跳过 flush + 复位）"与"正确实现"：
    // 让 av_seek_frame() 真的失败的唯一手段（默认轨采样索引整体清空）
    // 会触发 libavformat 自己的 seek_frame_generic() 兜底——它会线性扫描
    // 剩余整个文件找一个"属于这条轨、dts 大于目标"的包，视频轨永远没有
    // 这种包，扫描因此一路吃到文件尾部才放弃，副作用是音频轨此后也没有
    // 新包可读了。跳过 flush/复位这条变异因此观察不到差异：两条路径下
    // 音频解码器都不会再有新包可送，"复位没复位"在音频包已经天然耗尽
    // 的前提下是同一个结果（reached_eof=1, frames_after=0，两边一致，
    // 已用变异体实测验证过）。已经尝试过的其它构造（分片 mp4 索引未完成、
    // 只清空关键帧表保留采样表）都会被 libavformat 自身更深一层的兜底
    // 悄悄救回成功，反而让 av_seek_frame() 不失败。这条用例仍然值得留着
    // ——它确实钉住了"seek() 的返回值如实反映 demuxer_->seek() 的失败"、
    // "队列不会被污染"、"不会卡死/不会误判 Error"这三件事，只是对这一个
    // 特定变异（跳过 flush + 复位）没有区分力，如实记录在此，不假装
    // 测到了。
    std::vector<uint8_t> corrupted = read_file_bytes(fixture("faststart.mp4"));
    REQUIRE(!corrupted.empty());
    REQUIRE(zero_out_video_track_index(&corrupted));

    const TempCacheDir cache_guard("pipeline_seek_failure_scratch");
    const std::string  path = cache_guard.path + "/zero-video-index.mp4";
    REQUIRE(write_file_bytes(path, corrupted));

    syp_status err = SYP_OK;
    auto       p   = Pipeline::create_file(path, PipelineConfig{}, &err);
    // 视频轨解码器打不开/打得开但从头到尾收不到一个包，都只是"该轨终止
    // 或空转"，不是整体创建失败（pipeline.cpp 里 open_err 只影响单轨的
    // ts.failed）——这份被动过手的素材必须仍然能建出一个 Pipeline，否则
    // 后面全部断言都无的放矢。
    REQUIRE(p != nullptr);

    // 先跑一段、故意不 pop——让队列（至少音频轨这条）真的攒着东西，
    // 这样"seek 后队列已清"这条断言才不是"从没跑过所以本来就是空"的
    // 假绿（同 pipeline_seek_flushes_everything 的手法）。
    bool saw_frame_before_seek = false;
    for (int i = 0; i < 200; ++i) {
        auto o = p->step();
        if (o.kind == StepOutcome::Kind::DecodedFrame) saw_frame_before_seek = true;
        if (o.kind == StepOutcome::Kind::Eof || o.kind == StepOutcome::Kind::Error) break;
        // 故意不调用 pop_frame：帧留在 FrameQueue 里。
    }
    REQUIRE(saw_frame_before_seek);

    // 核心前提断言：这次 seek() 真的失败了——不然这条用例在验证一个从未
    // 发生过的场景，后面的检查全部立不住。视频轨从头到尾没有任何采样，
    // av_find_default_stream_index() 在 codecpar 宽高仍然可读的情况下
    // 仍会把它判成默认轨（打分见 avformat.c），seek(-1, ...) 落在这条
    // 空索引轨上失败。
    const syp_status seek_rc = p->seek(2000000);
    std::printf("  [seek-fail] seek_rc=%d (SYP_OK=%d)\n", static_cast<int>(seek_rc),
                static_cast<int>(SYP_OK));
    REQUIRE(seek_rc != SYP_OK);

    // 第 3、4 步的契约，即便第 2 步失败：
    //   1) 队列已清——旧帧不能还在 FrameQueue 里等着被取出。
    for (const TrackInfo& t : p->tracks()) {
        CHECK(!p->pop_frame(t.index).has_value());
    }
    //   2) 解码器已 flush、EOF 相关标志已复位——继续 step() 必须干净地
    //      推进到 Eof（不卡死在 Blocked，不误判 Error），且不能吐出
    //      "跳过 flush 才会残留"的陈旧帧。
    //
    //      这份素材的失败机制本身有个副作用要交代清楚（详见本文件顶部
    //      zero_out_video_track_index() 长注释）：mov 对索引查找失败的默认轨
    //      的兜底是 seek_frame_generic()——它会线性扫描整个剩余文件找一个
    //      "属于这条轨、dts 大于目标"的包；视频轨永远不会有这种包，扫描
    //      因此会一路吃到文件真正的尾部才放弃、返回失败，副作用是这次
    //      失败的 seek() 已经把底层读位置顶到了文件末尾——这不是
    //      Pipeline 能控制或者需要修的东西，是 libavformat 对"index-based
    //      seek 失败"的通用兜底策略，音频轨此后确实不会再有新包可读。
    //      断言因此不是"还能解出新帧"（那件事在这份素材上已经不可能），
    //      是"不会卡死、不会误判 Error，能干净地推进到 Eof"——且如果
    //      忘记 flush 解码器，音频轨在被送入这次的 flush 信号之前，
    //      receive() 有机会先吐出一个属于 seek 之前那批已 send 但还没
    //      receive 的陈旧帧，跟着 pop 出来污染 frames_after；忘记复位
    //      EOF 标志在这条路径上刚好不会造成卡死（下面用一个远小于
    //      "卡死"量级的 Blocked 上限顶住，万一以后素材/FFmpeg 版本变了
    //      导致这条路径退化成卡死，也能第一时间抓到，不必等 ctest
    //      TIMEOUT）。
    constexpr int kMaxConsecutiveBlocked = 2000;
    int           consecutive_blocked    = 0;
    bool          reached_eof            = false;
    int64_t       frames_after           = 0;
    for (int i = 0; i < 1000000; ++i) {
        auto o = p->step();
        if (o.kind == StepOutcome::Kind::DecodedFrame) {
            consecutive_blocked = 0;
            auto f              = p->pop_frame(o.track_index);
            REQUIRE(f.has_value());
            ++frames_after;
        } else if (o.kind == StepOutcome::Kind::DemuxedPacket) {
            consecutive_blocked = 0;
        } else if (o.kind == StepOutcome::Kind::Blocked) {
            ++consecutive_blocked;
            REQUIRE(consecutive_blocked <= kMaxConsecutiveBlocked);
        } else if (o.kind == StepOutcome::Kind::Eof) {
            reached_eof = true;
            break;
        }
        REQUIRE(o.kind != StepOutcome::Kind::Error);
    }
    std::printf("  [seek-fail] reached_eof=%d frames_after=%lld\n",
                static_cast<int>(reached_eof), static_cast<long long>(frames_after));
    CHECK(reached_eof);
    CHECK_EQ(frames_after, int64_t{0});
}

TEST_CASE(pipeline_create_file_rejects_invalid_config) {
    // 条件 2：config_is_valid()
    // ——配了 12 行注释和实测数据
    // （max_frames_per_track=0 时 5000 步内 blocked=4999、且没有任何
    // 错误返回值提示调用方配置本身是坏的），但此前没有一条用例真的
    // 调用过一次非法配置：变异（函数体整体换成 `return true;`）
    // 16/16 全绿地漏过去了。三个字段各来一条，一次只坏一个字段。
    const std::string path = fixture("faststart.mp4");

    {
        PipelineConfig cfg;
        cfg.max_packets_per_track = 0;
        syp_status err            = SYP_OK;
        auto       p              = Pipeline::create_file(path, cfg, &err);
        CHECK(p == nullptr);
        CHECK_EQ(err, SYP_ERR_INVALID_ARG);
    }
    {
        PipelineConfig cfg;
        cfg.max_bytes_per_track = 0;
        syp_status err          = SYP_OK;
        auto       p            = Pipeline::create_file(path, cfg, &err);
        CHECK(p == nullptr);
        CHECK_EQ(err, SYP_ERR_INVALID_ARG);
    }
    {
        PipelineConfig cfg;
        cfg.max_frames_per_track = 0;
        syp_status err           = SYP_OK;
        auto       p             = Pipeline::create_file(path, cfg, &err);
        CHECK(p == nullptr);
        CHECK_EQ(err, SYP_ERR_INVALID_ARG);
    }
    // 判别力自检：确认一个正常配置在同一份素材上确实能建成功——不然上面
    // 三条 `p == nullptr` 可能只是巧合地测到了"这份素材本来就打不开"这种
    // 退化场景，跟 config_is_valid() 毫无关系。
    {
        syp_status err = SYP_OK;
        auto       p   = Pipeline::create_file(path, PipelineConfig{}, &err);
        REQUIRE(p != nullptr);
        CHECK_EQ(err, SYP_OK);
    }
}

// I-1 的真正回归保护：`demuxer_->seek()` 失败时仍然要跑完第 3 步
// （flush 解码器）与第 4 步（复位 EOF 标志）。
//
// 为什么需要第二条用例——上面那条
// pipeline_seek_failure_still_completes_steps_3_and_4 对
// 「在 demuxer_->seek() 之后提前 `return rc;`」这个变异**没有区分力**
// （它自己的注释里如实记着这件事）：
// 那条用例的素材靠"清空视频轨采样索引"制造失败，而
// libavformat 对空索引轨的兜底是 seek_frame_generic() 的线性扫描，
// 它会把剩余码流整个吃光——于是"已 flush"与"没 flush"在那份素材上
// 产生逐字节相同的可观测行为。
//
// 这条用例走的是另一条失败路径，副作用完全不同（源码走查 + 探针实测，
// FFmpeg 8.1.2）：
//
//   1. 素材：把视频轨的 stss 改成"唯一一条 sync sample 记录指向一个
//      不存在的样本号"（blank_out_video_keyframe_table）。注意不能把
//      entry_count 清零——mov_read_stss() 对空 stss 会置 keyframe_absent，
//      mov_build_index() 反而把所有样本都标成关键帧（这正是此前
//      试过并被证伪的构造）。改写之后整条视频轨的 AVIndexEntry 一个
//      AVINDEX_KEYFRAME 都没有，其余（stsz/stts/stco/stsc/mdat）全部
//      完好，两条轨都照常出包、照常解码。
//   2. seek(3s)：av_find_default_stream_index() 仍然选中视频轨
//      （宽高已知，得分高于音频轨），ff_index_search_timestamp() 在
//      AVSEEK_FLAG_BACKWARD 下要往回找一条 AVINDEX_KEYFRAME，一条都
//      没有 ⇒ 返回 -1；目标时间戳又不小于 index_entries[0].timestamp，
//      走不到 mov_seek_stream() 里"钳到样本 0"那条兜底 ⇒
//      mov_read_seek() 返回 AVERROR_INVALIDDATA。
//   3. 落到 seek_frame_generic()：index<0 且 nb_index_entries>0 ⇒ 进
//      `if (sti->nb_index_entries)` 那支，第一件事就是
//      `avio_seek(s->pb, ie->pos, SEEK_SET)`（ie 是最后一条索引项，
//      在文件尾附近，是一次远超缓冲区的前跳，必然真的去调
//      AVIOContext::seek）——本用例让这一次、且只让这一次回调返回
//      EIO，于是 seek_frame_generic() 在**线性扫描之前**就 return，
//      整个 av_seek_frame() 失败而底层读位置一个字节都没动。
//      探针实测：seek 期间 seek 回调被调用 1 次，失败后 src.pos 不变，
//      随后仍能读出 6098 个包。
//
//   ⇒ 于是「seek 失败」和「码流还活着」第一次同时成立，第 3 步
//     （flush）有了可观测后果：解码器被 flush 之后丢掉了当前 GOP 的
//     全部参考帧，H.264 解码器在遇到下一个 IDR 之前不再输出任何帧。
//
// 两个断言分别钉住两个时刻（实测数值见断言旁注释）：
//   A) GOP 开头之后不久 seek 失败 ⇒ 视频出现一次整 GOP 的 pts 断层（跳过第 3 步
//      时断层恰好等于一个帧间隔，解码器毫发无损）。
//   B) 最后一个 packet 已经 demux 完、解码器正在 drain 的时刻 seek
//      失败 ⇒ 解码器里压着的重排序帧必须被 flush 掉，一帧都不能再吐。
//
// 覆盖边界（不回避）：这两条断言钉住的都是**第 3 步**（flush）。第 4 步
// （复位 EOF 标志）在失败路径上仍然没有独立的回归保护——本轮试过一条
// 构造（MemSource 先截断到 4MB 让管线跑到 Eof，再掀掉截断 + 做一次失败
// 的 seek，指望"复位了 demux_eof_ 才会重新去问 demuxer"变成可观测的
// 帧数差），实测不成立：AVIOContext::eof_reached 是粘性的，只有**成功**
// 的 avio_seek 才会清零（aviobuf.c 里 avio_seek 末尾那句
// `s->eof_reached = 0;` 在每条失败分支之后），失败的 seek 之后底层源
// 再也读不出东西，正确实现与变异体同样都是 0 帧。那个变异
// （提前 return）会同时跳过第 3、4 步，被 A/B 任何一条抓住就够；但如果
// 有人只删掉第 4 步那三行，本用例仍然是绿的——那一半靠
// pipeline_seek_flushes_everything 的"成功 seek"路径守着。
TEST_CASE(pipeline_failed_seek_still_flushes_decoders) {
    // 运行期守护：这份素材真的带 B 帧（断言 B 依赖"drain 阶段确实还有
    // 帧压在解码器里"这个前提；零 B 帧素材上 drain 帧数恒为 0，断言 B
    // 会退化成恒真）。
    REQUIRE(material_has_b_frames(fixture("bframes_faststart.mp4")));

    std::vector<uint8_t> data = read_file_bytes(fixture("bframes_faststart.mp4"));
    REQUIRE(!data.empty());
    REQUIRE(blank_out_video_keyframe_table(&data));

    // ---- A) 流中段：seek 失败，但解码器必须已经被 flush ----
    // 在第 2 帧视频之后 seek：这一刻刚过 GOP 开头的 IDR，离下一个 IDR
    // 最远（gen-fixtures.sh 的 GOP 是 1~4 秒，随 seed 变）——flush 生效
    // 时丢掉的正好是整整一个 GOP，跟"没 flush"的一个帧间隔差着一个数量级。
    // seek 之后只再跑 6000 步就收工：这一半的断言只看"断层"和"流还活着"，
    // 不需要把剩下的流也解完。
    const SeekFailRun mid = run_to_eof(data, -1, 3000000, 6000, -1, 2);
    REQUIRE(mid.created);
    // A0：这次 seek 真的失败了（否则下面测的是"成功 seek"，另一回事）。
    REQUIRE(mid.seek_rc != SYP_OK);
    // A1：失败没有把底层码流吃掉——失败之后管线还能继续正常出帧。
    //     （这一条同时把这份素材跟上一条用例那份"失败即耗尽整条流"的
    //     素材区分开：那份素材上 video_after_seek 恒为 0。）
    REQUIRE(!mid.saw_error);
    REQUIRE(mid.video_after_seek > 100);
    REQUIRE(mid.last_video_pts_before_seek >= 0);
    REQUIRE(mid.first_video_pts_after_seek >= 0);

    // A2：解码器确实被 flush 了 ⇒ 当前 GOP 剩余帧因为参考帧被丢弃而
    //     解不出来（H.264 解码器在 avcodec_flush_buffers 之后要等到下一个
    //     IDR 才恢复输出），视频 pts 出现一次整 GOP 的断层。
    //       跑完第 3 步（正确实现）  断层 = 一整个 GOP（≥1s，随 seed 变）
    //       跳过第 3 步（变异）      断层 = 一个帧间隔（fps 24/25/30 ⇒
    //                                33~42ms，解码器毫发无损）
    //     门槛 500ms：高于任何单帧间隔（最慢 24fps ⇒ 41.7ms）一个数量级，
    //     又低于 gen-fixtures.sh 里最短的 GOP（1 秒）。
    //     跨 seed 实测（三份独立生成的素材，覆盖 GOP 2s/1s/1s、
    //     fps 25/30/25、640x360 与 1280x720）：
    //       正确实现 gap = 1960000 / 966667 / 960000 us —— 全部 > 500000
    //       变异     gap =   40000 /  33334 /  40000 us —— 全部变红
    //     最坏一档仍有 1.9 倍余量，另一侧有 12 倍余量。
    const int64_t gap = mid.first_video_pts_after_seek - mid.last_video_pts_before_seek;
    std::printf("  [failed-seek] mid: rc=%d video_after=%lld pts %lld -> %lld gap=%lldus\n",
                static_cast<int>(mid.seek_rc), static_cast<long long>(mid.video_after_seek),
                static_cast<long long>(mid.last_video_pts_before_seek),
                static_cast<long long>(mid.first_video_pts_after_seek),
                static_cast<long long>(gap));
    CHECK(gap > 500000);

    // ---- B) drain 时刻：解码器里压着的重排序帧必须被 flush 掉 ----
    // 在"最后一个 packet 已经 demux 完"之后的第一次 step 之前 seek。
    // 此刻两条轨的 PacketQueue 已经排空（drive_decoder 只有在包队列空了
    // 才会发 flush 信号），所以第 1 步的清队列在这里是空操作——唯一还能
    // 造成差异的就是第 3 步的 flush。
    //
    // 这一半跑在"源被截断到 4MB"的同一份素材上（MemSource::cutoff）：
    // demux 在整条流的 1/8 处就到底，drain 状态提前出现，两趟运行合计
    // 省掉约 3/4 的解码量。截断只影响可读数据量，不影响本用例关心的
    // 任何一件事——seek 失败路径（stss 无关键帧 ⇒ mov_read_seek 失败
    // ⇒ seek_frame_generic 的 avio_seek 被我们打回）逐字不变。
    constexpr int64_t kCutoff = 4 << 20;
    const SeekFailRun short_ref = run_to_eof(data, -1, 0, -1, kCutoff);
    REQUIRE(short_ref.created);
    REQUIRE(!short_ref.saw_error);
    REQUIRE(short_ref.reached_eof);
    REQUIRE(short_ref.video_frames > 0);
    REQUIRE(short_ref.last_demux_step > 0);
    const SeekFailRun drain = run_to_eof(data, short_ref.last_demux_step + 1, 3000000, -1, kCutoff);
    REQUIRE(drain.created);
    REQUIRE(drain.seek_rc != SYP_OK);
    REQUIRE(drain.reached_eof);
    REQUIRE(!drain.saw_error);
    std::printf("  [failed-seek] short_ref: steps=%lld video=%lld last_demux_step=%lld\n",
                static_cast<long long>(short_ref.steps),
                static_cast<long long>(short_ref.video_frames),
                static_cast<long long>(short_ref.last_demux_step));
    std::printf("  [failed-seek] drain: rc=%d video_after=%lld audio_after=%lld\n",
                static_cast<int>(drain.seek_rc),
                static_cast<long long>(drain.video_after_seek),
                static_cast<long long>(drain.audio_after_seek));
    // 实测（同样三份素材）：跑完第 3 步 ⇒ 0/0/0；跳过第 3 步 ⇒ 2/2/2
    // （= 解码器重排序延迟，material_has_b_frames() 保证它 > 0）。
    CHECK_EQ(drain.video_after_seek, int64_t{0});
    CHECK_EQ(drain.audio_after_seek, int64_t{0});
}

// ---------------------------------------------------------------------
// request_abort() 在 create_file()/create_avio() 上也要生效
// ---------------------------------------------------------------------

// 中止之后 step() 立刻报 Error(SYP_ERR_CANCELED)，不再往下解——三条打开
// 路径同一个契约。修复前 create_file 上 request_abort() 是 no-op，管线
// 照常出包出帧。
TEST_CASE(pipeline_request_abort_on_create_file_stops_step_with_canceled) {
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), PipelineConfig{}, &err);
    REQUIRE(p != nullptr);
    for (int i = 0; i < 20; ++i) {
        const StepOutcome o = p->step();
        REQUIRE(o.kind != StepOutcome::Kind::Error);
        if (o.kind == StepOutcome::Kind::DecodedFrame) (void)p->pop_frame(o.track_index);
    }
    p->request_abort();
    for (int i = 0; i < 3; ++i) {
        const StepOutcome o = p->step();
        CHECK(o.kind == StepOutcome::Kind::Error);
        CHECK_EQ(o.status, SYP_ERR_CANCELED);
    }
}

namespace {

// 读到 block_ 置位之后就在 cv 上无限期等待，直到 io_abort 钩子把 aborted_
// 置位——模拟"网络卡死的 AVIOContext"。这正是 demo 壳 URL 播放
// （AvioBridge → syp_source_read）在网络挂住时的形状。
struct BlockingMemSource {
    const std::vector<uint8_t>* buf = nullptr;
    int64_t                     pos = 0;
    std::mutex                  mu;
    std::condition_variable     cv;
    std::atomic<bool>           block{false};
    std::atomic<bool>           in_block{false};
    bool                        aborted = false;   // mu 保护
};

int blocking_read(void* opaque, uint8_t* out, int n) {
    auto* s = static_cast<BlockingMemSource*>(opaque);
    if (s->block.load()) {
        std::unique_lock<std::mutex> lk(s->mu);
        s->in_block.store(true);
        s->cv.wait(lk, [s] { return s->aborted; });
        return AVERROR_EXIT;
    }
    const int64_t size = static_cast<int64_t>(s->buf->size());
    if (s->pos >= size) return AVERROR_EOF;
    const int m = static_cast<int>(size - s->pos < n ? size - s->pos : n);
    std::memcpy(out, s->buf->data() + s->pos, static_cast<std::size_t>(m));
    s->pos += m;
    return m;
}

int64_t blocking_seek(void* opaque, int64_t off, int whence) {
    auto* s = static_cast<BlockingMemSource*>(opaque);
    if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) return static_cast<int64_t>(s->buf->size());
    const int w = whence & ~AVSEEK_FORCE;
    const int64_t np = (w == SEEK_CUR) ? s->pos + off
                     : (w == SEEK_END) ? static_cast<int64_t>(s->buf->size()) + off
                                       : off;
    if (np < 0) return AVERROR(EINVAL);
    s->pos = np;
    return np;
}

}  // namespace

// create_avio 的阻塞点在**调用方的** AVIOContext 里，Pipeline 自己够不着，
// 只能由调用方交一个 io_abort 钩子进来，request_abort() 负责调它。
// 判据：卡在读里的那次 step() 被打断后报 Error(SYP_ERR_CANCELED)——不是
// SYP_ERR_IO（mov 解封装器把 AVERROR_EXIT 原样往上抛，Demuxer 走 Error
// 分支），也不是 Eof。
TEST_CASE(pipeline_request_abort_on_create_avio_unblocks_a_hanging_read) {
    syp::test::Watchdog wd("pipeline_request_abort_on_create_avio_unblocks_a_hanging_read",
                           10000, 30000, "卡住说明 request_abort() 没有调到 io_abort 钩子");
    const std::vector<uint8_t> data = read_file_bytes(fixture("faststart.mp4"));
    REQUIRE(!data.empty());
    BlockingMemSource src;
    src.buf = &data;
    const int kBuf = 4096;
    auto* iobuf = static_cast<unsigned char*>(av_malloc(static_cast<std::size_t>(kBuf)));
    AVIOContext* pb = avio_alloc_context(iobuf, kBuf, 0, &src, blocking_read, nullptr, blocking_seek);
    pb->seekable = AVIO_SEEKABLE_NORMAL;

    std::atomic<int> hook_calls{0};
    syp_status err = SYP_OK;
    auto p = Pipeline::create_avio(pb, PipelineConfig{}, &err, [&src, &hook_calls] {
        hook_calls.fetch_add(1);
        std::lock_guard<std::mutex> g(src.mu);
        src.aborted = true;
        src.cv.notify_all();
    });
    REQUIRE(p != nullptr);

    src.block.store(true);   // 从下一次缓冲区回填起卡死
    StepOutcome last{};
    std::thread stepper([&] {
        for (int i = 0; i < 1000000; ++i) {
            last = p->step();
            if (last.kind == StepOutcome::Kind::Error || last.kind == StepOutcome::Kind::Eof) break;
            for (const TrackInfo& t : p->tracks()) {
                while (p->pop_frame(t.index).has_value()) {}
            }
        }
    });
    for (int i = 0; i < 10000 && !src.in_block.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(src.in_block.load());   // 现场具备区分力：step() 真的卡在读里
    p->request_abort();
    stepper.join();

    CHECK_EQ(hook_calls.load(), 1);
    CHECK(last.kind == StepOutcome::Kind::Error);
    CHECK_EQ(last.status, SYP_ERR_CANCELED);
    CHECK(!wd.fired());

    p.reset();
    av_free(pb->buffer);
    avio_context_free(&pb);
}

// ---- 解码方式配置 ----

TEST_CASE(pipeline_hardware_mode_with_unsupported_backend_fails_create) {
    syp::test::FakeHwBackend be;
    be.supports_result = false;
    PipelineConfig cfg;
    cfg.video_decode = VideoDecodeMode::Hardware;
    cfg.hw_backend   = &be;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), cfg, &err);
    CHECK(p == nullptr);
    CHECK_EQ(err, SYP_ERR_NOT_IMPLEMENTED);
}

TEST_CASE(pipeline_hardware_mode_without_backend_fails_create) {
    PipelineConfig cfg;
    cfg.video_decode = VideoDecodeMode::Hardware;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), cfg, &err);
    CHECK(p == nullptr);
    CHECK_EQ(err, SYP_ERR_NOT_IMPLEMENTED);
}

TEST_CASE(pipeline_software_mode_reports_no_hardware_decoding) {
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), PipelineConfig{}, &err);
    REQUIRE(p != nullptr);
    CHECK(!p->video_hardware_decoding());
}

// 假后端 prepare 成功但不真挂设备：open 成功、实例处于硬解模式，足以验证查询接线。
// （真实出帧由 VideoToolbox 集成测负责。）
TEST_CASE(pipeline_hardware_mode_with_supporting_backend_reports_hardware) {
    syp::test::FakeHwBackend be;
    PipelineConfig cfg;
    cfg.video_decode = VideoDecodeMode::Hardware;
    cfg.hw_backend   = &be;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), cfg, &err);
    REQUIRE(p != nullptr);
    CHECK(p->video_hardware_decoding());
    CHECK_EQ(be.prepare_calls, 1);
}

// 封面图（attached_pic，png）恒软解：硬解模式 + 不支持的后端下，只有音频 + 封面图的
// 文件仍然能打开——封面图不是视频流，不参与"硬解不支持就整体失败"。
TEST_CASE(pipeline_hardware_mode_keeps_cover_art_on_software) {
    syp::test::TempDir tmp;
    const std::string path = syp::test::synth_audio_with_cover_art(tmp.path);
    REQUIRE(!path.empty());
    syp::test::FakeHwBackend be;
    be.supports_result = false;
    PipelineConfig cfg;
    cfg.video_decode = VideoDecodeMode::Hardware;
    cfg.hw_backend   = &be;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(path, cfg, &err);
    CHECK(p != nullptr);
    CHECK_EQ(err, SYP_OK);
    CHECK_EQ(be.supports_calls, 0);   // 封面图轨根本没问过硬件后端
}

// 硬解模式 + hwaccel 初始化失败（假后端不挂设备）跑到终态——
// 视频轨不得弹出任何非 VIDEOTOOLBOX 帧，且必须 track_failed()，不许"干净 Eof、
// 只剩音频"的静默退化。
namespace {
void run_hw_mode_without_device(const char* name) {
    syp::test::FakeHwBackend be;
    PipelineConfig cfg;
    cfg.video_decode = VideoDecodeMode::Hardware;
    cfg.hw_backend   = &be;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture(name), cfg, &err);
    REQUIRE(p != nullptr);
    int32_t vidx = -1;
    for (const TrackInfo& t : p->tracks()) {
        if (t.is_video && !t.attached_pic) vidx = t.index;
    }
    REQUIRE(vidx >= 0);
    int64_t non_hw = 0;
    bool terminal = false;
    for (int i = 0; i < 2000000 && !terminal; ++i) {
        const StepOutcome o = p->step();
        if (o.kind == StepOutcome::Kind::Eof || o.kind == StepOutcome::Kind::Error) terminal = true;
        for (const TrackInfo& t : p->tracks()) {
            while (auto f = p->pop_frame(t.index)) {
                if (t.index == vidx && f->pix_fmt() != static_cast<int32_t>(AV_PIX_FMT_VIDEOTOOLBOX)) {
                    ++non_hw;
                }
            }
        }
    }
    CHECK(terminal);
    CHECK_EQ(non_hw, int64_t{0});
    CHECK(p->track_failed(vidx));
}
}  // namespace

TEST_CASE(pipeline_hardware_mode_hwaccel_init_failure_h264_fails_video_track) {
    run_hw_mode_without_device("faststart.mp4");
}

TEST_CASE(pipeline_hardware_mode_hwaccel_init_failure_hevc_fails_video_track) {
    run_hw_mode_without_device("hevc.mp4");
}

// 硬解模式下视频轨 open 的任何失败（不只 NOT_IMPLEMENTED）都让
// create 整体失败并保留原错误码——否则 avcodec_open2 失败会退化成"视频轨终止、
// 只剩音频"。触发 avcodec_open2 失败：把 hevc.mp4 的 hvcC 里第一个 NAL 的长度
// 字段改成 0xFFFF，hevc_decode_init → ff_hevc_decode_extradata 报
// "Invalid NAL unit size in extradata" 并返回 AVERROR_INVALIDDATA（H.264 不行：
// h264_decode_init 在非 AV_EF_EXPLODE 下吞掉 extradata 错误）。
namespace {
bool corrupt_hvcc_first_nal_size(std::vector<uint8_t>* data) {
    static const uint8_t tag[4] = {'h', 'v', 'c', 'C'};
    for (std::size_t i = 0; i + 4 + 28 <= data->size(); ++i) {
        if (std::memcmp(data->data() + i, tag, 4) != 0) continue;
        uint8_t* c = data->data() + i + 4;   // hvcC 负载
        // 22 字节定长头 + numOfArrays(1)，第一个数组：type(1) numNalus(2) nalUnitLength(2)
        if (c[0] != 1 || c[22] == 0) return false;
        c[26] = 0xFF;
        c[27] = 0xFF;
        return true;
    }
    return false;
}

std::unique_ptr<Pipeline> create_from_bytes(const std::vector<uint8_t>& data, MemSource* src,
                                            AVIOContext** pb_out, const PipelineConfig& cfg,
                                            syp_status* err) {
    const int      kBuf  = 32768;
    unsigned char* iobuf = static_cast<unsigned char*>(av_malloc(static_cast<std::size_t>(kBuf)));
    src->buf = &data;
    AVIOContext* pb = avio_alloc_context(iobuf, kBuf, 0, src, mem_read, nullptr, mem_seek);
    pb->seekable = AVIO_SEEKABLE_NORMAL;
    *pb_out = pb;
    return Pipeline::create_avio(pb, cfg, err);
}

void free_pb(AVIOContext** pb) {
    av_free((*pb)->buffer);
    avio_context_free(pb);
}
}  // namespace

TEST_CASE(pipeline_hardware_mode_any_video_open_failure_fails_create) {
    std::vector<uint8_t> data = read_file_bytes(fixture("hevc.mp4"));
    REQUIRE(!data.empty());
    REQUIRE(corrupt_hvcc_first_nal_size(&data));

    // 前提（区分力）：软解模式下同一份字节确实让视频轨 open 失败、而 create 成功。
    {
        MemSource    src;
        AVIOContext* pb  = nullptr;
        syp_status   err = SYP_OK;
        auto p = create_from_bytes(data, &src, &pb, PipelineConfig{}, &err);
        REQUIRE(p != nullptr);
        int32_t vidx = -1;
        for (const TrackInfo& t : p->tracks()) {
            if (t.is_video) vidx = t.index;
        }
        REQUIRE(vidx >= 0);
        CHECK(p->track_failed(vidx));
        p.reset();
        free_pb(&pb);
    }

    syp::test::FakeHwBackend be;
    PipelineConfig cfg;
    cfg.video_decode = VideoDecodeMode::Hardware;
    cfg.hw_backend   = &be;
    MemSource    src;
    AVIOContext* pb  = nullptr;
    syp_status   err = SYP_OK;
    auto p = create_from_bytes(data, &src, &pb, cfg, &err);
    CHECK(p == nullptr);
    CHECK_EQ(err, SYP_ERR_IO);   // 保留 open() 的原错误码（avcodec_open2 失败 → IO）
    CHECK_EQ(be.prepare_calls, 1);
    p.reset();
    free_pb(&pb);
}

// track_drained——解到底且队列排空才为真；越界/非托管为假。
TEST_CASE(pipeline_track_drained_true_only_after_track_fully_consumed) {
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), PipelineConfig{}, &err);
    REQUIRE(p != nullptr);
    int32_t audio = -1;
    for (const TrackInfo& t : p->tracks()) if (!t.is_video) audio = t.index;
    REQUIRE(audio >= 0);
    CHECK(!p->track_drained(audio));
    CHECK(!p->track_drained(-1));
    CHECK(!p->track_drained(9999));
    bool eof = false;
    for (int i = 0; i < 2000000 && !eof; ++i) {
        const StepOutcome o = p->step();
        if (o.kind == StepOutcome::Kind::Eof) eof = true;
        if (o.kind == StepOutcome::Kind::Error) break;
        for (const TrackInfo& t : p->tracks()) while (p->pop_frame(t.index)) {}
    }
    REQUIRE(eof);
    CHECK(p->track_drained(audio));
}

TEST_CASE(pipeline_video_catchup_toggles_and_is_queryable) {
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("bframes.mp4"), PipelineConfig{}, &err);
    REQUIRE(p != nullptr);
    int32_t vidx = -1;
    for (const TrackInfo& t : p->tracks()) if (t.is_video) vidx = t.index;
    REQUIRE(vidx >= 0);
    CHECK(!p->video_catchup());
    CHECK(!p->track_skip_nonref(vidx));
    p->set_video_catchup(true);
    CHECK(p->video_catchup());
    CHECK(p->track_skip_nonref(vidx));
    p->set_video_catchup(false);
    CHECK(!p->video_catchup());
    CHECK(!p->track_skip_nonref(vidx));
}

// set_video_catchup 的"非封面"排除——封面图轨（attached_pic）不该被
// set_skip_nonref 影响，即使整条管线的 video_catchup() 已经打开。这条规则
// 只在 set_video_catchup() 的实现里写了一次（!ts.cover_art），此前没有测试
// 覆盖，回归会静默发生。
TEST_CASE(pipeline_video_catchup_skips_cover_art_track) {
    syp::test::TempDir tmp;
    const std::string path = syp::test::synth_audio_with_cover_art(tmp.path);
    REQUIRE(!path.empty());
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(path, PipelineConfig{}, &err);
    REQUIRE(p != nullptr);
    int32_t cover_idx = -1;
    for (const TrackInfo& t : p->tracks()) if (t.is_video && t.attached_pic) cover_idx = t.index;
    REQUIRE(cover_idx >= 0);
    p->set_video_catchup(true);
    CHECK(p->video_catchup());
    CHECK(!p->track_skip_nonref(cover_idx));
}

// ---- 缓冲水位观测（同步模式）----

// 起始无数据；不取帧一路 step 到 Blocked（帧队列先满，随后包队列读满）→ full、水位为正；
// seek 清空后复位。
TEST_CASE(pipeline_buffer_stats_sync_mode_tracks_queues_and_resets_on_seek) {
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), PipelineConfig{}, &err);
    REQUIRE(p != nullptr);
    const BufferStats s0 = p->buffer_stats();
    CHECK_EQ(s0.buffered_until_us, AV_NOPTS_VALUE);
    CHECK(!s0.demux_eof);
    CHECK(!s0.loading);
    CHECK(!s0.full);
    CHECK_EQ(s0.queued_bytes, int64_t{0});

    bool blocked = false;
    for (int i = 0; i < 100000 && !blocked; ++i) {
        const StepOutcome o = p->step();
        REQUIRE(o.kind != StepOutcome::Kind::Error);
        if (o.kind == StepOutcome::Kind::Blocked) blocked = true;
    }
    REQUIRE(blocked);
    const BufferStats s1 = p->buffer_stats();
    CHECK(s1.full);
    CHECK(s1.buffered_until_us > 0);
    CHECK(s1.queued_bytes > 0);
    CHECK(!s1.demux_eof);
    CHECK(!s1.loading);

    REQUIRE(p->seek(0) == SYP_OK);
    const BufferStats s2 = p->buffer_stats();
    CHECK_EQ(s2.buffered_until_us, AV_NOPTS_VALUE);
    CHECK_EQ(s2.queued_bytes, int64_t{0});
    CHECK(!s2.full);
}

// 读完且各轨排空到 decoder_eof：没有在播轨了，水位无上限。
TEST_CASE(pipeline_buffer_stats_unbounded_after_all_tracks_drained) {
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("hls/vod_single/source.mp4"), PipelineConfig{}, &err);
    REQUIRE(p != nullptr);
    bool eof = false;
    for (int i = 0; i < 2000000 && !eof; ++i) {
        const StepOutcome o = p->step();
        REQUIRE(o.kind != StepOutcome::Kind::Error);
        if (o.kind == StepOutcome::Kind::Eof) eof = true;
        for (const TrackInfo& t : p->tracks()) while (p->pop_frame(t.index)) {}
    }
    REQUIRE(eof);
    const BufferStats s = p->buffer_stats();
    CHECK(s.demux_eof);
    CHECK_EQ(s.buffered_until_us, kBufferedUntilUnbounded);
}

// 测试缝：覆写之后 buffer_stats() 原样返回覆写值；config() 如实回报创建参数。
TEST_CASE(pipeline_debug_override_buffer_stats_and_config_accessor) {
    PipelineConfig cfg;
    cfg.max_frames_per_track = 5;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("hls/vod_single/source.mp4"), cfg, &err);
    REQUIRE(p != nullptr);
    CHECK_EQ(p->config().max_frames_per_track, std::size_t{5});
    BufferStats fake;
    fake.buffered_until_us = 1234;
    fake.demux_eof         = true;
    fake.loading           = true;
    fake.queued_bytes      = 7;
    fake.full              = true;
    p->debug_override_buffer_stats([fake] { return fake; });
    const BufferStats s = p->buffer_stats();
    CHECK_EQ(s.buffered_until_us, int64_t{1234});
    CHECK(s.demux_eof);
    CHECK(s.loading);
    CHECK_EQ(s.queued_bytes, int64_t{7});
    CHECK(s.full);
}

// ---- 线程模式（加载线程）----

namespace {

// 线程模式驱动器：每步排空所有轨、按轨记 pts。Blocked 在线程模式下只表示"加载
// 线程这一刻还没送到"，让出 200 微秒再试；同步模式下同样可用（几乎不会 Blocked）。
struct DriveRun {
    std::map<int32_t, std::vector<int64_t>> pts_by_track;
    StepOutcome                             last{};
};

DriveRun drive_to_end(Pipeline& p, int64_t max_steps = 20000000) {
    DriveRun r;
    for (int64_t i = 0; i < max_steps; ++i) {
        r.last = p.step();
        for (const TrackInfo& t : p.tracks()) {
            while (auto f = p.pop_frame(t.index)) r.pts_by_track[t.index].push_back(f->pts_us());
        }
        if (r.last.kind == StepOutcome::Kind::Eof || r.last.kind == StepOutcome::Kind::Error) break;
        if (r.last.kind == StepOutcome::Kind::Blocked) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    return r;
}

template <typename Pred>
bool wait_until(Pred pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// 【慢速 AVIO 桩】可控进度的内存源：
//   · 读到偏移 >= limit 的数据之前，读回调在 cv 上阻塞，直到 gated_release() 抬高
//     limit，或 io_abort 钩子（gated_abort）置 aborted——后者让读返回 AVERROR_EXIT；
//   · fail_at >= 0 时，读到这个偏移起一律返回 AVERROR(EIO)（模拟网络重试耗尽）；
//   · in_block 在读回调真正卡住时为真，用例据此确认"现场具备区分力"；
//   · seek_fail 为真时 seek 回调（AVSEEK_SIZE 除外）一律返回 AVERROR(EIO)，并计入
//     failed_seeks——配合 blank_out_video_keyframe_table() 让 av_seek_frame() 真的失败。
// 全部状态由 mu 保护：读/seek 回调先在调用方线程（create_avio 探测）、之后在
// 加载线程上跑，用例线程改 limit/aborted。
struct GatedMemSource {
    const std::vector<uint8_t>* buf = nullptr;
    std::mutex                  mu;
    std::condition_variable     cv;
    int64_t                     pos     = 0;
    int64_t                     limit   = std::numeric_limits<int64_t>::max();
    int64_t                     fail_at = -1;
    bool                        aborted = false;
    bool                        seek_fail = false;
    std::atomic<bool>           in_block{false};
    std::atomic<int>            hook_calls{0};
    std::atomic<int>            failed_seeks{0};
};

int gated_read(void* opaque, uint8_t* out, int n) {
    auto* s = static_cast<GatedMemSource*>(opaque);
    std::unique_lock<std::mutex> lk(s->mu);
    if (s->fail_at >= 0 && s->pos >= s->fail_at) return AVERROR(EIO);
    while (!s->aborted && s->pos >= s->limit) {
        s->in_block.store(true);
        s->cv.wait(lk);
    }
    s->in_block.store(false);
    if (s->aborted) return AVERROR_EXIT;
    const int64_t size = static_cast<int64_t>(s->buf->size());
    if (s->pos >= size) return AVERROR_EOF;
    int64_t end = std::min(size, s->limit);
    if (s->fail_at >= 0) end = std::min(end, s->fail_at);
    const int m = static_cast<int>(std::min<int64_t>(end - s->pos, n));
    std::memcpy(out, s->buf->data() + s->pos, static_cast<std::size_t>(m));
    s->pos += m;
    return m;
}

int64_t gated_seek(void* opaque, int64_t off, int whence) {
    auto* s = static_cast<GatedMemSource*>(opaque);
    std::lock_guard<std::mutex> g(s->mu);
    if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) return static_cast<int64_t>(s->buf->size());
    if (s->seek_fail) {
        s->failed_seeks.fetch_add(1);
        return AVERROR(EIO);
    }
    const int     w  = whence & ~AVSEEK_FORCE;
    const int64_t np = (w == SEEK_CUR) ? s->pos + off
                     : (w == SEEK_END) ? static_cast<int64_t>(s->buf->size()) + off
                                       : off;
    if (np < 0) return AVERROR(EINVAL);
    s->pos = np;
    return np;
}

AVIOContext* make_gated_avio(GatedMemSource* src) {
    const int kBuf = 4096;
    auto* iobuf = static_cast<unsigned char*>(av_malloc(static_cast<std::size_t>(kBuf)));
    AVIOContext* pb = avio_alloc_context(iobuf, kBuf, 0, src, gated_read, nullptr, gated_seek);
    pb->seekable = AVIO_SEEKABLE_NORMAL;
    return pb;
}

void gated_release(GatedMemSource* s) {
    std::lock_guard<std::mutex> g(s->mu);
    s->limit = std::numeric_limits<int64_t>::max();
    s->cv.notify_all();
}

void gated_set_seek_fail(GatedMemSource* s, bool on) {
    std::lock_guard<std::mutex> g(s->mu);
    s->seek_fail = on;
}

void gated_abort(GatedMemSource* s) {
    s->hook_calls.fetch_add(1);
    std::lock_guard<std::mutex> g(s->mu);
    s->aborted = true;
    s->cv.notify_all();
}

int32_t first_video_track(const Pipeline& p) {
    for (const TrackInfo& t : p.tracks()) if (t.is_video) return t.index;
    return -1;
}

// 先解 300 步（让旧位置的数据确实在队列/解码器里），seek 到 seek_us，收集之后
// 视频轨的前 want 个 pts。
std::vector<int64_t> video_pts_after_seek(Pipeline& p, int64_t seek_us, std::size_t want) {
    std::vector<int64_t> out;
    const int32_t vidx = first_video_track(p);
    if (vidx < 0) return out;
    for (int i = 0; i < 300; ++i) {
        const StepOutcome o = p.step();
        if (o.kind == StepOutcome::Kind::Error || o.kind == StepOutcome::Kind::Eof) return out;
        for (const TrackInfo& t : p.tracks()) while (p.pop_frame(t.index)) {}
        if (o.kind == StepOutcome::Kind::Blocked) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (p.seek(seek_us) != SYP_OK) return out;
    for (int64_t i = 0; i < 20000000 && out.size() < want; ++i) {
        const StepOutcome o = p.step();
        if (o.kind == StepOutcome::Kind::Error || o.kind == StepOutcome::Kind::Eof) break;
        for (const TrackInfo& t : p.tracks()) {
            while (auto f = p.pop_frame(t.index)) {
                if (t.index == vidx && out.size() < want) out.push_back(f->pts_us());
            }
        }
        if (o.kind == StepOutcome::Kind::Blocked) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return out;
}

// 逐步驱动直到每条轨都出过至少一帧，返回各轨第一帧 pts；遇 Eof/Error 提前停
// （返回的 map 因而不满，调用方据此判失败）。
std::map<int32_t, int64_t> first_pts_per_track(Pipeline& p) {
    std::map<int32_t, int64_t> first;
    const std::size_t          n = p.tracks().size();
    for (int64_t i = 0; i < 20000000 && first.size() < n; ++i) {
        const StepOutcome o = p.step();
        if (o.kind == StepOutcome::Kind::Error || o.kind == StepOutcome::Kind::Eof) break;
        for (const TrackInfo& t : p.tracks()) {
            while (auto f = p.pop_frame(t.index)) first.emplace(t.index, f->pts_us());   // 只留第一帧
        }
        if (o.kind == StepOutcome::Kind::Blocked) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return first;
}

// 文件里第一个字节偏移 >= min_pos 的音频包的偏移；找不到返回 -1。
int64_t first_audio_packet_pos_from(const std::string& path, int64_t min_pos) {
    syp_status err = SYP_OK;
    auto       d   = Demuxer::open_file(path, &err);
    if (d == nullptr) return -1;
    for (;;) {
        AVPacket* pkt = nullptr;
        int32_t   ti  = -1;
        if (d->read(&pkt, &ti) != Demuxer::ReadResult::Packet) return -1;
        const bool audio = ti >= 0 && static_cast<std::size_t>(ti) < d->tracks().size() &&
                           !d->tracks()[static_cast<std::size_t>(ti)].is_video;
        const int64_t pos = pkt->pos;
        av_packet_free(&pkt);
        if (audio && pos >= min_pos) return pos;
    }
}

}  // namespace

TEST_CASE(pipeline_demux_thread_rejects_nonpositive_water_levels) {
    PipelineConfig cfg;
    cfg.demux_thread  = true;
    cfg.max_buffer_ms = 0;
    syp_status err = SYP_OK;
    CHECK(Pipeline::create_file(fixture("hls/vod_single/source.mp4"), cfg, &err) == nullptr);
    CHECK_EQ(err, SYP_ERR_INVALID_ARG);
    cfg.max_buffer_ms    = 30000;
    cfg.max_buffer_bytes = 0;
    CHECK(Pipeline::create_file(fixture("hls/vod_single/source.mp4"), cfg, &err) == nullptr);
    CHECK_EQ(err, SYP_ERR_INVALID_ARG);
    cfg.demux_thread = false;   // 同步模式不看这两个字段
    CHECK(Pipeline::create_file(fixture("hls/vod_single/source.mp4"), cfg, &err) != nullptr);
}

// 线程模式与同步模式逐轨帧序列一致（跨轨交织顺序不比：线程模式下解码分支会先把
// 低轨号的包解空，交织节奏本就不同）。
TEST_CASE(pipeline_demux_thread_decodes_same_frames_as_sync_mode) {
    syp::test::Watchdog wd("pipeline_demux_thread_decodes_same_frames_as_sync_mode", 60000, 120000,
                           "卡住多半是泵线程拿不到终止标记（加载线程没写 load_term_ 或 absorb 没读）");
    syp_status err = SYP_OK;
    auto sync_p = Pipeline::create_file(fixture("faststart.mp4"), PipelineConfig{}, &err);
    REQUIRE(sync_p != nullptr);
    const DriveRun a = drive_to_end(*sync_p);
    REQUIRE(a.last.kind == StepOutcome::Kind::Eof);
    REQUIRE(a.pts_by_track.size() >= 2);

    PipelineConfig cfg;
    cfg.demux_thread = true;
    auto thr_p = Pipeline::create_file(fixture("faststart.mp4"), cfg, &err);
    REQUIRE(thr_p != nullptr);
    const DriveRun b = drive_to_end(*thr_p);
    CHECK(b.last.kind == StepOutcome::Kind::Eof);
    CHECK(a.pts_by_track == b.pts_by_track);
    CHECK(!wd.fired());
}

// 加载线程读完整个文件（终止标记已到）之后，泵线程仍先交出全部已缓冲的帧，最后才报 Eof。
TEST_CASE(pipeline_demux_thread_eof_only_after_buffered_packets_drain) {
    syp::test::Watchdog wd("pipeline_demux_thread_eof_only_after_buffered_packets_drain", 30000, 60000,
                           "卡住多半是终止标记没被吸收成 demux_eof_");
    syp_status err = SYP_OK;
    auto sync_p = Pipeline::create_file(fixture("hls/vod_single/source.mp4"), PipelineConfig{}, &err);
    REQUIRE(sync_p != nullptr);
    const DriveRun ref = drive_to_end(*sync_p);
    REQUIRE(ref.last.kind == StepOutcome::Kind::Eof);

    PipelineConfig cfg;
    cfg.demux_thread = true;
    auto p = Pipeline::create_file(fixture("hls/vod_single/source.mp4"), cfg, &err);
    REQUIRE(p != nullptr);
    REQUIRE(wait_until([&] { return p->buffer_stats().demux_eof; }, 10000));
    const StepOutcome first = p->step();
    CHECK(first.kind == StepOutcome::Kind::DecodedFrame);   // 标记已到，但数据先交
    if (first.kind == StepOutcome::Kind::DecodedFrame) {
        auto f = p->pop_frame(first.track_index);
        REQUIRE(f.has_value());
        DriveRun rest = drive_to_end(*p);
        rest.pts_by_track[first.track_index].insert(rest.pts_by_track[first.track_index].begin(),
                                                    f->pts_us());
        CHECK(rest.last.kind == StepOutcome::Kind::Eof);
        CHECK(rest.pts_by_track == ref.pts_by_track);
    }
    CHECK(!wd.fired());
}

// 网络 IO 错误（重试耗尽）：先把错误点之前的数据解完，再报 Error(SYP_ERR_IO)。
TEST_CASE(pipeline_demux_thread_io_error_reported_after_buffered_data) {
    syp::test::Watchdog wd("pipeline_demux_thread_io_error_reported_after_buffered_data", 30000, 60000,
                           "卡住多半是错误标记没写或 Eof 出口没改写成 Error");
    const std::vector<uint8_t> data = read_file_bytes(fixture("faststart.mp4"));
    REQUIRE(!data.empty());
    GatedMemSource src;
    src.buf     = &data;
    src.fail_at = 4 * 1024 * 1024;   // 约 10 秒处（faststart：moov 在头部）
    AVIOContext* pb = make_gated_avio(&src);
    PipelineConfig cfg;
    cfg.demux_thread = true;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_avio(pb, cfg, &err, [&src] { gated_abort(&src); });
    REQUIRE(p != nullptr);
    const DriveRun r = drive_to_end(*p);
    CHECK(r.last.kind == StepOutcome::Kind::Error);
    CHECK_EQ(r.last.status, SYP_ERR_IO);
    const int32_t vidx = first_video_track(*p);
    REQUIRE(vidx >= 0);
    const auto it = r.pts_by_track.find(vidx);
    REQUIRE(it != r.pts_by_track.end() && !it->second.empty());
    CHECK(it->second.back() > 3000000);   // 错误点之前的数据确实播出去了
    p.reset();
    free_pb(&pb);
    CHECK(!wd.fired());
}

// 加载线程阻塞在读里：step() 不被拖住，照常交出已缓冲的帧；request_abort() 调到钩子、
// step() 立刻报 CANCELED；析构 join 不挂。
TEST_CASE(pipeline_demux_thread_step_not_blocked_by_read_and_abort_cancels) {
    syp::test::Watchdog wd("pipeline_demux_thread_step_not_blocked_by_read_and_abort_cancels",
                           10000, 30000,
                           "卡住说明 step() 仍在泵线程上同步读，或 request_abort()/析构没打断加载线程");
    const std::vector<uint8_t> data = read_file_bytes(fixture("faststart.mp4"));
    REQUIRE(!data.empty());
    GatedMemSource src;
    src.buf   = &data;
    src.limit = 8 * 1024 * 1024;   // 远大于探测读取量，create 不会卡
    AVIOContext* pb = make_gated_avio(&src);
    PipelineConfig cfg;
    cfg.demux_thread = true;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_avio(pb, cfg, &err, [&src] { gated_abort(&src); });
    REQUIRE(p != nullptr);
    REQUIRE(wait_until([&] { return src.in_block.load(); }, 10000));
    CHECK(p->buffer_stats().loading);

    int64_t decoded = 0;
    for (int i = 0; i < 2000; ++i) {
        const StepOutcome o = p->step();
        REQUIRE(o.kind != StepOutcome::Kind::Error);
        if (o.kind == StepOutcome::Kind::DecodedFrame) {
            ++decoded;
            (void)p->pop_frame(o.track_index);
        }
    }
    CHECK(decoded > 0);
    CHECK(src.in_block.load());   // 读仍卡着，泵线程却在推进

    p->request_abort();
    CHECK_EQ(src.hook_calls.load(), 1);
    const StepOutcome after = p->step();
    CHECK(after.kind == StepOutcome::Kind::Error);
    CHECK_EQ(after.status, SYP_ERR_CANCELED);
    p.reset();
    free_pb(&pb);
    CHECK(!wd.fired());
}

TEST_CASE(pipeline_demux_thread_destructor_joins_while_read_is_blocked) {
    syp::test::Watchdog wd("pipeline_demux_thread_destructor_joins_while_read_is_blocked", 10000, 30000,
                           "卡住说明 ~Pipeline() 没有打断阻塞读就 join");
    const std::vector<uint8_t> data = read_file_bytes(fixture("faststart.mp4"));
    REQUIRE(!data.empty());
    GatedMemSource src;
    src.buf   = &data;
    src.limit = 8 * 1024 * 1024;
    AVIOContext* pb = make_gated_avio(&src);
    PipelineConfig cfg;
    cfg.demux_thread = true;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_avio(pb, cfg, &err, [&src] { gated_abort(&src); });
    REQUIRE(p != nullptr);
    REQUIRE(wait_until([&] { return src.in_block.load(); }, 10000));
    p.reset();
    CHECK_EQ(src.hook_calls.load(), 1);
    free_pb(&pb);
    CHECK(!wd.fired());
}

// 异步 seek：旧位置（0~十几秒）的包在 seek 时正堆在队列里；seek 到 45 秒后前 30 帧
// 视频 pts 与同步模式逐项相等——任何一个旧代号的包被解码都会让首帧 pts 远小于落点。
TEST_CASE(pipeline_demux_thread_async_seek_first_frames_match_sync_mode) {
    syp::test::Watchdog wd("pipeline_demux_thread_async_seek_first_frames_match_sync_mode", 60000, 120000,
                           "卡住多半是 seek 代号没被加载线程处理（served_gen_ 未更新）");
    syp_status err = SYP_OK;
    auto sp = Pipeline::create_file(fixture("faststart.mp4"), PipelineConfig{}, &err);
    REQUIRE(sp != nullptr);
    const std::vector<int64_t> ref = video_pts_after_seek(*sp, 45000000, 30);
    REQUIRE(ref.size() == 30);

    PipelineConfig cfg;
    cfg.demux_thread = true;
    auto tp = Pipeline::create_file(fixture("faststart.mp4"), cfg, &err);
    REQUIRE(tp != nullptr);
    REQUIRE(wait_until([&] { return tp->buffer_stats().buffered_until_us > 10000000; }, 10000));
    const std::vector<int64_t> got = video_pts_after_seek(*tp, 45000000, 30);
    CHECK(got == ref);
    CHECK(!wd.fired());
}

// seek 时加载线程正阻塞在读里：seek() 立即返回、不打断那次读（非目标之一）；
// 读返回后结果作废、执行 seek，之后各轨第一帧与同步模式一致。
//
// 【区分力】闸门钉在 8MB 之后第一个音频包的第二个字节上：被卡住的那次读确定是一个
// 约 20 秒处的音频包。漏掉读返回后的代号比对时，这个旧包会在清空之后进入音频队列
// 队首，首帧音频 pts 落在 20 秒附近而不是 30 秒——视频首帧抓不住它（旧包若是视频
// 非关键帧，flush 之后的 H.264 解码器会静默丢掉），所以音频首帧是必要断言。
TEST_CASE(pipeline_demux_thread_seek_while_read_blocked_applies_after_read_returns) {
    syp::test::Watchdog wd("pipeline_demux_thread_seek_while_read_blocked_applies_after_read_returns",
                           30000, 60000, "卡住多半是读返回后没有比对代号、没执行待定 seek");
    const int64_t kSeekUs = 30000000;
    syp_status err = SYP_OK;
    auto sp = Pipeline::create_file(fixture("faststart.mp4"), PipelineConfig{}, &err);
    REQUIRE(sp != nullptr);
    REQUIRE(sp->seek(kSeekUs) == SYP_OK);
    const std::map<int32_t, int64_t> ref = first_pts_per_track(*sp);
    REQUIRE(ref.size() == sp->tracks().size());
    REQUIRE(ref.size() >= 2);

    const int64_t gate = first_audio_packet_pos_from(fixture("faststart.mp4"), 8 * 1024 * 1024);
    REQUIRE(gate > 0);
    const std::vector<uint8_t> data = read_file_bytes(fixture("faststart.mp4"));
    REQUIRE(!data.empty());
    GatedMemSource src;
    src.buf   = &data;
    src.limit = gate + 1;
    AVIOContext* pb = make_gated_avio(&src);
    PipelineConfig cfg;
    cfg.demux_thread = true;
    auto p = Pipeline::create_avio(pb, cfg, &err, [&src] { gated_abort(&src); });
    REQUIRE(p != nullptr);
    REQUIRE(wait_until([&] { return src.in_block.load(); }, 10000));
    REQUIRE(p->seek(kSeekUs) == SYP_OK);
    CHECK(src.in_block.load());
    gated_release(&src);

    const std::map<int32_t, int64_t> got = first_pts_per_track(*p);
    CHECK(got == ref);
    if (got != ref) {
        for (const auto& [ti, pts] : got) {
            const auto it = ref.find(ti);
            std::fprintf(stderr, "  track %d first=%lld ref=%lld\n", ti, static_cast<long long>(pts),
                         static_cast<long long>(it == ref.end() ? -1 : it->second));
        }
    }
    p.reset();
    free_pb(&pb);
    CHECK(!wd.fired());
}

// 【seek 冲刷 vs 并发 push】异步 seek 的 PacketQueue::clear() 与加载线程
// 的 push 真实交错：本地文件读得飞快、队列无上限，seek 的那一刻加载线程几乎
// 总在推包。两个落点相距 35 秒、交替来回 seek，每轮解的帧数不同（含"连 seek 两次、
// 中间一步不走"——加载线程正在执行上一个 seek 时代号又变），断言每轮：
//   · 第一帧视频 pts 与同步模式该落点的第一帧逐项相等；
//   · 之后的视频帧 pts 严格递增（faststart 无 B 帧）且落在 [首帧, 落点 + 10 秒]，
//     音频帧落在 [首帧 − 1 秒, 落点 + 10 秒]——任何一个旧位置（另一落点之后）的包
//     漏过清空被解出来，都会跑出窗口或打乱单调。
// 本用例是 TSan 目标：load_mu_ 下的"清空 + 递增代号"与"比对代号 + 入队"必须互斥。
TEST_CASE(pipeline_demux_thread_rapid_seeks_race_loader_push_never_decode_pre_seek_packets) {
    syp::test::Watchdog wd("pipeline_demux_thread_rapid_seeks_race_loader_push_never_decode_pre_seek_packets",
                           60000, 120000, "卡住多半是连续 seek 时代号作废后没有重做 seek");
    const int64_t kNear = 5000000;
    const int64_t kFar  = 40000000;
    syp_status err = SYP_OK;
    auto sp = Pipeline::create_file(fixture("faststart.mp4"), PipelineConfig{}, &err);
    REQUIRE(sp != nullptr);
    const std::vector<int64_t> ref_near = video_pts_after_seek(*sp, kNear, 1);
    const std::vector<int64_t> ref_far  = video_pts_after_seek(*sp, kFar, 1);
    REQUIRE(ref_near.size() == 1 && ref_far.size() == 1);

    PipelineConfig cfg;
    cfg.demux_thread = true;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), cfg, &err);
    REQUIRE(p != nullptr);
    const int32_t vidx = first_video_track(*p);
    REQUIRE(vidx >= 0);

    int bad_rounds = 0;
    for (int round = 0; round < 100; ++round) {
        const bool    far    = (round % 2) == 0;
        const int64_t target = far ? kFar : kNear;
        const int64_t first  = far ? ref_far[0] : ref_near[0];
        if (round % 3 == 0) REQUIRE(p->seek(far ? kNear : kFar) == SYP_OK);   // 立刻被下一次覆盖
        REQUIRE(p->seek(target) == SYP_OK);

        const std::size_t    want = 1 + static_cast<std::size_t>((round * 7) % 24);
        std::vector<int64_t> got;
        bool                 in_window = true;
        for (int64_t i = 0; i < 20000000 && got.size() < want; ++i) {
            const StepOutcome o = p->step();
            REQUIRE(o.kind != StepOutcome::Kind::Error && o.kind != StepOutcome::Kind::Eof);
            for (const TrackInfo& t : p->tracks()) {
                while (auto f = p->pop_frame(t.index)) {
                    const int64_t pts = f->pts_us();
                    if (t.index != vidx) {
                        // 音频包每个都出帧（不像 h264 flush 后会丢掉关键帧之前的 P 帧），
                        // 漏过来的旧音频包必然现形。mov 的 BACKWARD seek 让音频落在视频
                        // 关键帧附近，下限留 1 秒余量。
                        if (pts < first - 1000000 || pts > target + 10000000) in_window = false;
                        continue;
                    }
                    if (got.size() >= want) continue;
                    if (pts < first || pts > target + 10000000) in_window = false;
                    if (!got.empty() && pts <= got.back()) in_window = false;
                    got.push_back(pts);
                }
            }
            if (o.kind == StepOutcome::Kind::Blocked) std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        REQUIRE(got.size() == want);
        if (got[0] != first || !in_window) {
            ++bad_rounds;
            std::fprintf(stderr, "  round %d target=%lld first=%lld want_first=%lld in_window=%d\n", round,
                         static_cast<long long>(target), static_cast<long long>(got[0]),
                         static_cast<long long>(first), in_window ? 1 : 0);
        }
    }
    CHECK_EQ(bad_rounds, 0);
    CHECK(!wd.fired());
}

// ---- 停读水位 + 消费后唤醒 ----

// 时长水位：不消费时加载线程读到 max_buffer_ms 就停（min 超出至多一个包），字节不再增长；
// 消费之后续读，水位继续前进。
TEST_CASE(pipeline_demux_thread_stops_at_max_buffer_ms_and_resumes_on_consume) {
    syp::test::Watchdog wd("pipeline_demux_thread_stops_at_max_buffer_ms_and_resumes_on_consume",
                           30000, 60000, "卡住多半是消费之后 wake_loader() 没唤醒加载线程");
    PipelineConfig cfg;
    cfg.demux_thread  = true;
    cfg.max_buffer_ms = 2000;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), cfg, &err);
    REQUIRE(p != nullptr);
    REQUIRE(wait_until([&] { return p->buffer_stats().full; }, 10000));
    const BufferStats s1 = p->buffer_stats();
    // 下限留 50ms：AAC 首包 pts 可能为负（priming），队列时长比 buffered_until 多出一截。
    CHECK(s1.buffered_until_us >= 2000000 - 50000);
    CHECK(s1.buffered_until_us <= 2000000 + 100000);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const BufferStats s2 = p->buffer_stats();
    CHECK_EQ(s2.queued_bytes, s1.queued_bytes);   // 真的停读了
    CHECK(!s2.loading);

    bool grew = false;
    for (int64_t i = 0; i < 20000000 && !grew; ++i) {
        const StepOutcome o = p->step();
        REQUIRE(o.kind != StepOutcome::Kind::Error);
        REQUIRE(o.kind != StepOutcome::Kind::Eof);
        for (const TrackInfo& t : p->tracks()) while (p->pop_frame(t.index)) {}
        if (p->buffer_stats().buffered_until_us > 3000000) grew = true;
        if (o.kind == StepOutcome::Kind::Blocked) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    CHECK(grew);
    CHECK(!wd.fired());
}

// 字节水位：高码率下字节上限先到，时长远未到 30 秒。
//
// 【素材码率前提】faststart.mp4 由 tools/gen-fixtures.sh 按种子随机生成：
// 视频 2500~5000kbps + 音频 128kbps、时长 40~100 秒，文件远大于 1MiB；1MiB 在
// ≤ 8MiB/2628kbps ≈ 3.2 秒内就攒够，所以 buffered_until_us < 30s 与"时长上限不会先到"
// 恒成立。"< 2MiB"依赖单个包 < 1MiB：最大的包是关键帧，≤720p ultrafast、≤5000kbps
// 下按码率估算至多几百 KB（本机种子 00818168 实测最大包约 31KB）。以后若把素材码率/分辨率调高，这两条前提要一起重新核对。
TEST_CASE(pipeline_demux_thread_stops_at_max_buffer_bytes) {
    syp::test::Watchdog wd("pipeline_demux_thread_stops_at_max_buffer_bytes", 30000, 60000,
                           "卡住多半是加载线程停读后 buffer_stats().full 读不到，或加载线程没启动");
    PipelineConfig cfg;
    cfg.demux_thread     = true;
    cfg.max_buffer_bytes = int64_t{1} << 20;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("faststart.mp4"), cfg, &err);
    REQUIRE(p != nullptr);
    REQUIRE(wait_until([&] { return p->buffer_stats().full; }, 10000));
    const BufferStats s1 = p->buffer_stats();
    CHECK(s1.queued_bytes >= (int64_t{1} << 20));
    CHECK(s1.queued_bytes < (int64_t{2} << 20));   // 超出至多一个包
    CHECK(s1.buffered_until_us < 30000000);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(p->buffer_stats().queued_bytes, s1.queued_bytes);
    CHECK(!wd.fired());
}

// 线程模式重播：解到 Eof → seek(0) → 再解到 Eof，两轮逐轨帧序列相同。加载线程第一轮
// 结束时停在"已终止"上等待，seek 必须把它叫醒重新读；水位压到 1 秒，两轮都反复
// 经历停读/续读。
TEST_CASE(pipeline_demux_thread_reaches_eof_again_after_seek_from_eof) {
    syp::test::Watchdog wd("pipeline_demux_thread_reaches_eof_again_after_seek_from_eof", 30000, 60000,
                           "卡住多半是 seek 没清终止标记/没唤醒已终止的加载线程，或停读后没被消费唤醒");
    PipelineConfig cfg;
    cfg.demux_thread  = true;
    cfg.max_buffer_ms = 1000;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_file(fixture("hls/vod_single/source.mp4"), cfg, &err);
    REQUIRE(p != nullptr);
    const DriveRun a = drive_to_end(*p);
    REQUIRE(a.last.kind == StepOutcome::Kind::Eof);
    REQUIRE(a.pts_by_track.size() >= 2);
    REQUIRE(p->seek(0) == SYP_OK);
    const DriveRun b = drive_to_end(*p);
    CHECK(b.last.kind == StepOutcome::Kind::Eof);
    for (const auto& [ti, pts] : a.pts_by_track) {
        const auto it = b.pts_by_track.find(ti);
        REQUIRE(it != b.pts_by_track.end());
        CHECK_EQ(it->second.size(), pts.size());
    }
    CHECK(a.pts_by_track == b.pts_by_track);
    CHECK(!wd.fired());
}

// 线程模式 seek 失败：之后每一步都报 Error(SYP_ERR_IO)（不是只报一次），
// 直到下一次 seek；下一次成功的 seek 让管线恢复出帧。
//
// 失败的构造同 pipeline_failed_seek_still_flushes_decoders：视频轨 stss 无关键帧 ⇒
// mov_read_seek 失败 ⇒ seek_frame_generic 先 avio_seek 到最后一条索引项——这一次
// 回调被 seek_fail 打回，av_seek_frame() 失败且读位置不动。恢复用 seek(-1 秒)：目标
// 早于视频轨第一条索引项时 mov_seek_stream() 钳到样本 0，只走索引、不碰 seek 回调
// 之外的兜底，于是这份素材上它能成功。
TEST_CASE(pipeline_demux_thread_failed_seek_errors_every_step_until_next_seek_recovers) {
    syp::test::Watchdog wd("pipeline_demux_thread_failed_seek_errors_every_step_until_next_seek_recovers",
                           30000, 60000, "卡住多半是 seek 失败的终止标记没写，或下一次 seek 没清掉它");
    std::vector<uint8_t> data = read_file_bytes(fixture("faststart.mp4"));
    REQUIRE(!data.empty());
    REQUIRE(blank_out_video_keyframe_table(&data));
    GatedMemSource src;
    src.buf = &data;
    AVIOContext* pb = make_gated_avio(&src);
    PipelineConfig cfg;
    cfg.demux_thread  = true;
    cfg.max_buffer_ms = 3000;
    syp_status err = SYP_OK;
    auto p = Pipeline::create_avio(pb, cfg, &err, [&src] { gated_abort(&src); });
    REQUIRE(p != nullptr);
    const int32_t vidx = first_video_track(*p);
    REQUIRE(vidx >= 0);

    // 码流先活着出一段视频。
    int64_t video = 0;
    for (int64_t i = 0; i < 20000000 && video < 10; ++i) {
        const StepOutcome o = p->step();
        REQUIRE(o.kind != StepOutcome::Kind::Error && o.kind != StepOutcome::Kind::Eof);
        for (const TrackInfo& t : p->tracks()) {
            while (p->pop_frame(t.index)) if (t.index == vidx) ++video;
        }
        if (o.kind == StepOutcome::Kind::Blocked) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    REQUIRE(video >= 10);

    gated_set_seek_fail(&src, true);
    REQUIRE(p->seek(3000000) == SYP_OK);   // 线程模式立即返回，失败稍后以终止标记出现
    StepOutcome o{};
    for (int64_t i = 0; i < 20000000; ++i) {
        o = p->step();
        for (const TrackInfo& t : p->tracks()) while (p->pop_frame(t.index)) {}
        if (o.kind == StepOutcome::Kind::Error) break;
        REQUIRE(o.kind != StepOutcome::Kind::Eof);
        if (o.kind == StepOutcome::Kind::Blocked) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    REQUIRE(o.kind == StepOutcome::Kind::Error);
    CHECK_EQ(o.status, SYP_ERR_IO);
    CHECK(src.failed_seeks.load() > 0);   // 错误确实来自这次 seek
    for (int i = 0; i < 5; ++i) {
        const StepOutcome again = p->step();
        CHECK(again.kind == StepOutcome::Kind::Error);
        CHECK_EQ(again.status, SYP_ERR_IO);
    }
    gated_set_seek_fail(&src, false);

    REQUIRE(p->seek(-1000000) == SYP_OK);
    int64_t video_after = 0;
    int64_t first_video = AV_NOPTS_VALUE;
    StepOutcome last{};
    for (int64_t i = 0; i < 20000000 && video_after < 50; ++i) {
        last = p->step();
        if (last.kind == StepOutcome::Kind::Error || last.kind == StepOutcome::Kind::Eof) break;
        for (const TrackInfo& t : p->tracks()) {
            while (auto f = p->pop_frame(t.index)) {
                if (t.index != vidx) continue;
                if (first_video == AV_NOPTS_VALUE) first_video = f->pts_us();
                ++video_after;
            }
        }
        if (last.kind == StepOutcome::Kind::Blocked) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    CHECK(last.kind != StepOutcome::Kind::Error);
    CHECK_EQ(video_after, int64_t{50});
    CHECK(first_video >= 0 && first_video < 1000000);   // 回到了开头，不是失败前的旧位置
    p.reset();
    free_pb(&pb);
    CHECK(!wd.fired());
}

int main() { return tiny_test_main(); }
