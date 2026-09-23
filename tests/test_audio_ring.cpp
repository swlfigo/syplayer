#include "media/audio_ring.h"
#include "tiny_test.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using syp::media::AudioRing;

TEST_CASE(ring_write_then_read_roundtrips) {
    AudioRing r(64);
    const uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_EQ(r.write(src, 8), int32_t{8});
    CHECK_EQ(r.readable(), int32_t{8});

    uint8_t dst[8] = {};
    CHECK_EQ(r.read(dst, 8), int32_t{8});
    CHECK_EQ(std::memcmp(src, dst, 8), 0);
    CHECK_EQ(r.readable(), int32_t{0});
    CHECK_EQ(r.consumed_bytes(), int64_t{8});
}

TEST_CASE(ring_write_returns_partial_when_full) {
    // 容量满时 write 必须返回实际写入量，不能溢出、不能假装全写进去了。
    AudioRing r(16);
    std::vector<uint8_t> big(32, 0xAB);
    const int32_t n = r.write(big.data(), 32);
    CHECK(n < 32);
    CHECK_EQ(n, r.capacity());          // 空环最多能写下 capacity 字节
    CHECK_EQ(r.writable(), int32_t{0});
}

TEST_CASE(ring_read_returns_partial_when_empty) {
    AudioRing r(16);
    uint8_t dst[8] = {};
    CHECK_EQ(r.read(dst, 8), int32_t{0});
    CHECK_EQ(r.consumed_bytes(), int64_t{0});
}

TEST_CASE(ring_underrun_does_not_advance_consumed_bytes) {
    // 音频时钟的地基。实时回调欠载时会自己补静音填满输出缓冲，
    // 但**补的静音绝不能计进 consumed_bytes** —— 否则时钟会在没有真正
    // 播出音频的情况下前进，表现为「网络一卡，画面就开始跑得比声音快」。
    // read() 只按真实读出量推进读指针，这条用例把它钉死。
    AudioRing r(64);
    const uint8_t src[4] = {9, 9, 9, 9};
    REQUIRE(r.write(src, 4) == 4);

    uint8_t dst[16] = {};
    CHECK_EQ(r.read(dst, 16), int32_t{4});          // 只读得出 4
    CHECK_EQ(r.consumed_bytes(), int64_t{4});       // 不是 16
}

TEST_CASE(ring_reset_drops_unread_data_but_keeps_consumed_bytes) {
    // reset() 是生产者侧独占操作（调用前必须确保消费者已停止），语义是
    // 「丢弃全部未读内容」——但 consumed_bytes 是时钟用的单调累计量，
    // reset() 绝不能碰它，否则 seek/变速之后时钟会倒退。
    AudioRing r(64);
    const uint8_t src[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    REQUIRE(r.write(src, 10) == 10);

    uint8_t dst[4] = {};
    REQUIRE(r.read(dst, 4) == 4);            // 消费 4 字节，consumed_bytes 变成 4
    CHECK_EQ(r.consumed_bytes(), int64_t{4});
    CHECK_EQ(r.readable(), int32_t{6});      // 还剩 6 字节没读

    r.reset();

    CHECK_EQ(r.readable(), int32_t{0});
    CHECK_EQ(r.writable(), r.capacity());
    CHECK_EQ(r.consumed_bytes(), int64_t{4});   // 重点：reset() 不改变它
}

TEST_CASE(ring_wraps_around_preserving_order) {
    // 绕圈是环形缓冲最容易写错的地方：写指针绕回去之后，
    // 读出来的字节顺序必须仍然是写入顺序。
    AudioRing r(16);
    std::vector<uint8_t> a(12);
    for (int i = 0; i < 12; ++i) a[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
    REQUIRE(r.write(a.data(), 12) == 12);

    uint8_t drain[10] = {};
    REQUIRE(r.read(drain, 10) == 10);   // 读走 10，读指针推进

    std::vector<uint8_t> b(10);
    for (int i = 0; i < 10; ++i) b[static_cast<size_t>(i)] = static_cast<uint8_t>(100 + i);
    REQUIRE(r.write(b.data(), 10) == 10);   // 这一写必然绕圈

    uint8_t out[12] = {};
    REQUIRE(r.read(out, 12) == 12);
    // 剩下的 2 个旧字节（10、11）后面接 10 个新字节（100..109）
    CHECK_EQ(out[0], uint8_t{10});
    CHECK_EQ(out[1], uint8_t{11});
    for (int i = 0; i < 10; ++i) CHECK_EQ(out[2 + i], static_cast<uint8_t>(100 + i));
}

TEST_CASE(ring_spsc_concurrent_transfers_every_byte_in_order) {
    // 单生产者单消费者并发：这是这个类存在的唯一理由，必须真的跑并发。
    // 判据是「每一个字节都到达且顺序不变」——用递增序列，读端校验连续性。
    constexpr int32_t kTotal = 1 << 20;      // 1 MiB
    AudioRing r(4096);
    std::atomic<bool> reader_ok{true};
    std::atomic<int64_t> read_count{0};

    std::thread consumer([&] {
        std::vector<uint8_t> buf(512);
        int64_t expect = 0;
        while (read_count.load(std::memory_order_relaxed) < kTotal) {
            const int32_t n = r.read(buf.data(), 512);
            for (int32_t i = 0; i < n; ++i) {
                if (buf[static_cast<size_t>(i)] != static_cast<uint8_t>(expect & 0xFF)) {
                    reader_ok.store(false, std::memory_order_relaxed);
                    return;
                }
                ++expect;
            }
            read_count.fetch_add(n, std::memory_order_relaxed);
        }
    });

    std::vector<uint8_t> chunk(512);
    int64_t produced = 0;
    while (produced < kTotal) {
        for (int32_t i = 0; i < 512; ++i)
            chunk[static_cast<size_t>(i)] = static_cast<uint8_t>((produced + i) & 0xFF);
        int32_t off = 0;
        while (off < 512) {
            const int32_t n = r.write(chunk.data() + off, 512 - off);
            off += n;                        // 写不进去就自旋重试，不 sleep
        }
        produced += 512;
    }
    consumer.join();

    CHECK(reader_ok.load());
    CHECK_EQ(read_count.load(), int64_t{kTotal});
    CHECK_EQ(r.consumed_bytes(), int64_t{kTotal});
}

int main() { return tiny_test_main(); }
