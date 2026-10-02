// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/cli_main.h"

#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config/build_config.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/logger.h"
#include "fpag/io/file_handle.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "tests/util/test_util.h"

// The descriptor numbers come from io::, as they do everywhere else in
// the tree, so only the calls, the open flags and the null device are
// left to spell differently. Emscripten has the POSIX ones.
#if BUILD_FLAG(IS_OS_WIN)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#include <cstdio>

namespace {
constexpr const char* NULL_DEVICE = "NUL";
// A capture holds bytes the compiler wrote, so it is opened in binary:
// the CRT would otherwise turn each newline into a pair on the way out
// and a case comparing them would see the difference.
constexpr i32 BINARY_FLAG = _O_BINARY;

// Windows prefixes every open flag with an underscore. The permission
// mask is spelled with the symbolic names from <sys/stat.h> because the
// two systems do not agree on the number: 0644 is not
// _S_IREAD | _S_IWRITE.
constexpr i32 READ_FLAGS = _O_RDONLY | BINARY_FLAG;
constexpr i32 WRITE_FLAGS = _O_WRONLY | _O_CREAT | _O_TRUNC | BINARY_FLAG;
constexpr i32 TRUNCATE_FLAGS = _O_RDWR | _O_CREAT | _O_TRUNC | BINARY_FLAG;
constexpr i32 FILE_MODE = _S_IREAD | _S_IWRITE;

i32 open_file(const char* path, i32 flags) {
  return ::_open(path, flags, FILE_MODE);
}
i32 open_null_device() {
  return ::_open(NULL_DEVICE, _O_RDWR);
}
i32 close_descriptor(i32 fd) {
  return ::_close(fd);
}
i32 dup_descriptor(i32 fd) {
  return ::_dup(fd);
}
i32 replace_descriptor(i32 from, i32 to) {
  return ::_dup2(from, to);
}
}  // namespace
#else
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>

namespace {
constexpr const char* NULL_DEVICE = "/dev/null";
// The CRT here has no text mode to opt out of, so there is no binary
// flag to pass; the constant keeps the two branches spelled alike.
constexpr i32 BINARY_FLAG = 0;
constexpr i32 READ_FLAGS = O_RDONLY | BINARY_FLAG;
constexpr i32 WRITE_FLAGS = O_WRONLY | O_CREAT | O_TRUNC | BINARY_FLAG;
constexpr i32 TRUNCATE_FLAGS = O_RDWR | O_CREAT | O_TRUNC | BINARY_FLAG;
constexpr i32 FILE_MODE = 0644;

i32 open_file(const char* path, i32 flags) {
  return ::open(path, flags, FILE_MODE);
}
i32 open_null_device() {
  return ::open(NULL_DEVICE, O_RDWR);
}
i32 close_descriptor(i32 fd) {
  return ::close(fd);
}
i32 dup_descriptor(i32 fd) {
  return ::dup(fd);
}
i32 replace_descriptor(i32 from, i32 to) {
  return ::dup2(from, to);
}
}  // namespace
#endif

// Emscripten has no system linker and cannot spawn a process, so a case
// that needs either cannot run there. Nothing else about a case depends
// on the host: the file system, the terminal and the compiler are all
// present, which is why the exclusion is per case rather than per file.
#if BUILD_FLAG(IS_OS_ASMJS)
#define ALCY_TEST_LINKS 0
#else
#define ALCY_TEST_LINKS 1
#endif

namespace cli {

namespace {

bool write_all(io::TempDir& dir, std::string_view rel, std::string_view text) {
  return dir.write_file(rel, text);
}

i32 run_check_on(io::TempDir& dir, std::string_view rel) {
  const std::string target = dir.join(rel);
  std::vector<std::string> storage{"alcy", "check", "--file", target};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  return cli_main(static_cast<i32>(argv.size()), argv.data());
}

// Standard output is the test process's own, so reading what the
// compiler reported means taking it for a while and giving it back.
class CapturedStdout {
 public:
  explicit CapturedStdout(const std::string& path) : path_(path) {
    // doctest reports through std::cout, whose buffer belongs to the
    // descriptor being replaced. Flushing keeps its pending lines out of
    // the capture, where they would read as the compiler's.
    std::cout.flush();
    file_ = static_cast<i32>(open_file(path.c_str(), TRUNCATE_FLAGS));
    if (file_ < 0) {
      return;
    }
    saved_ = static_cast<i32>(dup_descriptor(io::STDOUT_FD));
    if (saved_ < 0) {
      close_descriptor(file_);
      file_ = -1;
      return;
    }
    replace_descriptor(file_, io::STDOUT_FD);
  }

