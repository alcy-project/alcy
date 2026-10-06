# Performance guide

How to find a cost in the compiler, show that it is there, and show that a
change removed it. `tools/perf.py` runs the measurements; this document is the
loop they are steps in.

## The number that decides

Retired instructions. `perf stat -e instructions` prints the work a program
did: the same number on every run, on a quiet machine and on a busy one, and a
cache miss adds cycles to a run rather than instructions. Wall clock moves with
the machine, so it answers the second question and not the first; ADR-0021
records why a timing is a report rather than a gate
(`docs/adr/0021-benchmark-measurement.md`).

Two questions, two measurements:

- *Is there work to remove?* Instructions against a generated package.
- *Did removing it help?* Wall clock, two binaries, alternating runs, the
  minimum of N (`tools/perf.py ab`).

A cost that grows faster than the package is the one worth finding. A cache
miss cannot make an instruction count grow, so per-module instructions that
rise with the module count are work that no module asked for.

## The loop

### 1. Scale the shape

```bash
uv run ./tools/perf.py scale --shape plain --sizes 300 600 1200
```

This writes the same package at each size under `out/perf/`, runs
`alcy check` on it, and reports instructions per module and the growth between
sizes:

```text
  modules  instructions  per module   growth
      300        200.6M      668.7K        -
      600        405.0M      675.0K    1.009
     1200        829.2M      691.0K    1.024
per module = 661.3K + 25 x modules; at 1200 modules 4.3% of the run grows with the package
```

`growth` is the per-module cost relative to the previous size: 1.00 is a cost
proportional to the package, and 1.05 per doubling is about 10% removable at
1200 modules. The fit line splits the run into the part every module pays for
and the part that grows with the package; the second is the target.

The shapes exist to put weight where a cost can hide, and a corpus whose
modules do not mention each other would not:

- `plain` - a module with types, methods, an enum, a match, a loop, and calls
  to every other module from `main`.
- `generic` - every module declares a generic type, instantiates two
  instantiations of it, and drops them.
- `drop` - every module declares a type that owns storage and has a
  destructor.

`--main-calls` fixes the size of `main`. By default `main` names every module,
so it grows with the package and its per-item work is indistinguishable from
package-scale work; a fixed `main` separates the two.

### 2. Localize it

Build a release binary that keeps its symbols. The knob exists, so no edit to
the build is committed and none is reverted afterwards:

```bash
uv run ./tools/build.py --target=alcy --mode=release \
  --build-subdir=opt --gn-arg=perf_symbols=true
```

The binary with symbols is `out/opt/exe.unstripped/alcy`; `out/opt/alcy` beside
it is the ordinary stripped one. Then:

```bash
uv run ./tools/perf.py profile out/perf/plain300 out/perf/plain1200 \
  --binary out/opt/exe.unstripped/alcy
```

This records the same shape at two sizes, reads each symbol's self time from
`perf report`, and reports it per module at both sizes, largest growth first. A
symbol whose per-module cost rises is the one to read. For a closer look at one
size:

```bash
perf record -F 999 -g --call-graph dwarf -o /tmp/p.data \
  out/opt/exe.unstripped/alcy check -j 1 out/perf/plain1200
perf report -i /tmp/p.data --stdio --no-children --percent-limit 1
```

Two traps:

- Inlining folds a callee into its caller. Profile the tree you are changing,
  and read a symbol's self time rather than its children.
- A cost proportional to the package is not growth. One-time work, and work on
  a `main` that grows with the package, both look like a rising per-module
  number in a single pair of profiles; the fixed-`main` shape tells them apart.
- A sample is a cycle, not an instruction. A symbol whose share rises may be
  doing the same work with worse locality, which no table removes; `scale`
  says whether there are instructions to remove, and `profile` only says where
  to look. A symbol whose per-module wall time does not move is the tell.

### 3. Read the code for the shape

A function whose per-module cost rises is asking a question whose answer got
more expensive, so look for the question rather than the instruction. Every
cost found this way so far was one of these:

- **A question answered by walking the package.** "Which declaration owns this
  type?" was asked once per method call, field read, match, and diagnostic, and
  answered by scanning every nominal. The fix is a table filled where the entry
  is created or completed; when the walk returned its first match, the table is
  filled so that the first entry of a key wins.
- **A request that re-scans what it could look up.** Generic instantiations
  were matched against every instance the package had minted, generic impls
  against every module's items, drop glue against every method, a loaded name
  against every loaded file, a module against every sibling.
- **A scan per item of input.** The keyword table was compared against every
  identifier, and the file selection was re-scanned once per file.
- **Addresses as keys.** An address as its own hash shares its low bits with
  every other aligned key, so a power-of-two bucket count sends them to every
  k-th bucket; mix the bits before the bucket is picked.
- **Per-call temporaries.** Sets, stacks, and projected lists built in fresh
  vectors per block per iteration. Keep them as members, clear them where they
  are used, and assign into a row rather than moving out of a temporary: the
  capacity stays and the sweep stops allocating.

The tables are asked and never walked, so the order a run reports does not
change (`docs/adr/0048-output-does-not-depend-on-the-job-count.md`).

### 4. Verify

- Emit the IR before and after the change and compare it byte for byte. Two
  binaries, one corpus: `alcy build --emit llvm-ir -o before.ll <package>`.
  Identical output is the strongest evidence that the change is a lookup and
  not a decision, and it holds for whole corpora.
- `uv run ./tools/perf.py determinism out/perf/plain300` builds the emitted IR
  at several job counts and repeats and fails when there is more than one hash.
- `uv run ./tools/perf.py ab <corpus> <before> <after>` for the wall clock,
  interleaved and minimum of N; keep one binary per step so each delta is
  attributable.
- The unit suite, the wasm suite, and `./tools/check.sh --no-coverage`.

## Tools

| command | what it does |
| --- | --- |
| `perf.py corpus --shape plain --modules 300` | writes one corpus under `out/perf/` |
| `perf.py scale --shape plain --sizes 300 600 1200` | instructions per module, and the growth between sizes |
| `perf.py profile A B --binary BIN` | per-module self cost at two sizes, from `perf report` |
| `perf.py ab CORPUS BEFORE AFTER` | interleaved wall clock, minimum of N, with per-phase totals |
| `perf.py determinism CORPUS` | one emitted-IR hash across job counts and repeats |

All of them take `--binary`, defaulting to `out/build_release/alcy`. Generated
corpora live under `out/perf/`, which is not tracked.

## Limits

- A profile's percentages are shares of a run. Compare two recordings of
  similar total work: for a four-fold size step, roughly thirty runs of the
  small corpus and eight of the large one, which `profile` does by default.
- The suite runs the phases in a fixed order and reports each phase's total, so
  a phase that nests inside another contributes to both; compare a phase with
  itself across two runs.
- A shared machine is not a measurement instrument. Prefer the instruction
  count, and when timing is unavoidable, alternate the binaries and take the
  minimum rather than the mean.
