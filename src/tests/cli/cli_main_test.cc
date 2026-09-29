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
#include "fpag/io/file_handle.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "tests/util/test_util.h"

#if !BUILD_FLAG(IS_OS_ASMJS) && !BUILD_FLAG(IS_OS_WIN)
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#endif

namespace cli {

namespace {

bool write_all(io::TempDir& dir, std::string_view rel, std::string_view text) {
  return dir.write_file(rel, text);
}

#if !BUILD_FLAG(IS_OS_ASMJS)
#if !BUILD_FLAG(IS_OS_WIN)
// Standard output is the test process's own, so a case that reads what
// the compiler reported has to capture it: the descriptor is replaced
// for the duration and put back afterwards, because doctest writes its
// own results there.
class CapturedStdout {
 public:
  explicit CapturedStdout(const std::string& path) : path_(path) {
    // doctest reports through std::cout, whose buffer belongs to the
    // descriptor that is about to be replaced. Flushing it here keeps its
    // pending lines out of the capture, and there they would read as part
    // of what the compiler wrote.
    std::cout.flush();
    file_ = static_cast<i32>(
        ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644));
    if (file_ < 0) {
      return;
    }
    saved_ = static_cast<i32>(::dup(STDOUT_FILENO));
    if (saved_ < 0) {
      ::close(file_);
      file_ = -1;
      return;
    }
    ::dup2(file_, STDOUT_FILENO);
  }

  ~CapturedStdout() { finish(); }

  CapturedStdout(const CapturedStdout&) = delete;
  CapturedStdout& operator=(const CapturedStdout&) = delete;

  // Restores the descriptor and reads back what was written. Idempotent,
  // so the destructor can call it for the cases that never got this far.
  void finish() {
    if (file_ < 0) {
      return;
    }
    // The compiler writes through the descriptor, so the C stream has
    // nothing pending; std::cout is flushed anyway so that whatever
    // doctest queued goes back to the terminal rather than into the next
    // capture.
    std::cout.flush();
    ::fflush(stdout);
    ::close(STDOUT_FILENO);
    ::dup2(saved_, STDOUT_FILENO);
    ::close(saved_);
    ::close(file_);
    file_ = -1;
    text_ = io::read_file(path_);
    ::remove(path_.c_str());
  }

  bool ok() const { return file_ >= 0; }
  // What the compiler wrote. Empty until finish(), since the bytes are
  // only readable once the descriptor is back.
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

// Standard error is the test process's own too, and `run` announces
// itself there rather than on standard output. The descriptor is
// replaced the same way, so a case can read the announcement back
// without it landing on the terminal.
class CapturedStderr {
 public:
  explicit CapturedStderr(const std::string& path) : path_(path) {
    file_ = static_cast<i32>(
        ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644));
    if (file_ < 0) {
      return;
    }
    saved_ = static_cast<i32>(::dup(STDERR_FILENO));
    if (saved_ < 0) {
      ::close(file_);
      file_ = -1;
      return;
    }
    ::dup2(file_, STDERR_FILENO);
  }

  ~CapturedStderr() { finish(); }

  CapturedStderr(const CapturedStderr&) = delete;
  CapturedStderr& operator=(const CapturedStderr&) = delete;

