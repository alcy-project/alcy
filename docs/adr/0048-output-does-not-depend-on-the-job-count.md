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

## What holds it

- `verify.cc` asks a table whether an index names a node it holds
  (`bound`) rather than comparing against a reach that counts the room between
  lanes, and walks the nodes a table holds (`for_each_node`) rather than every
  index up to that reach.
- `emit_mode_test.cc` builds a nine-module package at one, four and eight jobs
  and compares the emitted module byte for byte.
- `emit_mode_test.cc` builds an eight-module package that names three things
  that are not there, at one, four and eight jobs, and compares the rendered
  diagnostics.

Both cases matter and neither covers the other: a phase can be right about
what it reports and wrong about what it emits, and the emits are compared
because a module is what a user diffs.

## Consequences

`-j` is a performance switch and nothing else. A user may set it from a build
system without changing what they get, which is what makes it safe to default
it to more than one.
