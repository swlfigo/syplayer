// test_packet_queue.cpp — PacketQueue 成为加载线程与泵线程之间的跨线程边界。
//
// 覆盖三件事：
//   1. buffered_until_us()：入队时维护（取已入队包结束时刻的最大值，B 帧 pts
//      乱序时仍单调），pop 不回退，clear 复位为 AV_NOPTS_VALUE；
//   2. queued_duration_us()：队尾结束 − 队首包开始时刻，加载线程停读水位用它；
//   3. 并发 push/pop：顺序与字节账不乱（TSan 目标）；
//   4. take_all()：摘出全部包、复位同 clear()（锁外释放用）。
// 容量语义（数量/字节双重设限）的既有用例留在 test_frame.cpp，不搬。
#include "media/packet_queue.h"
#include "tiny_test.h"

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
}

#include <cstdint>
#include <deque>
#include <limits>
#include <thread>
#include <vector>

using syp::media::PacketQueue;

namespace {

AVPacket* make_packet(int64_t pts, int64_t duration, int size) {
    AVPacket* p = av_packet_alloc();
    av_new_packet(p, size);
    p->pts      = pts;
    p->dts      = pts;
    p->duration = duration;
    return p;
}

constexpr std::size_t kNoCount = std::numeric_limits<std::size_t>::max();
constexpr int64_t     kNoBytes = std::numeric_limits<int64_t>::max();

}  // namespace

TEST_CASE(packet_queue_buffered_until_tracks_end_and_survives_pop_until_clear) {
    PacketQueue q(kNoCount, kNoBytes, AVRational{1, 1000});
    CHECK_EQ(q.buffered_until_us(), AV_NOPTS_VALUE);
    REQUIRE(q.push(make_packet(0, 40, 8)));
    REQUIRE(q.push(make_packet(40, 40, 8)));
    CHECK_EQ(q.buffered_until_us(), int64_t{80000});
    for (int i = 0; i < 2; ++i) {
        AVPacket* p = q.pop();
        REQUIRE(p != nullptr);
        av_packet_free(&p);
    }
    CHECK_EQ(q.buffered_until_us(), int64_t{80000});   // pop 不回退：已送进解码器的数据仍算"已缓冲到"
    REQUIRE(q.push(make_packet(120, 0, 8)));            // duration 未知：结束时刻取开始时刻
    CHECK_EQ(q.buffered_until_us(), int64_t{120000});
    q.clear();
    CHECK_EQ(q.buffered_until_us(), AV_NOPTS_VALUE);
}

TEST_CASE(packet_queue_buffered_until_is_max_end_not_last_for_reordered_pts) {
    PacketQueue q(kNoCount, kNoBytes, AVRational{1, 1000});
    REQUIRE(q.push(make_packet(200, 40, 8)));
    REQUIRE(q.push(make_packet(80, 40, 8)));   // B 帧：后入队的 pts 更小
    CHECK_EQ(q.buffered_until_us(), int64_t{240000});
}

TEST_CASE(packet_queue_queued_duration_is_tail_end_minus_head_start) {
    PacketQueue q(kNoCount, kNoBytes, AVRational{1, 1000});
    CHECK_EQ(q.queued_duration_us(), int64_t{0});
    REQUIRE(q.push(make_packet(0, 40, 8)));
    REQUIRE(q.push(make_packet(40, 40, 8)));
    REQUIRE(q.push(make_packet(80, 40, 8)));
    CHECK_EQ(q.queued_duration_us(), int64_t{120000});
    AVPacket* p = q.pop();
    av_packet_free(&p);
    CHECK_EQ(q.queued_duration_us(), int64_t{80000});
    p = q.pop();
    av_packet_free(&p);
    p = q.pop();
    av_packet_free(&p);
    CHECK_EQ(q.queued_duration_us(), int64_t{0});
}

TEST_CASE(packet_queue_without_time_base_reports_unknown) {
    PacketQueue q(8, 1 << 20);   // 旧构造形式：时基未知
    REQUIRE(q.push(make_packet(100, 40, 8)));
    CHECK_EQ(q.buffered_until_us(), AV_NOPTS_VALUE);
    CHECK_EQ(q.queued_duration_us(), int64_t{0});
}

TEST_CASE(packet_queue_nopts_pts_falls_back_to_dts) {
    PacketQueue q(kNoCount, kNoBytes, AVRational{1, 1000});
    AVPacket* p = make_packet(0, 10, 8);
    p->pts = AV_NOPTS_VALUE;
    p->dts = 500;
    REQUIRE(q.push(p));
    CHECK_EQ(q.buffered_until_us(), int64_t{510000});
}

TEST_CASE(packet_queue_concurrent_push_pop_keeps_order_and_accounting) {
    constexpr int kCount = 20000;
    PacketQueue q(kNoCount, kNoBytes, AVRational{1, 1000});
    std::thread producer([&q] {
        for (int i = 0; i < kCount; ++i) {
            AVPacket* p = make_packet(i, 1, 3);
            if (!q.push(p)) av_packet_free(&p);
        }
    });
    std::vector<int64_t> got;
    got.reserve(kCount);
    while (static_cast<int>(got.size()) < kCount) {
        AVPacket* p = q.pop();
        if (p == nullptr) {
            // 读侧的其它查询也在并发路径上被 TSan 看到
            (void)q.bytes();
            (void)q.buffered_until_us();
            (void)q.queued_duration_us();
            std::this_thread::yield();
            continue;
        }
        got.push_back(p->pts);
        av_packet_free(&p);
    }
    producer.join();
    CHECK(q.empty());
    CHECK_EQ(q.bytes(), int64_t{0});
    bool ordered = true;
    for (int i = 0; i < kCount; ++i) {
        if (got[static_cast<std::size_t>(i)] != i) ordered = false;
    }
    CHECK(ordered);
    CHECK_EQ(q.buffered_until_us(), int64_t{kCount} * 1000);
}

// take_all()：一次交出全部包（所有权转给调用方），效果等同 clear() 但不在锁内释放——
// Pipeline 的异步 seek 持 load_mu_ 摘包、解锁后再逐个 av_packet_free。
TEST_CASE(packet_queue_take_all_hands_out_packets_and_resets_like_clear) {
    PacketQueue q(kNoCount, kNoBytes, AVRational{1, 1000});
    REQUIRE(q.push(make_packet(0, 40, 8)));
    REQUIRE(q.push(make_packet(40, 40, 16)));
    std::deque<AVPacket*> taken = q.take_all();
    REQUIRE(taken.size() == 2);
    CHECK_EQ(taken[0]->pts, int64_t{0});
    CHECK_EQ(taken[1]->pts, int64_t{40});
    CHECK(q.empty());
    CHECK_EQ(q.size(), std::size_t{0});
    CHECK_EQ(q.bytes(), int64_t{0});
    CHECK_EQ(q.buffered_until_us(), AV_NOPTS_VALUE);
    CHECK_EQ(q.queued_duration_us(), int64_t{0});
    for (AVPacket* p : taken) av_packet_free(&p);
    CHECK(q.take_all().empty());
    REQUIRE(q.push(make_packet(80, 40, 8)));   // 摘空之后照常可用
    CHECK_EQ(q.buffered_until_us(), int64_t{120000});
}

int main() { return tiny_test_main(); }
