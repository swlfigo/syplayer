// test_packet_digest.cpp — 验证差分比对的口径本身站得住。
//
// 参照比对的可信度都压在这条上：FFmpeg 走 file: 协议读同一文件两次，
// 结果必须完全相同。不成立的话所有比对都会随机假红，口径要重新设计。
//
// 额外两条（sha256_of_is_a_real_digest / diff_report_catches_a_corrupted_payload_byte）
// 是补上一个洞：原先四条用例即使把 sha256_of 换成 memset(out, 0, 32) 也照样全绿，
// 因为素材本身逐字节相同——没有一条用例真正证明「payload 差异会冒到 diff_report 顶层」。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "packet_digest.h"
#include "packet_digest_internal.h"
#include "tiny_test.h"

using namespace syp::probe;

namespace {

std::string fixture(const char* name) {
    const char* dir = std::getenv("SYP_FIXTURE_DIR");
    return std::string(dir ? dir : "") + "/" + name;
}

uint32_t read_be32(std::ifstream& f) {
    unsigned char b[4] = {};
    f.read(reinterpret_cast<char*>(b), 4);
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
}

uint64_t read_be64(std::ifstream& f) {
    unsigned char b[8] = {};
    f.read(reinterpret_cast<char*>(b), 8);
    uint64_t v = 0;
    for (unsigned char c : b) v = (v << 8) | c;
    return v;
}

// 在顶层 box 里找 mdat，返回它的数据区间 [起点, 终点)。mp4 是 ISO base media box
// 结构：每个 box 开头 4 字节大端 size + 4 字节 type；size==1 表示后面紧跟 8 字节大端
// largesize；size==0 表示这个 box 一直到文件末尾。跑时定位、不依赖任何固定素材布局，
// 这样无论 gen-fixtures.sh 用哪个种子生成，都能找到正确的 mdat 数据区。
std::optional<std::pair<uint64_t, uint64_t>> find_mdat_range(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    f.seekg(0, std::ios::end);
    const auto end_pos = f.tellg();
    if (end_pos < 0) return std::nullopt;
    const uint64_t file_size = static_cast<uint64_t>(end_pos);

    uint64_t pos = 0;
    while (pos + 8 <= file_size) {
        f.seekg(static_cast<std::streamoff>(pos));
        uint64_t box_size = read_be32(f);
        char type[5] = {};
        f.read(type, 4);
        uint64_t header_size = 8;
        if (box_size == 1) {
            box_size = read_be64(f);
            header_size = 16;
        } else if (box_size == 0) {
            box_size = file_size - pos;
        }
        if (box_size < header_size || pos + box_size > file_size) break;
        if (std::string(type) == "mdat") {
            return std::make_pair(pos + header_size, pos + box_size);
        }
        pos += box_size;
    }
    return std::nullopt;
}

bool copy_file(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return static_cast<bool>(out);
}

bool flip_byte_at(const std::string& path, uint64_t offset) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) return false;
    f.seekg(static_cast<std::streamoff>(offset));
    char byte = 0;
    f.read(&byte, 1);
    if (!f) return false;
    byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0xFFu);
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(&byte, 1);
    return static_cast<bool>(f);
}

}  // namespace

TEST_CASE(file_protocol_is_deterministic) {
    const DemuxOptions opt;
    const DemuxResult a = demux_file(fixture("moovend.mp4"), opt, {});
    REQUIRE(a.error_stage.empty());
    REQUIRE(a.packets.size() > 100);

    const DemuxResult b = demux_file(fixture("moovend.mp4"), opt, {});
    CHECK_EQ(diff_report(a, b), std::string{});
}

TEST_CASE(faststart_and_moovend_carry_same_packets) {
    // 两份素材由 -c copy 重排而来，媒体数据逐字节相同，
    // packet 序列必然一致。这条同时验证了素材生成脚本没搞错。
    //
    // pos（packet 在文件里的字节偏移）例外：faststart 把 moov 挪到了文件头部，
    // moovend 的 moov 在文件尾部——同一段媒体数据在两份文件里天然处于不同的字节
    // 偏移，这是容器布局的差异，不是「packet 序列不一致」。pos 要抓的是「同一份
    // 文件、两条协议路径（file: vs 我们的下载层）」这种场景下的字节错位，跟这里
    // 比较两份不同布局的文件是两回事，所以比较前把 pos 清零，其余字段原样比对。
    const DemuxOptions opt;
    DemuxResult end = demux_file(fixture("moovend.mp4"), opt, {});
    DemuxResult fast = demux_file(fixture("faststart.mp4"), opt, {});
    REQUIRE(end.error_stage.empty());
    REQUIRE(fast.error_stage.empty());
    for (PacketDigest& p : end.packets) p.pos = 0;
    for (PacketDigest& p : fast.packets) p.pos = 0;
    CHECK_EQ(diff_report(end, fast), std::string{});
}

