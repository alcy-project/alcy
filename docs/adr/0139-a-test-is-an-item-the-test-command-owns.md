# ADR-0139: A test is an item the test command owns

- Subject: the language
- Status: Accepted
- Date: 2026-11-06

## Context

Nothing in a package can state an expectation about itself. The
compiler's own tests are doctest files that link the compiler's
modules, and the standard library has neither: its behaviour is
exercised only from the outside, by buildable example packages, so a
defect in `Array` or `String` is found when some other package
happens to hit it. Those examples cost a manifest each, and they see
the public surface only - which for a standard library written in the
language itself is the wrong side of the boundary: the interesting
invariants live in `core`'s internals.

What a test needs from the language is small: a place to put it, a way
to name it, and a way to observe a failure. It must not need a return
type, an error type, or a runner written in the language, because a
panicking program is already how this language reports an unexpected
state (`docs/spec/errors.md`): `panic` diverges and aborts. A test that expects
two values to be equal fails exactly when the expectation does not
hold, and the abort says where.

The other half of the design is what the item is worth outside a test
run. A `test` block's contents are the module's own code and must
compile against it, so the checker has to see them; but a program
shipped to a user must not carry tests, and a library that is
already generic must not pay for them at all in a build.

## Decision

**`test` is a keyword introducing a module-level block of tests. Its
`test` entries and helper `fn` items are checked and compiled only
when the package is being tested, and `alcy test` is the command that
tests it.**

- The block is module level only, and does not nest: a module states
  its tests once, the way it states its items once. Its members are
  `test name() { ... }` entries and `fn` items, one per line, and
  nothing in it is `pub` - a test is not reachable from another
  module, so a visibility there would mean nothing. Any other item is a
  parse error: a run compiles the entries and the helpers, so an item
  outside those two kinds is code nothing would run.
- An entry takes no arguments, and the parameter list is empty by
  syntax: arguments written at an entry are a parse error, which is
  always detected. A non-`()` return type is the `alcy test` command's
  error rather than the parse's, so a program that is only built never
  sees a body it cannot run. The declared type is kept in the tree for
  that report.
- Success is returning. A failing expectation panics, which aborts the
  process: a nonzero status is what a runner reads, so a panic, an
  abort, or a fault all report a failure, and no error type is
  involved (`docs/spec/errors.md`, ADR-0009 keeps the compiler ignorant of
  `Result`).
- The block is spliced into the program's items only in test mode. The
  rest of the pipeline is unchanged: in test mode each entry becomes
  an ordinary free function, the helpers alongside it, and the entry
  point the test binary runs is generated. In every other mode the
  block is parsed, formatted, and named by the outline, and reaches
  neither the checker nor the generated code.
- One run is one test in one process: the default is a child process
  per test, so an abort costs only the test that aborted, and a
  `--execution=single-thread` mode runs them in the runner's own
  thread for a debugger that wants one process. Parallel execution is
  not offered yet - a test may touch the machine, and contention
  would be the runner's to explain rather than the test's.
- The report goes to standard error, names each test
  `<module path>::<name>`, and orders them as the package writes them:
  module file order, then block order, then entry order. Nothing is
  discovered outside the package under test, so a run is a property of
  the package, not of the tree.

## Run shape

A run compiles the package under test a second time, and the tree it
enters through is that package's own modules with the driver spliced in.

- The entry module is the one the package's first target names - a
  `[[bin]]` target's module, else the `[lib]` target's - and the driver is
  spliced into that module rather than placed beside it: the module keeps
  its items, the driver's follow, and the run declares one entry point
  where the source had another. A test body must resolve its module's
  siblings and its dependencies exactly as its own module does, so it is
  compiled inside that module; a driver in a module of its own would see
  what a stranger sees, which is not the code under test.
- The entry module's own `main` stays an item of the source and is left
  out of the tree a run enters through when the run carries a test,
  because a tree has one entry point and the driver is it: the run's
  `main` reads the test index from its arguments and calls the test that
  index names. A package that declares no test has nothing for the driver
  to call, so the tree enters through the entry module as it is - its
  `main` included - and what gets built is the program the package is; a
  package with no entry point to keep builds through the driver's
  stand-in as before. A module that is not the entry module keeps its
  `main`: an entry point is a rule of the root module alone, and nothing
  in a run calls the other.
