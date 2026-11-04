# Performance guide

How to find a cost in the compiler, show that it is there, and show that a
change removed it. `tools/perf.py` runs the measurements; this document is the
loop they are steps in.

## The number that decides

Retired instructions. `perf stat -e instructions` prints the work a program
did: the same number on every run, on a quiet machine and on a busy one, and a
cache miss adds cycles to a run rather than instructions. Wall clock moves with
the machine, so it answers the second question and not the first; ADR-0021
records why a timing is a report rather than a check
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

A slope at or below zero is the usual result and is not a measurement error:
startup, artifact resolution, and the summary sweeps are fixed work, so they
amortize as modules multiply and the fit tilts down. A slope far below zero
means the line no longer describes the range at all - a fit of -235 x modules
reports a negative share of the run - so read the constant, not the slope.
Run every shape before concluding a run is linear: the constant ranges from
545K to 884K per module across them, so a change that moves one shape's
constant proves nothing about another's.

Memory reads the same way. `perf.py rss --shape plain --sizes 300 600 1200`
prints the peak per module at each size; across the shapes the peak is 65-71
MiB at 300 modules and 86-114 MiB at 1200, so the per-module figure falls from
221-243K to 73-97K and the fit's slope is at or below zero. Read the marginal
and not the total: the base is the runtime and the heap the first modules
touched, and it is what makes a small corpus look expensive per module. What
the modules add is 24 to 50 KiB each, depending on the shape.

The formatter and the linter scale the same way. `--subcommand` names the run
to measure, as words:

```bash
uv run ./tools/perf.py scale --subcommand 'fmt --check' --sizes 300 600 1200
uv run ./tools/perf.py scale --subcommand lint --sizes 300 600 1200
```

Both are linear, and neither is cheap: `fmt --check` runs 719.5K per module at
300 modules and 710.5K at 1200, `lint` 642.0K and 631.4K, against 578.6K and
568.0K for `check`. The formatter pays the most per module of the three
because a package-wide run resolves the artifacts first - which parses every
file it will later format - and then lexes each one again into the concrete
syntax tree it prints. Only `check` has its corpus validated first: a
formatter rewrites files and exits non-zero on a package it would change, and a
linter reports findings, so both are measured as they run.

A series that already exists needs no rebuild. `perf.py series` measures the
corpora named on the command line and prints instructions and peak RSS per
unit, with the growth between neighbours:

```bash
uv run ./tools/perf.py series --subcommand 'fmt --check' --units 5 50 100 200 \
    --unit depth out/perf/nest5 out/perf/nest50 out/perf/nest100 out/perf/nest200
```

```text
    depth  instructions    per unit   growth    peak RSS   per unit   growth  corpus
        5          4.1M      824.3K        -      54484K   10896.8K        -  nest5
       50          5.1M      101.1K    0.123      54744K    1094.9K    0.100  nest50
      100          6.1M       60.8K    0.602      55500K     555.0K    0.507  nest100
      200          8.1M       40.7K    0.669      56108K     280.5K    0.505  nest200
```

`--units` is what grows across the series - nesting depth here, and the modules
each corpus declares when it is left out. Subtracting the base every run pays
leaves about 1.03M of instructions per 50 levels of nesting and 1.6 MiB of peak
RSS for the whole span, so both axes are linear too.

The shapes exist to put weight where a cost can hide, and a corpus whose
modules do not mention each other would not:

- `plain` - a module with types, methods, an enum, a match, a loop, and calls
  to every other module from `main`.
- `generic` - every module declares a generic type, instantiates two
  instantiations of it, and drops them.
- `drop` - every module declares a type that owns storage and has a
  destructor.
- `wide` - one function declares 64 names, guards each in a block of its own,
  and reads them all back; `--width` moves the count.

`--main-calls` fixes the size of `main`. By default `main` names every module,
so it grows with the package and its per-item work is indistinguishable from
package-scale work; a fixed `main` separates the two.

