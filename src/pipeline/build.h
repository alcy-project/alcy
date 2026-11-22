// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "lowering/lowering.h"
#include "path/path.h"
#include "pipeline/artifact.h"
#include "pipeline/build_plan.h"
#include "pipeline/cache.h"
#include "pipeline/emit_mode.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "source/source.h"

namespace pipeline {

// Shared frontend: type checking, lowering, and borrow checking over a
// resolved tree. Used by build and run so both lower identical IR.
base::Result<lowering::LoweredPackage, diag::Reported> compile_tree(
    PipelineContext& ctx,
    analyzer::ModuleTree tree,
    BuildPlan plan,
    bool needs_entry,
    std::string_view entry_name);

// Links the objects into an executable, in the order given. An empty driver
// in `link` selects the default toolchain driver, and its arguments follow
// the objects on the command line.
base::Result<void, diag::Reported> link_executable(
    PipelineContext& ctx,
    LinkOptions link,
    std::span<const std::string> object_paths,
    const std::string& exe_path);

// How many objects a package's code generation is split into. A function of
// the program and not of the machine: a small package stays whole, and a
// large one gains nothing from parts finer than the work a core can take.
// How many parts a package's emission splits into. A profile with a
// middle end spreads that middle end over parts sized by the function
// count; a profile without one spreads only code generation, which is
// cheap enough that the split's round trip is the cost to beat - there
// the parts follow the workers once the module has the bodies to feed
// them, and stay at one while it has not.
[[nodiscard]] u32 codegen_parts(const lowering::LoweredPackage& package,
                                bool optimize,
                                u32 workers);

// Emits the package's code generation as objects under `scratch`, one per
// part, and answers their paths in order. The part count follows the
// program; without a middle end it is also bounded by the workers, since
// extra parts would only add bitcode round trips.
base::Result<std::vector<std::string>, diag::Reported> emit_package_objects(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    BuildPlan plan,
    const io::TempDir& scratch,
    bool is_lib,
    u32 parts);

base::Result<std::string, diag::Reported> build_single_file(
    PipelineContext& ctx,
    std::string_view input,
    std::string_view output,
    BuildPlan plan,
    EmitMode mode,
    const StdSelection& selection);

// The input-independent tail of a single-file build: everything from
// a loaded root onwards, which does not care whether the bytes came
// from a file or a pipe. `output` names the artifact unconditionally.
base::Result<std::string, diag::Reported> build_single_root(
    PipelineContext& ctx,
    source::FileId root,
    std::string_view output,
    BuildPlan plan,
    EmitMode mode,
    const StdSelection& selection);

// One target of a package build, planned before anything compiles: what
// it is, the mode it is emitted in (a library's executable request is an
// object one), and the path its artifact lands at.
struct PlannedArtifact {
  Artifact* target;
  EmitMode mode;
  std::string path;
};

// The front half of a target's lifecycle: the cache lookup, the compile
// that stores the unoptimized package when the build was quiet, and the
// summary a library leaves beside its artifact before optimization
//. A build and a run both start here; `check` names no
// artifact and stops earlier.
base::Result<lowering::LoweredPackage, diag::Reported> lower_target(
    PipelineContext& ctx,
    const PlannedArtifact& planned,
    const path::Path& root,
    source::FileId manifest_file,
    bool* cached_out = nullptr,
    std::optional<FrontCache>* cache_out = nullptr);

// The back half of the same lifecycle: the middle end, the emission, and
// the link. Success names the path written. The cache location the
// lowering filled drives the object cache: a hit skips the middle end
// and the emission, and a miss stores the objects under it.
base::Result<std::string, diag::Reported> emit_target(
    PipelineContext& ctx,
    const PlannedArtifact& planned,
    BuildPlan plan,
    lowering::LoweredPackage& lowered,
    const std::optional<FrontCache>& front_cache = std::nullopt);

// What a build built, besides the path its first artifact landed at.
// `name` is the manifest's package name for a package build, and the
// suite's `owner/name` (or `name` when the suite declares no owner) for
// a suite build. `count` is the number of targets a package build
// produced, or the number of member packages a suite build built.
// `output_path` is the first artifact's path for a package build, and
// the directory the suite's artifacts share for a suite build. `cached`
// is true only when every target in the scope was served from the
// build cache.
struct BuildResult {
  std::string output_path;
  std::string name;
  bool suite = false;
  usize count = 0;
  bool cached = false;
};

// An optional observer for build progress, handed plain callbacks as
// elsewhere in the pipeline. `begin` fires once, right after the
// manifest is read, so the scope of the build is known before any
// compilation starts. `member_done` fires once per suite member, in the
// order the suite builds them; it does not fire for a member that
// failed. `cached` says the target's frontend was served from the
// build cache, so a caller can stay silent about it.
struct BuildProgressObserver {
  const void* ctx = nullptr;
  void (*begin)(const void* ctx,
                std::string_view name,
                usize count,
                bool suite) = nullptr;
  void (*member_done)(const void* ctx,
                      std::string_view member,
                      u64 duration_ns,
                      bool cached) = nullptr;
};

base::Result<BuildResult, diag::Reported> build_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    std::string_view output,
    BuildPlan plan,
    EmitMode mode,
    const BuildProgressObserver* progress = nullptr);

// Builds every member of the suite at `root`, each through its own
// manifest and into its own directory under the suite's `out/`
//. Success names that directory; `-o` is refused because it
// cannot name more than one member's artifact.
base::Result<BuildResult, diag::Reported> build_suite(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    std::string_view output,
    BuildPlan plan,
    EmitMode mode,
    const BuildProgressObserver* progress = nullptr);

}  // namespace pipeline
