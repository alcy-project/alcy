# ADR-0040: The checks that need a build live beside the builds

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
  `src/` falls below `build/coverage_baseline.json`.
- `check_exe.py --sanitize`, which compiles what alcy emits through clang so
  the sanitizers can instrument a program the compiler wrote itself.
- `check.sh` itself, the check `CONTRIBUTING.md` tells a contributor to run
  before pushing.

The last one is the problem the other two share. A check the project tells you
to run locally, and a check CI runs, are two checks; when they disagree the
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

`style.yaml` keeps `spelling` and a new `specification` job, and loses
`lint`. The spec check moves out of the lint job because it reads
documents: it needs the repository, `uv`, and nothing else - no toolchain, no
submodules - and giving it a job that pays for all three is how a fast check
ends up looking like a slow one.

`build.yaml` loses the benchmark smoke step.

`coverage` caches nothing. A stale instrumented tree yields profile data
describing a compiler that no longer exists, and the ratchet then fails on a
number nobody can account for.

## Cost

Every one of these fits a CI job, and the fit was measured rather than
assumed. `tools/measure_checks.py` times each check and reports it as a
multiple of lint, which is the largest: lint is a minute of clang-tidy over
every translation unit, and coverage is about a tenth of it, the sanitized
run about a twentieth, and the benchmark smoke about two thirds.

Two claims in the tree were wrong by this measurement and stayed wrong for
as long as nobody ran it. `0017-verification-strategy.md` and
`CONTRIBUTING.md` both called the coverage ratchet the slowest check in
`check.sh`; it is among the cheapest. The cost was never the reason coverage
was local-only - nothing had established that it was the reason.

`measure_checks.py` is a report and never a check.
`docs/adr/0021-benchmark-measurement.md` refuses to judge a compiler by its
timing because a shared runner is not a quiet machine, and that
reasoning applies to how long a check took at least as much as it applies to
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

**The job that ran `check.sh` whole is gone.** It was the union over the
tree, which built what the ten matrix entries had just built and asked every
check a second time on one runner, so the job cost more than the rest of the
workflow's Linux jobs to answer questions its neighbours had already
answered. The contract it stood for is kept the other way around now: CI
asks each check as its own job, and `check.sh` is the union a contributor
runs, kept a superset by
carrying the checks it once lacked -- the direct-backend wasm acceptance,
the release-mode tests, e2e cases, and exe programs, and the benchmark
smoke. A check the project tells you to run and a check CI runs are still
two checks; what keeps them from drifting is that the local union now
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

**The matrix asks what a platform answers, and a job asks what a verb
does.** `build.yaml` ran the e2e corpus, the exe acceptance, the borrow
rules, the wasm acceptance, and the benchmark smoke in each of its ten
entries, so one question about one corpus had ten answers at ten times the
cost. The borrow rules and the wasm acceptance now run once, in `ci.yaml`'s
`harness` matrix, where each entry is one command and the job is named
after the verb: `Borrow Rules (alcy check)`, `Wasm Acceptance (alcy build
--target=wasm)`. Both read source and ask the driver about it, and the wasm
module comes out of the driver's own backend to be run by the same bun on
every platform: the host answers for none of them.

The e2e corpus and the exe acceptance stay in all ten entries. A case in
either one links a program or runs one, and what the linker names the
objects, what the frame walk finds, and what the runtime prints are
answers a Unix and Windows give differently. `samples` is not a subset of
them -- six example programs, none of them an exe case -- so pointing one
entry's step at it left the other nine unasked about three hundred and
eighty cases, and the failures that arrive on one platform and not the
others arrive there.

**A check that compiles nothing takes the binary instead of making one.**
The format check rebuilt the tree to run a compiler that builds nothing,
which was 543 s of a run for a second copy of a binary the Linux debug
entry had just made. It takes that binary from the entry's artifacts, and
`setup-build` grew the switch that installs the Python venv without the
toolchain. That switch is the only setting the harness passes: nothing it
runs builds a tree, so it has no cache and no build step of its own.

**The first lint shard is the format job.** `tools/lint.py` runs the whole
tree's format dry run from its first shard, so `style.yaml`'s `format` job
paid a toolchain download and a gn generation to ask what the lint job asks
with both already in place: 319 s of setup, 168 s of build, and 161 s of
check. Style now runs no toolchain at all.

**The cache holds the objects, not the binaries.** A debug tree built the
way CI builds it is 1.8 GB, and 894 MB of that is the three binaries a link
produces -- `alcy`, `tests`, and `benchmarks` -- which are also the part a
second run remakes in one step from objects it already has. A 10 GB budget
holds five entries of that size and the matrix keeps ten, and an entry is
written by the run that reads it next, so a run evicted what the next run of
that platform wanted and compiled the tree from nothing. A build directory
now drops the linked outputs before it is saved and keeps every object: the
same tree is 0.94 GB, the budget holds ten of those where it held five, and
what is left is mostly the LLVM install, which changes when the pin does.

**What a run keeps, and for how long.** A build artifact is kept 14 days,
which is the span in which a `main` commit is still read to find out what
broke. A wasm artifact is kept as long for the same reason. The driver the
harness takes is kept one day: the run that made it and the run that reads
it are the same run, and nothing after that downloads it. A failed entry's
`build-record` is kept 7 days, because it answers a question about a commit
someone is fixing now -- nothing downloads it, and by the following week the
log has either been read or the fix has landed.

**Each check reports under its own name.** Protection once named one
context, the `Gate` job, whose single step read the conclusions of the
fourteen jobs it needed and failed unless every one of them succeeded: a job
added above was required by a line in this repository instead of by an edit
to the settings, and the step read the job names out of `ci.yaml` so a job
it did not wait for was a red. That answer had no reader. `main` takes
direct pushes, so nothing consumed it, and the job's only work was to report
last what the jobs before it had already reported.

The job is gone, and with it the audit that a conclusion matched `changes`.
A branch that wants CI to block a merge requires the checks it cares about
by name, or requires the workflow at the organization level, and either way
the answer comes from the job that ran rather than from a summary of it. The
cost is the one the job was written for: a job skipped on a change that did
reach CI is now as green as one that ran, and the only `if:` a job carries
is the one that reads `changes`, so the list that job publishes is the one
thing that has to stay right.

**A required context has to report on every pull request.** `style.yaml` has
no `paths` filter, so its four jobs report on every change and can stay the
contexts they are. `site.yaml` becomes advisory: it deploys, its filter means
it cannot report on every change, and the run that builds the site is what
says a site broke. `ci.yaml` loses its own filter for the same reason the
others keep theirs -- a workflow a filter skipped reports nothing at all, so
a check a branch requires would stay "Expected" with nothing left that could
make it green, and it would block exactly the merges it was meant to unblock.
The workflow runs on every change instead, and a `Changed Paths` job reads
the diff: prose and the site skip the jobs that read a tree, and every other
path runs them. A skipped job still reports a conclusion, which is what lets
it be required by name, and what it cannot report is why it skipped: the
classification `Changed Paths` prints is what a reader of a green run has to
trust.

**These are checks, not gates.** Every one of them is named `check` in the
tree: `check.sh` is the union a contributor runs, `check_fmt.py`,
`check_exe.py`, `check_coverage.py`, `check_treesitter.py` and
`check_spec.py` are parts of it, `CONTRIBUTING.md` calls the ones that read
source "the tree checks", and a status check is what GitHub calls the
conclusion a job reports. "Gate" was a second name for the same thing, and a
second name is a word a reader has to translate. `tools/measure_gates.py`
is `tools/measure_checks.py` now, and the `Gate` job is gone rather than
renamed.
