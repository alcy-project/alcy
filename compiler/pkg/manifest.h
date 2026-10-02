// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <charconv>
#include <string_view>
#include <system_error>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "source/source.h"

namespace pkg {

// Manifest file name looked up in package directories.
constexpr std::string_view MANIFEST_FILE_NAME = "alcy.toml";

struct Version {
  u32 major = 0;
  u32 minor = 0;
  u32 patch = 0;
};

constexpr bool operator==(const Version& lhs, const Version& rhs) {
  return lhs.major == rhs.major && lhs.minor == rhs.minor &&
         lhs.patch == rhs.patch;
}

enum class VersionError : u8 {
  Empty,
  BadFormat,
};

// Parses strict X.Y.Z numerics (no prerelease/build metadata in MVP).
inline base::Result<Version, VersionError> parse_version(
    std::string_view text) {
  if (text.empty()) {
    return base::make_err(VersionError::Empty);
  }
  Version version;
  u32* parts[3] = {&version.major, &version.minor, &version.patch};
  for (i32 i = 0; i < 3; ++i) {
    std::string_view part;
    if (i < 2) {
      const usize dot = text.find('.');
      if (dot == std::string_view::npos) {
        return base::make_err(VersionError::BadFormat);
      }
      part = text.substr(0, dot);
      text.remove_prefix(dot + 1);
    } else {
      part = text;
      text = {};
    }
    if (part.empty()) {
      return base::make_err(VersionError::BadFormat);
    }
    u32 value = 0;
    const char* const begin = part.data();
    const char* const end = begin + part.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end) {
      return base::make_err(VersionError::BadFormat);
    }
    *parts[i] = value;
  }
  return base::make_ok(version);
}

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
  // Registry requirement (`=1.2.3`, `1.2`, `1`); empty means latest.
  std::string_view version;
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
  Version version;
  // Optional edition string; empty when absent.
  std::string_view edition;
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
};

// Structural failure of a manifest assembled outside parse_manifest.
enum class ManifestError : u8 {
  EmptyName,
  NullDependencyArray,
  NullBinArray,
  NullModuleInclude,
  NullModuleExport,
  EmptyDependencyName,
  EmptyDependencyPath,
  EmptyBinPath,
  EmptyLibPath,
  EmptyModuleEntry,
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

}  // namespace pkg
