// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/cli_main.h"

#include <fcntl.h>
#include <unistd.h>

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

#if !BUILD_FLAG(IS_OS_ASMJS)
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
#endif  // !BUILD_FLAG(IS_OS_ASMJS

}  // namespace

#if !BUILD_FLAG(IS_OS_ASMJS)
TEST_CASE("Compile reads a program from standard input") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_compile_stdin_ok_");
  // No file on disk is named: the program exists only as the pipe's
  // content, which is the whole point of the flag.
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
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) != 0);
}
#endif  // !BUILD_FLAG(IS_OS_ASMJS

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
  CHECK(run_compile_on(dir, "main.al", "main.o", "object") == 0);
  // The extension alone would have been an executable called main.o, so
  // the file is checked rather than just the exit code.
  CHECK(io::is_file(dir.join("main.o")));
  const std::optional<std::string> bytes = io::read_file(dir.join("main.o"));
  CHECK(bytes.has_value());
  if (bytes.has_value() && bytes->size() > 4) {
    CHECK(bytes->compare(0, 4,
                         "\x7f"
                         "ELF") == 0);
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
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) != 0);
}

#if !BUILD_FLAG(IS_OS_ASMJS)
TEST_CASE("Time trace writes a json file") {
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
  // every host. The trace sits beside the named output.
  const std::string target = dir.join("main.al");
  const std::string out = dir.join("main.ll");
  std::vector<std::string> storage{"alcy", "-t", "compile", target,
                                   "-o",   out,  "--emit",  "llvm-ir"};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  CHECK(cli_main(static_cast<i32>(argv.size()), argv.data()) == 0);
  const std::optional<std::string> trace = io::read_file(out + ".trace.json");
  CHECK(trace.has_value());
  if (trace.has_value()) {
    CHECK(trace->find("\"traceEvents\"") != std::string::npos);
    CHECK(trace->find("\"analyze\"") != std::string::npos);
    CHECK(trace->find("\"lower\"") != std::string::npos);
    CHECK(trace->find("\"borrow\"") != std::string::npos);
  }
}
#endif  // !BUILD_FLAG(IS_OS_ASMJS

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

bool write_package(io::TempDir& dir,
                   std::string_view rel,
                   std::string_view program) {
  return write_all(dir, std::string(rel) + "/alcy.toml",
                   "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                   "[[bin]]\nname = \"app\"\npath = \"main.al\"\n") &&
         write_all(dir, std::string(rel) + "/main.al", program);
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
  CHECK(run_run_on(dir, "proj") == 3);
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
  CHECK(run_run_on(dir, "main.al") != 0);
}

TEST_CASE("Init creates a package in an existing directory") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cli_init_test_");
  CHECK(run_init_on(dir, "proj") == 0);
  io::FileHandle manifest;
  CHECK(manifest.open(dir.join("proj/alcy.toml"), io::FileAccess::Read));
  io::FileHandle main;
  CHECK(main.open(dir.join("proj/main.al"), io::FileAccess::Read));
  CHECK(run_init_on(dir, "proj") != 0);
}
#endif

}  // namespace cli
