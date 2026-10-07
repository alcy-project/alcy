// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/target.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if BUILD_FLAG(IS_OS_WIN)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "analyzer/resolve.h"
#include "config/build_config.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "fpag/io/file_handle.h"
#include "fpag/io/io_util.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "pipeline/dependencies.h"
#include "pipeline/diag_code.h"
#include "pipeline/embedded_std.h"
#include "pipeline/modules.h"
#include "pipeline/parse.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "pipeline/std_stage.h"
#include "pkg/arena_copy.h"
#include "pkg/manifest.h"
#include "pkg/toolchain.h"
#include "source/source.h"

namespace pipeline {

base::Result<ManifestProbe, diag::Reported> require_package_manifest(
    PipelineContext& ctx,
    std::string_view raw,
    bool file_hint) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "manifest",
                                           "frontend");
  base::Result<path::Path, path::PathError> dir = path::Path::from_native(raw);
  if (dir.is_err()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineInvalidTarget>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError, raw);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  path::Path root = std::move(dir).unwrap();
  const path::Path manifest_path = root.join(pkg::MANIFEST_FILE_NAME);
  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(manifest_path.as_view());
  if (manifest.is_err()) {
    // Naming the target is the useful part; when there is none, saying
    // "current directory" reads better than saying ".".
    const bool current = root == ".";
    if (file_hint) {
      if (current) {
        const u32 index = ctx.bag.emit<
            i18n::Key::PipelineNoManifestWithFileHintInCurrentDirectory>(
            diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoManifest);
        (void)index;
      } else {
        const u32 index =
            ctx.bag.emit<i18n::Key::PipelineNoManifestWithFileHint>(
                diag::Severity::Error, diag::Stage::Pipeline,
                DiagCode::NoManifest, raw);
        (void)index;
      }
    } else if (current) {
      const u32 index =
          ctx.bag.emit<i18n::Key::PipelineNoManifestInCurrentDirectory>(
              diag::Severity::Error, diag::Stage::Pipeline,
              DiagCode::NoManifest);
      (void)index;
    } else {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineNoManifest>(
          diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoManifest,
          raw);
      (void)index;
    }
    return base::make_err(diag::Reported{});
  }
  const source::FileId loaded = std::move(manifest).unwrap();
  return base::make_ok(ManifestProbe{true, std::move(root), loaded,
                                     std::string(manifest_path.as_view())});
}

namespace {

bool suite_file_exists(std::string_view path) {
  io::FileHandle probe;
  return probe.open(path, io::FileAccess::Read);
}

// The process's own directory, absolute. Empty when it cannot be
// read.
std::string suite_current_dir_path() {
  char buffer[4096];
#if BUILD_FLAG(IS_OS_WIN)
  if (::_getcwd(buffer, sizeof(buffer)) == nullptr) {
    return {};
  }
#else
  if (::getcwd(buffer, sizeof(buffer)) == nullptr) {
    return {};
  }
#endif
  return std::string(buffer);
}

// Last segment of a path spelling, for the member path. Empty for
// roots and ".".
std::string_view suite_dir_basename(std::string_view dir) {
  if (dir.empty() || dir == "." || dir == "..") {
    return {};
  }
  const usize slash = dir.rfind(path::DEFAULT_PATH_SEPARATOR);
  if (slash == std::string_view::npos) {
    return dir;
  }
  return dir.substr(slash + 1);
}

}  // namespace

std::string suite_spelling(const pkg::SuiteManifest& suite) {
  if (suite.owner.empty()) {
    return std::string(suite.name);
  }
  return std::string(suite.owner) + "/" + std::string(suite.name);
}

bool suite_lists_member(const pkg::SuiteManifest& suite,
                        std::string_view member) {
  for (u32 i = 0; i < suite.package_count; ++i) {
    if (suite.packages[i] == member) {
      return true;
    }
  }
  return false;
}

