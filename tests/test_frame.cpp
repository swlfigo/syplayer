// test_frame.cpp — Frame 的所有权语义与两个有界队列。
//
// Frame 是整条管线的所有权基石：独占持有底层资源、不可拷贝、只可移动。
// 这三条一旦破，队列里会出现悬垂或双重释放，而症状会出现在很远的地方。
#include "media/frame.h"
#include "media/frame_queue.h"
#include "media/packet_queue.h"
#include "tiny_test.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavcodec/packet.h>
}

#include <type_traits>
#include <utility>

using namespace syp::media;

namespace {

// 造一个 320x240 yuv420p 的空帧，pts 用 1/1000 时基的 500 → 应换算成 500000 微秒
AVFrame* make_video_av_frame(int64_t pts) {
    AVFrame* f = av_frame_alloc();
    f->format = 0;              // AV_PIX_FMT_YUV420P
    f->width  = 320;
    f->height = 240;
    f->pts    = pts;
    av_frame_get_buffer(f, 0);
    return f;
}

}  // namespace

TEST_CASE(frame_is_move_only) {
    // 拷贝会让两个 Frame 争夺同一份 AVFrame 的所有权 —— 编译期就该挡住
    static_assert(!std::is_copy_constructible_v<Frame>);
    static_assert(!std::is_copy_assignable_v<Frame>);
    static_assert(std::is_move_constructible_v<Frame>);
    static_assert(std::is_move_assignable_v<Frame>);
}

TEST_CASE(frame_converts_pts_to_microseconds) {
    // 时基换算在解码器出口做一次，之后全管线统一用微秒。
    // 这条断言钉住「不会有第二种口径」。
    Frame f = Frame::from_av(make_video_av_frame(500), AVRational{1, 1000}, true);
    CHECK_EQ(f.pts_us(), int64_t{500000});
    CHECK(f.is_video());
    CHECK_EQ(f.width(), 320);
    CHECK_EQ(f.height(), 240);
    CHECK(f.plane(0) != nullptr);
    CHECK(f.stride(0) >= 320);   // 带对齐 padding，只保证不小于宽度
}

TEST_CASE(frame_move_leaves_source_empty) {
    Frame a = Frame::from_av(make_video_av_frame(0), AVRational{1, 1000}, true);
    const uint8_t* p = a.plane(0);
    Frame b = std::move(a);
    CHECK_EQ(b.plane(0), p);       // 资源转移过去了
    CHECK(a.plane(0) == nullptr);  // 源被掏空，析构时不会二次释放
}

TEST_CASE(frame_move_assign_releases_own_resource) {
    // 移动赋值必须先释放自己原有的资源。漏掉那行是泄漏不是崩溃，
    // 而 macOS 上 ASan 查不了泄漏，所以这里用
    // AVBufferRef 的引用计数做确定性检出。
    Frame a = Frame::from_av(make_video_av_frame(1), AVRational{1, 1000}, true);
    AVFrame* b_raw = make_video_av_frame(2);
    // 在 b 接管之前，先自己拿一份 b 底层 buffer 的引用
    AVBufferRef* keep = av_buffer_ref(b_raw->buf[0]);
    REQUIRE(keep != nullptr);
    Frame b = Frame::from_av(b_raw, AVRational{1, 1000}, true);

    CHECK_EQ(av_buffer_get_ref_count(keep), 2);   // b 一份 + keep 一份

    b = std::move(a);        // b 必须先释放自己原来那份

    CHECK_EQ(av_buffer_get_ref_count(keep), 1);   // 只剩 keep —— 漏了这行就是 2
    CHECK_EQ(b.pts_us(), int64_t{1000});          // 确实换成了 a 的内容
    av_buffer_unref(&keep);
}