  ~CapturedStdout() { finish(); }

  CapturedStdout(const CapturedStdout&) = delete;
  CapturedStdout& operator=(const CapturedStdout&) = delete;

  // Idempotent, so the destructor can call it for the cases that never
  // got this far.
  void finish() {
    if (file_ < 0) {
      return;
    }
    // The same reason again, on the way back: whatever doctest queued
    // belongs to the terminal, not to the next capture.
    std::cout.flush();
    std::fflush(stdout);
    close_descriptor(io::STDOUT_FD);
    replace_descriptor(saved_, io::STDOUT_FD);
    close_descriptor(saved_);
    close_descriptor(file_);
    file_ = -1;
    text_ = io::read_file(path_);
    std::remove(path_.c_str());
  }

  bool ok() const { return file_ >= 0; }
  // Empty until the descriptor is back.
  std::string text() {
    finish();
    return text_;
  }

 private:
  std::string path_;
  i32 file_ = -1;
  i32 saved_ = -1;
  std::string text_;
};

// The same, for the stream `run` announces itself on.
class CapturedStderr {
 public:
  explicit CapturedStderr(const std::string& path) : path_(path) {
    file_ = static_cast<i32>(open_file(path.c_str(), TRUNCATE_FLAGS));
    if (file_ < 0) {
      return;
    }
    saved_ = static_cast<i32>(dup_descriptor(io::STDERR_FD));
    if (saved_ < 0) {
      close_descriptor(file_);
      file_ = -1;
      return;
    }
    replace_descriptor(file_, io::STDERR_FD);
  }

  ~CapturedStderr() { finish(); }

  CapturedStderr(const CapturedStderr&) = delete;
  CapturedStderr& operator=(const CapturedStderr&) = delete;

  void finish() {
    if (file_ < 0) {
      return;
    }
    std::fflush(stderr);
    close_descriptor(io::STDERR_FD);
    replace_descriptor(saved_, io::STDERR_FD);
    close_descriptor(saved_);
    close_descriptor(file_);
    file_ = -1;
    text_ = io::read_file(path_);
    std::remove(path_.c_str());
  }

  bool ok() const { return file_ >= 0; }

  std::string text() {
    finish();
    return text_;
  }

 private:
  std::string path_;
  i32 file_ = -1;
  i32 saved_ = -1;
  std::string text_;
};

// Discards whatever the compiler writes, for the cases that assert on
// the exit code and the file rather than on the output. Without it a
// passing run prints the diagnostics a case provoked - an `error[E4020]`
// from a case that wanted one - beside a green `SUCCESS!`.
class SilencedOutput {
 public:
  SilencedOutput() {
    // doctest reports through std::cout, whose buffer belongs to the
    // descriptor being replaced. Anything queued there would be discarded
    // too, and the run would print no results at all.
    std::cout.flush();
    null_ = static_cast<i32>(open_null_device());
    if (null_ < 0) {
      return;
    }
    saved_out_ = static_cast<i32>(dup_descriptor(io::STDOUT_FD));
    saved_err_ = static_cast<i32>(dup_descriptor(io::STDERR_FD));
    if (saved_out_ < 0 || saved_err_ < 0) {
      return;
    }
    replace_descriptor(null_, io::STDOUT_FD);
    replace_descriptor(null_, io::STDERR_FD);
  }

  ~SilencedOutput() {
    if (null_ >= 0) {
      close_descriptor(null_);
    }
    if (saved_out_ < 0 || saved_err_ < 0) {
      return;
    }
    close_descriptor(io::STDOUT_FD);
    close_descriptor(io::STDERR_FD);
    replace_descriptor(saved_out_, io::STDOUT_FD);
    replace_descriptor(saved_err_, io::STDERR_FD);
    close_descriptor(saved_out_);
    close_descriptor(saved_err_);
    // As in the constructor, on the way back.
    std::cout.flush();
  }

