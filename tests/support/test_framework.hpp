// Runtime Evolution Fabric - minimal deterministic test framework.
//
// No timeouts are used anywhere: a test either completes or the process fails.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace reftest {

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

std::vector<TestCase>& registry();
void report_failure(const char* file, int line, const std::string& message);
void report_note(const std::string& message);
int run_all(int argc, char** argv, const char* binary_name);

extern int g_checks;
extern int g_failures;
extern const char* g_current_test;

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body);
};

}  // namespace reftest

#define REF_TEST(suite_name, test_name)                                                        \
  static void suite_name##_##test_name##_body();                                               \
  static const ::reftest::Registrar suite_name##_##test_name##_registrar(                       \
      #suite_name, #test_name, suite_name##_##test_name##_body);                                \
  static void suite_name##_##test_name##_body()

#define REF_CHECK(condition)                                                                   \
  do {                                                                                         \
    ++::reftest::g_checks;                                                                     \
    if (!(condition)) {                                                                         \
      ::reftest::report_failure(__FILE__, __LINE__, "check failed: " #condition);               \
    }                                                                                          \
  } while (0)

#define REF_CHECK_MSG(condition, message)                                                      \
  do {                                                                                         \
    ++::reftest::g_checks;                                                                     \
    if (!(condition)) {                                                                         \
      ::reftest::report_failure(__FILE__, __LINE__, std::string("check failed: " #condition) +  \
                                                          " (" + (message) + ")");              \
    }                                                                                          \
  } while (0)

#define REF_CHECK_EQ(actual, expected)                                                         \
  do {                                                                                         \
    ++::reftest::g_checks;                                                                     \
    const auto& ref_actual_value = (actual);                                                    \
    const auto& ref_expected_value = (expected);                                                \
    if (!(ref_actual_value == ref_expected_value)) {                                            \
      ::reftest::report_failure(__FILE__, __LINE__,                                             \
                                std::string("expected " #actual " == " #expected));            \
    }                                                                                          \
  } while (0)

#define REF_CHECK_STATUS_OK(expression)                                                        \
  do {                                                                                         \
    ++::reftest::g_checks;                                                                     \
    const ::ref::Status ref_status_value = (expression);                                        \
    if (ref_status_value.is_failure()) {                                                        \
      ::reftest::report_failure(__FILE__, __LINE__,                                             \
                                std::string(#expression " failed: ") +                          \
                                    ref_status_value.to_string());                              \
    }                                                                                          \
  } while (0)

#define REF_CHECK_STATUS_CODE(expression, expected_code)                                       \
  do {                                                                                         \
    ++::reftest::g_checks;                                                                     \
    const ::ref::Status ref_status_value = (expression);                                        \
    if (ref_status_value.code() != (expected_code)) {                                           \
      ::reftest::report_failure(                                                               \
          __FILE__, __LINE__,                                                                  \
          std::string(#expression " returned ") + ref_status_value.to_string() + " expected " + \
              ::ref::to_string(expected_code));                                                \
    }                                                                                          \
  } while (0)

#define REF_CHECK_OUTCOME(expression, expected)                                                \
  do {                                                                                         \
    ++::reftest::g_checks;                                                                     \
    const auto ref_outcome_value = (expression);                                               \
    if (ref_outcome_value != (expected)) {                                                     \
      ::reftest::report_failure(__FILE__, __LINE__,                                             \
                                std::string(#expression " outcome mismatch"));                  \
    }                                                                                          \
  } while (0)

#define REF_NOTE(message) ::reftest::report_note(message)
