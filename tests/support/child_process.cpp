#include "support/child_process.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace reftest {

std::string quote_argument(const std::string& argument) {
  if (!argument.empty() && argument.find_first_of(" \t\"") == std::string::npos) return argument;
  std::string out = "\"";
  std::size_t backslashes = 0;
  for (const char c : argument) {
    if (c == '\\') {
      ++backslashes;
      continue;
    }
    if (c == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out += '"';
      backslashes = 0;
      continue;
    }
    out.append(backslashes, '\\');
    backslashes = 0;
    out += c;
  }
  out.append(backslashes * 2, '\\');
  out += '"';
  return out;
}

#if defined(_WIN32)

ChildProcess::~ChildProcess() {
  if (started_ && !exited_) kill();
  if (stdout_read_ != nullptr) CloseHandle(static_cast<HANDLE>(stdout_read_));
  if (stdin_write_ != nullptr) CloseHandle(static_cast<HANDLE>(stdin_write_));
  if (stderr_read_ != nullptr) CloseHandle(static_cast<HANDLE>(stderr_read_));
  if (process_.hThread != nullptr) CloseHandle(process_.hThread);
  if (process_.hProcess != nullptr) CloseHandle(process_.hProcess);
}

ref::Status ChildProcess::start(const std::string& executable,
                               const std::vector<std::string>& arguments) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  // The child's ends are inheritable; the parent's ends are not, so no child
  // ever inherits another child's pipe and no pipe outlives its process.
  HANDLE child_stdin_read = nullptr;    // child reads its stdin here
  HANDLE parent_stdin_write = nullptr;  // parent writes here
  HANDLE parent_stdout_read = nullptr;  // parent reads here
  HANDLE child_stdout_write = nullptr;  // child writes its stdout here
  HANDLE parent_stderr_read = nullptr;
  HANDLE child_stderr_write = nullptr;
  if (!CreatePipe(&child_stdin_read, &parent_stdin_write, &attributes, 0)) {
    return ref::Status::failure(ref::ErrorCode::IoFailure, "stdin pipe creation failed");
  }
  if (!SetHandleInformation(parent_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
    return ref::Status::failure(ref::ErrorCode::IoFailure, "stdin pipe configuration failed");
  }
  if (!CreatePipe(&parent_stdout_read, &child_stdout_write, &attributes, 0)) {
    return ref::Status::failure(ref::ErrorCode::IoFailure, "stdout pipe creation failed");
  }
  if (!SetHandleInformation(parent_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
    return ref::Status::failure(ref::ErrorCode::IoFailure, "stdout pipe configuration failed");
  }
  if (!CreatePipe(&parent_stderr_read, &child_stderr_write, &attributes, 0)) {
    return ref::Status::failure(ref::ErrorCode::IoFailure, "stderr pipe creation failed");
  }
  if (!SetHandleInformation(parent_stderr_read, HANDLE_FLAG_INHERIT, 0)) {
    return ref::Status::failure(ref::ErrorCode::IoFailure, "stderr pipe configuration failed");
  }

  std::string command_line = quote_argument(executable);
  for (const auto& argument : arguments) {
    command_line += ' ';
    command_line += quote_argument(argument);
  }
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = child_stdin_read;
  startup.hStdOutput = child_stdout_write;
  startup.hStdError = child_stderr_write;

  const BOOL created = CreateProcessA(executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_);
  CloseHandle(child_stdin_read);
  CloseHandle(child_stdout_write);
  CloseHandle(child_stderr_write);
  if (!created) {
    CloseHandle(parent_stdin_write);
    CloseHandle(parent_stdout_read);
    CloseHandle(parent_stderr_read);
    return ref::Status::failure(ref::ErrorCode::IoFailure, "child process could not be created");
  }
  stdin_write_ = parent_stdin_write;
  stdout_read_ = parent_stdout_read;
  stderr_read_ = parent_stderr_read;
  pid_ = static_cast<std::uint32_t>(process_.dwProcessId);
  started_ = true;
  return ref::Status::ok();
}

ref::Status ChildProcess::read_available(std::string& out) {
  std::array<char, 4096> chunk{};
  DWORD read = 0;
  if (!ReadFile(static_cast<HANDLE>(stdout_read_), chunk.data(), static_cast<DWORD>(chunk.size()), &read,
                nullptr)) {
    return ref::Status::failure(ref::ErrorCode::Truncated, "child stdout closed");
  }
  if (read == 0) return ref::Status::failure(ref::ErrorCode::Truncated, "child stdout closed");
  out.append(chunk.data(), read);
  return ref::Status::ok();
}

ref::Status ChildProcess::read_line(std::string& out) {
  while (true) {
    const std::size_t newline = buffer_.find('\n');
    if (newline != std::string::npos) {
      out = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);
      if (!out.empty() && out.back() == '\r') out.pop_back();
      return ref::Status::ok();
    }
    std::string chunk;
    const ref::Status read = read_available(chunk);
    if (read.is_failure()) {
      if (!buffer_.empty()) {
        out = buffer_;
        buffer_.clear();
        return ref::Status::ok();
      }
      return read;
    }
    buffer_ += chunk;
  }
}