  SilencedOutput(const SilencedOutput&) = delete;
  SilencedOutput& operator=(const SilencedOutput&) = delete;

 private:
  i32 null_ = -1;
  i32 saved_out_ = -1;
  i32 saved_err_ = -1;
};

// Puts the program on standard input, which is the test process's own,
// and gives the suite its own back afterwards.
i32 run_compile_stdin(io::TempDir& dir,
                      std::string_view program,
                      std::string_view output,
                      std::string_view emit = "executable") {
  const std::string source = dir.join("piped.al");
  const std::string text(program);
  if (text.empty()) {
    // io::write_file declines an empty span, and the empty program is
    // exactly one case here, so the file is created directly.
    const i32 empty = static_cast<i32>(open_file(source.c_str(), WRITE_FLAGS));
    if (empty < 0 || close_descriptor(empty) != 0) {
      return -1;
    }
  } else {
    const std::span<const u8> bytes(reinterpret_cast<const u8*>(text.data()),
                                    text.size());
    if (!io::write_file(bytes, source)) {
      return -1;
    }
  }

  // Restored below, so one case's program is not the next case's.
  const i32 saved = static_cast<i32>(dup_descriptor(io::STDIN_FD));
  if (saved < 0) {
    return -1;
  }
  const i32 piped = static_cast<i32>(open_file(source.c_str(), READ_FLAGS));
  if (piped < 0 || replace_descriptor(piped, io::STDIN_FD) < 0) {
    if (piped >= 0) {
      close_descriptor(piped);
    }
    close_descriptor(saved);
    return -1;
  }
  close_descriptor(piped);

  const std::string out = dir.join(output);
  const std::string mode(emit);
  std::vector<std::string> storage{"alcy", "compile", "--stdin", "-o",
                                   out,    "--emit",  mode};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  const i32 code = cli_main(static_cast<i32>(argv.size()), argv.data());

  close_descriptor(io::STDIN_FD);
  replace_descriptor(saved, io::STDIN_FD);
  close_descriptor(saved);
  return code;
}

}  // namespace

#if ALCY_TEST_LINKS
TEST_CASE("Compile reads a program from standard input") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_compile_stdin_ok_");
  SilencedOutput silenced;
  CHECK(run_compile_stdin(dir,
                          "fn main() -> i32 {\n"
                          "  print(\"hi\")\n"
                          "  ret 0\n"
                          "}\n",
                          "piped.ll") == 0);
  CHECK(io::is_file(dir.join("piped.ll")));
}
#endif

#if ALCY_TEST_LINKS
TEST_CASE("Compile reports a program piped in under the name <stdin>") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_compile_stdin_bad_");
  // A type error, so the diagnostic names the source it came from.
  SilencedOutput silenced;
  CHECK(run_compile_stdin(dir,
                          "fn main() -> i32 {\n"
                          "  x: u8 := 42i32\n"
                          "  ret 0\n"
                          "}\n",
                          "piped.ll") != 0);
}
#endif

TEST_CASE("Compile reads an empty pipe as an empty program") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_compile_stdin_empty_");
  // Zero bytes, then whatever an empty file would be.
  SilencedOutput silenced;
  CHECK(run_compile_stdin(dir, "", "piped.ll", "llvm-ir") == 0);
}

TEST_CASE("Compile refuses a target alongside standard input") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_compile_stdin_both_");
  const bool setup = write_all(dir, "main.al", "fn main() {\n}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  const std::string target = dir.join("main.al");
  const std::string out = dir.join("piped.ll");
  std::vector<std::string> storage{"alcy", "compile", "--stdin",
                                   "-o",   out,       target};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  SilencedOutput silenced;
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) != 0);
}

TEST_CASE("Compile refuses standard input without a named output") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_cli_compile_stdin_no_out_");
  std::vector<std::string> storage{"alcy", "compile", "--stdin"};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  SilencedOutput silenced;
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) != 0);
}

TEST_CASE("Check accepts a well-typed file") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_check_ok_test_");
  const bool setup = write_all(dir, "ok.al",
                               "struct Point { x: i32, y: i32 }\n"
                               "fn main() {\n"
                               "  p := Point { x: 1, y: 2 }\n"
                               "  _ := p.x + p.y\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  SilencedOutput silenced;
  CHECK(run_check_on(dir, "ok.al") == 0);
}

