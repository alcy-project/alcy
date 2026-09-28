# Benchmarks

Two runners, kept apart because they measure different things.

- The **process runner** (`tools/run_benchmarks.py`) times the
  real `alcy` binary as a process, so it includes startup, argument
  parsing, the filesystem, the linker, and process overhead. This is
  what a user waits for.
- The **microbenchmark engine** (`src/benchmarks`) times compiler
  modules in process, so it excludes all of the above and can attribute
  time to a phase. It is not written yet; `docs/backlog.md` tracks it.

The two write the same result schema, so a comparison reads the same
either way. What differs is the fixture lifecycle and what the number
includes.

## Process cases

A case is declared in `benchmarks/suites/cases.toml` and runs a
fixture directory under `benchmarks/fixtures/`. The command is an
argument vector, never a shell string, and the runner accepts no
arbitrary flags beyond the ones the case declares.

    uv run ./tools/run_benchmarks.py --build-subdir build_release
    uv run ./tools/run_benchmarks.py --cases compile/executable
    uv run ./tools/run_benchmarks.py --compare before.jsonl after.jsonl

Results are JSONL, one object per case, and the raw samples are always
kept: an aggregate without its samples cannot be checked against a
re-run, and a percentile is a claim about a distribution rather than a
measurement. A schema version travels with every record, so a later
change to the format is detectable rather than silent.

Numbers are only comparable within one host, one build configuration,
and one fixture digest. The runner records all three; the comparison
refuses to compare records that disagree on them rather than averaging
across them.

`compile/executable` and `compile/executable-unoptimized` are the same
work at two optimization levels. Reading their two lines off one run is
what measures what `--release` bought; the comparison subcommand is for
a before and an after of the *same* case, and it refuses records whose
command, fixture, host, or build directory disagree.
