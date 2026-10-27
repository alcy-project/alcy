# ADR-0098: There are no feature flags

- Subject: the language
- Status: Accepted
- Date: 2026-10-27

## Context

Cargo's features are additive flags a dependency's consumer turns on.
The regret is structural: a package's meaning depends on who asked for
it, the build matrix grows with every flag, "it works with my feature
set" is a class of bug report that cannot be reproduced, and a feature
cannot be removed once anything enables it. Conditional compilation
driven by the build has the same shape: the code the compiler checks
is not the code the author sees.

alcy's manifest names a package's modules, its dependencies, and the
suite members it selects. There is no `[features]` key, and the build
identity (roadmap: source bytes, the manifest among them, pointer
width, and the toolchain) is what a cache keys on.

## Decision

**A package has one configuration. There are no optional features and
no conditional compilation.**

- A manifest's dependencies are unconditional: what a package needs is
  what it names, and selecting a package from a suite selects the
  whole of it.
- Platform variation is the target's, not a flag's: a target's
  properties are known to the build, and a future `cfg`-shaped
  facility is spelled `platform_*` (ADR-0088) and is decided by the
  target rather than by a consumer.
- A build's result follows from the sources, the manifest, the
  target, and the toolchain. Nothing about the machine, the
  environment, or who requested the build changes what is compiled.
- A library that would ship optional capability ships separate
  packages, or pays for the capability always. The suite's layering
  (ADR-0084) is the model: `unicode` does not depend on `alloc`, and
  a program that wants no allocation does not select it.

## Consequences

- One configuration means one thing to test: every combination a user
  reports is the combination the maintainers run.
- A dependency's closure is explicit and closed (ADR-0016), so
  selecting a package is a statement about the whole program.
- The cost is more packages where another ecosystem would use flags,
  and a program that wants a smaller dependency pays by selecting
  less rather than by disabling parts of one.

## Alternatives considered

- **Cargo-style features.** Rejected: additive-only, a combinatorial
  test surface, non-reproducible reports, and a package whose
  surface depends on the consumer.
- **Build-time flags or environment-driven compilation.** Rejected:
  the checked code must be the code in the file; a build that reads
  the environment cannot be reproduced from the sources (ADR-0102).
- **A `features`-like selection at the suite level only.** Rejected:
  it is the same mechanism one level up; the suite's package names
  already do the job.