The other axis is one module's body. `--sizes 1 --main-calls 300`, repeated at
600, 1200, 2400, and 4800 with `--binary` naming the symbols build, grows
`main` and nothing else, and it is linear there too: an added statement costs
the same 24.6K instructions at every size, with no drift as the body fills, and
the peak rises 2.2 KiB per line. A per-line cost that climbed with the body
would be a scan or a lookup over what the body already holds, which no
whole-package size can show and a single module can.

Depth is the third axis, and it is the one that stops: nesting deeper than 256
is rejected, and `error[EB005]` names that limit at about 84 nested `if`
statements, so no input carries a scope chain long enough for a walk over it to
add up. Under the cap a level costs the same at either end - 50 levels of `if`
with a binding each measure 5.74M instructions and 100 measure 5.82M, 1.6K a
level - so a lookup that climbed every scope trades against a constant and not
against the input.

Width is the fourth axis, and it is separate from depth: a walk climbs the names
in scope, not the scopes. One function declares `--width` names, guards each in
a block of its own, and reads them all back at the end, so the blocks open and
close around the whole name set. At 64, 128, 256, and 512 names `check` on that
module measures 6.68M, 9.18M, 14.2M, and 24.2M instructions: 39K a name over a
4.2M floor, the same per name at every count. A walk that resettled its view of
the name set once per block would pay for every name once per statement, and the
per-name cost would climb with the count instead.

Every figure above sits on a floor the process pays before it reads a byte, and
that floor is worth knowing because the growth slopes are fitted over it.
`alcy check` on a one-module package runs 4.49M instructions: 1.09M of that is
`_dl_relocate_object_no_relro` alone, and most of the rest is LLVM's
`_GLOBAL__sub_I_*` registration blocks and the command line tables they fill at
startup, none of which `check`, `fmt`, or `lint` ever read. At 1200 modules
the floor is under a percent of the run, but a small-package pair that moves by
a few hundred thousand instructions is reading the floor and not the change. It
is a property of the build - one binary that carries the backend for
subcommands that never reach it - not something an edit under `src/` removes.

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

A sample says which symbol to read; it does not say how many instructions the
symbol costs, and inlining can leave the top of the table without a name. For
that there is callgrind, which counts instead of sampling:

```bash
uv run ./tools/perf.py costs out/perf/plain1200 --limit 25
uv run ./tools/perf.py costs out/perf/plain1200 --callees 'DropElaborator::run'
```

The count is exact and does not need symbols, at the cost of a run an order of
magnitude slower, so it comes after `scale` has said there is something to
remove. The dump is written under `out/perf/` and stays usable
with `--dump`, which compares two rounds at the speed of a parse.

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
- **A call frame around a handful of instructions.** A function whose body is
  one comparison, one reservation check, and one call still pays a frame on
  every entry when it is left out of line, which is a quarter of its own cost
  at a hundred calls per file. A definition in the class body does not decide
  this: the optimizer outlines what it likes. Mark the cheap case
  `[[gnu::always_inline]]` at the call site and leave the rest behind a
  `<name>_slow` or `<name>_indexed` function. Forcing this on the cursor
  tests, the arena appends, and the forwarders of the hot paths took 1.0% off
  the corpus in four steps, each one byte-identical in the emitted IR.

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

## Where the instructions are

The second step of the loop needs a table of modules, and the release binary is
stripped, so a dump attributes very little of it to a source file. `uv run
./tools/perf.py hotspots <corpus>` groups the same self costs by the namespace
each symbol was mangled from, which is the module whose code it is, and
`--by function` keeps the rows when one of them is worth reading alone.

`plain100`, one job, 41.17M instructions:

| module | self | share | |
| --- | --- | --- | --- |
| `lowering` | 7.55M | 18.3% | |
| `parser` | 7.11M | 17.3% | |
| `analyzer` | 6.33M | 15.4% | |
| loader and C library | 5.03M | 12.2% | nothing names a namespace: `_dl_relocate_object_no_relro` 1.67M, `memset`/`memcpy`/`memcmp` 1.31M, three exit handlers 0.31M, 1.4M over 1,200 small ones |
| `lexer` | 3.55M | 8.6% | |
| `std` | 3.00M | 7.3% | containers, inlined into the callers above |
| `ir` | 2.26M | 5.5% | 1.67M of it the storage verifier |
| `borrow` | 1.52M | 3.7% | |
| `base` | 1.31M | 3.2% | the allocator: `operator new` 0.63M, `operator delete` 0.43M, `carve` 0.24M |
| `ast` | 1.00M | 2.4% | |
| `mem` | 0.98M | 2.4% | the arenas |
| `llvm` | 0.70M | 1.7% | |
| the rest | 0.86M | 2.1% | `toml` 0.38M, `pipeline` 0.11M, `hash` 0.10M, `debug`, `path`, `comp` |

Half of the run is the three modules that describe and pick apart a program -
lowering, parsing, checking - and three fifths once the lexer is counted with
them. Everything a list of functions shows on top of that is a
subtotal of one of these rows, which is why the first question about a profile
is which row it belongs to.

A one-module run inverts the table. `plain1` costs 4.54M, and 3.12M of it is
not compiler code: 2,863 `__cxa_atexit` registrations made by LLVM's static
constructors, the handlers that unwind them when the process ends, and the
relocation of the image that the loader section below measures. The compiler's
own share of that run is about a tenth. Small inputs are
therefore a measurement of what happens before `main`, and the loop reads the
larger corpora for anything but start-up.

## What verification costs

Three boundary checks run on every package, in the release binary as well as
the debug one (`docs/adr/0015-boundary-validation.md`, decision 4): the token
stream, the syntax tree, and the IR storage. Their share of `plain1200`:

| call | instructions | share | runs |
| --- | --- | --- | --- |
| `ir::verify_storage` | 20.56M | 4.29% | once per package |
| `ast::verify_file` | 11.53M | 2.40% | once per file |
| `lexer::verify_token_stream` | 3.94M | 0.82% | once per file |

A binary whose three calls return early runs the corpus in `442.80M` against
`479.63M`, `-7.68%`, and emits byte-identical IR for all seven shapes: a
verifier reads the artifact and writes nothing, so the checks are the whole
of that delta. `perf.py costs out/perf/plain1200` puts `ir::verify_storage` at
the top of the self-cost table, and `perf.py callers <corpus> --symbol NAME`
names the entry that calls each.

This is the largest cost left in the run and the one the loop cannot take.
Decision 4 makes the call at the boundary the guarantee, so a release build
that skips it changes what the compiler promises; that is an amendment to
ADR-0015, not a commit that deletes three lines.

## What the loader costs

Every function pointer and vtable a statically linked library contributes to a
position-independent executable becomes a relocation the loader resolves before
`main` runs. The binary carries 132,703 of them, 3.2 MB of `.rela.dyn`, and
resolving them costs 1.6M instructions in every run:

| run | position independent | linked `-no-pie` | |
| --- | --- | --- | --- |
| `check`, 1 module | 4.50M | 2.91M | -35.3% |
| `check`, 101 modules | 40.41M | 38.82M | -3.9% |
| `lint`, 101 modules | 43.76M | 42.17M | -3.6% |
| `fmt --check`, 101 modules | 43.10M | 41.52M | -3.7% |
| `check`, 301 modules | 113.04M | 111.46M | -1.4% |
| `check`, 1201 modules | 440.21M | 438.61M | -0.4% |

The saving is the same 1.59M in every row; only its share changes. Peak RSS
falls with it by 3.6 MiB at any size, because the relocation table and the
pages the loader writes are gone, and the binary loses 3.0 MiB.

`-Wl,--pack-dyn-relocs=relr` keeps the relocations and stores them as 21 KB of
bitmaps the loader walks instead. It is refused at start-up - `DT_RELR without
GLIBC_ABI_DT_RELR dependency` - unless lld found that version on the libc it
linked against, and this host's `libc.so.6` does not export it.

Both are link flags. `-no-pie` trades the address-space randomization of the
executable image for the 1.59M, which belongs to the distribution rather than
to a change made in passing; the table is here so that it can be decided on
numbers.

## What the job count costs