void ChildProcess::drain_stderr() {
  if (stderr_read_ == nullptr) return;
  std::array<char, 2048> chunk{};
  DWORD available = 0;
  while (PeekNamedPipe(static_cast<HANDLE>(stderr_read_), nullptr, 0, nullptr, &available, nullptr) &&
         available > 0) {
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(stderr_read_), chunk.data(), static_cast<DWORD>(chunk.size()), &read,
                  nullptr) ||
        read == 0) {
      break;
    }
    stderr_text_.append(chunk.data(), read);
  }
}

ref::Status ChildProcess::wait_for_line(const std::string& prefix, std::string& out) {
  while (true) {
    const ref::Status read = read_line(out);
    if (read.is_failure()) {
      drain_stderr();
      std::string message = "child produced no line starting with '";
      message += prefix;
      message += "' before exiting";
      if (!stderr_text_.empty()) {
        message += "; stderr: ";
        message += stderr_text_;
      }
      return ref::Status::failure(read.code(), message);
    }
    if (out.rfind(prefix, 0) == 0) return ref::Status::ok();
  }
}

ref::Status ChildProcess::write_line(const std::string& line) {
  std::string data = line;
  data += "\r\n";
  DWORD written = 0;
  if (!WriteFile(static_cast<HANDLE>(stdin_write_), data.data(), static_cast<DWORD>(data.size()), &written,
                 nullptr)) {
    return ref::Status::failure(ref::ErrorCode::IoFailure, "child stdin write failed");
  }
  return ref::Status::ok();
}

ref::Status ChildProcess::close_stdin() {
  if (stdin_write_ == nullptr) return ref::Status::ok();
  CloseHandle(static_cast<HANDLE>(stdin_write_));
  stdin_write_ = nullptr;
  return ref::Status::ok();
}

bool ChildProcess::running() {
  if (!started_ || exited_) return false;
  const DWORD status = WaitForSingleObject(process_.hProcess, 0);
  if (status == WAIT_TIMEOUT) return true;
  exited_ = true;
  DWORD code = 0;
  GetExitCodeProcess(process_.hProcess, &code);
  exit_code_ = static_cast<int>(code);
  return false;
}

