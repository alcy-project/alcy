# ADR-0057: Suites are scaffolded, inherited, and built as one

- Subject: the toolchain
- Status: Accepted
- Date: 2026-10-08

## Context

A suite is a named set of packages under one owner
(`docs/adr/0016-suites-and-the-std-split.md`). The loader knows them:
`[suite]` parses, a path dependency may point at a suite directory and
pull one member or every member, and the standard library is one. The
commands do not. `alcy new` and `alcy init` scaffold one package;
`alcy build` in a suite directory answers that the manifest is a suite
manifest, not a package manifest; and a member built by hand writes
`<member>/out/`, so a suite of five members leaves five build
directories and nothing that says they belong together.

Identity has the same gap. A suite names an owner and nothing else; a
package names neither owner nor license; and the only inheritance rule
is the one a reader invents after finding two manifests.

Three decisions arrive together because each constrains the others:

- **Scaffolding and membership.** `alcy new --suite` and the member it
  later adds need a manifest shape that the writer and the reader agree
  on. Appending a member is the first time a command rewrites a
  manifest it did not write in the same run, so the member list's shape
  and the edit are one decision.
- **Identity.** A package that belongs to a suite must not silently
  borrow the suite's owner, version, or license. A build that reads one
  manifest and gets a value from a second manifest is the implicit
  behavior this project keeps out.
- **Selection.** A member's directory may nest
  (`alcy new first-party/foo`), and a dependency still has to name one
  member without spelling a directory path.

## Decision

### Identity is declared; inheritance is spelled

`[package]` gains `owner` and `license`; `[suite]` gains `version` and
`license`. `license` is required in both and may be the empty string:
that is how a closed-source package says it grants nothing, and it
keeps the key's presence checkable. `owner` defaults to the empty
string. `[suite] version` is optional and is needed only when a member
inherits it; `[package] version` is required unless the package spells
inheritance.

A key that takes the suite's value says so, and only says so:

```toml
[package]
name = "cli"
version.suite = true
license.suite = true
```

`owner.suite = true` has the same shape. A literal stays a literal even
when it differs from the suite's; validating that the two agree belongs
to the package manager, not here. A package that spells
`X.suite = true` with no enclosing suite is an error, as is a missing
`version` or `license` that nothing inherits.

The standard library is alcy's own code, so `lib/std/alcy.toml` and
every member declare `license = "Apache-2.0 WITH LLVM-exception"`; the
repository's other manifests are fixtures rather than distributions and
declare `license = ""`.

### A member is a path; an address is a name

`[suite] packages` holds suite-relative paths (`"core"`,
`"first-party/foo"`). A member path may nest because the suite root is
where packages are made, and forcing every package to the root would
make grouping directories impossible. Names stay what a person writes
and reads: the third segment of `owner/suite/name` is a member's
package name, never a path. Within one suite, package names are
unique, and the last segment of a member's path must equal the name in
its `[package]` table; both are checked when a suite loads.

A dependency entry keeps its two shapes. `owner/package` with `path`
loads a package directory directly. `owner/suite/name` with `path`
loads the suite manifest, checks its owner and name, and resolves the
member whose package name matches; `owner/suite/*` takes every member.
Pointing a two-segment key at a suite, or a three-segment key at a
package, is an error: the key's shape says which manifest to expect.

### Dependency values are tables

A dependency's value is always a TOML table. A bare string cannot say
whether it is a version or a path without a rule about filesystem
lookups, and that rule is exactly the implicit behavior the manifest
refuses.

`version` holds a requirement, with this grammar:

```text
requirement = term ("," term)*
term        = comparator X.Y.Z | X.Y.Z | X.Y.x | X.x
comparator  = "=" | ">" | ">=" | "<" | "<="
```

`X.Y.Z` is exact. `X.Y.x` covers `[X.Y.0, X.(Y+1).0)` and `X.x`
covers `[X.0.0, (X+1).0.0)`, so the precision written is the range
meant. Bare `x` and `*` are errors -- a major is the least a
requirement can name. `0.x` is allowed; whether to discourage it is a
manifest lint's business, not the grammar's. Commas are conjunction.
Caret and tilde are not in the grammar: they are not Cargo-specific,
but their expansions differ between ecosystems, and the ranges they
name have readable spellings here (`~1.2.3` is `>=1.2.3, 1.2.x`).