std::optional<EnclosingSuite> find_enclosing_suite(
    PipelineContext& ctx,
    const path::Path& package_dir) {
  path::Path at = package_dir;
  std::string member(suite_dir_basename(at.as_view()));
  if (member.empty()) {
    // "." has no basename in its spelling; the process's own directory
    // is where it is, and it anchors the walk.
    base::Result<path::Path, path::PathError> cwd =
        path::Path::from_native(suite_current_dir_path());
    if (cwd.is_err()) {
      return std::nullopt;
    }
    at = std::move(cwd).unwrap();
    member = std::string(suite_dir_basename(at.as_view()));
  }
  at = at.parent();
  while (true) {
    const path::Path manifest_path = at.join(pkg::MANIFEST_FILE_NAME);
    if (suite_file_exists(manifest_path.as_view())) {
      const std::string bytes =
          io::read_file(std::string(manifest_path.as_view()));
      switch (pkg::probe_manifest_kind(bytes)) {
        case pkg::ManifestKind::Suite: {
          base::Result<pkg::SuiteManifest, diag::Reported> parsed =
              pkg::parse_suite_manifest(bytes, manifest_path.as_view(),
                                        source::UNKNOWN_FILE, ctx.bag,
                                        ctx.arena);
          if (parsed.is_err()) {
            return std::nullopt;
          }
          pkg::SuiteManifest suite = std::move(parsed).unwrap();
          base::Result<void, pkg::SuiteError> verified =
              pkg::verify_suite_manifest(suite);
          if (verified.is_err()) {
            pkg::report_suite_error(std::move(verified).unwrap_err(),
                                    manifest_path.as_view(), ctx.bag);
            return std::nullopt;
          }
          return EnclosingSuite{std::move(at), suite, std::move(member)};
        }
        case pkg::ManifestKind::Package: return std::nullopt;
        case pkg::ManifestKind::Unknown:
          // Report through the suite parser: the nearest manifest is
          // the one the walk would have joined.
          (void)pkg::parse_suite_manifest(bytes, manifest_path.as_view(),
                                          source::UNKNOWN_FILE, ctx.bag,
                                          ctx.arena);
          return std::nullopt;
      }
    }
    const path::Path parent = at.parent();
    if (parent == at) {
      return std::nullopt;
    }
    member.insert(0, "/");
    member.insert(0, suite_dir_basename(at.as_view()));
    at = parent;
  }
}