Two loops spread work: reading the package (`pipeline::parse_files`, a file per
slice) and asking LLVM for the objects (`codegen_llvm::emit`). Everything after
the frontend - checking, lowering, borrow checking - runs on one thread, so the
job count can only shorten the frontend. `--jobs` defaults to half the machine
(`PipelineContext::requested_jobs`), four here, and what those four cost is
measurable:

| corpus | `-j 1` | `-j 4` | | wall `-j 1` | wall `-j 4` |
| --- | --- | --- | --- | --- | --- |
| `plain100` | 40.60M | 42.36M | +4.3% | 11 ms | 14 ms |
| `plain600` | 223.12M | 233.50M | +4.7% | 49 ms | 45 ms |
| `plain1200` | 442.48M | 462.47M | +4.5% | 95 ms | 92 ms |
| `plain2400` | 596.29M | 639.99M | +7.3% | 133 ms | 130 ms |
| `plain1-main4800` | 103.98M | 108.65M | +4.5% | 18 ms | 20 ms |

The instruction counts are `perf stat -e cpu_core/instructions/u` and sum every
thread; the wall times are the best of three and sit within a few milliseconds
of each other. The default therefore costs 4 to 7% more instructions than
`-j 1` and buys about 3% of the latency on the corpora long enough for the
difference to show, nothing on the ones where start-up dominates. More workers
do not buy more: `-j 8` on `plain600` is 238.44M and 51 ms, worse than `-j 4`
on both counts, because 600 files in slices of 18 leaves each slice too small
to pay for handing it out.

The cost is per arena allocation, not contention. `plain100` under callgrind
moves from 41.17M with one job to 42.67M with four, and 1.46M of that 1.51M is
`mem::ConcurrentArena::alloc_from`, `lane_size` and `lane_begin`: a lane of a
syntax arena is a range of the same buffer, and finding the bounds of that
range happens on every allocation once there are several lanes, where the
one-lane run folds it away. The price follows the arena traffic the parallel
stage does, which is why it is largest on the largest corpus.

That makes the default a trade of 4 to 7% of the run's instructions for a few
percent of its latency, taken for a user who is not waiting on this compiler
alone. It is the right trade for an interactive run and the wrong one for a
measurement: every number in this document is `-j 1`, which is also the shape
ADR-0048 makes byte-identical to the parallel one, so the two can be compared.

## What the formatter costs

`fmt` costs more than `check` on the same corpus (`473.97M` against
`440.45M` on `plain1200`, both `perf stat -e cpu_core/instructions/u`,
one job). Neither axis it has is super-linear, so no input makes it blow
up:

| corpus | instructions | per module |
| --- | --- | --- |
| `plain100` | 43.13M | 0.43M |
| `plain300` | 121.53M | 0.41M |
| `plain600` | 239.18M | 0.40M |
| `plain1200` | 473.97M | 0.39M |

Doubling the nesting depth costs `+1.03M` from `nest50` to `nest100`
and `+2.06M` from `nest100` to `nest200`: on the depth axis the run is
linear over a constant, so the constant is where the work is.

On `plain100` it splits into the printer that turns the syntax tree into
a layout document (19%), the renderer that gives that document a width
(14%), building the tree (`cst::lex` and `Builder::build_sequence`,
11%), lexing (7%), and memory work (`operator new`/`delete`,
`memcpy`, 6%) spread across all three. A document is a tree of
`std::vector<Doc>` with 56-byte nodes, so a concatenation allocates and
a node move copies 56 bytes; that is the shape a constant-factor change
here has to act on.

## What did not pay

Measured and rejected, so the next attempt starts from a number rather than
from the idea.

- **Gating the keyword lookup on the first byte.** Every identifier is looked
  up among the reserved words, and a 256-entry table saying which first bytes
  can start one looks like a cheaper test than the lookup it guards. It costs
  `+0.43%` on `plain100` (`40.43M` to `40.60M`, three runs each): the
  length switch in front of the table already answers the common case, so the
  pre-test is one more load and one more branch on a path that was going to
  find the answer anyway.
