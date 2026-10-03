# ADR-0038: Library Packages, Loaded by Source

- Subject: the language
- Status: Accepted
- Date: 2026-10-02

## Context

The manifest grammar ends at `[[bin]]`: a package without one
cannot build, check, or run, because every pipeline entry resolves
a binary target first. Path dependencies resolve recursively but
only tests call the resolver, so a build never loads them — the
specification's "path dependencies are source-included" describes an
intent the compiler does not implement, and `[modules] export` is
parsed and never read. ADR-0004 already fixed the architecture
around this gap: closed-world analysis over everything visible,
per-package emission, with the archive format a future design. What
is missing is the shape of a library in that world: how one is
declared, how it builds alone, and how another package reaches its
names.

## Decision

**A library is a `[lib]` table beside `[[bin]]`.** One table per
package, with `name` defaulting to the package name and an explicit
`path` naming the root module file, mirroring what `path` means for
a bin. A package holds at most one of each; `alcy build` emits every
declared target, an executable per bin and a relocatable object per
lib, and one `-o` cannot name two artifacts. A manifest declaring
neither target is the error a manifest without `[[bin]]` already
was.

**A lib builds without an entry.** Entry synthesis — the `main`
wrapper around `alcy_main` — fires for bins only, and nothing
requires a `main` in a lib package; a function that happens to be
named `main` is an ordinary item there. `alcy check` needs the tree
rather than the target, so it stops resolving bins first. `alcy run`
on a package with no bin names the absence instead: there is
nothing to run, which is not the same as declaring nothing.

**Dependencies load as source, under their own roots.** The
pipeline resolves path dependencies with the existing resolver and
stages each package's modules into the one tree behind a fileless
package root, the way staged standard-library members already sit
behind theirs. Package identity is the manifest name with `-`
normalized to `_`, and a `use` spelling `<package>::<path>`
resolves against the dependency's root exactly as the modules
chapter describes. Cycles stay forbidden by the resolver.

**`export` trims at the boundary, nowhere else.** A `use` that
stays inside its package sees every item the module makes visible;
a `use` that crosses into a dependency sees only that package's
`[modules] export` list. Anything else across the boundary is
unresolved, with the diagnostic naming the package that withholds
it. The prelude injection is the package's own implicit surface and
is unaffected.

**Emission stays what it is.** The whole tree analyzes as one
closed world and each build emits its root package — executables
and objects exactly as today, with dependency code compiled in the
way staged standard-library sources already are. No archive format,
no summaries, no new emission model: those arrive with IR
serialization, which is its own backlog item, and until then a lib
artifact is an object file, linkable but opaque. Each target's tree
is the package's declared modules behind that target's root, so a
package with both halves compiles its sources into both artifacts;
per-target module sets are a later manifest question.

**Suite roots resolve through their manifests.** A path dependency
pointing at a directory whose manifest carries `[suite]` instead of
`[package]` resolves to its member packages, each read through its
own manifest; member selection reuses the dependency specifier
grammar the manifest layer already parses. The compiler reads suite
manifests for this and nothing else: registry and git sources keep
their explicit not-implemented, and lockfiles stay out of builds.

## Consequences

What this buys, in slices, is the package ecosystem in working
order: `[lib]` alone builds; path dependencies then load with
cross-package `use` and export trimming as the first end-to-end
value; suite roots generalize the loading last. Each slice is
testable alone — a lib object with no importer, then a bin
importing across a boundary, then a suite root standing in for two
path edges.

What it costs is that every build compiles its whole closure from
source, with no incrementality and no isolation beyond the export
list. That is the closed-world half of ADR-0004 kept honestly: the
per-package half waits on the archive design rather than arriving
as a second tree that pretends at separation.

What is explicitly out of scope: summaries and the archive format;
registry and git fetchers; lockfile consumption by builds; mixing
two bins in one package, which stays rejected; and `alcy new`
scaffolding a lib, which follows the shape once it settles.

## Staged landing

**Landed:** `[lib]` builds; path dependencies load from source with
cross-package `use` and export trimming. A package root answers a
`use` or a qualified path by its identity, and both spellings trim
to the export list at the boundary. A use from inside the
dependency stays inside it, and the standard library keeps its
facade model: its members are addressable by identity, but their
export lists do not trim yet.

**Follow-up:** suite roots (slice 3), where a path dependency
pointing at a suite manifest resolves to its member packages.
