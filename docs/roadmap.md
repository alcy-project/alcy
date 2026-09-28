# Roadmap

Near-term tasks only. Checked when shipped.

- [x] Optimized builds: wire the optimization level through to LLVM
  behind the existing `--release`.
- [ ] Benchmark harness: process runner over the real binary first,
  then the microbenchmark engine on top of O3 and the trace output.
- [ ] `build/scripts` → `tools/` rename, in one shot.
- [ ] `grammar.ebnf` maintenance: every grammar change diffs the file
  in the same commit.
- [ ] Noun-form audit: modules, structs, and classes read as nouns
  (stdlib and compiler alike).
- [ ] Slice support: confirm done or finish it.
- [ ] `unsafe` design as an ADR; implementation waits for the package
  suite.
- [ ] Doc-comment collection in the parser (the SSG itself waits).
- [ ] alcy IR text format: define, serialize, deserialize; ahead of
  `--emit=ir` and any cache.
- [ ] Lib packages: the suite's other half next to `[[bin]]`.
- [ ] Version control option for `new`/`init`: scaffold the ignore
  file for the chosen VCS, never initialize a repository.
- [ ] Command result envelope: one output path rendering as text or
  `--json`, with statistics in the text line and the time trace
  embedded alongside it.