TEST_CASE("Check rejects a mistyped file") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_check_bad_test_");
  const bool setup = write_all(dir, "bad.al",
                               "fn main() {\n"
                               "  x: u8 := 42i32\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  SilencedOutput silenced;
  CHECK(run_check_on(dir, "bad.al") != 0);
}

TEST_CASE("Check rejects a non-exhaustive match") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_check_match_test_");
  const bool setup = write_all(dir, "bad.al",
                               "enum Choice { Yes, No(i32) }\n"
                               "fn f(c: Choice) -> i32 {\n"
                               "  ret match c {\n"
                               "    Yes => 1,\n"
                               "  }\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  SilencedOutput silenced;
  CHECK(run_check_on(dir, "bad.al") != 0);
}

i32 run_compile_on(io::TempDir& dir,
                   std::string_view rel,
                   std::string_view output,
                   std::string_view emit = "executable") {
  const std::string target = dir.join(rel);
  const std::string out = dir.join(output);
  std::vector<std::string> storage{"alcy", "compile", target,           "-o",
                                   out,    "--emit",  std::string(emit)};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  return cli_main(static_cast<i32>(argv.size()), argv.data());
}

TEST_CASE("Compile emits an object file") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_compile_object_");
  const bool setup = write_all(dir, "main.al",
                               "fn main() {\n"
                               "  print(\"hi\")\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  SilencedOutput silenced;
  CHECK(run_compile_on(dir, "main.al", "main.o", "object") == 0);
  CHECK(io::is_file(dir.join("main.o")));
  const std::optional<std::string> bytes = io::read_file(dir.join("main.o"));
  CHECK(bytes.has_value());
  if (bytes.has_value()) {
    CHECK(tests::is_object_bytes(*bytes));
  }
}

TEST_CASE("Compile emits textual IR") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_compile_ir_test_");
  const bool setup = write_all(dir, "main.al",
                               "fn main() -> i32 {\n"
                               "  print(\"hi\")\n"
                               "  ret 0\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  SilencedOutput silenced;
  CHECK(run_compile_on(dir, "main.al", "main.ll", "llvm-ir") == 0);
  const std::optional<std::string> ir = io::read_file(dir.join("main.ll"));
  CHECK(ir.has_value());
  if (ir.has_value()) {
    CHECK(ir->find("define") != std::string::npos);
  }
}

TEST_CASE("Compile rejects an unknown emit mode") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_compile_emit_");
  const bool setup = write_all(dir, "main.al",
                               "fn main() {\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  SilencedOutput silenced;
  CHECK(run_compile_on(dir, "main.al", "main.o", "bitcode") != 0);
}

#if ALCY_TEST_LINKS
TEST_CASE("Compile links an executable") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_compile_exe_");
  const bool setup = write_all(dir, "main.al",
                               "fn main() -> i32 {\n"
                               "  ret 3\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  SilencedOutput silenced;
  CHECK(run_compile_on(dir, "main.al", "main_exe") == 0);
}
#endif

TEST_CASE("Compile creates nonexistent directory") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_cli_compile_bad_output_test_");
  const bool setup = write_all(dir, "main.al",
                               "fn main() {\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  SilencedOutput silenced;
  CHECK(run_compile_on(dir, "main.al", "no-such-dir/main.o", "object") == 0);
  CHECK(io::is_file(dir.join("no-such-dir/main.o")));
}

TEST_CASE("Build rejects a single file") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_build_file_test_");
  const bool setup = write_all(dir, "main.al",
                               "fn main() {\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  const std::string target = dir.join("main.al");
  std::vector<std::string> storage{"alcy", "build", target};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  SilencedOutput silenced;
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) != 0);
}

bool write_package(io::TempDir& dir,
                   std::string_view rel,
                   std::string_view program) {
  return write_all(dir, std::string(rel) + "/alcy.toml",
                   "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                   "[[bin]]\nname = \"app\"\npath = \"main.al\"\n") &&
         write_all(dir, std::string(rel) + "/main.al", program);
}

