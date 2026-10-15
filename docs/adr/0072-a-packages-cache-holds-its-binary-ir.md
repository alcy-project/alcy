# ADR-0072: A package's cache holds its binary IR

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-16

## Context

ADR-0056 gave the binary IR a `FILES` table with content hashes and an
`IDENTITY` digest "so a cache can compare one number instead of walking
the tables", and left the build cache itself — key, location,
invalidation, hit reporting — to its own decision. The roadmap's
package-artifact item opens with that cache.

Two things shape the decision. The binary form already round-trips and
verifies on load (`ir::deserialize` returns a `VerifiedStorage`), but no
pipeline path feeds it; the form's stated purpose is the cache, and the
reader is written and tested, so what is missing is the wiring and the
decisions around it. And the form is *backend-free and unoptimized*:
ADR-0056 writes it "before a backend is chosen", refuses `--release`,
and says the profile composes above the digest. An artifact with those
properties is the same bytes under every profile, every emit mode, and
every backend, which is what makes it the right thing to store first.

What steers the lowered package: the sources' bytes, the pointer width,
and the toolchain — `ir::identity_digest` digests those; the manifest,
whose module membership and dependency selection the digest does not
name; and the named target, which the lowered package may read. The
selection-only knobs (`--emit`, `--backend`) and the profile do not
steer it: they act after the IR exists.

## Decision

**A successful build stores the package's binary IR under one digest
over the inputs that steer it, and a later build with the same digest
loads it instead of running the front end.**

- The **key** is one `u64` (`fnv1a64`, like the identity) over, in a
  fixed order: `ir::identity_digest` over the loaded sources, the
  pointer width, and the compiler version; the manifest's content hash;
  and the named target. The target joins conservatively — a key that
  names an input twice costs a miss, one that misses an input costs a
  wrong artifact.
- The **artifact** is the binary IR: the checked, lowered, unoptimized
  package. A hit loads it and codegen, the optimizer, and the link
  proceed from it as they would from a fresh lowering, so every emit
  mode answers from one entry.
- The **location** is `<package>/out/.cache/<key>.irb`. `out/` is the
  package's output directory, the scaffolded ignore file already names
  it, and `rm -rf out` stays the clean gesture. Only the goal package's
  cache is read (ADR-0019's visibility rule). An entry is written to a
  temporary name and renamed into place, so a hit never reads a
  half-written file, and a file that fails to load is a miss, not an
  error: the cache heals itself by rebuilding.
- **Invalidation** is the key alone: no time-based expiry and no
  partial validation. A compiler upgrade changes the identity's
  version, so it starts a fresh entry set; old entries wait until the
  directory is removed.
- **Only a quiet build stores.** A build whose check reported any
  diagnostic (or that failed) writes no entry, so a hit cannot change
  what an envelope reports: a cached rebuild of the same inputs was
  silent the first time and stays silent. The cost is that a build with
  a standing warning never caches, which is honest until diagnostics
  replay has its own decision.
- The **envelope** (ADR-0020) reports the cache's outcome — `hit`,
  `miss`, or `off` — in the result line and as a JSON field, and
  `--no-cache` turns the lookup and the store off for one invocation so
  the pipeline can be measured and debugged without it.

## Consequences

A rebuild of unchanged inputs skips lexing, parsing, name resolution,
checking, and lowering — the front end — and pays only the load, the
backend, and the link. `check` keeps no cache: its report *is* the
result, and a cached report would be a stale one. `run` inherits the
cache through the build it wraps.

Out of scope, each with its own decision: caching a backend's output
(the profile joins above that key, which is what ADR-0056 anticipated);
replaying diagnostics, which the quiet-build rule avoids for now;
eviction, since one file per key grows until someone removes it; the
summaries that let a dependent package consume a cached one, which are
the item's next slice; and hashing the link inputs a driver resolves
outside the package, which matters only once emitted outputs are
cached.

## Amendments

**The key hashes with XXH3-128.** ADR-0073 replaced fnv1a64
project-wide: the identity, every `FILES` hash, and the binary footer
are XXH3-128, so the key is XXH3-128 over the identity, the manifest's
content hash, and the named target, and an entry's name carries 32 hex
digits. The decision's shape is unchanged — one digest over the inputs
that steer the artifact — and the envelope's `hit`, `miss`, and `off`
are what a caller reads.
