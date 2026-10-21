# benchmarks

The measurement engine: a runner over pluggable fixtures, a statistics
pool, and the cases themselves. What a number from each case means, and
which cases exist, are the contract in
[`benchmarks/README.md`](../../benchmarks/README.md); `docs/adr/0021` is
the design.

- `runner.cc` measures one case under a policy (iterations or a time
  budget), rejecting noise rather than averaging it away.
- `statistics.cc` pools samples; a report carries the spread, not only
  the median.
- `case_frontend.cc`, `case_pipeline.cc`, and `case_codegen.cc` are the
  fixtures and cases the smoke run exercises.
- `tools/run_benchmarks.py` drives it; the CI job runs `smoke`.

Cases are measured through the same public entries a user's build would
take, so a benchmark cannot hold an error path open as a shortcut.