TEST_CASE(seek_is_deterministic) {
    const DemuxOptions opt;
    const std::vector<SeekPlan> plan{{5000000, 40}, {20000000, 40}, {1000000, 40}};
    const DemuxResult a = demux_file(fixture("moovend.mp4"), opt, plan);
    REQUIRE(a.error_stage.empty());
    REQUIRE(a.packets.size() == 120);

    const DemuxResult b = demux_file(fixture("moovend.mp4"), opt, plan);
    CHECK_EQ(diff_report(a, b), std::string{});
}

TEST_CASE(diff_report_catches_a_flipped_byte) {
    // 口径必须真的能抓到 payload 差异，否则「全等」毫无意义。
    // 这条验的是 operator== 覆盖了 payload_sha256 数组成员；真正的
    // 「摘要读了 payload、payload 差异会冒到顶层」由下面两条新用例验证。
    const DemuxOptions opt;
    DemuxResult a = demux_file(fixture("moovend.mp4"), opt, {});
    REQUIRE(a.packets.size() > 10);
    DemuxResult b = a;
    b.packets[5].payload_sha256[0] ^= 0xFF;
    CHECK(!diff_report(a, b).empty());
}

TEST_CASE(packet_digest_carries_a_real_duration) {
    // "packet 摘要漏了 pkt->duration" 的反向自检：先证明这个
    // 字段确实读到了真实数据（不是恒为 0 的空实现），再证明 diff_report
    // 真的会因为它不同而报红——两者缺一都证明不了「加这个字段有意义」。
    const DemuxOptions opt;
    const DemuxResult a = demux_file(fixture("moovend.mp4"), opt, {});
    REQUIRE(a.error_stage.empty());
    REQUIRE(a.packets.size() > 10);

    bool any_positive_duration = false;
    for (const PacketDigest& p : a.packets) {
        if (p.duration > 0) { any_positive_duration = true; break; }
    }
    CHECK(any_positive_duration);

    DemuxResult b = a;
    b.packets[5].duration += 1;
    const std::string report = diff_report(a, b);
    CHECK(!report.empty());
    // 顺带确认差异真的是从 duration 冒出来的，不是巧合撞上别的字段判断。
    CHECK(report.find("duration") != std::string::npos);
}

