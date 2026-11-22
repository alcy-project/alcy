// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/artifact.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pipeline/build.h"

#if BUILD_FLAG(IS_OS_WIN)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "analyzer/resolve.h"
#include "analyzer/summary.h"
#include "ast/ast.h"
#include "config/build_config.h"
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "fpag/io/file_handle.h"
#include "fpag/io/io_util.h"
#include "i18n/messages.h"
#include "ir/deserialize.h"
#include "path/path.h"
#include "pipeline/dependencies.h"
#include "pipeline/diag_code.h"
#include "pipeline/embedded_std.h"
#include "pipeline/frontend.h"
#include "pipeline/modules.h"
#include "pipeline/parse.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "pipeline/std_stage.h"
#include "pkg/arena_copy.h"
#include "pkg/config.h"
#include "pkg/manifest.h"
#include "pkg/toolchain.h"
#include "source/source.h"
#include "testgen/driver.h"
#include "testgen/tests.h"

namespace pipeline {

namespace {

// What a test run's entry module is filed under. Nothing on disk is
// discovered by this name: the driver is synthesized once the package's
// files are in, and it is named after the package because one source
// manager may resolve more than one.
std::string driver_file_name(std::string_view package) {
  std::string name(package);
  name += "-test.al";
  return name;
}

}  // namespace

bool check_entry_point(PipelineContext& ctx, const Artifact& target) {
  if (target.kind == ArtifactKind::Lib || has_free_main(ctx, target.tree)) {
    return true;
  }
  const u32 index = ctx.bag.emit<i18n::Key::PipelineNoEntryPoint>(
      DiagCode::NoEntryPoint, target.name);
  (void)index;
  return false;
}

base::Result<ManifestProbe, diag::Reported> require_package_manifest(
    PipelineContext& ctx,
    std::string_view raw,
    bool file_hint) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "manifest",
                                           "frontend");
  base::Result<path::Path, path::PathError> dir = path::Path::from_native(raw);
  if (dir.is_err()) {
    const u32 index =
        ctx.bag.emit<i18n::Key::PipelineInvalidTarget>(DiagCode::IoError, raw);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  path::Path root = std::move(dir).unwrap();
  const path::Path manifest_path = root.join(pkg::MANIFEST_FILE_NAME);
  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(manifest_path.as_view());
  if (manifest.is_err()) {
    // Naming the artifact is the useful part; when there is none, saying
    // "current directory" reads better than saying ".".
    const bool current = root == ".";
    if (file_hint) {
      if (current) {
        const u32 index = ctx.bag.emit<
            i18n::Key::PipelineNoManifestWithFileHintInCurrentDirectory>(
            DiagCode::NoManifest);
        (void)index;
      } else {
        const u32 index =
            ctx.bag.emit<i18n::Key::PipelineNoManifestWithFileHint>(
                DiagCode::NoManifest, raw);
        (void)index;
      }
    } else if (current) {
      const u32 index =
          ctx.bag.emit<i18n::Key::PipelineNoManifestInCurrentDirectory>(
              DiagCode::NoManifest);
      (void)index;
    } else {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineNoManifest>(
          DiagCode::NoManifest, raw);
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
// everything every artifact resolves against. None of that work depends
// on an artifact - only the entry file and the directory names are read
// relative to - so it happens once and this owns the storage the targets
// borrow while they read it.
struct PackageSources {
  ParsedFiles parsed;
  std::vector<analyzer::ParsedModule> prelude;
  std::vector<analyzer::ModuleInput> selection;
  // The package's modules named the way the tree holds them, for a
  // package that stages itself as an embedded member: the same files as
  // `selection`, behind the member's own root. Empty otherwise, when the
  // tree holds `selection` as it is. These inputs are not in `prelude`
  // yet: a run splices them first and appends them afterwards, so that the
  // prelude names the items the splice wrote.
  std::vector<analyzer::ModuleInput> staged_member;
  // The path dependencies' modules, appended in resolution order.
  // `dependencies` holds spans into this vector, so nothing resizes
  // it once the last module is in: the targets borrow the spans for
  // as long as they resolve.
  std::vector<analyzer::ParsedModule> dependency_modules;
  // The generic items each dependency's summary named but could not
  // spell. The module inputs borrow these, so nothing resizes them
  // once the last module is in.
  std::vector<std::vector<std::string_view>> summary_generic_names;
  std::vector<analyzer::DependencyPackage> dependencies;
  // Every package's spec policy: the root's, each path dependency's,
  // and each selected staged member's, in that order. The tree borrows
  // the rows while it is checked.
  std::vector<analyzer::PackagePolicy> policies;
  usize file_count = 0;
};

// Discovers, stages, parses, and selects the whole package, with
// every path dependency it declares loaded from source into the
// same closed world.
//
// `member` names the embedded suite member this package's directory is a
// copy of, when it is one: the package is then staged as that member
// rather than beside it, so its own modules are the ones the tree holds
// under the member's name and the embedded copy is not staged at all.
base::Result<PackageSources, diag::Reported> collect_package_sources(
    PipelineContext& ctx,
    const path::Path& root,
    const pkg::PackageManifest& manifest,
    std::string_view member) {
  // Path dependencies load first: a dependency's own manifest names
  // standard-library members, and the selection below covers the
  // whole closure rather than this manifest alone.
  base::Result<DependencyClosure, diag::Reported> resolved =
      resolve_dependencies(ctx, root, manifest);
  if (resolved.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  const DependencyClosure dependencies = std::move(resolved).unwrap();

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
  StdSelection selection = std::move(selected).unwrap();
  // A member under test stages its own files below, so the embedded copy
  // of that member is dropped: staging both would name one module twice.
  // Everything else the closure selected stays, because a member reaches
  // the members it depends on the same way any package does.
  if (!member.empty()) {
    selection.members.erase(
        std::remove(selection.members.begin(), selection.members.end(), member),
        selection.members.end());
  }

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
              DiagCode::DependencyIdentityClash, identities[i]);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    for (usize j = 0; j < i; ++j) {
      if (identities[i] == identities[j]) {
        const u32 index =
            ctx.bag.emit<i18n::Key::PipelineDependencyClashesWithDependency>(
                DiagCode::DependencyIdentityClash, identities[i]);
        (void)index;
        return base::make_err(diag::Reported{});
      }
    }
    for (std::string_view member : selection.members) {
      if (identities[i] == member) {
        const u32 index =
            ctx.bag.emit<i18n::Key::PipelineDependencyClashesWithStdMember>(
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
  // The module-input naming is artifact-relative; the only work `resolve`
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
  // A member under test brings its own modules into the staged tree: the
  // run then checks it where every other package reaches it, which is the
  // only place its dialect holds - declarations of builtin types,
  // implementations of the specs its suite seals, and the facade that
  // offers its surface without a `use`. The name is the one the embedded
  // copy has, so a `use` path spells the member either way, and the
  // facade keeps the role the embedded path gives it.
  if (!member.empty()) {
    sources.staged_member.reserve(sources.selection.size());
    for (const analyzer::ModuleInput& input : sources.selection) {
      std::string path(member);
      path += '/';
      path += input.name;
      sources.staged_member.push_back(analyzer::ModuleInput{
          pkg::copy_str(ctx.arena, path), input.id, input.name == "prelude"});
    }
  }

  // A dependency's modules pair with the files they came from the
  // way the package's own do, and the identity the resolver stages
  // them behind is the arena-copied name the clash checks above
  // already hold.
  for (const LoadedDependency& dependency : dependencies) {
    // A package a summary answers for spells its surface once, after the
    // tree's own sources were parsed, and each of its modules takes the
    // items the surface holds for it.
    std::optional<analyzer::SummarySurface> surface;
    if (dependency.artifact.has_value()) {
      surface = analyzer::spell_summary_surface(
          *dependency.artifact, package_identity(dependency.manifest.name),
          ctx.ast, ctx.strings);
    }
    for (const analyzer::ModuleInput& input : dependency.modules) {
      if (input.id != source::UNKNOWN_FILE) {
        sources.dependency_modules.push_back(
            parsed_module(sources.parsed, input));
        continue;
      }
      std::span<const ast::ItemIdx> items;
      std::span<const analyzer::SummarySymbol> symbols;
      std::span<const std::string_view> generic_names;
      if (surface.has_value()) {
        for (const analyzer::SummarySurface::Module& module :
             surface->modules) {
          if (analyzer::summary_module_name(module.path) == input.name) {
            items = module.items;
            symbols = module.symbols;
            if (!module.generic_names.empty()) {
              // The names are views of the name table, which outlives
              // the resolution; the list that holds them is this run's.
              sources.summary_generic_names.push_back(module.generic_names);
              generic_names = sources.summary_generic_names.back();
            }
            break;
          }
        }
      }
      // The resolver reads no path for a dependency's modules, so an
      // empty one stands for the module a summary spelled.
      analyzer::ModuleInput spelled = input;
      spelled.summary_symbols = symbols;
      spelled.summary_generic_names = generic_names;
      if (dependency.artifact.has_value()) {
        // The artifact lives in the run's dependency table, so the
        // tree may borrow its storage and interner.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        const ir::LoadedIr& artifact = *dependency.artifact;
        spelled.summary_storage = &*artifact.storage;
        spelled.summary_strings = artifact.strings.get();
        spelled.summary_needs_drop = artifact.needs_drop;
        spelled.summary_not_movable = artifact.not_movable;
      }
      sources.dependency_modules.push_back(analyzer::ParsedModule{
          spelled, path::Path::from_native(std::string_view{}).unwrap(),
          items});
    }
  }
  // The spans hold into `dependency_modules`, which holds every
  // module by now, so the views stay valid for every artifact.
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
            sources.dependency_modules.data() + begin, end - begin),
        dependency.object});
    begin = end;
  }
  // The root's policy names the package itself and no suite; a sealed
  // spec it declares is implementable only from inside it. A package
  // staged as an embedded member is the exception: it carries the
  // embedded suite's identity, because that is the identity its tree was
  // staged with and the one its own sealed specs are implemented under.
  sources.policies.push_back(analyzer::PackagePolicy{
      manifest.name, member.empty() ? std::string_view{} : STD_SUITE_IDENTITY,
      std::span<const std::string_view>(manifest.suite_only,
                                        manifest.suite_only_count),
      std::span<const std::string_view>(manifest.implement,
                                        manifest.implement_count)});
  for (usize i = 0; i < dependencies.size(); ++i) {
    const pkg::PackageManifest& dependency = dependencies[i].manifest;
    sources.policies.push_back(analyzer::PackagePolicy{
        identities[i], dependencies[i].suite,
        std::span<const std::string_view>(dependency.suite_only,
                                          dependency.suite_only_count),
        std::span<const std::string_view>(dependency.implement,
                                          dependency.implement_count)});
  }
  // Every selected staged member has the one embedded suite's identity,
  // so a member's implementation may reach a spec another member seals.
  for (const analyzer::PackagePolicy& policy :
       std_package_policies(selection)) {
    sources.policies.push_back(policy);
  }
  return base::make_ok(std::move(sources));
}

// Resolves one target, binary or library, into its module tree, from a
// package whose files have already been parsed. The root file becomes the
// nameless entry module; the remaining selected modules resolve around it
// exactly as for a binary. Parsing does not depend on the target, so the
// only work here is naming: which file is the entry, and which directory
// the other names are read relative to.
base::Result<Artifact, diag::Reported> resolve_target(
    PipelineContext& ctx,
    const path::Path& root,
    const pkg::PackageManifest& manifest,
    const PackageSources& sources,
    std::string_view target_path,
    std::string_view target_name,
    ArtifactKind kind) {
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
        kind == ArtifactKind::Lib
            ? ctx.bag.emit<i18n::Key::PipelineLibArtifactNotDiscovered>(
                  DiagCode::NoTargets, target_path)
            : ctx.bag.emit<i18n::Key::PipelineBinArtifactNotDiscovered>(
                  DiagCode::NoTargets, target_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }

  bool root_selected = false;
  std::vector<analyzer::ModuleInput> inputs;
  // The name each file has claimed. A name is compared against the one
  // already claimed rather than against every input so far.
  std::unordered_map<std::string_view, source::FileId> claimed;
  // Module paths resolve relative to the target root's directory.
  std::string_view input;
  {
    const usize slash = target_path.rfind('/');
    if (slash != std::string_view::npos) {
      input = target_path.substr(0, slash);
    }
  }
  for (const analyzer::ModuleInput& entry : sources.selection) {
    if (entry.id == root_file) {
      root_selected = true;
      inputs.push_back({"", entry.id});
      continue;
    }
    std::string_view name = entry.name;
    if (!input.empty() && name.size() > input.size() &&
        name.substr(0, input.size()) == input && name[input.size()] == '/') {
      name.remove_prefix(input.size() + 1);
    }
    // Stripping the target's directory can map two distinct files onto
    // one module name; the resolver would report that as a duplicate,
    // which is true but hides which file collided. Name the pair here.
    const auto prior = claimed.find(name);
    if (prior != claimed.end() && prior->second != entry.id) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineModuleSharedByTwoFiles>(
          DiagCode::NoTargets, diag::Span{}, name, input);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    claimed.emplace(name, entry.id);
    inputs.push_back({name, entry.id});
  }
  if (!root_selected) {
    const u32 index =
        kind == ArtifactKind::Lib
            ? ctx.bag.emit<i18n::Key::PipelineLibArtifactNotSelected>(
                  DiagCode::NoTargets, target_path)
            : ctx.bag.emit<i18n::Key::PipelineBinArtifactNotSelected>(
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
  Artifact target;
  target.tree = std::move(tree).unwrap();
  target.file_count = sources.file_count;
  // A dependency a summary answered for contributes its own object to
  // the link.
  for (const analyzer::DependencyPackage& dependency : sources.dependencies) {
    if (!dependency.object.empty()) {
      target.linked_objects.emplace_back(dependency.object);
    }
  }
  // A target that names nothing is the package, which is what makes a
  // one-file manifest readable.
  target.name = target_name.empty() ? manifest.name : target_name;
  target.kind = kind;
  return base::make_ok(target);
}

// The package a command works on: its manifest with the keys it takes
// from the suite it sits in, and whether that suite lists it.
struct LoadedPackage {
  pkg::PackageManifest manifest;
  std::optional<EnclosingSuite> suite;
  bool member = false;
};

// Parses the manifest the caller probed and merges the suite it sits in.
// Every command that reads a package starts here: a package's license, its
// version, and its spec seals all come from the same walk.
base::Result<LoadedPackage, diag::Reported> load_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name) {
  // Entry validation: the caller supplies a raw manifest id, which must
  // name a loaded file, and the bytes must then be structurally whole
  // before any field is read.
  const std::optional<std::string_view> manifest_bytes =
      ctx.sources.bytes(manifest_file);
  if (!manifest_bytes.has_value()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineManifestNotLoaded>(
        DiagCode::IoError, manifest_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  base::Result<pkg::PackageManifest, diag::Reported> parsed =
      pkg::parse_manifest(*manifest_bytes, manifest_name, manifest_file,
                          ctx.bag, ctx.arena);
  if (parsed.is_err()) {
    return base::make_err(diag::Reported{});
  }
  LoadedPackage loaded;
  loaded.manifest = std::move(parsed).unwrap();
  // A package that sits inside a suite takes the keys it spelled from
  // it; one that is not listed is standalone, and a marker with no
  // suite to resolve against is an error rather than a silent {0,0,0}.
  loaded.suite = find_enclosing_suite(ctx, root);
  if (!loaded.suite.has_value() && ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  loaded.member =
      loaded.suite.has_value() &&
      suite_lists_member(loaded.suite->manifest, loaded.suite->member);
  if (loaded.member) {
    base::Result<void, pkg::InheritError> inherited =
        pkg::inherit_from_suite(loaded.manifest, loaded.suite->manifest);
    if (inherited.is_err()) {
      pkg::report_inherit_error(std::move(inherited).unwrap_err(),
                                manifest_name, ctx.bag);
      return base::make_err(diag::Reported{});
    }
  } else if (loaded.manifest.version_from_suite ||
             loaded.manifest.owner_from_suite ||
             loaded.manifest.license_from_suite) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineInheritsWithoutSuite>(
        DiagCode::NoManifest, manifest_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  base::Result<void, pkg::ManifestError> verified =
      pkg::verify_manifest(loaded.manifest);
  if (verified.is_err()) {
    pkg::report_manifest_error(std::move(verified).unwrap_err(), manifest_name,
                               ctx.bag);
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(std::move(loaded));
}

// The embedded suite member a package's directory is a copy of, or empty
// when it is not one: the directory sits in a suite named for the embedded
// one, that suite lists it, and the embedded library ships a member of
// that name. The names alone are not enough, because a suite of one's own
// may be spelled the same way; the shipped member is what makes the
// staging below the same tree every package compiles against.
std::string_view embedded_member_of(const LoadedPackage& package) {
  if (!package.member || !package.suite.has_value()) {
    return {};
  }
  if (suite_spelling(package.suite->manifest) != STD_SUITE_IDENTITY) {
    return {};
  }
  const std::string_view name = package.manifest.name;
  if (std_seals_for(name) == nullptr) {
    return {};
  }
  return name;
}

}  // namespace

base::Result<std::vector<Artifact>, diag::Reported> resolve_artifacts(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    ArtifactScope scope,
    std::string* package_name_out) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "targets", "frontend");
  base::Result<LoadedPackage, diag::Reported> loaded =
      load_package(ctx, root, manifest_file, manifest_name);
  if (loaded.is_err()) {
    return base::make_err(diag::Reported{});
  }
  LoadedPackage package = std::move(loaded).unwrap();
  if (package_name_out != nullptr) {
    *package_name_out = std::string(package.manifest.name);
  }
  if (package.manifest.bin_count == 0 && package.manifest.lib == nullptr) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineManifestNoTarget>(
        DiagCode::NoTargets, manifest_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  if (package.manifest.bin_count > 1) {
    const u32 index =
        ctx.bag.emit<i18n::Key::PipelineManifestTooManyBinArtifacts>(
            DiagCode::NoTargets, manifest_name, package.manifest.bin_count);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  base::Result<PackageSources, diag::Reported> sources =
      collect_package_sources(ctx, root, package.manifest, {});
  if (sources.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  const PackageSources collected = std::move(sources).unwrap();

  std::vector<Artifact> targets;
  // The binary comes first: `alcy run` takes the front artifact, and a
  // package's headline artifact is its executable.
  const auto add =
      [&](std::string_view path, std::string_view name,
          ArtifactKind kind) -> base::Result<void, diag::Reported> {
    base::Result<Artifact, diag::Reported> target = resolve_target(
        ctx, root, package.manifest, collected, path, name, kind);
    if (target.is_err() || ctx.bag.has_errors()) {
      return base::make_err(diag::Reported{});
    }
    targets.push_back(std::move(target).unwrap());
    return base::make_ok();
  };
  if (package.manifest.bin_count == 1) {
    base::Result<void, diag::Reported> added =
        add(package.manifest.bins[0].path, package.manifest.bins[0].name,
            ArtifactKind::Bin);
    if (added.is_err()) {
      return base::make_err(diag::Reported{});
    }
  }
  if (scope == ArtifactScope::All && package.manifest.lib != nullptr) {
    base::Result<void, diag::Reported> added =
        add(package.manifest.lib->path, package.manifest.lib->name,
            ArtifactKind::Lib);
    if (added.is_err()) {
      return base::make_err(diag::Reported{});
    }
  }
  // A member's artifacts belong to the suite: one out/ for the whole
  // suite, one directory per member.
  path::Path output_dir = root.join(path::DEFAULT_OUT_DIR);
  const std::optional<EnclosingSuite>& suite = package.suite;
  if (suite.has_value() && package.member) {
    output_dir = suite->root.join(path::DEFAULT_OUT_DIR).join(suite->member);
  }
  for (Artifact& target : targets) {
    target.output_dir = output_dir;
  }
  return base::make_ok(targets);
}

namespace {

// A target's path names the module a build enters through: `main.al` is
// the module `main`, and `text/run.al` is `text/run`.
std::string_view module_of(std::string_view target_path) {
  if (target_path.ends_with(path::SOURCE_EXTENSION)) {
    target_path.remove_suffix(path::SOURCE_EXTENSION.size());
  }
  return target_path;
}

}  // namespace

base::Result<TestPackage, diag::Reported> resolve_test_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    testgen::Execution execution,
    const BuildProgressObserver* progress) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "tests", "frontend");
  base::Result<LoadedPackage, diag::Reported> loaded =
      load_package(ctx, root, manifest_file, manifest_name);
  if (loaded.is_err()) {
    return base::make_err(diag::Reported{});
  }
  LoadedPackage package = std::move(loaded).unwrap();
  if (progress != nullptr && progress->begin != nullptr) {
    progress->begin(progress->ctx, package.manifest.name, 1, false);
  }
  // A package whose directory is a copy of an embedded member is tested as
  // that member: the run stages the package's own files in the member's
  // tree, so the sealed specs and builtin type declarations its dialect
  // allows are in force exactly where every other package meets them.
  const std::string_view member = embedded_member_of(package);
  // A run needs modules rather than a target: a package that builds no
  // artifact of its own - every member of a suite is one - still has
  // tests, so this resolves the selection rather than a target's tree.
  base::Result<PackageSources, diag::Reported> collected =
      collect_package_sources(ctx, root, package.manifest, member);
  if (collected.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  PackageSources sources = std::move(collected).unwrap();

  // A build enters the tree through the module its target names, and a run
  // enters the same way: a test body then reaches the package's other
  // modules by the names the module's own code already uses, and the
  // driver stands in for that module's entry point instead of beside it. A
  // package that declares no target - every member of a suite is one -
  // enters through the driver, and its modules hang below it.
  const analyzer::ModuleInput* entry = nullptr;
  if (member.empty()) {
    std::string_view entry_name;
    if (package.manifest.bin_count > 0) {
      entry_name = module_of(package.manifest.bins[0].path);
    } else if (package.manifest.lib != nullptr) {
      entry_name = module_of(package.manifest.lib->path);
    }
    for (const analyzer::ModuleInput& input : sources.selection) {
      if (!entry_name.empty() && input.name == entry_name) {
        entry = &input;
        break;
      }
    }
  }

  TestPackage result;
  // Splicing replaces a file's items before the modules are paired with
  // their files, so the tree resolves the functions a `#[test] fn` became.
  // A file reached by two module inputs is spliced once, and the second
  // input keeps the items the first wrote.
  std::vector<source::FileId> spliced;
  for (const analyzer::ModuleInput& input : sources.selection) {
    if (std::find(spliced.begin(), spliced.end(), input.id) != spliced.end()) {
      continue;
    }
    const ParsedFile* const file = sources.parsed.find(input.id);
    DCHECK(file != nullptr);
    if (file == nullptr) {
      continue;
    }
    testgen::GatheredModule module =
        testgen::gather_tests(file->items, input.name, ctx.ast);
    sources.parsed.files[sources.parsed.by_id[input.id]].items = module.items;
    spliced.push_back(input.id);
    for (const testgen::RefusedTest& refused : module.refused) {
      switch (refused.reason) {
        case testgen::Refusal::Returns:
          (void)ctx.bag.emit<i18n::Key::PipelineTestReturns>(
              DiagCode::TestReturns, refused.name.span, input.name,
              refused.name.name);
          break;
        case testgen::Refusal::NamedMain:
          (void)ctx.bag.emit<i18n::Key::PipelineTestNamedMain>(
              DiagCode::TestNamedMain, refused.name.span, input.name);
          break;
        case testgen::Refusal::Collides:
          (void)ctx.bag.emit<i18n::Key::PipelineTestCollidesWithItem>(
              DiagCode::TestCollidesWithItem, refused.name.span,
              refused.name.name, input.name);
          break;
        case testgen::Refusal::Duplicates:
          (void)ctx.bag.emit<i18n::Key::PipelineTestDuplicateName>(
              DiagCode::TestDuplicateName, refused.name.span, refused.name.name,
              input.name);
          break;
      }
    }
    for (testgen::TestCase& test : module.tests) {
      test.in_root = entry != nullptr && entry->id == input.id;
      result.tests.push_back(std::move(test));
    }
  }
  // The member's modules join the prelude the tree resolves, and only
  // now: the splice above replaced their items, so this is where the
  // prelude names what the run wrote.
  for (const analyzer::ModuleInput& input : sources.staged_member) {
    sources.prelude.push_back(parsed_module(sources.parsed, input));
  }

  // A package with no tests has nothing for the driver to call: a module
  // the run enters through that declares its own entry point keeps it and
  // the driver joins as an empty module, so the package builds as the
  // program it is - its entry point included, which a run with tests
  // replaces. A package whose entry declares no entry point, and one
  // entered through the driver itself, keep the stand-in: either way the
  // tree a run builds can be entered.
  bool entry_keeps_its_main = false;
  if (entry != nullptr && result.tests.empty()) {
    const ParsedFile* const entry_file = sources.parsed.find(entry->id);
    if (entry_file != nullptr) {
      for (const ast::ItemIdx index : entry_file->items) {
        if (!ctx.ast.items.bound(index)) {
          continue;
        }
        const ast::ItemNode& item = ctx.ast.items[index];
        if (item.kind == ast::ItemKind::Fn &&
            item.payload.get<ast::ItemFn>().name.name == "main") {
          entry_keeps_its_main = true;
          break;
        }
      }
    }
  }
  // The driver is syntax like any other module, so it is parsed through
  // the same path: it is the entry module of the tree the run enters, or
  // the items of that module when the package names one.
  // The driver's imports start at the tree the driver and the package's
  // own modules form, whichever module the tree grew from.
  const std::string driver =
      entry_keeps_its_main ? std::string()
                           : testgen::driver_source(result.tests, execution);
  const source::FileId driver_file =
      ctx.sources.add_virtual(driver_file_name(package.manifest.name), driver);
  base::Result<ParsedFiles, diag::Reported> driver_parsed =
      parse_files(ctx, std::span<const source::FileId>(&driver_file, 1));
  if (driver_parsed.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const ParsedFiles parsed_driver = std::move(driver_parsed).unwrap();
  // A member's tree is the member's: the driver is its root module, so
  // the imports above reach the member's modules below it and the member
  // reaches them the way its own code does.
  const analyzer::ModuleInput driver_input{
      member.empty() ? std::string_view{} : member, driver_file};
  source::FileId tree_root = driver_file;
  if (entry != nullptr) {
    const ParsedFile* const module_file = sources.parsed.find(entry->id);
    const ParsedFile* const driver_items_file = parsed_driver.find(driver_file);
    DCHECK(module_file != nullptr);
    DCHECK(driver_items_file != nullptr);
    if (module_file != nullptr && driver_items_file != nullptr) {
      if (entry_keeps_its_main) {
        // The module builds as the program it is, entry point included;
        // the driver joins nothing, and the tree enters through the
        // module it always does.
        tree_root = entry->id;
      } else {
        // The driver joins the module here, so here is where a name the
        // two share is refused: resolution would find the duplicate and
        // point at the generated module, which names neither the item the
        // reader wrote nor the reason it cannot stay.
        const std::vector<ast::Ident> clashes =
            testgen::driver_collisions(module_file->items, execution, ctx.ast);
        if (!clashes.empty()) {
          for (const ast::Ident& clash : clashes) {
            (void)ctx.bag.emit<i18n::Key::PipelineTestDriverReserved>(
                DiagCode::TestDriverReserved, clash.span, clash.name,
                entry->name);
          }
          return base::make_err(diag::Reported{});
        }
        sources.parsed.files[sources.parsed.by_id[entry->id]].items =
            testgen::entry_items(module_file->items, driver_items_file->items,
                                 ctx.ast);
        tree_root = entry->id;
      }
    }
  }

  // A member's modules are already the staged tree, so the driver joins
  // that tree as its root rather than standing beside it: naming them
  // again would open one module twice.
  std::vector<analyzer::ParsedModule> modules;
  if (member.empty()) {
    modules.reserve(sources.selection.size() + 1);
    if (entry == nullptr) {
      modules.push_back(parsed_module(parsed_driver, driver_input));
    }
    for (const analyzer::ModuleInput& input : sources.selection) {
      modules.push_back(parsed_module(sources.parsed, input));
    }
  } else {
    sources.prelude.insert(sources.prelude.begin(),
                           parsed_module(parsed_driver, driver_input));
  }
  base::Result<analyzer::ModuleTree, diag::Reported> tree = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "resolve-tree",
                                             "frontend");
    return analyzer::resolve_modules(tree_root, modules, package.manifest.name,
                                     ctx.ast, ctx.bag, sources.prelude,
                                     std_hints(), sources.dependencies,
                                     sources.policies, ctx.profiler);
  }();
  if (tree.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  result.artifact.tree = std::move(tree).unwrap();
  result.artifact.file_count = sources.file_count;
  result.artifact.name = package.manifest.name;
  result.artifact.kind = ArtifactKind::Bin;
  for (const analyzer::DependencyPackage& dependency : sources.dependencies) {
    if (!dependency.object.empty()) {
      result.artifact.linked_objects.emplace_back(dependency.object);
    }
  }
  return base::make_ok(std::move(result));
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
  ctx.plan_files.push_back(loaded);
  const std::optional<std::string_view> bytes = ctx.sources.bytes(loaded);
  if (!bytes.has_value()) {
    const u32 index = ctx.bag.emit<i18n::Key::PkgToolchainNotLoaded>(
        DiagCode::IoError, path.as_view());
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return pkg::parse_toolchain(*bytes, path.as_view(), loaded, ctx.bag,
                              ctx.arena);
}

}  // namespace pipeline
