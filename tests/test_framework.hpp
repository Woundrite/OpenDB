#ifndef ATOMDB_TEST_FRAMEWORK_HPP
#define ATOMDB_TEST_FRAMEWORK_HPP

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace atomdb::test {

struct Test {
    std::string name;
    std::function<void()> body;
};

class Registry {
public:
    static Registry& instance() {
        static Registry r;
        return r;
    }
    void add(std::string name, std::function<void()> body) {
        tests_.push_back({std::move(name), std::move(body)});
    }
    const std::vector<Test>& tests() const noexcept { return tests_; }

private:
    std::vector<Test> tests_;
};

struct FailureCount {
    static int& ref() {
        static int n = 0;
        return n;
    }
};

inline void fail(std::string_view file, int line, std::string_view msg) {
    std::fprintf(stderr, "  FAIL %.*s:%d: %.*s\n",
                 static_cast<int>(file.size()), file.data(), line,
                 static_cast<int>(msg.size()), msg.data());
    ++FailureCount::ref();
}

inline int run_all() {
    int passed = 0;
    int failed = 0;
    auto it = Registry::instance().tests().begin();
    for (; it != Registry::instance().tests().end(); ++it) {
        const auto& t = *it;
        int before = FailureCount::ref();
        std::printf("[ RUN      ] %s\n", t.name.c_str());
        std::fflush(stdout);
        try {
            t.body();
        } catch (const std::exception& e) {
            fail("unknown", 0, std::string("uncaught exception: ") + e.what());
        } catch (...) {
            fail("unknown", 0, "uncaught unknown exception");
        }
        int after = FailureCount::ref();
        if (after == before) {
            std::printf("[       OK ] %s\n", t.name.c_str());
            ++passed;
        } else {
            std::printf("[  FAILED  ] %s\n", t.name.c_str());
            ++failed;
        }
        std::fflush(stdout);
    }
    std::printf("\n==== PASSED %d / FAILED %d ====\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

} // namespace atomdb::test

#define ATOMDB_TEST_NAME_2(name, line) atomdb_test_##name##_##line
#define ATOMDB_TEST_NAME_1(name, line) ATOMDB_TEST_NAME_2(name, line)
#define ATOMDB_TEST_NAME(name) ATOMDB_TEST_NAME_1(name, __LINE__)

#define TEST(name)                                                              \
    static void ATOMDB_TEST_NAME(name)();                                       \
    static int ATOMDB_TEST_NAME(_reg_##name) = [] {                             \
        ::atomdb::test::Registry::instance().add(#name,                         \
            ATOMDB_TEST_NAME(name));                                            \
        return 0;                                                              \
    }();                                                                       \
    static void ATOMDB_TEST_NAME(name)()

#define EXPECT(expr)                                                           \
    do {                                                                       \
        if (!(expr)) {                                                         \
            ::atomdb::test::fail(__FILE__, __LINE__, #expr);                    \
        }                                                                      \
    } while (0)

#define EXPECT_EQ(a, b)                                                        \
    do {                                                                       \
        auto&& _a = (a);                                                       \
        auto&& _b = (b);                                                       \
        if (!(_a == _b)) {                                                     \
            ::atomdb::test::fail(__FILE__, __LINE__,                           \
                std::string(#a " == " #b " failed: got ") +                     \
                std::to_string(_a) + " expected " + std::to_string(_b));        \
        }                                                                      \
    } while (0)

#define EXPECT_THROWS(expr)                                                    \
    do {                                                                       \
        bool _threw = false;                                                   \
        try { (void)(expr); }                                                  \
        catch (...) { _threw = true; }                                         \
        if (!_threw) {                                                         \
            ::atomdb::test::fail(__FILE__, __LINE__,                           \
                std::string(#expr " did not throw"));                          \
        }                                                                      \
    } while (0)

#endif // ATOMDB_TEST_FRAMEWORK_HPP