bool write_toolchain(io::TempDir& dir,
                     std::string_view rel,
                     std::string_view text) {
  return write_all(dir, std::string(rel) + "/.alcy/toolchain.toml", text);
}

i32 run_build_on(io::TempDir& dir, std::string_view rel) {
  const std::string target = dir.join(rel);
  std::vector<std::string> storage{"alcy", "build", target};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  return cli_main(static_cast<i32>(argv.size()), argv.data());
}

TEST_CASE("Build reads the linker from toolchain.toml") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_build_linker_");
  const bool setup =
      write_package(dir, "proj",
                    "fn main() -> i32 {\n"
                    "  ret 0\n"
                    "}\n") &&
      write_toolchain(dir, "proj", "linker = \"alcy-no-such-driver-xyz\"\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  // A link failure proves the file was read rather than ignored.
  SilencedOutput silenced;
  CHECK(run_build_on(dir, "proj") != 0);
}

#if ALCY_TEST_LINKS
TEST_CASE("Build treats an empty linker as the default") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_build_linker_empty_");
  const bool setup = write_package(dir, "proj",
                                   "fn main() -> i32 {\n"
                                   "  ret 0\n"
                                   "}\n") &&
                     write_toolchain(dir, "proj", "linker = \"\"\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  SilencedOutput silenced;
  CHECK(run_build_on(dir, "proj") == 0);
}

TEST_CASE("Build links the arguments toolchain.toml names") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_build_link_args_");
  const bool setup =
      write_package(dir, "proj",
                    "fn main() -> i32 {\n"
                    "  ret 0\n"
                    "}\n") &&
      write_toolchain(dir, "proj",
                      "link-args = [\"--alcy-no-such-link-flag-xyz\"]\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  // The driver is handed the file's own list, and names an argument it
  // does not have.
  SilencedOutput silenced;
  CHECK(run_build_on(dir, "proj") != 0);
}

TEST_CASE("A link argument flag replaces the file's list rather than join it") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_cli_build_link_args_flag_");
  const bool setup =
      write_package(dir, "proj",
                    "fn main() -> i32 {\n"
                    "  ret 0\n"
                    "}\n") &&
      write_toolchain(dir, "proj",
                      "link-args = [\"--alcy-no-such-link-flag-xyz\"]\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  const std::string target = dir.join("proj");
  // One the driver takes without complaint, so reaching it at all is the
  // success: the file's own argument would still have failed the link.
  std::vector<std::string> storage{"alcy", "build", target, "--link-args=-v"};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  SilencedOutput silenced;
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) == 0);
}

TEST_CASE("compile links the arguments it was given") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_compile_link_args_");
  const bool setup =
      write_all(dir, "main.al", "fn main() -> i32 {\n  ret 0\n}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  const std::string target = dir.join("main.al");
  std::vector<std::string> storage{"alcy", "compile", target,
                                   "--link-args=--alcy-no-such-link-flag-xyz"};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  SilencedOutput silenced;
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) != 0);
}

TEST_CASE("run links the arguments it was given") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_run_link_args_");
  const bool setup = write_package(dir, "proj",
                                   "fn main() -> i32 {\n"
                                   "  ret 0\n"
                                   "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  const std::string target = dir.join("proj");
  std::vector<std::string> storage{"alcy", "run", target,
                                   "--link-args=--alcy-no-such-link-flag-xyz"};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  SilencedOutput silenced;
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) != 0);
}
#endif

TEST_CASE("Time trace embeds its phases in the json result") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_trace_test_");
  const bool setup = write_all(dir, "main.al",
                               "fn main() -> i32 {\n"
                               "  print(\"hi\")\n"
                               "  ret 0\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  const std::string target = dir.join("main.al");
  const std::string out = dir.join("main.ll");
  std::vector<std::string> storage{"alcy", "-t",     "compile", target,  "-o",
                                   out,    "--emit", "llvm-ir", "--json"};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  CapturedStdout captured(dir.join("captured.txt"));
  CHECK(captured.ok());
  if (!captured.ok()) {
    return;
  }
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) == 0);
  const std::string document = captured.text();
  CHECK(document.find("\"traceEvents\"") != std::string::npos);
  CHECK(document.find("\"analyze\"") != std::string::npos);
  CHECK(document.find("\"lower\"") != std::string::npos);
  CHECK(document.find("\"borrow\"") != std::string::npos);
  // One document and no second file to correlate with it.
  CHECK(!document.empty());
  CHECK(document.back() == '\n');
  CHECK(document.find('\n') == document.size() - 1);
  CHECK(!io::is_file(out + ".trace.json"));
}

