# ADR-0048: A run's output does not depend on how many jobs it was given

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-05

## Context

The frontend reads a package on several threads
([ADR-0047](0047-the-frontends-parallelism-is-bounded-by-the-arena.md)), and
the passes after it will too
([ADR-0046](0046-the-frontend-cost-is-per-function.md)). A compiler whose
answer uses more of the machine is worth having only if the answer is the
same answer. `-j 1` and `-j 8` must be two ways to run one compiler, not two
compilers.

Nothing enforces that except the shape of how work is handed out and reported.
The order a thread finishes in is not the order a person reads in, and a node
table several parsers fill does not hold a file's nodes together: which worker
took a file decides which offsets its nodes got, and that differs run to run.
So the tree a run builds is **not** the same tree across job counts, and the
output is the same only because nothing downstream reads the tree in that
order.

## Decision

**A phase may spread its work, and the run's output may not depend on it.**
Three rules carry that, and every phase that spreads has to keep all three.

1. **A unit reports into storage indexed by its unit, not by its worker.**
   `base::for_each` hands out an index and a worker; what a unit says goes to
   the index. `for_each`'s own contract says so, and `parse_files` does it:
   one bag per file, indexed by the file.

2. **What the units reported is read in unit order.** The bags are merged in
   file order, not in the order they were filled. A phase that reads its
   results in worker order has already lost, because which worker did what is
   the schedule.

3. **Which units are reported follows the unit order too.** The file reader
   stops at the lowest failing file index and merges the bags up to it, so a
   run that failed says the same thing whenever it failed and whoever found
   it.

And one consequence that is not a rule about reporting:

4. **What a unit writes must not be read in the order it was written in.**
   Passes after the parse read the tree in the order the modules and their
   items appear, which is the order they were written in, and never in the
   order nodes landed. `verify_file` walks a table's lanes in lane order,
   which is schedule-dependent, and that is safe for exactly one reason: every
   node in one table has the same fault, so whichever lane it is found in, the
   same error is reported.

## Repeated runs, not only job counts

Two ways to run one compiler on one package is one property; **the same
answer twice** is another, and it is the one a build system caches on. They
are different: a difference between two runs need not involve a thread, and a
difference between two job counts need not appear twice.

So each case reads the package three times at one, four and eight jobs - nine
builds - and compares every one against the first. A round is the same input
read again and the counts inside a round are the other half.

## What can differ between runs, and why none of it can be read

Every order a run does not control, and what keeps it out of the output:

| order the run does not control | what keeps it out of the output |
| --- | --- |
| which worker took which file, and so which offsets a file's nodes got | nothing reads a table in offset order to produce anything: the passes read a module's items in the order they were written (§4), and the one walk that does read a table in lane order reports an error code that every node in that table shares |
| the interning order, and so the numbers a `StringPoolId` gets | an id is only ever used to look up the spelling. No container is keyed by one, none is compared, sorted, or printed, so the numbering cannot be observed |
| the order units in a `for_each` finish in | a unit reports into storage indexed by its unit, and the reader walks that storage in unit order (§1, §2) |
| the order a directory is read in | `discover_sources` sorts the paths before it loads them, so the file ids it mints follow the sorted order |
| which of several failing files is found last | the lowest failing index decides which are reported, so a failure reports the same thing whenever it is found (§3) |
| the timings, and the order events are appended in | the trace is a record of the run and cannot be deterministic, and it is not part of what a build produces |

Two properties of the code keep that table true rather than merely accurate
today:

- **An unordered container may be looked up and never walked.** Every
  `std::unordered_map` in the compiler is read through `find`; the walk that
  would make hash order observable does not exist. There is no `std::map` or
  `std::set` anywhere, so there is no container ordered by a key that a
  schedule could decide.
- **Nothing orders by address.** A sort whose comparator can tie is
  reproducible given the same sequence, but a container or sort keyed on a
  pointer is reproducible only within one run, because the addresses are the
  loader's to choose.

Neither rule needs a sort to hold today. The two places where a sort is the
mechanism already have one - the closure of a target (`target.cc`) and the
files found under a root (`pipeline.cc`) - and both are there because a
directory and a set of discovered ids arrive in an order that is nobody's
decision.

## What holds it

- `verify.cc` asks a table whether an index names a node it holds
  (`bound`) rather than comparing against a reach that counts the room between
  lanes, and walks the nodes a table holds (`for_each_node`) rather than every
  index up to that reach.
- `emit_mode_test.cc` builds a nine-module package three times at each of one,
  four and eight jobs and compares the emitted module byte for byte.
- `emit_mode_test.cc` builds an eight-module package that names three things
  that are not there, three times at each of one, four and eight jobs, and
  compares the rendered diagnostics.

Both cases matter and neither covers the other: a phase can be right about
what it reports and wrong about what it emits, and the emits are compared
because a module is what a user diffs.

## Consequences

`-j` is a performance switch and nothing else. A user may set it from a build
system without changing what they get, which is what makes it safe to default
it to more than one.
