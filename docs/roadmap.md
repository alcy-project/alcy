# Roadmap

Near-term tasks only. Checked when shipped.

- [ ] Optimized builds: wire the optimization level through to LLVM
  behind the existing `--release`.
- [ ] Benchmark harness: microbenchmark engine plus a process runner
  over the real binary, on top of O3 and the Chromium trace output.
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