Prerelease and build metadata stay out, as they are out of
`parse_version` today.

### The CLI scaffolds, and the suite is one build target

`alcy new --suite <name>` creates a directory named `<name>`;
`alcy init --suite <name>` writes into an existing directory. An
`owner/name` value names both. The scaffold writes `[suite]` with
`name`, `owner`, `version`, `license`, and `packages = []`; the member
list may be empty.

`alcy new` and `alcy init` look for the nearest ancestor manifest,
starting at the target's parent. A suite manifest there makes the new
package a member: its suite-relative path is appended to `packages` by
a textual insertion that preserves the rest of the file, and standard
output says so. A package manifest stops the walk; the new package is
standalone. A path with slashes creates the directories it names, and
the package name is the last segment, validated like any other.

Building is the same shape as a package build with the roles scaled up:

- A suite root's `build` and `check` cover every member. `run` needs
  one program, so a bare suite root refuses and takes a member path
  (`alcy run first-party/foo`).
- A member's output goes to `<suite root>/out/<member path>/`, whether
  it was built from the suite root or from inside the member. A
  standalone package keeps `<package>/out/`.
- A member build resolves its enclosing suite by walking up from its
  root; a package under a suite root that the suite does not list is
  standalone, not silently claimed.
- The goal's `.alcy/` is the suite root's when the suite is the goal
  (`docs/adr/0019-goal-config-in-dot-alcy.md`'s rule, applied to the
  new target).
- `-o` is refused when the target expands to more than one member, the
  same way it is refused for a package with two targets.

## Consequences

- Every manifest in the repository gains a `license` key. The change is
  mechanical, and it is the point: no later reader has to guess whether
  the key's absence meant nothing or meant `""`.
- `[suite] owner` relaxes from required to optional. Dependency
  matching still compares it, so a suite without an owner is a local
  build target rather than a dependency address until the package
  manager defines one.
- The linter gains a manifest item for `0.x` requirements.
- `valid_version_req` is replaced by the requirement parser. Spellings
  the hub never served (`"1.2"`, `"=1.2.3"`) are rejected in favor of
  the grammar above; there is no migration to write because no fetcher
  reads them yet.
- A member scaffolded inside a suite spells `version.suite = true` and
  `license.suite = true`, so the suite's version and license reach it
  without a second edit, and the member's identity reads from its own
  manifest.
- Suites stay one level deep, and an address never has more than three
  segments; membership is zero or one suite per package.

## Alternatives considered

- **Implicit inheritance.** Rejected: a manifest read alone would no
  longer describe the package, and the value would depend on which
  directory the reader walked up from.
- **Bare-string dependency values.** Rejected: a TOML string cannot say
  whether it is a version or a path without hidden rules, which is the
  ambiguity this manifest format otherwise avoids.
- **Caret and tilde requirements.** Rejected: widespread but not
  uniform (`~1.2` means a minor lock in Cargo and a major lock in
  Composer), and every range they spell is writable here as a wildcard
  plus a comparator.
- **Nested member paths in the address** (`owner/suite/foo/bar`).
  Rejected: the address's meaning would depend on the suite's internal
  directory layout, and the same member could be addressed two ways.
- **Members only at the suite root.** Rejected: grouping directories
  are the reason `alcy new first-party/foo` exists, and the address
  hides the layout anyway.
- **One flat `out/` for the suite with collision errors.** Rejected:
  member names are unique in the address space, but target names are
  not, and a member's prefix keeps an artifact's owner visible at the
  path.
- **An `alcy suite <verb>` command family.** Rejected: a suite
  directory is a target the way a package directory is, so `build`,
  `check`, and `run` keep their spellings.
- **Erroring on a package under a suite root that is not listed.**
  Rejected: a suite claims exactly what it lists, and scratch
  directories under a suite root are the author's business.