  void finish() {
    if (file_ < 0) {
      return;
    }
    ::fflush(stderr);
    ::close(STDERR_FILENO);
    ::dup2(saved_, STDERR_FILENO);
    ::close(saved_);
    ::close(file_);
    file_ = -1;
    text_ = io::read_file(path_);
    ::remove(path_.c_str());
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

// Discards whatever the compiler writes for the life of the guard. A case
// that asserts on output captures it instead; this is for the ones that
// only care about the exit code and the file the command left behind.
//
// Without it a case writes its result to the terminal, where it lands
// between doctest's own lines and the results of every other case. A
// passing run then prints provoked diagnostics — an `error[E4020]` from
// a case that wanted one — beside a green `SUCCESS!`, and a reader
// cannot tell the two apart.
class SilencedOutput {
 public:
  SilencedOutput() {
    // doctest reports through std::cout, whose buffer belongs to the
    // descriptor about to be replaced. Anything still queued there would
    // be discarded along with the compiler's output, and the run would
    // finish having printed no results at all.
    std::cout.flush();
    null_ = static_cast<i32>(::open("/dev/null", O_RDWR));
    if (null_ < 0) {
      return;
    }
    saved_out_ = static_cast<i32>(::dup(STDOUT_FILENO));
    saved_err_ = static_cast<i32>(::dup(STDERR_FILENO));
    if (saved_out_ < 0 || saved_err_ < 0) {
      return;
    }
    ::dup2(null_, STDOUT_FILENO);
    ::dup2(null_, STDERR_FILENO);
  }

  ~SilencedOutput() {
    if (null_ >= 0) {
      ::close(null_);
    }
    if (saved_out_ < 0 || saved_err_ < 0) {
      return;
    }
    ::close(STDOUT_FILENO);
    ::close(STDERR_FILENO);
    ::dup2(saved_out_, STDOUT_FILENO);
    ::dup2(saved_err_, STDERR_FILENO);
    ::close(saved_out_);
    ::close(saved_err_);
    // The same reason as in the constructor: doctest's next line belongs
    // on the real descriptor, not the one just put back.
    std::cout.flush();
  }

  SilencedOutput(const SilencedOutput&) = delete;
  SilencedOutput& operator=(const SilencedOutput&) = delete;

 private:
  i32 null_ = -1;
  i32 saved_out_ = -1;
  i32 saved_err_ = -1;
};

// Standard input is the test process's own, so a case that feeds the
// compiler has to put the program there: the descriptor is replaced for the
// duration and put back afterwards, because the rest of the suite reads it.
i32 run_compile_stdin(io::TempDir& dir,
                      std::string_view program,
                      std::string_view output,
                      std::string_view emit = "executable") {
  const std::string source = dir.join("piped.al");
  const std::string text(program);
  if (text.empty()) {
    // io::write_file declines an empty span, and the empty program is
    // exactly one case here, so the file is created directly.
    const i32 empty = static_cast<i32>(
        ::open(source.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    if (empty < 0 || ::close(empty) != 0) {
      return -1;
    }
  } else {
    const std::span<const u8> bytes(reinterpret_cast<const u8*>(text.data()),
                                    text.size());
    if (!io::write_file(bytes, source)) {
      return -1;
    }
  }

  // Saved and restored, so one case's program is not the next case's input.
  const i32 saved = static_cast<i32>(::dup(STDIN_FILENO));
  if (saved < 0) {
    return -1;
  }
  const i32 piped = static_cast<i32>(::open(source.c_str(), O_RDONLY));
  if (piped < 0 || ::dup2(piped, STDIN_FILENO) < 0) {
    if (piped >= 0) {
      ::close(piped);
    }
    ::close(saved);
    return -1;
  }
  ::close(piped);

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

  ::close(STDIN_FILENO);
  ::dup2(saved, STDIN_FILENO);
  ::close(saved);
  return code;
}
#endif  // !BUILD_FLAG(IS_OS_WIN)
#endif  // !BUILD_FLAG(IS_OS_ASMJS

}  // namespace

#if !BUILD_FLAG(IS_OS_ASMJS) && !BUILD_FLAG(IS_OS_WIN)
TEST_CASE("Compile reads a program from standard input") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_compile_stdin_ok_");
  // No file on disk is named: the program exists only as the pipe's
  // content, which is the whole point of the flag.
  SilencedOutput silenced;
  CHECK(run_compile_stdin(dir,
                          "fn main() -> i32 {\n"
                          "  print(\"hi\")\n"
                          "  ret 0\n"
                          "}\n",
                          "piped.ll") == 0);
  CHECK(io::is_file(dir.join("piped.ll")));
}

TEST_CASE("Compile reports a program piped in under the name <stdin>") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_compile_stdin_bad_");
  // A type error, so the diagnostic has a line and a caret to place, and
  // the name it places them against is the only observable that the
  // virtual source was used rather than a file.
  SilencedOutput silenced;
  CHECK(run_compile_stdin(dir,
                          "fn main() -> i32 {\n"
                          "  x: u8 := 42i32\n"
                          "  ret 0\n"
                          "}\n",
                          "piped.ll") != 0);
}

TEST_CASE("Compile reads an empty pipe as an empty program") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_compile_stdin_empty_");
  // Not a crash and not a hang: the read ends at zero bytes and the
  // program is then whatever an empty file would be. IR emission needs
  // no entry point, so no link is attempted.
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
  // Both would be a contradiction, and silently preferring one would leave
  // the user guessing which.
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
  // The pipe names no file, so an unnamed output has no extension to
  // replace; the caller must name one.
  SilencedOutput silenced;
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) != 0);
}
#endif  // !BUILD_FLAG(IS_OS_ASMJS) && !BUILD_FLAG(IS_OS_WIN)

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