- A module that is not the entry module is reached the way any other
  module is reached: each of its entries is an ordinary `pub fn` in its
  own module, and the driver names it with an ordinary
  `use package::<module>::<entry> as alcy_test_<i>;` where `<i>` is the
  test's position in the run. The index is what the driver and the
  runner agree on, so neither holds a table and the driver's text is a
  function of the package's order alone.
- A package that declares no target - every member of a suite - is
  rooted at the driver instead: the driver file becomes the tree's root
  module and the tests hang below it, so a target-less package is
  testable and a suite is still refused as a whole. The driver declares
  the intrinsics it reads its arguments and writes its notes with, so it
  needs neither the standard library nor a dependency the package did not
  already have.
- Only the package under test is spliced. A dependency's tests are never
  read, so a library cannot slow the packages that use it by testing
  itself more.
- The driver is filed under `<package>-test.al`, a name nothing on disk
  is discovered by: it is synthesized once the package's files are in,
  and one source manager can hold more than one package's run.

## Process contract

- One test is one child process. The runner starts the executable with
  the test's index in decimal as its only argument and reads the status:
  `posix_spawnp` and `waitpid` on the POSIX hosts, `CreateProcessA` on
  Windows, where a structured exception carries the failure a signal
  carries elsewhere.
- Failure is a nonzero status or a signal, and nothing else: a test that
  returns has passed, and a test that aborts, faults, or exits nonzero
  has failed. The report names which of the two ended it -
  `FAILED (exit status {})` for a status, `FAILED (aborted by signal
  {})` for a signal - because a fault and a nonzero return send a
  reader looking in different places.
- A panic aborts the process (`docs/spec/errors.md`), so a failed expectation is
  signal 6 on a host with signals and an abnormal status on one without.
  That costs the test that aborted and no other, and the run continues
  with the next index.
- Standard output belongs to the program under test and standard error to
  the report, so a test that prints is never interleaved with the line
  that says whether it passed. In `--json` mode standard output carries
  the document, so a test's bytes are captured and counted as
  `stats.output_bytes` instead of being written there.
- `--execution=single-thread` runs every selected test in one process,
  in selection order, passing all the indices at once. One status cannot
  say which test ended the process, so that mode writes
  `alcy-test: running <label>` to standard error before each test, for
  the person watching the run and for a debugger set on a breakpoint in
  the test. The run does not read the note back: the status is all it
  gets from a process that carries several tests. A process that ends
  other than by returning stopped inside some test, and the ones after
  it have no verdict: the run says it stopped instead of counting the
  tests it selected as passes, and the document carries the same
  statement as `stats.test_ended_early` for a tool reading it rather
  than the lines.
- An executable that cannot be started at all is a failure of the run,
  not of a test: the run has nothing to report a status from, so it
  reports `CannotStartProcess` rather than a link failure, which names
  the step that did produce something.

## Name clashes

A block is spliced into a namespace that already has an owner, so the
splice is the point at which a name can turn out to be taken, and it is
where every one of these is refused.

- `TestCollidesWithItem` - a block item's name is already declared at the
  module's own level. The two are items of one namespace once spliced, so
  the block item is the duplicate: "`{}` in a `test` block is already
  declared in module `{}`; rename the block item". A helper may be
  renamed freely, which is why the block item is the one to move.
- `TestDuplicateName` - two block items of one module take the same name.
  A block holds entries and helpers in one scope, so an entry and a
  helper clash exactly as two entries do: "`{}` is declared twice by the
  `test` blocks of module `{}`".
- `TestEntryNamedMain` - a block declares `main`. The run writes the
  entry point of the tree it enters through, so a test named `main` would
  collide with the one name the driver must have in the module the run
  enters through: "A `test` block in module `{}` declares `main`; the run
  writes the package entry point". The name is refused in every module
  rather than only in the one the manifest happens to name, so what a
  block may declare does not depend on which target the package lists
  first.
- `TestDriverReserved` - a module the run enters through declares a
  name the generated driver declares: the intrinsics it spells out
  (`arg_count`, `arg`, `str_len`, `str_byte`, and `sys_write` when the
  run is one process), and anything under the `alcy_test_` prefix, which
  covers the items the driver names and the aliases it imports. The two
  would be items of one namespace, and a
  duplicate definition would point at the generated module rather than at
  the item the reader wrote, so the join is refused and the reader
  renames the item.