- **A digest in the type instantiation search.** A request for a generic type
  compares its arguments against every instance the nominal has minted, once by
  index and once structurally, which is the shape the function instantiation
  chain had. Storing `args_digest` per instance and skipping both comparisons
  on a mismatch moves `+0.001%` on `plain1200`, `+0.024%` on the `generic`
  shape, and `+0.002%` and `+0.001%` on probes that spell 64 and 128 distinct
  instantiations of one nominal with a twelve-element tuple argument. The list
  behind a nominal stays short, and `types_equal` rejects a candidate after a
  few index comparisons, which is what the digest pays to compute. It paid on
  the function chain because an argument vector there is compared against a
  long list of candidates, not because arguments are expensive to compare.
- **Spreading the borrow check.** Both passes walk the functions in order, and
  the second one only reads what the first wrote, which makes the bodies look
  like free work to hand to threads. On `plain1200` at `-j 4`:
  - a scratch per worker costs `+9.3%` instructions and `+43 MiB` peak, and the
    run is about `10%` slower: the scratch is sized by the package, so every
    worker pays the whole package's setup to check a quarter of it;
  - one scratch shared by the workers, which is sound because a function's
    registers and blocks are its own and so the rows two workers touch are
    disjoint, costs `+1.7%` instructions and `+1 MiB`, and moves the total by
    less than the run-to-run spread: the bodies are a few milliseconds of the
    phase, and the summary sweeps in front of them do not spread.

  Borrow is about `22 ms` of a `138 ms` run at `-j 1` on that corpus, so even a
  perfect spread of the bodies is worth low single-digit percent.
- **Spreading `analyze` and `lower` instead**, where the serial time actually
  is, is not the same move. Both append to the IR as they go, so the type,
  function, and instruction tables would end up ordered by whichever thread
  arrived first, and that order is part of the output
  (`docs/adr/0048-output-does-not-depend-on-the-job-count.md`). Those phases
  need per-module storage merged in module order, which is a design of its own
  rather than a loop to mark parallel.
- **Sorting the drop-elaboration place table.** Building that table by
  appending every key and deduplicating once with a sort, instead of scanning
  it per event, costs `+0.14%` on `plain1200`: the table holds a few hundred
  places, so the scan it replaces is short, while the sort pays `log n` per key.
  Sizing the same table up front instead removes its growth copies and is worth
  `-0.20%`.
- **Forcing the expression ladder to inline.** `parse_range` down to
  `parse_primary` is ten functions calling each other, each calling its child
  twice, and `[[gnu::always_inline]]` on all ten is worth `-1.9%` on
  `plain1200`: the calls between the levels are what the attribute removes. The
  build pays for it: a level that inlines two copies of the level below makes
  the tower exponential, and the file goes from `5 s` to `228 s` to compile.
  The overhead is real, and `parse_binary`'s precedence table is what removes
  it without the copies: `-1.9%`, for a file that then compiles in `2.7 s`
  instead of `5 s`. The attribute is not. Carrying `||`, `&&` and the
  comparisons in the same table folds the three levels that stood above the bit
  operators into the loop, and is worth `-0.85%`: a level costs more for its
  prologue and its own switch than a step of the loop that reads the table,
  while the comparison that cannot chain stays a flag on its row.
- **Walking the trivia of `Lexer::tokenize` by hand.** Reading `pos_` and
  `bytes_` into locals and keeping the byte in a local index, with a run of
  spaces skipped by its own loop, costs `+0.52%` on `plain1200`: the member
  calls it replaces are already inlined into the same loop, so the inner run
  test is paid for on every token and saved on none. Scanning a line comment
  with `find` instead is worth `-0.21%`, because there the loop it replaces
  does real per-byte work.
- **Reopening a checker scope on the buffer the last one closed.** Handing a
  closed scope's binding buffer to the next scope that opens costs `+0.11%` on
  `plain1200`: it saves the allocation a block's scope would make, but every
  scope then pays a move out of the buffer on close and a move back in on open,
  which costs more than growing a buffer of a few bindings from empty.
