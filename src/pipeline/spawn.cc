// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/spawn.h"

#include <string>
#include <utility>
#include <vector>

#include "config/build_config.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

#if BUILD_FLAG(IS_OS_WIN)
#include <windows.h>
#else
#include <errno.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if BUILD_FLAG(IS_OS_APPLE)
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif

namespace pipeline {

namespace {

#if BUILD_FLAG(IS_OS_WIN)

// Minimal Windows quoting: wrap arguments containing whitespace or
// quotes, escaping interior quotes and trailing backslashes.
void append_quoted(std::string& command, const std::string& arg) {
  bool quote =
      arg.empty() || arg.find_first_of(" \t\n\v\"") != std::string::npos;
  if (!quote) {
    command += arg;
    return;
  }
  command += '"';
  usize backslashes = 0;
  for (char c : arg) {
    if (c == '\\') {
      ++backslashes;
      continue;
    }
    if (c == '"') {
      command.append(backslashes * 2 + 1, '\\');
    } else {
      command.append(backslashes, '\\');
    }
    backslashes = 0;
    command += c;
  }
  command.append(backslashes * 2, '\\');
  command += '"';
}

std::string command_line(const std::vector<std::string>& argv) {
  std::string command;
  for (usize i = 0; i < argv.size(); ++i) {
    if (i > 0) {
      command += ' ';
    }
    append_quoted(command, argv[i]);
  }
  return command;
}

// Waits for the child and reads its exit status, closing the handle either
// way. A Windows process a signal ended is reported as an exit status with
// the exception bit set rather than as a signal.
base::Result<SpawnOutcome, SpawnError> finish_wait(HANDLE process) {
  if (::WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) {
    ::CloseHandle(process);
    return base::make_err(SpawnError::WaitFailed);
  }
  DWORD code = 0;
  const BOOL ok = ::GetExitCodeProcess(process, &code);
  ::CloseHandle(process);
  if (ok == 0) {
    return base::make_err(SpawnError::WaitFailed);
  }
  return base::make_ok(SpawnOutcome{.code = static_cast<i32>(code),
                                    .signaled = code >= 0xC0000000u});
}

#elif !BUILD_FLAG(IS_OS_ASMJS)

std::vector<char*> child_argv(const std::vector<std::string>& argv) {
  std::vector<char*> args;
  args.reserve(argv.size() + 1);
  for (const std::string& arg : argv) {
    // posix_spawnp takes a mutable argv and does not write through it.
    // ast-grep-ignore: no-const-cast
    args.push_back(const_cast<char*>(arg.c_str()));
  }
  args.push_back(nullptr);
  return args;
}

char** child_env() {
#if BUILD_FLAG(IS_OS_APPLE)
  return *_NSGetEnviron();
#else
  return ::environ;
#endif
}

// Waits for the child and reads its status as an exit or a signal. A status
// that reports neither cannot be read as a result of the child, so it is
// reported as the wait failing rather than as an exit code that was never
// there.
base::Result<SpawnOutcome, SpawnError> finish_wait(pid_t pid) {
  i32 status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    return base::make_err(SpawnError::WaitFailed);
  }
  if (WIFSIGNALED(status)) {
    return base::make_ok(SpawnOutcome{
        .code = static_cast<i32>(WTERMSIG(status)), .signaled = true});
  }
  if (!WIFEXITED(status)) {
    return base::make_err(SpawnError::WaitFailed);
  }
  return base::make_ok(
      SpawnOutcome{.code = static_cast<i32>(WEXITSTATUS(status))});
}

#endif

}  // namespace

base::Result<SpawnOutcome, SpawnError> run_command(
    const std::vector<std::string>& argv) {
  if (argv.empty()) {
    return base::make_err(SpawnError::EmptyArgv);
  }
#if BUILD_FLAG(IS_OS_ASMJS)
  // WebAssembly has no process model.
  (void)argv;
  return base::make_err(SpawnError::SpawnFailed);
#elif BUILD_FLAG(IS_OS_WIN)
  std::string command = command_line(argv);
  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info = {};
  if (::CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &startup, &info) == 0) {
    return base::make_err(SpawnError::SpawnFailed);
  }
  ::CloseHandle(info.hThread);
  return finish_wait(info.hProcess);
