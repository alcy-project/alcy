# Backlog

Peripheral work, unordered. When every item is checked the list is
replaced. The foundations are in `roadmap.md`.

- [x] Optimized builds: wire the optimization level through to LLVM
  behind the existing `--release`.
- [ ] Benchmark harness: the microbenchmark engine, on top of the
  process runner in `build/scripts/run_benchmarks.py` and the phase
  timings the result envelope now reports.
- [ ] `build/scripts` → `tools/` rename, in one shot.
- [ ] `grammar.ebnf` maintenance: every grammar change diffs the file
  in the same commit.
- [ ] Noun-form audit: modules, structs, and classes read as nouns
  (stdlib and compiler alike).
- [ ] `unsafe` design as an ADR; implementation waits for the package
  suite.
- [ ] Doc-comment collection in the parser (the SSG itself waits).
- [ ] alcy IR text format: define, serialize, deserialize; ahead of
  `--emit=ir` and any cache.
- [ ] Lib packages: the suite's other half next to `[[bin]]`.
- [x] Version control option for `new`/`init`: scaffold the ignore
  file for the chosen VCS, never initialize a repository.
- [x] Command result envelope: one output path rendering as text or
  `--json`, with statistics in the text line and the time trace
  embedded alongside it. Designed in `docs/adr/0020`.