#if !BUILD_FLAG(IS_OS_ASMJS)
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
  // The extension alone would have been an executable called main.o, so
  // the file is checked rather than just the exit code.
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
  // The split is the feature: a file belongs to `compile`, so `build`
  // fails rather than guessing.
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
  // The frontend runs before the link, so a link failure proves the file
  // was read rather than ignored.
  SilencedOutput silenced;
  CHECK(run_build_on(dir, "proj") != 0);
}

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

#if !BUILD_FLAG(IS_OS_ASMJS)
#if !BUILD_FLAG(IS_OS_WIN)
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
  // Textual IR needs no backend, so the trace covers every phase on
  // every host.
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
  // One document, ending in a newline, with nothing beside it: the
  // trace is no longer a second file to correlate with this one.
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
  CHECK(text.find("phase timings") != std::string::npos);
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
  // The resolved span is what an editor needs to place the squiggle, so
  // the document carries it rather than leaving it to be located again.
  CHECK(document.find("\"status\":\"error\"") != std::string::npos);
  CHECK(document.find("\"severity\":\"error\"") != std::string::npos);
  CHECK(document.find("\"code\":") != std::string::npos);
  CHECK(document.find("\"file\":\"") != std::string::npos);
  CHECK(document.find("bad.al") != std::string::npos);
  CHECK(document.find("\"offset\":") != std::string::npos);
  CHECK(document.find("\"length\":") != std::string::npos);
  // The rendered form is an alternative to this, not an addition to it.
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
  std::vector<std::string> storage{"alcy", "check", "--file",
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
  CHECK(captured.text().find("Checked   1 file, 1 module, 1 function  (") !=
        std::string::npos);
}
#endif  // !BUILD_FLAG(IS_OS_WIN)
#endif  // !BUILD_FLAG(IS_OS_ASMJS)

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

TEST_CASE("Run announces the target before the program, not after") {
  // The label is what tells the reader which program's output they are
  // looking at, so it has to come first. Printed afterwards it would sit
  // below that output and read as more of it.
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_run_announce_");
  // Written out rather than through write_package, because that manifest
  // names no std dependency and this program prints.
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
  CapturedStderr captured(dir.join("stderr.txt"));
  CapturedStdout out(dir.join("stdout.txt"));
  CHECK(captured.ok());
  if (!captured.ok()) {
    return;
  }
  CHECK(run_run_on(dir, "proj") == 0);
  const std::string announced = captured.text();
  const std::string program = out.text();
  // Standard error carries the announcement and whatever the link driver
  // says for itself, which is a warning on some platforms. Asserting the
  // whole stream would be asserting the linker is quiet, which is not
  // this case's subject; what matters is that the label is one whole
  // line of its own and is the one naming this program.
  CHECK(announced.find("Running   app\n") != std::string::npos);
  // The program keeps standard output to itself, so a pipe into `run`
  // carries program output and nothing else.
  CHECK(program == "marker\n");
}

TEST_CASE("Run does not announce a program that failed to build") {
  // The announcement is a claim that a process is about to start. One
  // made before the compile would be a claim about work not yet done.
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_run_noannounce_");
  const bool setup = write_package(dir, "proj",
                                   "fn main() {\n"
                                   "  x: u8 := 42i32\n"
                                   "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  CapturedStderr announced(dir.join("stderr.txt"));
  CapturedStdout reported(dir.join("stdout.txt"));
  CHECK(announced.ok());
  CHECK(reported.ok());
  if (!announced.ok() || !reported.ok()) {
    return;
  }
  CHECK(run_run_on(dir, "proj") != 0);
  CHECK(announced.text().find("Running   app\n") == std::string::npos);
  // The failure was reported, so the run did something rather than
  // nothing. Without this the assertion above would also hold for a
  // command that printed no announcement because it printed nothing.
  CHECK(reported.text().find("type mismatch") != std::string::npos);
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
#endif

}  // namespace cli