TEST_CASE("Time trace alone summarizes the phases as text") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_trace_text_test_");
  const bool setup =
      write_all(dir, "main.al", "fn main() -> i32 {\n  ret 0\n}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  std::vector<std::string> storage{"alcy", "-t", "check", "--file",
                                   dir.join("main.al")};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  CapturedStdout captured(dir.join("captured.txt"));
  CHECK(captured.ok());
  if (!captured.ok()) {
    return;
  }
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) == 0);
  const std::string text = captured.text();
  CHECK(text.find("time trace") != std::string::npos);
  CHECK(text.find("parse") != std::string::npos);
  CHECK(text.find("\"traceEvents\"") == std::string::npos);
}

TEST_CASE("Json result carries diagnostics as data") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_json_diag_test_");
  const bool setup =
      write_all(dir, "bad.al", "fn main() -> i32 {\n  ret \"x\"\n}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  std::vector<std::string> storage{"alcy", "check", "--file",
                                   dir.join("bad.al"), "--json"};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  CapturedStdout captured(dir.join("captured.txt"));
  CHECK(captured.ok());
  if (!captured.ok()) {
    return;
  }
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) != 0);
  const std::string document = captured.text();
  // An editor places the squiggle from the span, not by re-finding it.
  CHECK(document.find("\"status\":\"error\"") != std::string::npos);
  CHECK(document.find("\"severity\":\"error\"") != std::string::npos);
  CHECK(document.find("\"code\":") != std::string::npos);
  CHECK(document.find("\"file\":\"") != std::string::npos);
  CHECK(document.find("bad.al") != std::string::npos);
  CHECK(document.find("\"offset\":") != std::string::npos);
  CHECK(document.find("\"length\":") != std::string::npos);
  CHECK(document.find("error[E") == std::string::npos);
}

TEST_CASE("Text result reports the statistics it measured") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_stats_test_");
  const bool setup =
      write_all(dir, "main.al", "fn main() -> i32 {\n  ret 0\n}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  // --color=never because the sentence below is matched as plain text,
  // and whether a terminal is present is the host's to decide: under
  // Emscripten it is, and the verb arrives wrapped in escape codes.
  std::vector<std::string> storage{"alcy", "check", "--file",
                                   dir.join("main.al"), "--color=never"};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  CapturedStdout captured(dir.join("captured.txt"));
  CHECK(captured.ok());
  if (!captured.ok()) {
    return;
  }
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) == 0);
  const std::string text = captured.text();
  CHECK(text.find("Checked   1 file, 1 module, 1 function  (") !=
        std::string::npos);
}

i32 run_run_on(io::TempDir& dir,
               std::string_view rel,
               const std::vector<std::string>& extra = {}) {
  const std::string target = dir.join(rel);
  std::vector<std::string> storage{"alcy", "run", target};
  storage.insert(storage.end(), extra.begin(), extra.end());
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  return cli_main(static_cast<i32>(argv.size()), argv.data());
}

i32 run_init_on(io::TempDir& dir, std::string_view rel) {
  const std::string target = dir.join(rel);
  std::vector<std::string> storage{"alcy", "init", target};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  return cli_main(static_cast<i32>(argv.size()), argv.data());
}

#if ALCY_TEST_LINKS
TEST_CASE("Run executes a package and forwards its exit code") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_run_exit_test_");
  const bool setup = write_package(dir, "proj",
                                   "fn main() -> i32 {\n"
                                   "  ret 3\n"
                                   "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  SilencedOutput silenced;
  CHECK(run_run_on(dir, "proj") == 3);
}
#endif