- **Walking a scope with `std::views::reverse`.** Reading the scope stack in
  `Lowerer::lookup_local` with an index loop instead of the reverse view, and
  dropping the header that view pulled in, moves `plain1200` by less than the
  run-to-run spread (`+0.0003%` over three interleaved pairs): the view settles
  its bounds once per loop and the iterator reads the same word the index does,
  so the loop that remains has nothing left to remove.
- **Recording the borrow check's predecessors as one flat pair.** `preds` is
  sized for the whole package and written once per function, so a `head`/`len`
  pair over a single array, which zeroes eight bytes per block instead of a
  vector's twenty-four, looks like a free third of that table. Over three
  interleaved pairs on `plain1200` the two binaries land within the spread
  (`+0.001%`): `preds.assign(blocks, {})` costs five instructions per row, and
  building the pair costs a count pass, a prefix sum, a fill pass and a pass
  that puts each row back in order over the same rows, which is the same five
  spent differently. The table is `0.11%`, and the five row tables beside it
  are `0.55%` together. The lever is their size, not their shape: a function
  names a few dozen blocks and the package a hundred thousand, so the sizing
  writes an entry for every block no function in it reads.

## Tracing

`alcy check --time-trace` (and `build`) records what the compiler's scopes did
and prints the tree under the result line; with `--json` the same events go out
as `traceEvents` for a viewer. A scope whose name already exists as a view - a
module path, an interned name - costs a null check when tracing is off, which
is what makes instrumentation cheap enough to leave in: the question is only
what to name.

- **The category is the phase, the name is the unit.** `frontend`, `analyze`,
  `lower-fn`, `borrow-fn`, `backend`, and `emit-fn` name phases; within one,
  the row names what spent the time: a step (`nominals`, `signatures`,
  `bodies`, `drops`, `discover`, `resolve-target`, `build-tree`,
  `collect-locals`, `resolve-exports`), a module path, or a function. The
  renderer prints a category only where it differs from its parent's and
  collapses every child past the reader's share into one row, so a hundred
  unit rows still read as a summary.
- **Unit granularity, never per instruction.** A traced `plain1200` carries
  about 23k events and costs 0.89G instructions against 0.82G untraced; one
  scope per instruction would cost the run again.
- **A name must be a view.** The name is interned when the scope is
  constructed, and the macro evaluates its argument before that, so a name
  built with `fmt::format` pays even when no profiler is attached. Pass what
  the work already holds.
- **A region spread over threads passes no profiler.** Recording takes a mutex
  per event, so the workers would contend and interleave; they pass null and
  the region keeps one scope on the calling thread. Per-thread recording is
  what would let a spread region be traced.

Reading the tree is the localization step: `signatures` carrying one module
row that is most of the sweep says the sweep is fine and that module is not,
and `resolve-tree` splitting into `build-tree` and `collect-locals` says which
half of the walk to read next.

## Tools

| command | what it does |
| --- | --- |
| `perf.py corpus --shape plain --modules 300` | writes one corpus under `out/perf/` |
| `perf.py scale --shape plain --sizes 300 600 1200` | instructions per module, and the growth between sizes |
| `perf.py profile A B --binary BIN` | per-module self cost at two sizes, from `perf report` |
| `perf.py ab CORPUS BEFORE AFTER` | interleaved wall clock, minimum of N, with per-phase totals |
| `perf.py costs CORPUS` | per-function self instructions from callgrind, and the call edges around a symbol |
| `perf.py hotspots CORPUS` | the same self instructions summed by module, or by function |
| `perf.py rss --shape plain --sizes 300 1200` | peak RSS per module at several sizes |
| `perf.py callers CORPUS --symbol NAME` | who reaches a symbol, and what runs under it |
| `perf.py determinism CORPUS` | one emitted-IR hash across job counts and repeats |

All of them take `--binary`, defaulting to `out/build_release/alcy`. The two
that read a callgrind dump take it of the unstripped copy the linker leaves
beside that binary, because a stripped one has no names to attribute.
Generated corpora live under `out/perf/`, which is not tracked.

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
