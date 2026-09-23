#include "platform/apple/inflight_gate.h"
#include "tiny_test.h"

#include <atomic>
#include <thread>
#include <vector>

using syp::platform::InflightGate;

TEST_CASE(inflight_gate_admits_up_to_capacity_then_refuses) {
    InflightGate g(3);
    CHECK(g.try_acquire());
    CHECK(g.try_acquire());
    CHECK(g.try_acquire());
    CHECK(!g.try_acquire());
    CHECK_EQ(g.in_flight(), 3);
    g.release();
    CHECK(g.try_acquire());
}

TEST_CASE(inflight_gate_limit_overload_caps_below_capacity) {
    InflightGate g(3);
    CHECK(g.try_acquire(2));
    CHECK(g.try_acquire(2));
    CHECK(!g.try_acquire(2));     // 本次上限 2：满
    CHECK(g.try_acquire());       // 构造容量 3 仍有一个
    CHECK(!g.try_acquire(5));     // 上限不超过构造容量
    CHECK_EQ(g.in_flight(), 3);
}

TEST_CASE(inflight_gate_is_safe_under_concurrent_acquire_release) {
    InflightGate g(3);
    std::atomic<int> max_seen{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < 20000; ++i) {
                if (g.try_acquire()) {
                    const int n = g.in_flight();
                    int m = max_seen.load();
                    while (n > m && !max_seen.compare_exchange_weak(m, n)) {}
                    g.release();
                }
            }
        });
    }
    for (auto& t : ts) t.join();
    CHECK(max_seen.load() <= 3);
    CHECK_EQ(g.in_flight(), 0);
}

int main() { return tiny_test_main(); }
