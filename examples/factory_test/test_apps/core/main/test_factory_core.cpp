#include <cstring>

#include "unity.h"

#define FT_ASSERT_TRUE(value) TEST_ASSERT_TRUE(value)
#define FT_ASSERT_FALSE(value) TEST_ASSERT_FALSE(value)
#define FT_ASSERT_EQ_INT(expected, actual) TEST_ASSERT_EQUAL_INT((expected), (actual))
#define FT_ASSERT_STREQ(expected, actual) TEST_ASSERT_EQUAL_STRING((expected), (actual))
#define FT_ASSERT_CONTAINS(needle, haystack) TEST_ASSERT_NOT_NULL(std::strstr((haystack), (needle)))

#include "factory_core_test_cases.h"

TEST_CASE("factory command parser accepts the line protocol", "[factory][parser]")
{
    factory_core_case_parser();
}

TEST_CASE("factory status transitions enforce the state machine", "[factory][state]")
{
    factory_core_case_state_transitions();
}

TEST_CASE("manual decisions cover retry and timeout", "[factory][state]")
{
    factory_core_case_manual_decisions();
}

TEST_CASE("cleanup guard invokes cleanup exactly once", "[factory][cleanup]")
{
    factory_core_case_cleanup_guard();
}

TEST_CASE("factory thresholds reject unsafe measurements", "[factory][threshold]")
{
    factory_core_case_thresholds();
}

TEST_CASE("factory aggregation and reset preserve metadata", "[factory][result]")
{
    factory_core_case_aggregation_and_reset();
}

TEST_CASE("factory JSON escapes strings and validates measurements", "[factory][json]")
{
    factory_core_case_json();
}

extern "C" void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