namespace {

// A package's sources after discovery, staging, parsing, and selection:
// everything every target resolves against. None of that work depends on
// a target - only the entry file and the directory names are read
// relative to - so it happens once and this owns the storage the targets
// borrow while they read it.
struct PackageSources {
  ParsedFiles parsed;
  std::vector<analyzer::ParsedModule> prelude;
  std::vector<analyzer::ModuleInput> selection;
  // The path dependencies' modules, appended in resolution order.
  // `dependencies` holds spans into this vector, so nothing resizes
  // it once the last module is in: the targets borrow the spans for
  // as long as they resolve.
  std::vector<analyzer::ParsedModule> dependency_modules;
  std::vector<analyzer::DependencyPackage> dependencies;
  // Every package's spec policy: the root's, each path dependency's,
  // and each selected staged member's, in that order. The tree borrows
  // the rows while it is checked (ADR-0053).
  std::vector<analyzer::PackagePolicy> policies;
  usize file_count = 0;
};

// Discovers, stages, parses, and selects the whole package, with
// every path dependency it declares loaded from source into the
// same closed world.
base::Result<PackageSources, diag::Reported> collect_package_sources(
    PipelineContext& ctx,
    const path::Path& root,
    const pkg::PackageManifest& manifest) {
  // Path dependencies load first: a dependency's own manifest names
  // standard-library members, and the selection below covers the
  // whole closure rather than this manifest alone.
  base::Result<std::vector<LoadedDependency>, diag::Reported> resolved =
      resolve_dependencies(ctx, root, manifest);
  if (resolved.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  const std::vector<LoadedDependency> dependencies =
      std::move(resolved).unwrap();

  base::Result<pipeline::DiscoveredSources, diag::Reported> discovered = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "discover",
                                             "frontend");
    return pipeline::discover_sources(root.as_view(), ctx.sources, ctx.bag);
  }();
  if (discovered.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const pipeline::DiscoveredSources found = std::move(discovered).unwrap();

  // Every manifest the closure holds selects the staged members:
  // this package's and each dependency's, merged, so a package
  // that needs another names it and the selection holds the edge.
  std::vector<pkg::Dependency> closure_deps;
  closure_deps.reserve(manifest.dependency_count);
  for (u32 i = 0; i < manifest.dependency_count; ++i) {
    closure_deps.push_back(manifest.dependencies[i]);
  }
  for (const LoadedDependency& dependency : dependencies) {
    for (u32 i = 0; i < dependency.manifest.dependency_count; ++i) {
      closure_deps.push_back(dependency.manifest.dependencies[i]);
    }
  }
  // The closure's dependencies select the staged members; anything
  // unselected is absent, not merely out of scope.
  base::Result<StdSelection, diag::Reported> selected = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "select",
                                             "frontend");
    return resolve_std_selection(closure_deps, ctx.bag);
  }();
  if (selected.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  const StdSelection selection = std::move(selected).unwrap();

  // A package root is one name in the tree: two dependencies behind
  // one identity, a dependency named like the package that loads it,
  // or one named like a staged member would open two roots under a
  // single spelling. The loader names the collision here, where the
  // fix - renaming one of them - is obvious.
  std::vector<std::string_view> identities;
  identities.reserve(dependencies.size());
  for (const LoadedDependency& dependency : dependencies) {
    identities.push_back(
        pkg::copy_str(ctx.arena, package_identity(dependency.manifest.name)));
  }
  for (usize i = 0; i < identities.size(); ++i) {
    if (identities[i] == manifest.name) {
      const u32 index =
          ctx.bag.emit<i18n::Key::PipelineDependencyClashesWithPackage>(
              diag::Severity::Error, diag::Stage::Pipeline,
              DiagCode::DependencyIdentityClash, identities[i]);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    for (usize j = 0; j < i; ++j) {
      if (identities[i] == identities[j]) {
        const u32 index =
            ctx.bag.emit<i18n::Key::PipelineDependencyClashesWithDependency>(
                diag::Severity::Error, diag::Stage::Pipeline,
                DiagCode::DependencyIdentityClash, identities[i]);
        (void)index;
        return base::make_err(diag::Reported{});
      }
    }
    for (std::string_view member : selection.members) {
      if (identities[i] == member) {
        const u32 index =
            ctx.bag.emit<i18n::Key::PipelineDependencyClashesWithStdMember>(
                diag::Severity::Error, diag::Stage::Pipeline,
                DiagCode::DependencyIdentityClash, identities[i],
                identities[i]);
        (void)index;
        return base::make_err(diag::Reported{});
      }
    }
  }
  // `std_prelude` snapshots `selection.members` for its cache, which is
  // why the selection is moved once and the bindings are named rather
  // than dropped at the call.
  const std::span<const analyzer::ModuleInput> prelude = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "std-stage",
                                             "frontend");
    return std_prelude(ctx, selection);
  }();
  // Every file the closure checks is counted once: the package's
  // own and each dependency's, and the source manager hands one id
  // to a file two of them share.
  std::vector<source::FileId> closure_ids;
  closure_ids.reserve(found.files.size());
  closure_ids.insert(closure_ids.end(), found.files.begin(), found.files.end());
  for (const LoadedDependency& dependency : dependencies) {
    closure_ids.insert(closure_ids.end(), dependency.files.begin(),
                       dependency.files.end());
  }
  std::sort(closure_ids.begin(), closure_ids.end());
  closure_ids.erase(std::unique(closure_ids.begin(), closure_ids.end()),
                    closure_ids.end());
  // The module-input naming is target-relative; the only work `resolve`
  // does not repeat between a binary and a library is this, so its own
  // scope shows what repeating it costs.
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "resolve-target",
                                           "frontend");
  // Parsing admits the closure's files and the staged prelude, and
  // the prelude's ids follow the loaded files', so the order is the
  // one discovery loaded in.
  std::vector<source::FileId> parse_ids = closure_ids;
  parse_ids.reserve(parse_ids.size() + prelude.size());
  for (const analyzer::ModuleInput& input : prelude) {
    parse_ids.push_back(input.id);
  }
  base::Result<ParsedFiles, diag::Reported> parse_result =
      parse_files(ctx, parse_ids);
  if (parse_result.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  PackageSources sources;
  sources.parsed = std::move(parse_result).unwrap();
  sources.file_count = closure_ids.size();
  sources.prelude.reserve(prelude.size());
  for (const analyzer::ModuleInput& input : prelude) {
    sources.prelude.push_back(parsed_module(sources.parsed, input));
  }
  base::Result<std::vector<analyzer::ModuleInput>, diag::Reported>
      selection_inputs = [&] {
        PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "select",
                                                 "frontend");
        return select_modules(ctx, manifest, root, found.files);
      }();
  if (selection_inputs.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  sources.selection = std::move(selection_inputs).unwrap();

  // A dependency's modules pair with the files they came from the
  // way the package's own do, and the identity the resolver stages
  // them behind is the arena-copied name the clash checks above
  // already hold.
  for (const LoadedDependency& dependency : dependencies) {
    for (const analyzer::ModuleInput& input : dependency.modules) {
      sources.dependency_modules.push_back(
          parsed_module(sources.parsed, input));
    }
  }
  // The spans hold into `dependency_modules`, which holds every
  // module by now, so the views stay valid for every target.
  sources.dependencies.reserve(dependencies.size());
  usize begin = 0;
  for (usize i = 0; i < dependencies.size(); ++i) {
    const LoadedDependency& dependency = dependencies[i];
    const usize end = begin + dependency.modules.size();
    sources.dependencies.push_back(analyzer::DependencyPackage{
        identities[i],
        std::span<const std::string_view>(
            dependency.manifest.modules.exports,
            dependency.manifest.modules.export_count),
        std::span<const analyzer::ParsedModule>(
            sources.dependency_modules.data() + begin, end - begin)});
    begin = end;
  }
  // The root's policy names the package itself and no suite; a sealed
  // spec it declares is implementable only from inside it.
  sources.policies.push_back(analyzer::PackagePolicy{
      manifest.name,
      {},
      std::span<const std::string_view>(manifest.suite_only,
                                        manifest.suite_only_count)});
  for (usize i = 0; i < dependencies.size(); ++i) {
    const pkg::PackageManifest& dependency = dependencies[i].manifest;
    sources.policies.push_back(analyzer::PackagePolicy{
        identities[i], dependencies[i].suite,
        std::span<const std::string_view>(dependency.suite_only,
                                          dependency.suite_only_count)});
  }
  // Every selected staged member has the one embedded suite's identity,
  // so a member's implementation may reach a spec another member seals.
  for (std::string_view member : selection.members) {
    const StdPackageSeals* const seals = std_seals_for(member);
    sources.policies.push_back(analyzer::PackagePolicy{
        member, STD_SUITE_IDENTITY,
        seals == nullptr ? std::span<const std::string_view>{}
                         : std::span<const std::string_view>(
                               seals->suite_only,
                               static_cast<usize>(seals->suite_only_count))});
  }
  return base::make_ok(std::move(sources));
}

