// Runtime Evolution Fabric - real OS child processes for the multiprocess
// proofs. No shell is involved: the child is created directly with redirected
// stdin, stdout and stderr pipes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ref/support.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace reftest {

// Quotes one argument for the Windows command line.
[[nodiscard]] std::string quote_argument(const std::string& argument);

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] ref::Status start(const std::string& executable,
                                  const std::vector<std::string>& arguments);
  // Reads one line (without the newline). Returns Truncated at end of output.
  [[nodiscard]] ref::Status read_line(std::string& out);
  // Reads lines until one starts with the prefix. Fails when the child exits
  // first, so a broken child can never hang a test.
  [[nodiscard]] ref::Status wait_for_line(const std::string& prefix, std::string& out);
  [[nodiscard]] ref::Status write_line(const std::string& line);
  [[nodiscard]] ref::Status close_stdin();
  [[nodiscard]] bool running();
  // Terminates the process (used only where process death is under test).
  void kill();
  // Waits for natural exit and returns the exit code.
  [[nodiscard]] int wait();
  // Exit code of a process that has already been reaped, -1 when unknown.
  [[nodiscard]] int exit_code() const noexcept { return exit_code_; }
  [[nodiscard]] std::uint32_t process_id() const noexcept { return pid_; }
  [[nodiscard]] bool started() const noexcept { return started_; }
  [[nodiscard]] const std::string& stderr_text() const noexcept { return stderr_text_; }
  void drain_stderr();

 private:
  [[nodiscard]] ref::Status read_available(std::string& out);

#if defined(_WIN32)
  PROCESS_INFORMATION process_{};
  void* stdout_read_{nullptr};
  void* stdin_write_{nullptr};
  void* stderr_read_{nullptr};
#else
  int pid_{-1};
  int stdout_fd_{-1};
  int stdin_fd_{-1};
  int stderr_fd_{-1};
#endif
  std::uint32_t pid_{0};
  bool started_{false};
  bool exited_{false};
  int exit_code_{-1};
  std::string buffer_{};
  std::string stderr_text_{};
};

}  // namespace reftest
