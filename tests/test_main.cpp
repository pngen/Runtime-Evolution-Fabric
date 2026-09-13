#include "support/test_framework.hpp"

#include <cstring>
#include <string>

namespace reftest {

int g_checks = 0;
int g_failures = 0;
const char* g_current_test = "";

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registrar::Registrar(const char* suite, const char* name, std::function<void()> body) {
  registry().push_back(TestCase{suite, name, std::move(body)});
}

void report_failure(const char* file, int line, const std::string& message) {
  ++g_failures;
  std::fprintf(stderr, "FAIL %s :: %s\n  %s:%d\n  %s\n", g_current_test, message.c_str(), file, line,
               message.c_str());
  std::fflush(stderr);
}

void report_note(const std::string& message) {
  std::fprintf(stdout, "NOTE %s :: %s\n", g_current_test, message.c_str());
  std::fflush(stdout);
}

int run_all(int argc, char** argv, const char* binary_name) {
  std::string filter;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    } else if (argument == "--list") {
      list_only = true;
    } else if (argument == "--help") {
      std::printf("usage: %s [--filter <substring>] [--list]\n", binary_name);
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
      return 2;
    }
  }

  std::size_t selected = 0;
  std::size_t failed_cases = 0;
  for (const auto& test : registry()) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) continue;
    ++selected;
    if (list_only) {
      std::printf("%s\n", full.c_str());
      continue;
    }
    const int failures_before = g_failures;
    g_current_test = full.c_str();
    std::printf("RUN  %s\n", full.c_str());
    std::fflush(stdout);
    try {
      test.body();
    } catch (const std::exception& error) {
      report_failure(__FILE__, __LINE__, std::string("unhandled exception: ") + error.what());
    } catch (...) {
      report_failure(__FILE__, __LINE__, "unhandled non-standard exception");
    }
    if (g_failures > failures_before) {
      ++failed_cases;
      std::printf("FAIL %s\n", full.c_str());
    } else {
      std::printf("PASS %s\n", full.c_str());
    }
    std::fflush(stdout);
  }
  if (list_only) return 0;
  std::printf("\n%d checks, %d failures, %zu of %zu tests selected, %zu tests failed\n", g_checks,
              g_failures, selected, registry().size(), failed_cases);
  std::fflush(stdout);
  return g_failures == 0 ? 0 : 1;
}

}  // namespace reftest

int main(int argc, char** argv) { return ::reftest::run_all(argc, argv, "ref_tests"); }
