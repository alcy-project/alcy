# ADR-0044: The trace has two readers, and the text one is pruned

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
carries its own duration and its share of the run's wall clock - the
share is of the wall, never of the parent, because two children that ran
on two threads together exceed their parent and the report is still
right. Equal intervals read as siblings: nothing in the events says which
wrapped which, and a tie broken by record order would make a threaded
region's shape depend on the schedule.

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
- `runtime` and `configure`, which were inside `emit-object` with no
  name of their own;
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
- The wall clock a share answers against is the run's earliest start to
  its latest end, not the envelope's `wall_ns`: the envelope covers the
  whole invocation and the trace only the phases inside it.
- `debug::ProfileSection` and the LLVM pass timer must read the same
  clock, or the pass events nest under nothing and read as roots. That is
  `debug::current_timestamp_ns` for both, and the ADR records it because
  the failure is silent - a flat list of passes that still looks like a
  profile.
- The LLVM pass callbacks fire on the thread the pass runs on. They
  profile nothing on another thread today because passes do not move
  work; a pass that does will show as the pass that moved it.
