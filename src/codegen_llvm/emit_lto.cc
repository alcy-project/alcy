// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/emit_lto.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codegen/backend.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/ModuleSummaryAnalysis.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/LTO/Config.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Support/Caching.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/AssignGUID.h"

namespace codegen_llvm {

std::string part_scope_name(const std::string_view phase,
                            const usize at,
                            const usize parts) {
  std::string name(phase);
  name += '[';
  name += std::to_string(at + 1);
  name += '/';
  name += std::to_string(parts);
  name += ']';
  return name;
}

// The parts, optimized and written by the ThinLTO backend: the thin link
// resolves one prevailing copy of everything the parts share, and each
// part's backend imports what the others define and runs the middle end and
// the code generator on its own thread. The same partitions in the same
// order make the same decisions, so what a run writes does not depend on how
// many threads did the work.
base::Result<std::vector<std::vector<u8>>, codegen::EmitError>
optimize_in_parts(std::vector<std::unique_ptr<llvm::Module>>& split,
                  u32 workers,
                  bool freestanding,
                  debug::Profiler* profiler) {
  llvm::lto::Config config;
  config.CPU = "generic";
  config.OptLevel = 3;
  config.CGFileType = llvm::CodeGenFileType::ObjectFile;
  config.Options = llvm::TargetOptions();
  // A freestanding program has no libc, so the thin link's middle end
  // may not invent one either.
  config.Freestanding = freestanding;
  // A diagnostic the link reports is the caller's to name, and the caller
  // reads the code generator's failures; this keeps the library's own
  // default handler from writing to a stream no command owns.
  config.DiagHandler = [](const llvm::DiagnosticInfo&) {};

  // Every part's bitcode, each named for itself. The names outlive the
  // inputs: a buffer's identifier is a view that the link keys a module by
  // long after the buffer is handed over.
  std::vector<std::string> part_names;
  part_names.reserve(split.size());
  for (usize at = 0; at < split.size(); ++at) {
    part_names.push_back("alcy_part_" + std::to_string(at));
  }
  std::vector<std::vector<u8>> bitcode(split.size());
  for (usize at = 0; at < split.size(); ++at) {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(
        profiler, part_scope_name("thinlto-summary", at, split.size()),
        "backend");
    // LLVM assigns the GUIDs a summary is keyed by in a pass, and a module
    // built here has none until it runs: without it the summary cannot be
    // built and the bitcode carries no symbols for the link to read.
    llvm::AssignGUIDPass::runOnModule(*split[at]);
    llvm::ProfileSummaryInfo profile(*split[at]);
    const llvm::ModuleSummaryIndex index =
        llvm::buildModuleSummaryIndex(*split[at], nullptr, &profile);
    llvm::SmallVector<char, 0> buffer;
    llvm::raw_svector_ostream out(buffer);
    llvm::WriteBitcodeToFile(*split[at], out,
                             /*ShouldPreserveUseListOrder=*/false, &index);
    bitcode[at].assign(buffer.begin(), buffer.end());
  }
  // The backend whose threads run the parts, told how many to be: the
  // default backend takes the whole machine, which would spend more threads
  // than the command asked for and fewer than the policy allows.
  llvm::lto::ThinBackend backend = llvm::lto::createInProcessThinBackend(
      llvm::heavyweight_hardware_concurrency(workers));
  llvm::lto::LTO lto(std::move(config), backend, workers);
  llvm::StringSet<> seen;
  std::vector<std::unique_ptr<llvm::lto::InputFile>> inputs;
  std::vector<std::vector<llvm::lto::SymbolResolution>> resolutions;
  inputs.reserve(bitcode.size());
  resolutions.reserve(bitcode.size());
  {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "thinlto-inputs",
                                             "backend");
    for (usize at = 0; at < bitcode.size(); ++at) {
      const llvm::StringRef bytes(
          reinterpret_cast<const char*>(bitcode[at].data()),
          bitcode[at].size());
      llvm::Expected<std::unique_ptr<llvm::lto::InputFile>> input =
          llvm::lto::InputFile::create(
              llvm::MemoryBufferRef(bytes, part_names[at]));
      if (!input) {
        llvm::consumeError(input.takeError());
        return base::make_err(codegen::EmitError::CannotEmit);
      }
      std::vector<llvm::lto::SymbolResolution> resolved;
      resolved.reserve(input.get()->symbols().size());
      for (const llvm::lto::InputFile::Symbol& symbol :
           input.get()->symbols()) {
        llvm::lto::SymbolResolution one;
        const bool undefined = symbol.isUndefined();
        // The runtime repeats in every part as `linkonce_odr`, so the first
        // copy prevails and the rest are dropped, which is what a linker does
        // with them. The entry is the one definition something outside the
        // parts refers to - the startup objects call `main` - and a symbol
        // visible to a regular object is what a partition exports; everything
        // else may be internalized and imported.
        // Only a definition can be the prevailing copy, and only the first
        // one: a part holds declarations of what its neighbours define, and
        // letting one of those take the slot would leave the definition
        // non-prevailing and dropped, undefined at the link.
        const bool first = !undefined && seen.insert(symbol.getName()).second;
        one.Prevailing = first;
        one.FinalDefinitionInLinkageUnit = false;
        one.VisibleToRegularObj = !undefined;
        resolved.push_back(one);
      }
      inputs.push_back(std::move(*input));
      resolutions.push_back(std::move(resolved));
    }
  }
  {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "thinlto-add",
                                             "backend");
    for (usize at = 0; at < inputs.size(); ++at) {
      if (llvm::Error added = lto.add(std::move(inputs[at]), resolutions[at])) {
        llvm::consumeError(std::move(added));
        return base::make_err(codegen::EmitError::CannotEmit);
      }
    }
  }
  // The link's own bound on how many objects it may write: a task is not an
  // input partition, so the partition count is the wrong size for this.
  std::vector<llvm::SmallVector<char, 0>> objects(lto.getMaxTasks());
  // The backend's writer wants a `CachedFileStream`, which wraps the stream
  // an object is written to; over a buffer of ours the object stays in
  // memory, which is what the caller asked for.
  llvm::AddStreamFn add_stream = [&objects](u32 task, const llvm::Twine&)
      -> llvm::Expected<std::unique_ptr<llvm::CachedFileStream>> {
    objects[task].clear();
    return std::make_unique<llvm::CachedFileStream>(
        std::make_unique<llvm::raw_svector_ostream>(objects[task]));
  };
  // The backend's threads do the part work. LLVM's in-process ThinBackend
  // has no hook alcy can put a scope on per task, so this one span names
  // all of it; the serial preparation above is measured per part.
  bool ran_ok = true;
  {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "thinlto-backend",
                                             "backend");
    if (llvm::Error ran = lto.run(add_stream, /*Cache=*/{})) {
      llvm::consumeError(std::move(ran));
      ran_ok = false;
    }
  }
  if (!ran_ok) {
    return base::make_err(codegen::EmitError::CannotEmit);
  }
  // A task that wrote nothing contributed nothing: the links tasks and the
  // partitions are not one to one, and a partition whose every definition
  // was imported elsewhere writes no object at all.
  std::vector<std::vector<u8>> written;
  written.reserve(objects.size());
  for (const llvm::SmallVector<char, 0>& object : objects) {
    if (object.empty()) {
      continue;
    }
    written.emplace_back(object.begin(), object.end());
  }
  return base::make_ok(std::move(written));
}

}  // namespace codegen_llvm
