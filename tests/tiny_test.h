// tiny_test.h — 单头测试框架：静态注册用例，失败不中断其它用例
#pragma once

#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace tiny_test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int& fails_in_case() { static int n = 0; return n; }

template<typename T>
std::string stringify(const T& v) {
    std::ostringstream oss;
    if constexpr (requires { oss << v; }) {
        oss << v;
        return oss.str();
    } else {
        return "<unprintable>";
    }
}

inline void fail(const char* file, int line, const char* expr, const std::string& extra = {}) {
    std::fprintf(stdout, "  FAIL %s:%d  %s", file, line, expr);
    if (!extra.empty()) std::fprintf(stdout, "  %s", extra.c_str());
    std::fprintf(stdout, "\n");
    ++fails_in_case();
}

inline int run() {
    // 行缓冲：裸跑 / ctest 场景下 stdout 默认块缓冲，某条用例挂死时
    // 连它自己的 "RUN xxx" 都看不到（诊断一次真实死锁时才发现这一点，
    // 当时靠 script -q /dev/null 包一层 pty 才看见）。改成行缓冲后
    // 每条 RUN/PASS/FAIL 一落地就冲出去，挂死时至少能看清卡在哪条用例。
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    int passed = 0;
    int failed = 0;
    for (const Case& c : registry()) {
        fails_in_case() = 0;
        std::printf("RUN  %s\n", c.name);
        c.fn();
        if (fails_in_case() > 0) {
            std::printf("FAIL %s\n", c.name);
            ++failed;
        } else {
            std::printf("PASS %s\n", c.name);
            ++passed;
        }
    }
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace tiny_test

inline int tiny_test_main() { return tiny_test::run(); }

#define TEST_CASE(name)                                                        \
    static void test_##name();                                                 \
    static const ::tiny_test::Registrar registrar_##name{#name, &test_##name}; \
    static void test_##name()

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            ::tiny_test::fail(__FILE__, __LINE__, #expr);                      \
        }                                                                      \
    } while (0)

// 第二参做成可变参：花括号初始化里的逗号不能被 () 保护，否则 CHECK_EQ(x, Range{0,1}) 会被拆成 3 个实参。
#define CHECK_EQ(a, ...)                                                       \
    do {                                                                       \
        const auto& _tt_a = (a);                                               \
        const auto& _tt_b = (__VA_ARGS__);                                     \
        if (!(_tt_a == _tt_b)) {                                               \
            ::tiny_test::fail(__FILE__, __LINE__, #a " == " #__VA_ARGS__,      \
                "left=" + ::tiny_test::stringify(_tt_a) +                      \
                " right=" + ::tiny_test::stringify(_tt_b));                    \
        }                                                                      \
    } while (0)

#define REQUIRE(expr)                                                          \
    do {                                                                       \
        if (!(expr)) {                                                         \
            ::tiny_test::fail(__FILE__, __LINE__, #expr);                      \
            return;                                                            \
        }                                                                      \
    } while (0)
