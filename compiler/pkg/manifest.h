// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "pkg/version.h"
#include "pkg/version_req.h"
#include "source/source.h"

namespace pkg {

// Manifest file name looked up in package directories.
constexpr std::string_view MANIFEST_FILE_NAME = "alcy.toml";

// A declared binary target ([[bin]] table). `name` is empty when the
// table omits it and defaults to the package name. Views borrow arena
// storage owned by the caller of parse_manifest().
struct BinTarget {
  std::string_view name;
  std::string_view path;
};

// A library target: one optional table per package, naming the root
// module the way a bin names its entry file. Views borrow arena
// storage owned by the caller of parse_manifest().
struct LibTarget {
  std::string_view name;
  std::string_view path;
};

// Where a dependency comes from. `Path` is a local directory (the
// only resolved source today); `Registry` and `Git` parse and validate
// but resolve to an explicit "not implemented" until their fetchers
// land. `Unspecified` is an empty table, which the selection layer
// accepts only for the embedded standard library. Views borrow arena
// storage owned by the caller of parse_manifest().
enum class DependencySource : u8 {
  Path,
  Registry,
  Git,
  Unspecified,
};

struct Dependency {
  // The specifier as written, e.g. "alcy/std/*".
  std::string_view spec;
  // One segment (`foo`): a local directory aliased `foo`, exactly the
  // old shape. Two (`acme/hash`): package `hash` of owner `acme`, held
  // in `member` with `suite` empty. Three (`acme/tools/cli`): package
  // `cli` of suite `tools`. A suite is selected with a trailing `/*`,
  // never bare: bare "alcy/std" is rejected at selection with the two
  // spellings it could mean.
  std::string_view owner;
  std::string_view suite;
  std::string_view member;
  bool suite_glob = false;
  DependencySource source = DependencySource::Unspecified;
  // Local directory for `Path`, repo-relative subpath for `Git`.
  std::string_view path;
  // The registry requirement as written (`1.2.x`,
  // `>=1.2.0, <1.5.0`); empty means latest.
  std::string_view version;
  // The requirement parsed into bounds, every one of which must hold.
  // Empty when `version` is.
  VersionReq version_req;
  std::string_view git;
  // At most one of branch, tag, rev; empty means the default branch.
  std::string_view git_ref_kind;
  std::string_view git_ref;
  std::string_view registry;
  // Legacy alias for one-segment entries, kept so existing manifests
  // keep meaning exactly what they meant.
  std::string_view name;
};

// Module set from the [modules] table. `include` lists module paths
// (`"utils/io"` maps to `utils/io.al` under the package root); a
// single `"*"` entry selects every discovered source file. `export`
// lists the public surface for library packages. Views borrow arena
// storage owned by the caller of parse_manifest().
struct ModuleSet {
  const std::string_view* include = nullptr;
  u32 include_count = 0;
  bool wildcard = false;
  const std::string_view* exports = nullptr;
  u32 export_count = 0;
};

struct PackageManifest {
  std::string_view name;
  // The package's own version. `has_version` is false only while the
  // manifest spells `version.suite = true`, which resolution fills;
  // `version_from_suite` is then set until it does.
  Version version;
  bool has_version = false;
  bool version_from_suite = false;
  // Optional edition string; empty when absent.
  std::string_view edition;
  // The owner, empty when the manifest declares none. Empty is a
  // value: it is the local, unowned package. `owner_from_suite` marks
  // the `owner.suite = true` spelling until resolution fills it.
  std::string_view owner;
  bool owner_from_suite = false;
  // The license expression; the key is required, and the empty string
  // is how a package says it grants nothing. `license_from_suite`
  // marks the `license.suite = true` spelling until resolution fills
  // it.
  std::string_view license;
  bool license_from_suite = false;
  // Arena-owned array, possibly empty.
  const Dependency* dependencies = nullptr;
  u32 dependency_count = 0;
  // Declared build targets ([[bin]] tables). MVP builds a single binary;
  // additional entries are parsed for forward compatibility and rejected
  // by the pipeline with guidance.
  const BinTarget* bins = nullptr;
  u32 bin_count = 0;
  // The optional library target ([lib] table). Null when absent.
  const LibTarget* lib = nullptr;
  // Module membership from the [modules] table. Absent means every
  // discovered source file.
  ModuleSet modules;
  // Specs this package seals to its suite, from the optional [spec]
  // table (`suite-only = [...]`); an arena-owned array, possibly
  // empty. An implementation of a sealed spec is refused outside the
  // declaring package's suite (ADR-0053).
  const std::string_view* suite_only = nullptr;
  u32 suite_only_count = 0;
};

// A suite manifest ([suite] table): a named set of packages under one
// owner, addressed as `<owner>/<suite>` with a member at
// `<owner>/<suite>/<package>`. `packages` is an arena-owned array of
// member names. Views borrow arena storage owned by the caller of
// parse_suite_manifest().
struct SuiteManifest {
  // The owner, empty when the suite declares none. Dependency matching
  // compares it, so an owner-less suite is a local build target rather
  // than an address.
  std::string_view owner;
  std::string_view name;
  // The suite's own version, when it declares one. It exists to be
  // inherited: a member that spells `version.suite = true` takes it.
  Version version;
  bool has_version = false;
  // The license expression members inherit; required, may be empty.
  std::string_view license;
  // Arena-owned array of suite-relative member paths, possibly empty.
  const std::string_view* packages = nullptr;
  u32 package_count = 0;
};

// Structural failure of a manifest assembled outside parse_manifest.
enum class ManifestError : u8 {
  EmptyName,
  NullDependencyArray,
  NullBinArray,
  NullModuleInclude,
  NullModuleExport,
  NullSuiteOnly,
  EmptyDependencyName,
  EmptyDependencyPath,
  EmptyBinPath,
  EmptyLibPath,
  EmptyModuleEntry,
  EmptySuiteOnlyEntry,
  UnresolvedInheritance,
};

// Pure structural verifier: pointer/count pairs agree and the strings
// parse_manifest guarantees are present. Its own output satisfies this;
// a manifest assembled directly (notably in tests) must pass it before
// crossing a public API. No I/O, no allocation, no bag writes, no input
// mutation.
base::Result<void, ManifestError> verify_manifest(
    const PackageManifest& manifest);

// Entry-point conversion helper: emits `manifest '<name>': <detail>`
// (code 1001, the manifest semantic range) into bag. Verifiers stay
// pure; this is how an entry turns their structured failure into a
// diagnostic.
void report_manifest_error(ManifestError error,
                           std::string_view name,
                           diag::DiagBag& bag);

// Parses manifest bytes; all strings reference arena copies. `file` backs
// spans for syntax errors (pass source::UNKNOWN_FILE when unknown).
// Parses one `--deps` fragment with the same grammar as a
// [dependencies] entry: either a bare specifier (`alcy/std/*`) or
// `specifier = { ... }` with an optional value table. A bare key without
// quotes is quoted before parsing, so slashes need no escaping.
base::Result<Dependency, diag::Reported> parse_dependency_flag(
    diag::DiagBag& bag,
    mem::Arena& arena,
    std::string_view fragment);

base::Result<PackageManifest, diag::Reported> parse_manifest(
    std::string_view bytes,
    std::string_view filename,
    source::FileId file,
    diag::DiagBag& bag,
    mem::Arena& arena);

// Why a member's inherited key cannot be filled.
enum class InheritError : u8 {
  SuiteVersionMissing,
};

// Fills each `X.suite = true` key of `member` from `suite` and clears
// the marker: `owner`, `license`, and `version`. The caller has
// established that the package is a member; a manifest with no marker
// is left alone, and a member that keeps a literal keeps it. Pure.
// After this, `verify_manifest` accepts the result.
base::Result<void, InheritError> inherit_from_suite(PackageManifest& member,
                                                    const SuiteManifest& suite);

// Entry-point conversion helper: emits `manifest '<name>': <detail>`
// (code 1001, the manifest semantic range) into bag.
void report_inherit_error(InheritError error,
                          std::string_view name,
                          diag::DiagBag& bag);

// Structural failure of a suite manifest assembled outside
// parse_suite_manifest. Mirrors ManifestError for the [suite] shape:
// pointer/count pairs agree, required strings are present, and the
// member list holds unique non-empty names.
enum class SuiteError : u8 {
  EmptyName,
  NullPackageArray,
  EmptyPackageEntry,
  BadPackageEntry,
  DuplicatePackageEntry,
  DuplicatePackageName,
};

// Pure structural verifier for suite manifests. Its own output satisfies
// this; a manifest assembled directly (notably in tests) must pass it
// before crossing a public API. No I/O, no allocation, no bag writes,
// no input mutation.
base::Result<void, SuiteError> verify_suite_manifest(
    const SuiteManifest& manifest);

// Entry-point conversion helper: emits `manifest '<name>': <detail>`
// (code 1001, the manifest semantic range) into bag. Verifiers stay
// pure; this is how an entry turns their structured failure into a
// diagnostic.
void report_suite_error(SuiteError error,
                        std::string_view name,
                        diag::DiagBag& bag);

// What an `alcy.toml`-shaped file opens with, for callers that have
// to choose a parser before running one, or to decide whether a
// manifest is theirs at all. `Unknown` covers a syntax error and a
// file with neither identity table; the parser reports the detail
// when the caller runs one.
enum class ManifestKind : u8 {
  Package,
  Suite,
  Unknown,
};

ManifestKind probe_manifest_kind(std::string_view bytes);

// The name a suite member is addressed by: the last segment of its
// suite-relative path, which is also the name its own manifest must
// carry (ADR-0057). Pure.
std::string_view suite_member_name(std::string_view path);

// Parses suite manifest bytes; all strings reference arena copies.
// `file` backs spans for syntax errors (pass source::UNKNOWN_FILE when
// unknown). A manifest holding [package] is rejected as the wrong kind,
// the way parse_manifest rejects one holding [suite].
base::Result<SuiteManifest, diag::Reported> parse_suite_manifest(
    std::string_view bytes,
    std::string_view filename,
    source::FileId file,
    diag::DiagBag& bag,
    mem::Arena& arena);

}  // namespace pkg
