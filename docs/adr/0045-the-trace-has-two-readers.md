# ADR-0045: The trace has two readers, and the text one is pruned

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-03

## Context

`--time-trace` recorded phases and printed one table: every name that
appeared, with its instances summed, sorted by total time. The table
answered "which phase is biggest" and nothing else. It lost the shape -
`resolve` containing `parse` containing `tokenize` read as three
unrelated rows - and it lost the order, because a table sorted by cost is
not a timeline.

The scopes themselves were also too coarse to be interesting. Nineteen
labels covered the whole compiler, most of them pipeline-level:
`analyze` was one number whether the time went into nominals,
signatures, bodies, or drop glue, and `optimize` was one number over an
O3 pipeline of hundreds of pass runs. A profile that cannot say where
inside a phase the time went cannot be acted on.

## Decision

The two outputs get two readers, and each gets the shape its reader can
use.

**Text is a tree.** Events nest by interval containment and order by
start time, so the output is the call tree as it ran, indented. Each row
carries its own duration and its share of the invocation's wall clock -
the share is of the wall, never of the parent, because two children that
ran on two threads together exceed their parent and the report is still
right. Equal intervals read as siblings: nothing in the events says which
wrapped which, and a tie broken by record order would make a threaded
region's shape depend on the schedule.

**The rows are a fixed table.** The name column is forty bytes wide,
with the two-space section margin and two per depth inside it, so the
duration starts at the same byte on every row and going deeper moves the
name without moving the figures. A duration keeps three significant
digits in whichever unit holds them - `4.62 us`, `337 us`, `57.9 ms` -
and a share below a tenth of a percent reads `<0.1%`, which is a
statement about the measurement rather than a rounded zero. A collapsed
row uses the same columns, carries the sum of what it stands for, and
says how many events that was. The header names the event count the
rows were pruned from, so a reader can tell a quiet run from a pruned
one.

**The trace text is not localized, and is ASCII.** It speaks to a reader
who knows the compiler's internals - a reader of the source rather than
a user of the command - so it stays out of the message catalog, which
covers the help text that names the flag. Units are spelt `us`/`ms`/`s`
and the truncation marker is `...`: the output prints on a terminal that
is not UTF-8, and the source stays ASCII, which is what the lint
requires. Colour is applied when the stream takes it - shares and
categories dim, a collapsed row dims whole, and the durations a reader
scans stay plain.

**Text is pruned, and pruning collapses rather than cuts.** A child below
half a percent of the wall is not shown on its own; among the rest, the
largest stay while they cover less than ninety-five percent of the
parent's own time; everything left collapses into one row that says how
many events it stands for and what they cost together. Cutting instead of
collapsing would silently subtract time from the totals a reader is
adding up. Both rules are fixed numbers rather than adaptive ones, so two
runs of one command agree about what they show.

**JSON is every event.** It always was: `traceEvents` in Chrome's
complete-event format, which is what a viewer and a diff read. The finer
scopes land there with no output-side work, which is the other half of
the split.

**The scopes get finer**, in the places where a cost can move without a
pipeline phase moving:

- the package front half: `discover`, `select`, `manifest`, `toolchain`,
  `std-stage`, `targets`, `resolve-target`, `resolve-tree`;
- the analyzer's four sweeps: `nominals`, `signatures`, `bodies`,
  `drops`;
- `lower-fn`, `borrow-fn`, and `emit-fn`, one region per function,
  named for the source spelling - two instantiations of one generic read
  as repeat reports of one name;
- `runtime`, which was inside `emit-object` with no name of its own;
- every LLVM pass run, as `llvm-pass`, through the pass instrumentation
  callbacks `optimize_module` already registers for LLVM's own
  statistics. An O3 build records thousands of these; they are exactly
  what the JSON reader is for.

Per-instruction and per-expression scopes are deliberately not added:
they would multiply the event count by orders of magnitude and the
recorder takes a mutex, so the profile would cost more than it explains.

## Consequences

- A release build's text trace reads as `emit-object` -> `optimize` ->
  the pass pipelines that mattered, with the remainder on one line. The
  individual passes are one JSON query away.
- Every share answers against the invocation's `wall_ns`, which is the
  duration the result line prints, so the note and the percentages agree
  instead of referring to two nearly equal totals. The trace's own span -
  earliest start to latest end - is the fallback for a caller with no
  envelope, and the pruning floor is measured against the same wall the
  shares use.
- `debug::ProfileSection` and the LLVM pass timer must read the same
  clock, or the pass events nest under nothing and read as roots. That is
  `debug::current_timestamp_ns` for both, and the ADR records it because
  the failure is silent - a flat list of passes that still looks like a
  profile.
- The LLVM pass callbacks fire on the thread the pass runs on. They
  profile nothing on another thread today because passes do not move
  work; a pass that does will show as the pass that moved it.
