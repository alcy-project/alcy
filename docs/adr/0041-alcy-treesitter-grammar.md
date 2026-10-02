# ADR-0041: The tree-sitter Grammar

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-02

## Context

The language is stated in `docs/spec/` and implemented once, in `compiler/`.
An editor, however, wants a syntax tree long before the compiler runs, and
wants it for a buffer that does not compile yet. That is a second reading of
the same language, written against the specification rather than against
the parser, and a second reading drifts.

The drift is not hypothetical here. The specification is the normative
statement of the grammar, and `docs/spec/grammar.ebnf` says a change to the
grammar means changing it in the same commit. It cannot say that of a
grammar held somewhere else, because nothing would notice.

A second reading also has requirements the first does not. It must tolerate
error recovery rather than reporting and stopping. It must not be so slow
that an editor stops using it. And it must be built by a tool that is not
the compiler, which means its output is generated, which means a commit can
contain a change with no effect unless the effect is committed too.

## Decision

The grammar lives in `treesitter/`, at the root, as a
[grammar.js](https://tree-sitter.github.io/tree-sitter/) grammar with an
external scanner.

**It is checked against the compiler, not only against itself.** Every
`.al` file the compiler accepts must parse without an `ERROR` or a
`MISSING` node. That is a relation over every source in the repository,
which is what makes it a test rather than an example. The cases whose
failure is a failure to parse are listed separately, and the list is empty:
the grammar reads code the compiler refuses for want of a pass that could
reach the diagnostic. That is the safe direction to be wrong in, and
`treesitter/README.md` says which constructs it happens to.

**The specification does not change.** `docs/spec/` names no implementation,
and a grammar is one. Nothing in the specification refers to `treesitter/`,
and nothing there may.

**It is not a GN target.** The grammar is a C library with no place in the
compiler's build, and adding it to `BUILD.gn` would put it under `gn check`
and `clang-tidy` for a toolchain it does not use. It gets its own check
script, which needs no toolchain but the CLI. `src/scanner.c` is
hand-written and hand-formatted to `.clang-format`; nothing in the
repository formats it automatically.

**Its two halves sit where ADR-0040 puts them.** The corpus needs no build,
so it runs in the Style workflow beside the specification check, which is
also a reading of the language. The comparison against the compiler needs a
binary, so it is one job in the CI workflow rather than a step in each of
the ten matrix entries: it asks whether the grammar and the compiler agree,
and ten answers to one question is the cost ADR-0040 removed the benchmark
smoke for. Both halves are in `check.sh`, which CI runs as `local-gate`,
because a gate the project tells you to run and a gate CI runs are two
gates.

**Generated sources are committed.** `src/` is what a consumer links
without running Node. The drift check is what keeps it honest:
`tree-sitter generate` runs on every check and `git diff` decides whether
what it wrote matches what is committed, so a change to `grammar.js` cannot
land without its tables.

**The CLI version is recorded in `config.toml`**, beside the LLVM one, and
compared by series. The series is the granularity a patch release is not
expected to change; a patch that does change the output is caught by the
drift check, with a diff that explains itself, rather than by a version
string that would refuse to.

## Consequences

An editor gets a tree for a buffer that does not compile, and the
specification gains a reader that will notice when it stops being what the
compiler implements. The cost is a third copy of the grammar to keep
honest: `grammar.ebnf`, `compiler/parser/`, and `grammar.js`. The
differential check is what makes the third affordable, and it is also the
thing that would have to grow first, since it is the only one of the three
that can notice all three disagreeing at once.

When the grammar and the compiler do disagree, the compiler is right. A
grammar has no pass that could reach a diagnostic, so it can only be more
permissive, and the cases where it is narrower than the compiler are the
ones worth reading `treesitter/README.md` for.

Out of scope: bindings for other languages, an injection grammar for the
`fmt` format strings, and any change to how the compiler reports a parse
error.
