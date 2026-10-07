#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

[[noreturn]] void fail(const std::string &message, int line)
{
    throw std::runtime_error("line " + std::to_string(line) + ": " + message);
}

void assert_true(bool value, const char *expression, int line)
{
    if (!value) {
        fail(std::string("expected true: ") + expression, line);
    }
}

void assert_equal_int(int expected, int actual, int line)
{
    if (expected != actual) {
        fail(
            "expected " + std::to_string(expected) + ", got " +
                std::to_string(actual),
            line);
    }
}

void assert_equal_string(const char *expected, const char *actual, int line)
{
    const std::string expected_string = expected != nullptr ? expected : "";
    const std::string actual_string = actual != nullptr ? actual : "";
    if (expected_string != actual_string) {
        fail(
            "expected string '" + expected_string + "', got '" +
                actual_string + "'",
            line);
    }
}

void assert_contains(const char *needle, const char *haystack, int line)
{
    const std::string source = haystack != nullptr ? haystack : "";
    if (needle == nullptr || source.find(needle) == std::string::npos) {
        fail(std::string("missing substring: ") + (needle != nullptr ? needle : ""), line);
    }
}

}  // namespace

#define FT_ASSERT_TRUE(value) assert_true((value), #value, __LINE__)
#define FT_ASSERT_FALSE(value) assert_true(!(value), "!(" #value ")", __LINE__)
#define FT_ASSERT_EQ_INT(expected, actual) assert_equal_int((expected), (actual), __LINE__)
#define FT_ASSERT_STREQ(expected, actual) assert_equal_string((expected), (actual), __LINE__)
#define FT_ASSERT_CONTAINS(needle, haystack) assert_contains((needle), (haystack), __LINE__)

#include "factory_core_test_cases.h"

int main()
{
    struct TestCase {
        const char *name;
        void (*run)();
    };
    const TestCase cases[] = {
        {"parser", factory_core_case_parser},
        {"state transitions", factory_core_case_state_transitions},
        {"manual decisions", factory_core_case_manual_decisions},
        {"cleanup guard", factory_core_case_cleanup_guard},
        {"thresholds", factory_core_case_thresholds},
        {"aggregation and reset", factory_core_case_aggregation_and_reset},
        {"JSON", factory_core_case_json},
    };

    int failures = 0;
    for (const TestCase &test : cases) {
        try {
            test.run();
            std::cout << "PASS: " << test.name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (sizeof(cases) / sizeof(cases[0])) - failures
              << "/" << sizeof(cases) / sizeof(cases[0])
              << " core tests passed\n";
    return failures == 0 ? 0 : 1;
}
