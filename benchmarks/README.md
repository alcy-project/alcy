# Benchmarks

Two runners, kept apart because they measure different things.

- The **process runner** times the real `alcy` binary as a process, so
  it includes startup, argument parsing, the filesystem, the linker,
  and process overhead. This is what a user waits for.
- The **microbenchmark engine** (`src/benchmarks`) times a compiler
  phase in process, so none of the above is in the number and a phase
  can be compared on its own.

Both are driven from `tools/run_benchmarks.py`:

    uv run ./tools/run_benchmarks.py process      # the real binary
    uv run ./tools/run_benchmarks.py micro        # phases, in process
    uv run ./tools/run_benchmarks.py compare a.jsonl b.jsonl
    uv run ./tools/run_benchmarks.py reconcile

The two write the same result schema, so a comparison reads the same
either way. What differs is the fixture lifecycle and what the number
includes. `kind` tells them apart and a comparison selects records by
`benchmark_id`, so they are never lined up against each other.

## Process cases

A case is declared in `benchmarks/suites/cases.toml` and runs a
fixture directory under `benchmarks/fixtures/`. The command is an
argument vector, never a shell string, and the runner accepts no
arbitrary flags beyond the ones the case declares.

    uv run ./tools/run_benchmarks.py process --build-subdir build_release
    uv run ./tools/run_benchmarks.py process --cases compile/executable
    uv run ./tools/run_benchmarks.py micro --output out/bench.jsonl
    uv run ./tools/run_benchmarks.py compare before.jsonl after.jsonl
    uv run ./tools/run_benchmarks.py reconcile --build-subdir build_release

Results are JSONL, one object per case, and the raw samples are always
kept: an aggregate without its samples cannot be checked against a
re-run, and a percentile is a claim about a distribution rather than a
measurement. A schema version travels with every record, so a later
change to the format is detectable rather than silent.

Numbers are only comparable within one host, one build configuration,
and one fixture digest. The runner records all three; the comparison
refuses to compare records that disagree on them rather than averaging
across them. A debug build's figures describe that build rather than
the compiler, so `build_mode` travels with the record too.

`compile/executable` and `compile/executable-unoptimized` are the same
work at two optimization levels. Reading their two lines off one run is
what measures what `--release` bought; the comparison subcommand is for
a before and an after of the *same* case.

## What the engine measures, and what it does not

The engine reports the *steady-state* cost of a phase: the same bytes,
run again and until the machine is warm. A `--time-trace` run is one
invocation, and the phases that touch the file pay its first touch once,
along with staging the standard library. The two are not expected to
agree — for a small file the difference is several times over — and
closing that gap would mean measuring something less useful. `reconcile`
prints them side by side so the difference is visible rather than
assumed, and gates on the thing that *can* be checked: that the engine
reproduces its own figures across two runs, which is what catches a
broken clock, a missing warmup, or a batch that swallowed the work. It
runs on a release build, because a debug build does not reproduce.