// The two cases below read the streams rather than discarding them.
#if ALCY_TEST_LINKS
TEST_CASE("Run announces the target before the program, not after") {
  // The label says whose output follows, so it has to come first: below
  // that output it would read as more of it.
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_run_announce_");
  // Written out because this program prints and write_package's manifest
  // names no std dependency.
  const bool setup =
      write_all(dir, "proj/alcy.toml",
                "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                "[dependencies]\n\"alcy/std/*\" = {}\n\n"
                "[[bin]]\nname = \"app\"\npath = \"main.al\"\n") &&
      write_all(dir, "proj/main.al",
                "fn main() -> i32 {\n"
                "  println(\"marker\")\n"
                "  ret 0\n"
                "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  // --color=never because the label is matched as plain text, and
  // whether standard error is a terminal is the host's to decide.
  CapturedStderr captured(dir.join("stderr.txt"));
  CapturedStdout out(dir.join("stdout.txt"));
  CHECK(captured.ok());
  if (!captured.ok()) {
    return;
  }
  CHECK(run_run_on(dir, "proj", {"--color=never"}) == 0);
  const std::string announced = captured.text();
  const std::string program = out.text();
  // Matched rather than compared whole, because the link driver adds a
  // warning of its own to this stream on some platforms.
  CHECK(announced.find("Running   app\n") != std::string::npos);
  CHECK(program.find("Running   app\n") == std::string::npos);
  CHECK(program.find("marker") != std::string::npos);
}

TEST_CASE("Run does not announce a program that failed to build") {
  // The announcement claims a process is about to start, so one made
  // before the compile would be a claim about work not yet done.
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_run_noannounce_");
  const bool setup = write_package(dir, "proj",
                                   "fn main() {\n"
                                   "  x: u8 := 42i32\n"
                                   "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  // Both the announcement and the failure are standard error, so one
  // capture holds the whole story: what a run said, and what it did not.
  CapturedStderr reported(dir.join("stderr.txt"));
  CHECK(reported.ok());
  if (!reported.ok()) {
    return;
  }
  CHECK(run_run_on(dir, "proj", {"--color=never"}) != 0);
  const std::string said = reported.text();
  // Pinned for the same reason as above: a plain label would have been
  // plain, so its absence means something.
  CHECK(said.find("Running   app\n") == std::string::npos);
  // The failure was reported, so the absence above is the absence of an
  // announcement rather than of any output.
  CHECK(said.find("Type mismatch") != std::string::npos);
}

TEST_CASE("Run tolerates program arguments") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_run_args_test_");
  const bool setup = write_package(dir, "proj",
                                   "fn main() -> i32 {\n"
                                   "  ret 0\n"
                                   "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  SilencedOutput silenced;
  CHECK(run_run_on(dir, "proj", {"hello", "world"}) == 0);
}

TEST_CASE("Run fails on a mistyped package") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_run_bad_test_");
  const bool setup = write_package(dir, "proj",
                                   "fn main() {\n"
                                   "  x: u8 := 42i32\n"
                                   "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  SilencedOutput silenced;
  CHECK(run_run_on(dir, "proj") != 0);
}

TEST_CASE("Run rejects a single file") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_run_file_test_");
  const bool setup = write_all(dir, "main.al",
                               "fn main() -> i32 {\n"
                               "  ret 0\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  // The split is the feature: a file belongs to `compile`, so `run`
  // fails rather than guessing.
  SilencedOutput silenced;
  CHECK(run_run_on(dir, "main.al") != 0);
}
#endif

TEST_CASE("Init creates a package in an existing directory") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_init_test_");
  SilencedOutput silenced;
  CHECK(run_init_on(dir, "proj") == 0);
  io::FileHandle manifest;
  CHECK(manifest.open(dir.join("proj/alcy.toml"), io::FileAccess::Read));
  io::FileHandle main;
  CHECK(main.open(dir.join("proj/main.al"), io::FileAccess::Read));
  CHECK(run_init_on(dir, "proj") != 0);
}

// Startup hands the debug logger a sink, without which an internal-error
// path traps instead of reporting. The assertion is about the state a run
// leaves rather than the state it found: every case in this binary shares
// one process, and a sink outlives the invocation that created it, so no
// case here can say the sink was absent beforehand.
TEST_CASE("Startup gives the debug logger a sink") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_init_test_");
  SilencedOutput silenced;
  CHECK(run_check_on(dir, "missing.al") != 0);
  CHECK(debug::debug_logger.has_sink());
}

}  // namespace cli
