# ADR-0021: What a benchmark number is, and what it is comparable within

- Status: Accepted
- Date: 2026-09-29

## Context

Two ways of timing the compiler exist. `--time-trace` attributes one run
to its phases, and the process runner in `tools/run_benchmarks.py` times
the real binary as a process, so a number there includes startup,
argument parsing, the filesystem, and the linker. Neither answers what a
phase costs once the file read is excluded, and neither yields a
distribution: both produce a single run.

What is missing is a measurement of one phase, in memory, repeated often
enough to have a percentile, and able to time an operation that is
shorter than the clock's own cost.

Google Benchmark was considered and not adopted. The compiler is
expected to be rewritten, and a third-party harness would have to be
rewritten with it; the harness is instead built in-tree, so that what
survives the rewrite is the measurement contract rather than a
dependency. That reasoning shapes this ADR in one specific way: what
must be small and replaceable is the *boundary*, not the amount of code.

## Decision

A microbenchmark engine lives in `src/benchmarks/`, built as its own
executable target and never linked into `alcy`. It calls the pipeline's
public API and adds nothing to it.

**Clock.** The clock is a policy with one method, `now_ns()`, behind a
`Runner<Clock>` template. The implementation is `std::chrono::steady_clock`
and nothing else: no platform branch, no third-party clock. The runner is
templated so a test supplies a fake clock, which makes durations
deterministic without a platform in the loop.

`resolution` is `steady_clock::period` and `overhead` is measured by
timing back-to-back reads; both travel in the result metadata, so a
reader can tell what the number includes. Dedicated platform clocks were
rejected: their stated advantage is immunity to clock slewing, which is
irrelevant for a run measured in hundreds of milliseconds, and their
vDSO cost is the same, so batching is required either way. `CLOCK_MONOTONIC_RAW`
is also not available under Emscripten, which a dedicated clock would
have to branch around in a target that must still compile. Replacing the
clock later touches one file, which is the property the rewrite
argument is actually asking for.

**Batching.** An operation whose single measurement is below
`min_batch_ns` is repeated until the batch reaches it, doubling from one.
The batch size and the per-operation figure are both recorded. Operation
and batch samples are never pooled into one percentile series: they
answer different questions and a percentile over the mixture is not a
statement about either.

**Policy.** Sample count is not fixed across cases, because a lexer's
token and a linker's link differ by orders of magnitude. A case declares
`{warmup, samples, min_duration_ns}` and the engine runs until both are
satisfied, recording what it actually did. Fewer than 30 samples is
recorded as low confidence.

**Isolation.** The default is one process for all cases. The compiler is
single-threaded, so LLVM's managed statics have little to contaminate,
and a separate process per case buys isolation at the cost of a runner
that has to supervise one process per case. `--isolate` is added if
contamination is observed. Process startup is never part of an engine
measurement; that is what the process runner is for.

**Fixtures.** Sources come from a deterministic generator with three
dimensions: function count, statements per function, and nesting depth.
The cases run through `check_source`, which takes bytes, so neither the
filesystem nor a temporary directory is in the measured path. Module
count is not a dimension here: a manifest is needed to vary it, and that
puts the filesystem back in.

**Statistics.** The engine computes min, p50, p95, max, mean, and
standard deviation. Raw samples are always written, so a distribution
question is answered from a stored file rather than by adding a
statistic to the engine. The percentile rule is the same one the process
runner uses, linear interpolation, and a test pins the C++ and Python
implementations to each other; two runners reporting `p50` must mean the
same number.

**Schema.** A micro record shares the process record's envelope and
distinguishes itself with `kind: "micro"`. Comparison selects records by
`benchmark_id`, so the two can never be lined up against each other by
accident.

**Reconciliation.** A percentile is self-consistent whatever it
measures, so the engine is checked against something. What it is checked
against is itself, run twice: a case's median has to land in the same
place twice on one machine, and if it does not, the clock, the warmup,
or the batching is wrong and every figure the engine ever produced is
suspect. This is the gate, and it runs in CI.

The engine is *not* checked against `--time-trace`, because the two do
not measure the same thing and cannot. The engine reports a phase's
steady-state cost: the same bytes, repeatedly, warm. A trace run is one
invocation, whose phases pay the file's first touch once and also stage
the standard library. Measured on the same input, the trace's tokenize
total ran several times the engine's, and the ratio changed with the
size of the input, so no tolerance would make the two agree and a check
built on agreement would be a check that passes by being loose.

`reconcile` therefore prints the two side by side, so the gap between
them is visible rather than assumed, and gates only on reproducibility.
The check runs on a release build: a debug build carries assertions and
sanitizers, and the same parse measured three times in one moves by
about a sixth, which is not a figure to gate on.

## Consequences

- The engine's numbers are known to be stable, and known *not* to mean
  what a trace run means. A reader comparing the two has to know that,
  and the tool says it every time.
- A rebuild of the harness in another language reuses the contract, the
  case list, the fixtures, and the statistics rules; only the
  implementation is new, and the clock boundary is the part that is one
  file wide.
- Absolute numbers are only meaningful within one host, one build
  configuration, and one fixture digest, and every record says which.
  A debug build's figure describes the build rather than the compiler.
- Raw samples make the result files larger than the summary a reader
  wants. That is the cost of not having to re-run to check a claim.
