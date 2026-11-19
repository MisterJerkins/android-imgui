#pragma once

#include <cmath>
#include <format>
#include <string>
#include <vector>

// A unit test is a function registered by name; a failed expectation is recorded with its
// place and the test goes on, so one run shows everything that is wrong.
class Test {
public:
    struct Case {
        const char* name;
        void (*run)();
    };

    static std::vector<Case>& Cases();
    static void Fail(const char* file, int line, const std::string& what);
    // Runs every test, or those whose names contain `filter`, and returns how many failed.
    static int RunAll(const char* filter);

    // Puts a test on the list when the program starts.
    struct Registered {
        Registered(const char* name, void (*run)()) {
            Cases().push_back({name, run});
        }
    };

    template <class T>
    static std::string Show(const T& value) {
        if constexpr (std::formattable<T, char>) {
            return std::format("{}", value);
        } else {
            return "<value>";
        }
    }

private:
    static inline int failuresInCase_ = 0;
};

#define TEST_JOIN_INNER(left, right) left##right
#define TEST_JOIN(left, right) TEST_JOIN_INNER(left, right)

// `TEST(Suite, "a sentence that says what holds") { ... }`. The body is a member of a class
// derived from `Suite`, so what the suite declares is at hand in it.
#define TEST(suite, name) \
    struct TEST_JOIN(suite, __LINE__) : suite { \
        static void RunCase(); \
        static inline const ::Test::Registered registered{name, RunCase}; \
    }; \
    void TEST_JOIN(suite, __LINE__)::RunCase()

#define EXPECT(condition) \
    do { \
        if (!(condition)) \
            ::Test::Fail(__FILE__, __LINE__, #condition); \
    } while (false)

#define EXPECT_EQ(left, right) \
    do { \
        const auto left_ = (left); \
        const auto right_ = (right); \
        if (!(left_ == right_)) \
            ::Test::Fail(__FILE__, __LINE__, std::format("{} == {} ({} against {})", #left, #right, ::Test::Show(left_), ::Test::Show(right_))); \
    } while (false)

#define EXPECT_NEAR(left, right, tolerance) \
    do { \
        const double left_ = static_cast<double>(left); \
        const double right_ = static_cast<double>(right); \
        if (!(std::fabs(left_ - right_) <= (tolerance))) \
            ::Test::Fail(__FILE__, __LINE__, std::format("{} ~ {} ({} against {})", #left, #right, left_, right_)); \
    } while (false)
