// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "app/spawn.h"

#include <string>
#include <vector>

#include "config/build_config.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

#if BUILD_FLAG(IS_OS_WIN)
#include <windows.h>
#else
#include <spawn.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#if BUILD_FLAG(IS_OS_APPLE)
#include <crt_externs.h>
#else
#include <unistd.h>
extern char** environ;
#endif
#endif

namespace app {

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

#endif

}  // namespace

base::Result<i32, SpawnError> run_command(
    const std::vector<std::string>& argv) {
  if (argv.empty()) {
    return base::make_err(SpawnError::EmptyArgv);
  }
#if BUILD_FLAG(IS_OS_ASMJS)
  // WebAssembly has no process model.
  (void)argv;
  return base::make_err(SpawnError::SpawnFailed);
#elif BUILD_FLAG(IS_OS_WIN)
  std::string command;
  for (usize i = 0; i < argv.size(); ++i) {
    if (i > 0) {
      command += ' ';
    }
    append_quoted(command, argv[i]);
  }
  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info = {};
  if (::CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &startup, &info) == 0) {
    return base::make_err(SpawnError::SpawnFailed);
  }
  ::CloseHandle(info.hThread);
  if (::WaitForSingleObject(info.hProcess, INFINITE) != WAIT_OBJECT_0) {
    ::CloseHandle(info.hProcess);
    return base::make_err(SpawnError::WaitFailed);
  }
  DWORD code = 0;
  if (::GetExitCodeProcess(info.hProcess, &code) == 0) {
    ::CloseHandle(info.hProcess);
    return base::make_err(SpawnError::WaitFailed);
  }
  ::CloseHandle(info.hProcess);
  return base::make_ok(static_cast<i32>(code));
#else
  std::vector<char*> args;
  args.reserve(argv.size() + 1);
  for (const std::string& arg : argv) {
    args.push_back(const_cast<char*>(arg.c_str()));
  }
  args.push_back(nullptr);
  pid_t pid = -1;
  // posix_spawnp resolves file through PATH; the child inherits
  // this environment explicitly for portability.
#if BUILD_FLAG(IS_OS_APPLE)
  char** child_env = *_NSGetEnviron();
#else
  char** child_env = ::environ;
#endif
  if (::posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), child_env) !=
      0) {
    return base::make_err(SpawnError::SpawnFailed);
  }
  i32 status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    return base::make_err(SpawnError::WaitFailed);
  }
  if (!WIFEXITED(status)) {
    return base::make_err(SpawnError::BadExit);
  }
  return base::make_ok(static_cast<i32>(WEXITSTATUS(status)));
#endif
}

}  // namespace app