// Resolves one target, binary or library, into its module tree, from a
// package whose files have already been parsed. The root file becomes the
// nameless entry module; the remaining selected modules resolve around it
// exactly as for a binary. Parsing does not depend on the target, so the
// only work here is naming: which file is the entry, and which directory
// the other names are read relative to.
base::Result<PackageTarget, diag::Reported> resolve_target(
    PipelineContext& ctx,
    const path::Path& root,
    const pkg::PackageManifest& manifest,
    const PackageSources& sources,
    std::string_view target_path,
    std::string_view target_name,
    bool is_lib) {
  const path::Path target_file_path = root.join(target_path);
  source::FileId root_file = source::UNKNOWN_FILE;
  for (const ParsedFile& file : sources.parsed.files) {
    if (file.path == target_file_path) {
      root_file = file.id;
      break;
    }
  }
  // The two rejections below name the kind of target that failed, so a
  // library and a binary in one package are told apart.
  if (root_file == source::UNKNOWN_FILE) {
    const u32 index =
        is_lib ? ctx.bag.emit<i18n::Key::PipelineLibTargetNotDiscovered>(
                     diag::Severity::Error, diag::Stage::Pipeline,
                     DiagCode::NoTargets, target_path)
               : ctx.bag.emit<i18n::Key::PipelineBinTargetNotDiscovered>(
                     diag::Severity::Error, diag::Stage::Pipeline,
                     DiagCode::NoTargets, target_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }

  bool root_selected = false;
  std::vector<analyzer::ModuleInput> inputs;
  // Module paths resolve relative to the target root's directory.
  std::string_view target_dir;
  {
    const usize slash = target_path.rfind('/');
    if (slash != std::string_view::npos) {
      target_dir = target_path.substr(0, slash);
    }
  }
  for (const analyzer::ModuleInput& entry : sources.selection) {
    if (entry.id == root_file) {
      root_selected = true;
      inputs.push_back({"", entry.id});
      continue;
    }
    std::string_view name = entry.name;
    if (!target_dir.empty() && name.size() > target_dir.size() &&
        name.substr(0, target_dir.size()) == target_dir &&
        name[target_dir.size()] == '/') {
      name.remove_prefix(target_dir.size() + 1);
    }
    // Stripping the target's directory can map two distinct files onto
    // one module name; the resolver would report that as a duplicate,
    // which is true but hides which file collided. Name the pair here.
    for (const analyzer::ModuleInput& prior : inputs) {
      if (prior.id != entry.id && prior.name == name) {
        const u32 index =
            ctx.bag.emit<i18n::Key::PipelineModuleSharedByTwoFiles>(
                diag::Severity::Error, diag::Stage::Pipeline,
                DiagCode::NoTargets, diag::Span{}, name, target_dir);
        (void)index;
        return base::make_err(diag::Reported{});
      }
    }
    inputs.push_back({name, entry.id});
  }
  if (!root_selected) {
    const u32 index =
        is_lib ? ctx.bag.emit<i18n::Key::PipelineLibTargetNotSelected>(
                     diag::Severity::Error, diag::Stage::Pipeline,
                     DiagCode::NoTargets, target_path)
               : ctx.bag.emit<i18n::Key::PipelineBinTargetNotSelected>(
                     diag::Severity::Error, diag::Stage::Pipeline,
                     DiagCode::NoTargets, target_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }

  std::vector<analyzer::ParsedModule> modules;
  modules.reserve(inputs.size());
  for (const analyzer::ModuleInput& input : inputs) {
    modules.push_back(parsed_module(sources.parsed, input));
  }
  base::Result<analyzer::ModuleTree, diag::Reported> tree = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "resolve-tree",
                                             "frontend");
    return analyzer::resolve_modules(
        root_file, modules, manifest.name, ctx.ast, ctx.bag, sources.prelude,
        std_hints(), sources.dependencies, sources.policies, ctx.profiler);
  }();
  if (tree.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  PackageTarget target;
  target.tree = std::move(tree).unwrap();
  target.file_count = sources.file_count;
  // A target that names nothing is the package, which is what makes a
  // one-file manifest readable.
  target.name = target_name.empty() ? manifest.name : target_name;
  target.is_lib = is_lib;
  return base::make_ok(target);
}

}  // namespace