TEST_CASE(sha256_of_is_a_real_digest) {
    // 已知向量：证明 sha256_of 确实在算 SHA-256，且确实读了入参内容——
    // 不是譬如 memset(out, 0, 32) 之类的空实现。
    uint8_t h[32];
    syp::probe::detail::sha256_of(reinterpret_cast<const uint8_t*>("abc"), 3, h);
    CHECK_EQ(syp::probe::detail::hex32(h),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    uint8_t h2[32];
    syp::probe::detail::sha256_of(reinterpret_cast<const uint8_t*>("abd"), 3, h2);
    CHECK(std::memcmp(h, h2, 32) != 0);
}

TEST_CASE(diff_report_catches_a_corrupted_payload_byte) {
    // 端到端、不可替代的一条：真的改文件里落在 mdat 数据区的一个字节，
    // 证明摘要读了真实 payload，且 payload 差异真的会冒到 diff_report 顶层。
    // faststart vs moovend 那条证不了这件事——那两份文件的 packet payload
    // 本来就逐字节相同。
    const std::string src = fixture("moovend.mp4");
    const std::string corrupt_path = fixture("moovend_corrupt_test.mp4");

    // RAII 兜底删除：copy_file 复制的是整份 moovend.mp4（约 29MB），
    // 下面任何一条 REQUIRE 提前 return 都不该在 fixture 目录里留一份
    // 孤儿副本——照抄 test_apple_http_backend.cpp 的 TempFileRemover。
    struct TempFileRemover {
        std::string path;
        ~TempFileRemover() { if (!path.empty()) std::remove(path.c_str()); }
    } temp_file{corrupt_path};

    REQUIRE(copy_file(src, corrupt_path));

    const auto mdat = find_mdat_range(corrupt_path);
    REQUIRE(mdat.has_value());
    REQUIRE(mdat->second > mdat->first);
    // 选 mdat 数据区中点：足够深入媒体数据本体，不挨着 box 头，
    // 也远离文件头部（ftyp）和 moovend 布局里位于文件尾部的 moov。
    const uint64_t offset = mdat->first + (mdat->second - mdat->first) / 2;
    REQUIRE(offset < mdat->second);
    REQUIRE(flip_byte_at(corrupt_path, offset));

    const DemuxOptions opt;
    const DemuxResult a = demux_file(src, opt, {});
    const DemuxResult b = demux_file(corrupt_path, opt, {});
    // 提前删（不必等函数返回，TempFileRemover 析构时再删是安全的空操作，
    // std::remove 对不存在的路径直接失败返回，不影响其它断言）——保持
    // 原来"两次 demux 之后就尽快清"的时序，RAII 只是兜底,不是替代。
    std::remove(corrupt_path.c_str());
    temp_file.path.clear();

    REQUIRE(a.error_stage.empty());
    REQUIRE(b.error_stage.empty());
    // 只改了媒体数据里的一个字节，不该动到容器结构：packet 数量必须不变。
    REQUIRE(a.packets.size() == b.packets.size());

    bool found_diff = false;
    for (size_t i = 0; i < a.packets.size(); ++i) {
        if (a.packets[i] == b.packets[i]) continue;
        found_diff = true;
        // 差异必须落在 payload 摘要上，元数据（含新加的 pos）必须原样不变——
        // 这才证明摘要确实覆盖了 payload 本体，而不是只在比元数据。
        CHECK_EQ(a.packets[i].stream_index, b.packets[i].stream_index);
        CHECK_EQ(a.packets[i].pts, b.packets[i].pts);
        CHECK_EQ(a.packets[i].dts, b.packets[i].dts);
        CHECK_EQ(a.packets[i].pos, b.packets[i].pos);
        CHECK_EQ(a.packets[i].size, b.packets[i].size);
        CHECK_EQ(a.packets[i].flags, b.packets[i].flags);
        CHECK_EQ(a.packets[i].duration, b.packets[i].duration);
        CHECK(std::memcmp(a.packets[i].payload_sha256, b.packets[i].payload_sha256, 32) != 0);
        break;
    }
    CHECK(found_diff);
    CHECK(!diff_report(a, b).empty());
}

TEST_CASE(diff_report_rejects_when_reference_failed) {
    // "两边都失败被判成相等"这个 bug 在 packet 层面的守卫，从
    // 立起来那天起就没有任何测试锁死过它——上面 8 处既有调用点全部构造
    // 在参照侧 error_stage 为空的前提下，没有一条测「参照失败」这个场景
    // 本身。曾经删掉 diff_report 的参照失败硬底线（现在在
    // packet_digest_internal.h::diff_failure_hardlines 里）之后 15/15
    // ctest 全绿，probe_e2e 也没红，正是因为这个洞。补两条组合，跟
    // test_frame_digest.cpp::diff_frames_rejects_when_reference_failed
    // 同款结构。
    DemuxResult failed_ref;
    failed_ref.streams.push_back(StreamSummary{});
    failed_ref.packets.push_back(PacketDigest{});
    failed_ref.averror     = 0;   // 故意跟下面 ok 的 averror 对齐——见下方注释
    failed_ref.error_stage = "模拟失败: open_input";

    // 参照失败、被测「看起来完全正常」：streams/packets/averror 全部跟
    // failed_ref 逐字段相同（直接拷贝再清空 error_stage），只有
    // error_stage 不同。这是唯一真正单独依赖「参照失败」硬底线本身的
    // 场景——averror 相等、streams 相等、packets 也相等，后面任何一条
    // 「XX 不同」分支都不成立，只有这条硬底线能让 diff_report 返回非空。
    // 「参照失败但两侧 packets 恰好相等」正是这个构造。
    DemuxResult ok = failed_ref;
    ok.error_stage.clear();
    CHECK(!diff_report(failed_ref, ok).empty());

    // 曾经踩过的字面场景：参照失败、被测报的是*同一个*失败
    // （error_stage/averror/streams/packets 逐字段相等）。这条实际由
    // 「被测路径失败」那条独立检查守住（跟参照是否失败无关）——跟上面
    // 那条不是同一处代码在起作用，但同样是要堵的场景，
    // 钉住它能防住"被测失败"那条检查以后被改成相对判断（比如"跟参照
    // 不一样才算失败"）这类回归。
    DemuxResult failed_actual = failed_ref;
    CHECK(!diff_report(failed_ref, failed_actual).empty());
}

int main() { return tiny_test_main(); }