#else
  std::vector<char*> args = child_argv(argv);
  pid_t pid = -1;
  // posix_spawnp resolves file through PATH; the child inherits
  // this environment explicitly for portability.
  if (::posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(),
                     child_env()) != 0) {
    return base::make_err(SpawnError::SpawnFailed);
  }
  return finish_wait(pid);
#endif
}

base::Result<CapturedSpawn, SpawnError> run_command_capture(
    const std::vector<std::string>& argv) {
  if (argv.empty()) {
    return base::make_err(SpawnError::EmptyArgv);
  }
#if BUILD_FLAG(IS_OS_ASMJS)
  // WebAssembly has no process model.
  (void)argv;
  return base::make_err(SpawnError::SpawnFailed);
#elif BUILD_FLAG(IS_OS_WIN)
  SECURITY_ATTRIBUTES attributes = {};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (::CreatePipe(&read_end, &write_end, &attributes, 0) == 0) {
    return base::make_err(SpawnError::SpawnFailed);
  }
  // The child must not inherit the read end, which is this process's.
  if (::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0) == 0) {
    ::CloseHandle(read_end);
    ::CloseHandle(write_end);
    return base::make_err(SpawnError::SpawnFailed);
  }
  std::string command = command_line(argv);
  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = write_end;
  startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION info = {};
  if (::CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                       nullptr, nullptr, &startup, &info) == 0) {
    ::CloseHandle(read_end);
    ::CloseHandle(write_end);
    return base::make_err(SpawnError::SpawnFailed);
  }
  ::CloseHandle(info.hThread);
  ::CloseHandle(write_end);
  u64 total = 0;
  char buffer[65536];
  DWORD read = 0;
  while (::ReadFile(read_end, buffer, sizeof buffer, &read, nullptr) != 0) {
    total += static_cast<u64>(read);
  }
  ::CloseHandle(read_end);
  base::Result<SpawnOutcome, SpawnError> ending = finish_wait(info.hProcess);
  if (ending.is_err()) {
    return base::make_err(std::move(ending).unwrap_err());
  }
  return base::make_ok(CapturedSpawn{.outcome = std::move(ending).unwrap(),
                                     .output_bytes = total});
#else
  i32 pipefd[2];
  if (::pipe(pipefd) != 0) {
    return base::make_err(SpawnError::SpawnFailed);
  }
  posix_spawn_file_actions_t actions;
  if (::posix_spawn_file_actions_init(&actions) != 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return base::make_err(SpawnError::SpawnFailed);
  }
  // The child's standard output is the write end, renamed onto file
  // descriptor 1; the read end is this process's and does not travel.
  ::posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  ::posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  std::vector<char*> args = child_argv(argv);
  pid_t pid = -1;
  char** env = child_env();
  const i32 spawn_result =
      ::posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), env);
  ::posix_spawn_file_actions_destroy(&actions);
  ::close(pipefd[1]);
  if (spawn_result != 0) {
    ::close(pipefd[0]);
    return base::make_err(SpawnError::SpawnFailed);
  }
  u64 total = 0;
  char buffer[65536];
  bool drained = true;
  for (;;) {
    const isize read = ::read(pipefd[0], buffer, sizeof buffer);
    if (read > 0) {
      total += static_cast<u64>(read);
      continue;
    }
    if (read == 0 || errno != EINTR) {
      drained = read == 0;
      break;
    }
  }
  ::close(pipefd[0]);
  base::Result<SpawnOutcome, SpawnError> ending = finish_wait(pid);
  if (!drained) {
    // The child's bytes were lost before the end of the pipe, so the count
    // is one of what was actually written.
    if (ending.is_ok()) {
      return base::make_err(SpawnError::WaitFailed);
    }
    return base::make_err(std::move(ending).unwrap_err());
  }
  if (ending.is_err()) {
    return base::make_err(std::move(ending).unwrap_err());
  }
  return base::make_ok(CapturedSpawn{.outcome = std::move(ending).unwrap(),
                                     .output_bytes = total});
#endif
}

}  // namespace pipeline
