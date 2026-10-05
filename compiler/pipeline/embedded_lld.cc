// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/embedded_lld.h"

#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "debug/dcheck.h"
#include "diag/bag.h"

#if defined(ALCY_EMBEDDED_LLD)
#include "codegen/target.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "i18n/messages.h"
#include "lld/Common/Driver.h"
#include "llvm/Support/raw_ostream.h"
#include "pipeline/diag_code.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"

LLD_HAS_DRIVER(elf)
#endif

namespace pipeline {

#if defined(ALCY_EMBEDDED_LLD)

namespace {

bool file_exists(const std::string& path) {
  std::ifstream stream(path);
  return stream.good();
}

// What a dynamically linked program starts from: the three startup
// objects, the loader the kernel hands control to, and the directories
// the C library is searched in. lld is a linker and not a driver, so
// what the spawned driver used to add around the object has to be named
// here.
struct Startup {
  std::string crt1;
  std::string crti;
  std::string crtn;
  std::string dynamic_linker;
  std::vector<std::string> lib_dirs;
};

// The host's architecture, which decides the multiarch directory and the
// loader's file name. Owned, because `host_triple()` hands back a string
// and a view into it would not outlive this call.
std::string host_arch() {
  const std::string triple = codegen::host_triple();
  return triple.substr(0, triple.find('-'));
}

// Probes the usual Linux locations, once per process. The host's C
// library is not a behavioral input the way an environment variable
// would be: it is the platform the compiler already targets.
const std::optional<Startup>& startup_inputs() {
  static const std::optional<Startup> FOUND = []() -> std::optional<Startup> {
    const std::string arch = host_arch();
    std::vector<std::string> dirs = {"/usr/lib64"};
    if (arch == "x86_64") {
      dirs.push_back("/usr/lib/x86_64-linux-gnu");
    } else if (arch == "aarch64") {
      dirs.push_back("/usr/lib/aarch64-linux-gnu");
    } else if (arch == "riscv64") {
      dirs.push_back("/usr/lib/riscv64-linux-gnu");
    }
    dirs.push_back("/usr/lib");

    std::string loader;
    if (arch == "x86_64") {
      loader = "/lib64/ld-linux-x86-64.so.2";
    } else if (arch == "aarch64") {
      loader = "/lib/ld-linux-aarch64.so.1";
    } else if (arch == "riscv64") {
      loader = "/lib/ld-linux-riscv64-lp64d.so.1";
    }
    if (loader.empty() || !file_exists(loader)) {
      return std::nullopt;
    }
    for (const std::string& dir : dirs) {
      Startup startup;
      startup.crt1 = dir + "/crt1.o";
      startup.crti = dir + "/crti.o";
      startup.crtn = dir + "/crtn.o";
      if (!file_exists(startup.crt1) || !file_exists(startup.crti) ||
          !file_exists(startup.crtn)) {
        continue;
      }
      startup.dynamic_linker = loader;
      startup.lib_dirs = std::move(dirs);
      return startup;
    }
    return std::nullopt;
  }();
  return FOUND;
}

}  // namespace

bool embedded_lld_ready() {
  return startup_inputs().has_value();
}

base::Result<void, diag::Reported> link_with_embedded_lld(
    PipelineContext& ctx,
    const LinkOptions& link,
    const std::string& object_path,
    const std::string& exe_path) {
  const std::optional<Startup>& found = startup_inputs();
  if (!found.has_value()) {
    // The probe runs once per process, so only a caller that skipped
    // `embedded_lld_ready()` gets here.
    DCHECK(false);
    return base::make_err(diag::Reported{});
  }
  const Startup& startup = *found;
  std::vector<std::string> args;
  args.reserve(9 + startup.lib_dirs.size() + link.args.size());
  args.emplace_back("ld.lld");
  // A plain crt1.o is the non-PIE startup, and lld does not default the
  // loader the way the system linker does.
  args.emplace_back("-no-pie");
  args.emplace_back("--dynamic-linker=" + startup.dynamic_linker);
  args.push_back(startup.crt1);
  args.push_back(startup.crti);
  for (const std::string& dir : startup.lib_dirs) {
    args.push_back("-L" + dir);
  }
  args.push_back(object_path);
  for (std::string_view argument : link.args) {
    args.emplace_back(argument);
  }
  args.emplace_back("-lc");
  args.push_back(startup.crtn);
  args.emplace_back("-o");
  args.push_back(exe_path);

  std::vector<const char*> argv;
  argv.reserve(args.size());
  for (const std::string& argument : args) {
    argv.push_back(argument.c_str());
  }
  // The driver's own output goes to the process streams, exactly as the
  // spawned driver's would.
  const lld::Result linked =
      lld::lldMain(llvm::ArrayRef<const char*>(argv.data(), argv.size()),
                   llvm::outs(), llvm::errs(), {{lld::Gnu, &lld::elf::link}});
  if (linked.retCode != 0) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineLinkFailed>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::LinkError,
        exe_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

#else  // !defined(ALCY_EMBEDDED_LLD)

bool embedded_lld_ready() {
  return false;
}

base::Result<void, diag::Reported> link_with_embedded_lld(PipelineContext&,
                                                          const LinkOptions&,
                                                          const std::string&,
                                                          const std::string&) {
  // The caller asks embedded_lld_ready() first, so only a mistake in the
  // caller reaches this.
  DCHECK(false);
  return base::make_err(diag::Reported{});
}

#endif

}  // namespace pipeline
