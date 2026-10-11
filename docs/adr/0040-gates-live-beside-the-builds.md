# ADR-0040: The gates that need a build live beside the builds

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-02

## Context

`style.yaml` and `build.yaml` split the checks along a line that was drawn
when the tree was smaller: style checks that read text, and build jobs that
produce binaries. Lint ended up on the wrong side of it. `lint.py` needs a
compilation database, so it needs `gn gen`, so it needs the toolchain - and
so it took the same time as a build while running on every push that touched
a document. A change to a comment could not be faster to check than a change
to the compiler.

Three checks were not in CI at all, all three reachable by hand:

- `check_coverage.py`, the ratchet that fails when line coverage of
  `compiler/` falls below `build/coverage_baseline.json`.
- `check_exe.py --sanitize`, which compiles what alcy emits through clang so
  the sanitizers can instrument a program the compiler wrote itself.
- `check.sh` itself, the gate `CONTRIBUTING.md` tells a contributor to run
  before pushing.

The last one is the problem the other two share. A gate the project tells you
to run locally, and a gate CI runs, are two gates; when they disagree the
disagreement surfaces as a failure on the server that nothing local
predicted, and the natural conclusion is that the server is flaky. CI
running `check.sh` is what makes the sentence true.

The benchmark smoke is the opposite case and was run ten times: `build.yaml`
is a ten-entry matrix, and each entry ran `run_benchmarks.py smoke`. The
check judges shape - every case present, every sample counted, a clock named,
a figure above zero - and judges nothing about timing. Ten entries were
answering one question ten times over.

## Decision

`ci.yaml` gains the jobs below, each standing on its own:

| Job | Command | Why it needs its own job |
| --- | --- | --- |
| `lint` | `tools/lint.py` | Needs a compilation database of its own |
| `coverage` | `tools/check_coverage.py` | Needs an instrumented build; deliberately uncached |
| `sanitizers` | `tools/check_exe.py --sanitize` | Needs a build, then clang on the output |
| `benchmarks` | `tools/run_benchmarks.py smoke` | One entry's worth of work, asked once |

`style.yaml` keeps `spelling`, `format`, and a new `specification` job, and
loses `lint`. The spec check moves out of the lint job because it reads
documents: it needs the repository, `uv`, and nothing else - no toolchain, no
submodules - and giving it a job that pays for all three is how a fast check
ends up looking like a slow one.

`build.yaml` loses the benchmark smoke step.

`coverage` caches nothing. A stale instrumented tree yields profile data
describing a compiler that no longer exists, and the ratchet then fails on a
number nobody can account for.

## Cost

Every one of these fits a CI job, and the fit was measured rather than
assumed. `tools/measure_gates.py` times each gate and reports it as a
multiple of lint, which is the largest: lint is a minute of clang-tidy over
every translation unit, and coverage is about a tenth of it, the sanitized
run about a twentieth, and the benchmark smoke about two thirds.

Two claims in the tree were wrong by this measurement and stayed wrong for
as long as nobody ran it. `0017-verification-strategy.md` and
`CONTRIBUTING.md` both called the coverage ratchet the slowest gate in
`check.sh`; it is among the cheapest. The cost was never the reason coverage
was local-only - nothing had established that it was the reason.

`measure_gates.py` is a report and never a gate.
`docs/adr/0021-benchmark-measurement.md` refuses to gate on a compiler's
timing because a shared runner is not a quiet machine, and that
reasoning applies to how long a gate took at least as much as it applies to
how fast a program is. A machine that could fail a build for slowness would
be a machine whose opinion changed with its load.

## Consequences

- Lint no longer runs on a push that touched only a document, and a build is
  no longer waiting on it.
- The fuzzer runs in CI as a bounded smoke rather than not at all, and the
  unbounded run stays a development tool. A smoke's value does not decay with
  the corpus the way a full run's does: the corpus and the known-failure
  manifest are what discriminate, so a pass re-verifies what the corpus
  banked and a fail reports a crash it did not.
- `check.sh` reported a missing `typos` by skipping in silence, and now says
  it skipped. The other optional tools in that script refuse; this one was
  the exception, and a check that is green because it did not run is the one
  failure a reader cannot see.

## Amendments

**The `local-gate` job is gone.** It ran `check.sh` whole, which built what
the ten matrix entries had just built and asked every gate a second time on
one runner, so the job cost more than the rest of the workflow's Linux jobs
to answer questions its neighbours had already answered. The contract it
stood for is kept the other way around now: CI asks each gate as its own
job, and `check.sh` is the union a contributor runs, kept a superset by
carrying the gates it once lacked -- the direct-backend wasm acceptance,
the release-mode tests, e2e cases, and exe programs, and the benchmark
smoke. A gate the project tells you to run and a gate CI runs are still
two gates; what keeps them from drifting is that the local union now
contains the CI set, not that one job ran the union.

**Lint is sharded and cpplint is gone.** clang-tidy is the whole of the
lint job's cost and the only net for the bug classes the compiler cannot
warn about -- a `DCHECK` with a side effect, a handle dangling off a
temporary, a use after a move -- so it stays, and the job runs as three
`--shard` shares of the tree so the wall clock is a third of it; the
checks that read the whole tree ride with the first shard. cpplint
overlapped clang-format's include sorting, clang-tidy's naming rules, and
the ast-grep rules, and cost a tool and a config file to say what three
other checks already said.

**A pin is read where it is used.** The workflows once parsed config.toml
in a `load-config` job and handed the values to the jobs that needed
them, and a release workflow that was never handed an LLVM version built
with none. Each job now reads the pin it uses from config.toml itself, so
a workflow cannot forget to ask for a value it uses, and the reusable
build workflows take no version inputs at all.
