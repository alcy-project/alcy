# ADR-0017: Each bug class gets the oracle that can see it

- Subject: the compiler
- Status: Accepted
- Date: 2026-09-27

## Context

The suite grew by adding tests next to the code they cover, which is
right for a function and wrong for a compiler. A wrong answer from the
lowering pass is a passing test suite: the value in the object file is
simply not what the program asked for, and nothing in the build
notices.

The bug hunt that produced this record found 21 defects, and they did not
arrive in one shape:

- 5 were stack exhaustion from unbounded recursion, each of them a
  distinct grammar shape (parenthesised expressions, blocks, unary
  operators, operator chains, tuple patterns) that needed its own
  diagnostic and its own test.
- 9 were semantic: a pattern that matched the wrong value, a format that
  printed twenty digits, a mangler that could not decode a name it had
  just encoded, a manifest diagnostic one column off after a non-ASCII
  character.
- The rest were memory safety, found by AddressSanitizer on inputs the
  suite never generated: a dangling `string_view` into a `std::vector`'s
  elements, a length prefix that ran into the bytes it measured.

Three different tools found these three shapes, and each one found only
its own. The decision is which oracle to reach for, and what each is
allowed to claim.

## Decision

**Verification is layered by bug class, and no layer is a substitute for
another.**

1. **Sanitizers own memory safety.** AddressSanitizer is on for every
   debug build, so a use-after-free or an out-of-bounds read fails the
   suite rather than shipping. It is not enabled in release, and its
   findings are the ones that must never be suppressed.

2. **Deterministic hostile input owns "the compiler answers at all".**
   Generated programs, truncated programs, raw byte strings and random
   path strings run in-process; the oracle is that the call *returns*.
   The generator is seeded (`xorshift64*`), so a failure reproduces from
   the seed without a fuzzer. A random source is the only input class
   that reaches paths a hand-written test never thinks of, and a
   surviving call is a real assertion because a crash ends the process.

3. **Property tests own "the answer is self-consistent".** A test states
   a *relation* between two computations, so it either holds for every
   input or names a counterexample. The strongest form routes through an
   **independent decoder**: `symbol::mangle` is checked by
   `symbol::demangle` round-tripping, so a lost or conflated field has no
   way to be invented back. This is the only layer that caught the
   mangler defect, and the first property test written found it on its
   first run.

   Where no independent implementation exists, the property is a fixed
   point (`serialize -> parse -> serialize`) or a self-consistency of the
   output (`diag::render` must agree with itself about the column it
   reports and the caret it draws). A property with no oracle is not
   written: a test that re-asserts the implementation is a test that
   cannot fail.

4. **libFuzzer owns coverage-guided exploration, interactively.** Six
   targets sit behind `is_fuzz` and are driven by `tools/run_fuzz.py`. They are
   a development tool, not a gate: they need clang's fuzzer runtime and
   take minutes, so `check.sh` does not run them. Two of them assert an
   invariant rather than merely surviving, and abort on a violation,
   because libFuzzer turns an abort into a minimising reproducer.

5. **A target blocked by a dependency defect is reported DISABLED.**
   toml++ aborts on an incomplete table header, which is a *class* of
   inputs no list of hashes can cover. `run.py` prints the target as
   disabled: never counted as passing, never counted as failing, always
   visible. A gate that cannot discriminate is worse than no gate, and
   quietly turning it green would hide that.

6. **Nesting is bounded by one limit, and it is a language property.**
   `base::MAX_NESTING` is shared by the parser, the analyzer and the
   lowerer rather than being four independent numbers, so a program
   accepted at one limit is accepted at all of them. The parser and the
   analyzer both need the guard and neither subsumes the other: a long
   operator chain parses in a loop and so is shallow to the parser while
   building a 20000-deep tree, which only the analyzer's own budget sees.

7. **Coverage is a ratchet, not a threshold.** A fixed target pushes
   effort at whichever module is easiest to move rather than the one that
   matters. `check_coverage.py` fails when line coverage of `compiler/` falls
   below `build/coverage_baseline.json`, and the report names the least
   covered modules so the next test goes where the gap is. All three
   suites contribute: measuring the unit tests alone called the lowering
   pass 33% covered, an artifact of where its tests live, and with the
   exe cases the honest figure is 62%. A report that sends the next test
   to the wrong place is worse than no report.
   `tools/measure_gates.py` times it against every other gate, and the
   ratchet is not among the slow ones - see the cost bullet below.

8. **A test that has text does not need a file.** `SourceManager::add_virtual`
   and `pipeline::check_source` let a case hand the checker a string.
   Most scratch directories in the suite exist only to hold an input
   source, and a case that creates a directory is a case that can fail
   for a reason unrelated to what it asserts.

## Consequences

- Each defect class has a named home, so "the tests pass" stops being the
  only signal and a new test can be placed by asking which oracle could
  have seen the bug.
- The property layer is the one that pays for itself: it found a real
  defect on its first execution, in a function the unit tests already
  covered, and the defect was in the encoding rather than the logic.
- Deterministic generation means a hostile-input failure is a bug report,
  not a "reproduce it locally with libFuzzer" instruction.
- Cost: the layers overlap in what they build, and the overlap is the
  expensive part. `check_coverage.py` rebuilds the tests and the compiler
  with instrumentation, so it builds a third tree beside the two
  `check.sh` already makes, and takes `--no-coverage` to skip.
  `lint` needs a compilation database of its own and is the slowest gate
  in the tree by a wide margin - this file used to call the ratchet the
  slowest, which was wrong, and stayed wrong for as long as nobody
  measured the pair. `tools/measure_gates.py` exists so that a claim
  about which gate is slow is a number somebody ran rather than a
  sentence somebody believed; it is a report and never a gate, because
  the objection to timing a shared runner in
  `docs/adr/0021-benchmark-measurement.md` applies to the length of a
  gate at least as much as it applies to the speed of a program.
- `fuzz/` maintains its own entry points into the same code the unit
  tests drive, which is the other overlap. It is not in CI: a fuzz run's
  value decays as its corpus saturates, so a per-push job would mostly
  re-verify crashes it already knows about.
- The nesting limit is deliberately low (256) and deliberately shared.
  Real code nests in single digits, so the budget only ever rejects
  generated input, and sharing it means the limit cannot drift between
  passes.
- Out of scope: a mutation-testing pass, a differential tester against
  another compiler, and coverage thresholds per function. The last was
  considered and rejected: a function that is mostly error paths is not a
  defect, and a per-function number would be met by deleting the error
  paths.
- Follow-up: the toml++ abort needs an upstream report; the remaining
  input-only directories in the suite are a mechanical conversion that
  wants one file at a time.