base::Result<std::vector<PackageTarget>, diag::Reported>
resolve_package_targets(PipelineContext& ctx,
                        const path::Path& root,
                        source::FileId manifest_file,
                        std::string_view manifest_name,
                        TargetScope scope) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "targets", "frontend");
  // Entry validation: the caller supplies a raw manifest id, which must
  // name a loaded file, and the bytes must then be structurally whole
  // before any field is read.
  const std::optional<std::string_view> manifest_bytes =
      ctx.sources.bytes(manifest_file);
  if (!manifest_bytes.has_value()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineManifestNotLoaded>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        manifest_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  base::Result<pkg::PackageManifest, diag::Reported> parsed =
      pkg::parse_manifest(*manifest_bytes, manifest_name, manifest_file,
                          ctx.bag, ctx.arena);
  if (parsed.is_err()) {
    return base::make_err(diag::Reported{});
  }
  pkg::PackageManifest manifest = std::move(parsed).unwrap();
  // A package that sits inside a suite takes the keys it spelled from
  // it; one that is not listed is standalone, and a marker with no
  // suite to resolve against is an error rather than a silent {0,0,0}.
  const std::optional<EnclosingSuite> suite = find_enclosing_suite(ctx, root);
  if (!suite.has_value() && ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  const bool member =
      suite.has_value() && suite_lists_member(suite->manifest, suite->member);
  if (member) {
    base::Result<void, pkg::InheritError> inherited =
        pkg::inherit_from_suite(manifest, suite->manifest);
    if (inherited.is_err()) {
      pkg::report_inherit_error(std::move(inherited).unwrap_err(),
                                manifest_name, ctx.bag);
      return base::make_err(diag::Reported{});
    }
  } else if (manifest.version_from_suite || manifest.owner_from_suite ||
             manifest.license_from_suite) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineInheritsWithoutSuite>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoManifest,
        manifest_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  base::Result<void, pkg::ManifestError> verified =
      pkg::verify_manifest(manifest);
  if (verified.is_err()) {
    pkg::report_manifest_error(std::move(verified).unwrap_err(), manifest_name,
                               ctx.bag);
    return base::make_err(diag::Reported{});
  }
  if (manifest.bin_count == 0 && manifest.lib == nullptr) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineManifestNoTarget>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets,
        manifest_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  if (manifest.bin_count > 1) {
    const u32 index =
        ctx.bag.emit<i18n::Key::PipelineManifestTooManyBinTargets>(
            diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets,
            manifest_name, manifest.bin_count);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  base::Result<PackageSources, diag::Reported> sources =
      collect_package_sources(ctx, root, manifest);
  if (sources.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  const PackageSources collected = std::move(sources).unwrap();

  std::vector<PackageTarget> targets;
  // The binary comes first: `alcy run` takes the front target, and a
  // package's headline artifact is its executable.
  const auto add = [&](std::string_view path, std::string_view name,
                       bool is_lib) -> base::Result<void, diag::Reported> {
    base::Result<PackageTarget, diag::Reported> target =
        resolve_target(ctx, root, manifest, collected, path, name, is_lib);
    if (target.is_err() || ctx.bag.has_errors()) {
      return base::make_err(diag::Reported{});
    }
    targets.push_back(std::move(target).unwrap());
    return base::make_ok();
  };
  if (manifest.bin_count == 1) {
    base::Result<void, diag::Reported> added =
        add(manifest.bins[0].path, manifest.bins[0].name, false);
    if (added.is_err()) {
      return base::make_err(diag::Reported{});
    }
  }
  if (scope == TargetScope::All && manifest.lib != nullptr) {
    base::Result<void, diag::Reported> added =
        add(manifest.lib->path, manifest.lib->name, true);
    if (added.is_err()) {
      return base::make_err(diag::Reported{});
    }
  }
  // A member's artifacts belong to the suite: one out/ for the whole
  // suite, one directory per member (ADR-0057).
  const path::Path output_dir =
      member ? suite->root.join(path::DEFAULT_OUT_DIR).join(suite->member)
             : root.join(path::DEFAULT_OUT_DIR);
  for (PackageTarget& target : targets) {
    target.output_dir = output_dir;
  }
  return base::make_ok(targets);
}

base::Result<pkg::Toolchain, diag::Reported> load_toolchain(
    PipelineContext& ctx,
    const path::Path& root) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "toolchain",
                                           "frontend");
  // The toolchain file is small, so the probe, load, and parse are one
  // request, with the load the only file access of the three.
  const path::Path path =
      root.join(pkg::CONFIG_DIR_NAME).join(pkg::TOOLCHAIN_FILE_NAME);
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(path.as_view());
  if (file.is_err()) {
    return base::make_ok(pkg::Toolchain{});
  }
  const source::FileId loaded = std::move(file).unwrap();
  const std::optional<std::string_view> bytes = ctx.sources.bytes(loaded);
  if (!bytes.has_value()) {
    const u32 index = ctx.bag.emit<i18n::Key::PkgToolchainNotLoaded>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        path.as_view());
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return pkg::parse_toolchain(*bytes, path.as_view(), loaded, ctx.bag,
                              ctx.arena);
}

}  // namespace pipeline