void ChildProcess::kill() {
  if (!started_ || exited_) return;
  TerminateProcess(process_.hProcess, 1);
  WaitForSingleObject(process_.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(process_.hProcess, &code);
  exit_code_ = static_cast<int>(code);
  exited_ = true;
}

int ChildProcess::wait() {
  if (!started_) return -1;
  if (exited_) return exit_code_;
  WaitForSingleObject(process_.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(process_.hProcess, &code);
  exit_code_ = static_cast<int>(code);
  exited_ = true;
  return exit_code_;
}

#else

ChildProcess::~ChildProcess() {
  if (started_ && !exited_) kill();
  if (stdout_fd_ >= 0) ::close(stdout_fd_);
  if (stdin_fd_ >= 0) ::close(stdin_fd_);
  if (stderr_fd_ >= 0) ::close(stderr_fd_);
}

ref::Status ChildProcess::start(const std::string& executable,
                               const std::vector<std::string>& arguments) {
  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
    return ref::Status::failure(ref::ErrorCode::IoFailure, "pipe creation failed");
  }
  const pid_t child = ::fork();
  if (child < 0) return ref::Status::failure(ref::ErrorCode::IoFailure, "fork failed");
  if (child == 0) {
    ::dup2(stdin_pipe[0], 0);
    ::dup2(stdout_pipe[1], 1);
    ::dup2(stderr_pipe[1], 2);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(stdin_pipe[0]);
  ::close(stdout_pipe[1]);
  ::close(stderr_pipe[1]);
  stdin_fd_ = stdin_pipe[1];
  stdout_fd_ = stdout_pipe[0];
  stderr_fd_ = stderr_pipe[0];
  pid_ = static_cast<std::uint32_t>(child);
  started_ = true;
  return ref::Status::ok();
}

ref::Status ChildProcess::read_available(std::string& out) {
  std::array<char, 4096> chunk{};
  const ssize_t read = ::read(stdout_fd_, chunk.data(), chunk.size());
  if (read <= 0) return ref::Status::failure(ref::ErrorCode::Truncated, "child stdout closed");
  out.append(chunk.data(), static_cast<std::size_t>(read));
  return ref::Status::ok();
}

ref::Status ChildProcess::read_line(std::string& out) {
  while (true) {
    const std::size_t newline = buffer_.find('\n');
    if (newline != std::string::npos) {
      out = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);
      if (!out.empty() && out.back() == '\r') out.pop_back();
      return ref::Status::ok();
    }
    std::string chunk;
    const ref::Status read = read_available(chunk);
    if (read.is_failure()) {
      if (!buffer_.empty()) {
        out = buffer_;
        buffer_.clear();
        return ref::Status::ok();
      }
      return read;
    }
    buffer_ += chunk;
  }
}

void ChildProcess::drain_stderr() {}

ref::Status ChildProcess::wait_for_line(const std::string& prefix, std::string& out) {
  while (true) {
    const ref::Status read = read_line(out);
    if (read.is_failure()) return read;
    if (out.rfind(prefix, 0) == 0) return ref::Status::ok();
  }
}

ref::Status ChildProcess::write_line(const std::string& line) {
  std::string data = line;
  data += "\n";
  const ssize_t written = ::write(stdin_fd_, data.data(), data.size());
  if (written != static_cast<ssize_t>(data.size())) {
    return ref::Status::failure(ref::ErrorCode::IoFailure, "child stdin write failed");
  }
  return ref::Status::ok();
}

ref::Status ChildProcess::close_stdin() {
  if (stdin_fd_ >= 0) {
    ::close(stdin_fd_);
    stdin_fd_ = -1;
  }
  return ref::Status::ok();
}

bool ChildProcess::running() {
  if (!started_ || exited_) return false;
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  if (result == 0) return true;
  exited_ = true;
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return false;
}

void ChildProcess::kill() {
  if (!started_ || exited_) return;
  ::kill(static_cast<pid_t>(pid_), SIGKILL);
  int status = 0;
  ::waitpid(static_cast<pid_t>(pid_), &status, 0);
  exited_ = true;
  exit_code_ = -1;
}

int ChildProcess::wait() {
  if (!started_) return -1;
  if (exited_) return exit_code_;
  int status = 0;
  ::waitpid(static_cast<pid_t>(pid_), &status, 0);
  exited_ = true;
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return exit_code_;
}

#endif

}  // namespace reftest