TEST_CASE(frame_queue_is_bounded) {
    FrameQueue q(2);
    CHECK(q.push(Frame::from_av(make_video_av_frame(0), AVRational{1, 1000}, true)));
    CHECK(q.push(Frame::from_av(make_video_av_frame(1), AVRational{1, 1000}, true)));
    CHECK(q.full());
    // 满了之后 push 必须失败且不吞掉传入的帧的资源 —— 由调用方保留
    Frame extra = Frame::from_av(make_video_av_frame(2), AVRational{1, 1000}, true);
    CHECK(!q.push(std::move(extra)));
    CHECK_EQ(static_cast<int>(q.size()), 2);
}

TEST_CASE(frame_queue_pop_is_fifo) {
    FrameQueue q(4);
    for (int i = 0; i < 3; ++i) {
        q.push(Frame::from_av(make_video_av_frame(i), AVRational{1, 1000000}, true));
    }
    for (int i = 0; i < 3; ++i) {
        auto f = q.pop();
        REQUIRE(f.has_value());
        CHECK_EQ(f->pts_us(), static_cast<int64_t>(i));
    }
    CHECK(!q.pop().has_value());
}

TEST_CASE(packet_queue_limits_by_count_and_bytes) {
    // 双重设限：packet 数与字节数任一到顶都算满。
    // 只按个数限会被大 packet 撑爆内存；只按字节限会被海量小 packet 拖垮。
    PacketQueue q(/*max_packets=*/100, /*max_bytes=*/1000);
    for (int i = 0; i < 3; ++i) {
        AVPacket* p = av_packet_alloc();
        av_new_packet(p, 400);
        CHECK(q.push(p));
    }
    CHECK(q.full());                       // 1200 字节 > 1000
    CHECK_EQ(static_cast<int>(q.size()), 3);
    AVPacket* extra = av_packet_alloc();
    av_new_packet(extra, 10);
    // 清理不能假设 push 一定失败：判据一旦被破坏，push 会成功接管 extra 的
    // 所有权（队列析构时会释放它），这里若再无条件释放一次就是 double free。
    // 只调用一次 push，并让清理只在它确实失败时才收尾。
    bool pushed = q.push(extra);
    CHECK(!pushed);
    if (!pushed) av_packet_free(&extra);
}

TEST_CASE(queues_clear_releases_everything) {
    FrameQueue fq(8);
    PacketQueue pq(8, 1 << 20);
    for (int i = 0; i < 4; ++i) {
        fq.push(Frame::from_av(make_video_av_frame(i), AVRational{1, 1000}, true));
        AVPacket* p = av_packet_alloc();
        av_new_packet(p, 16);
        pq.push(p);
    }
    fq.clear();
    pq.clear();
    CHECK_EQ(static_cast<int>(fq.size()), 0);
    CHECK_EQ(static_cast<int>(pq.size()), 0);
    CHECK_EQ(pq.bytes(), int64_t{0});
}

TEST_CASE(frame_hw_handle_is_null_for_software_frames) {
    AVFrame* av = av_frame_alloc();
    av->format = AV_PIX_FMT_YUV420P;
    av->width  = 4;
    av->height = 4;
    REQUIRE(av_frame_get_buffer(av, 0) == 0);
    const Frame f = Frame::from_av(av, AVRational{1, 25}, true);
    CHECK(f.hw_handle() == nullptr);
}

TEST_CASE(frame_hw_handle_returns_data3_for_videotoolbox_frames) {
    AVFrame* av = av_frame_alloc();
    av->format = AV_PIX_FMT_VIDEOTOOLBOX;
    static int sentinel = 0;
    av->data[3] = reinterpret_cast<uint8_t*>(&sentinel);   // 不解引用，只比地址
    Frame f = Frame::from_av(av, AVRational{1, 25}, true);
    CHECK(f.hw_handle() == static_cast<void*>(&sentinel));
    // data[3] 没有 buf 支撑，av_frame_free 不会解引用或释放它，析构安全。
}

TEST_CASE(frame_hw_handle_is_null_for_empty_frame) {
    const Frame f;
    CHECK(f.hw_handle() == nullptr);
}

int main() { return tiny_test_main(); }