The splice answers the block's names rather than the analyzer because
the splice is where the name leaves the block for the module: it knows
the block item the reader wrote and the module it would join, and the
analyzer would meet a duplicate definition of a function the reader
never wrote. A refused item gets no function, so the tree a run enters
through holds no duplicate and the rest of the package is still checked
and reported. The driver's own names are answered where the driver joins
the module it enters through, for the same reason stated in one place:
the join is where those names leave the generated module for the tree,
and a duplicate there would point the reader at a file that is not
theirs.

## Exit codes

- `0` - every selected test passed, or the run did not need to pass one:
  a listing, or a `--no-run` build. A filter that selects nothing also
  exits `0`, since it ran everything it was asked to run. The filter
  says what executes, not what compiles: a package that declares tests
  builds them however the filter narrowed them, so a package whose tests
  do not build fails a run that selected none of them. A package that
  declares no test still builds, for the same reason: zero tests is not a
  pass, and a package that does not build fails the run however few tests
  it declares. What it builds is the program it is: a module the run
  enters through keeps its own entry point rather than the driver's
  stand-in, so the entry point is compiled too - and a package that has
  no entry point to keep builds with the stand-in as before.
- `1` - a test failed. It is the code every other command uses for its
  own failure, and it is shared with a rejected command line: a shell
  reads one number for "this command failed", and a caller that needs to
  tell the two apart reads the report.
- `4` - the package cannot be tested: it did not build, the directory is
  a suite rather than a package, the target cannot start a process (a
  freestanding target, or the wasm backend), or the executable could not
  be started. Each of those is a statement about the package, not about a
  test, so no test's status is invented for it.
- `2` stays unused. It was the retired `test` stub's "not implemented",
  and no run returns it.

## Consequences

- The standard library can state its own expectations, in the module
  whose invariants they are, with no manifest and no example package.
- A test is checked like the code beside it: the helper a test calls
  is an ordinary `fn` in the same block, and the module's own items
  are as visible from a test as from anywhere else.
- `alcy build` on a tree that has tests is unaffected: the tests parse
  into the tree and stop there, so a malformed body in a test does not
  fail a build - which is what makes shipping a library with tests in
  its source possible at all.
- Test code carries the cost of a second compilation of the package
  under test, not of the program using it. A dependency's tests are
  never built.
- The package a run builds is staged in place of the embedded copy a
  build would use, so while that run builds it the member's modules
  answer as the standard library's own code does: a scalar
  specification target and a sealed implementation target are open to
  them (ADR-0090). Nothing else is staged that way - a dependency a
  suite loaded is still a dependency, and its code is not the suite's.
- The limits are stated, not hidden: tests are per package, sequential
  in one process at a time, and report success only as a status.

## Alternatives considered

- **`test fn name() { ... }` items at module level, one per test**,
  with no block. Rejected: the shared helpers a module's tests need
  have nowhere to live that is not the module's own namespace, so
  every helper becomes a private item of the module and every test
  name must dodge the module's real names. A block is a namespace for
  tests that costs the module nothing.
- **Every `fn` in the block is a test**, as a test module is in some
  languages. Rejected: the helpers are the half of a test suite that
  grows, and a runner that runs them would report a helper as a test.
- **A naming convention instead of a keyword** (`fn test_foo()`).
  Rejected: a convention is invisible to the language and would make
  the block's contents indistinguishable from production code, which
  is exactly the thing the block is for.
- **`Result`-returning tests**, so a test can fail with a message.
  Deferred, not refused: with no `Result` in the compiler
  (ADR-0009), a test returning one would need the runner to know the
  type, and a panic already fails a test with a message and a
  location.
- **A runner written in the language, as an item the package
  declares.** Rejected: it would need the command line, the
  filesystem, and a process interface from `std` before `std` has
  been tested by anything, and it would put the failure model in the
  user's hands.

## Deferred

- Tests that return a `Result`, and with them a failure that is not a
  panic.
- Parallel execution (`--execution=multi-thread`, and `-j N`).
- Running the tests of a dependency, or of every package in the tree,
  from one command.
- The driver's own helpers - the argument intrinsics it declares and the
  decimal it reads - moving to `std::sys` once `std` offers a process
  interface. Until then the driver declares what it needs, which is what
  keeps it dependency-free.
- A test-only assertion helper in the standard library (`std::testing`),
  imported into a block that wants one, so a suite stops writing the same
  helper in every module whose tests check something. Until one exists, a
  block that wants an assertion writes its own.
