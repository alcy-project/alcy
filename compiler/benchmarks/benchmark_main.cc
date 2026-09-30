// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "benchmarks/case_codegen.h"
#include "benchmarks/case_frontend.h"
#include "benchmarks/case_pipeline.h"
#include "benchmarks/clock.h"
#include "benchmarks/generator.h"
#include "benchmarks/runner.h"
#include "benchmarks/sink.h"
#include "config/build_config.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/signal_handler.h"
#include "fpag/debug/terminate_handler.h"
#include "fpag/io/io_util.h"
#include "fpag/term/console.h"

namespace {

// The host a number belongs to, asked of the build rather than guessed
// from the binary's path.
constexpr std::string_view HOST =
#if BUILD_FLAG(IS_OS_LINUX)
    "linux";
#elif BUILD_FLAG(IS_OS_APPLE)
    "macos";
#elif BUILD_FLAG(IS_OS_WIN)
    "windows";
#else
    "other";
#endif

void print_usage() {
  std::fputs(
      "usage: benchmarks [options]\n"
      "  --output <file>     write JSONL here instead of standard output\n"
      "  --emit-source <f>   write the generated source, for a check run to\n"
      "                      read so the two can be reconciled\n"
      "  --build-subdir <d>  out/ subdirectory this driver was built in\n"
      "  --build-mode <m>    debug or release, recorded with the figures\n"
      "  --cases <names>     only cases whose name is listed\n"
      "  --functions <n>     generated functions (default 64)\n"
      "  --statements <n>    statements per function (default 8)\n"
      "  --depth <n>         if-nesting depth (default 3)\n",
      stderr);
}

}  // namespace

i32 main(i32 argc, char** argv) {
  term::register_console();
  debug::register_terminate_handler();
  debug::register_signal_handlers();

  std::string output;
  std::string emit_source;
  std::string build_subdir = "build";
  std::string build_mode = "unknown";
  std::string case_filter;
  bench::SourceSpec spec;
  for (i32 i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    const bool has_value = i + 1 < argc;
    if (flag == "--output" && has_value) {
      output = argv[++i];
    } else if (flag == "--emit-source" && has_value) {
      emit_source = argv[++i];
    } else if (flag == "--build-subdir" && has_value) {
      build_subdir = argv[++i];
    } else if (flag == "--build-mode" && has_value) {
      build_mode = argv[++i];
    } else if (flag == "--cases" && has_value) {
      case_filter = argv[++i];
    } else if (flag == "--functions" && has_value) {
      spec.functions = static_cast<u32>(std::atoi(argv[++i]));
    } else if (flag == "--statements" && has_value) {
      spec.statements = static_cast<u32>(std::atoi(argv[++i]));
    } else if (flag == "--depth" && has_value) {
      spec.depth = static_cast<u32>(std::atoi(argv[++i]));
    } else if (flag == "--help" || flag == "-h") {
      print_usage();
      return 0;
    } else {
      print_usage();
      return 2;
    }
  }

  // The digest is of the source the cases ran on, so a result can be
  // told apart from one taken over different input.
  const std::string generated = bench::generate_source(spec);
  const std::string digest = bench::source_digest(generated);
  if (!emit_source.empty()) {
    // Written so that a `check --file` run can be pointed at the very
    // source these cases measured, which is what lets the engine's
    // figures be reconciled against a real run's phases.
    std::FILE* source = std::fopen(emit_source.c_str(), "wb");
    if (source == nullptr) {
      std::fprintf(stderr, "benchmarks: cannot write '%s'\n",
                   emit_source.c_str());
      return 1;
    }
    const usize source_bytes =
        std::fwrite(generated.data(), 1, generated.size(), source);
    std::fclose(source);
    if (source_bytes != generated.size()) {
      return 1;
    }
  }
  const bench::RunMetadata metadata{
      .target = HOST,
      .build_subdir = build_subdir,
      .build_mode = build_mode,
      .fixture_digest = digest,
      .clock = "steady_clock",
      .clock_resolution_ns = bench::resolution_ns(),
      .clock_overhead_ns = bench::measure_overhead_ns(),
  };

  bench::ResultWriter writer(metadata);
  bench::Emitter emit{&writer};
  const bench::SteadyClock clock;
  bench::Runner<bench::SteadyClock> runner(clock);
  const bench::CaseFilter filter{case_filter};
  bench::run_frontend_cases(runner, spec, filter, emit);
  bench::run_pipeline_cases(runner, spec, filter, emit);
  bench::run_codegen_cases(runner, spec, filter, emit);
  if (emit.count == 0) {
    std::fprintf(stderr, "benchmarks: no case matched '%s'\n",
                 case_filter.c_str());
    return 1;
  }

  if (output.empty()) {
    io::write(io::STDOUT_FD, writer.text().data(), writer.text().size());
    return 0;
  }
  std::FILE* file = std::fopen(output.c_str(), "wb");
  if (file == nullptr) {
    std::fprintf(stderr, "benchmarks: cannot write '%s'\n", output.c_str());
    return 1;
  }
  const usize written =
      std::fwrite(writer.text().data(), 1, writer.text().size(), file);
  std::fclose(file);
  return written == writer.text().size() ? 0 : 1;
}
