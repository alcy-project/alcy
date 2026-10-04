# ADR-0047: The frontend's parallelism is bounded by the arena's indexing

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-05

## Context

[ADR-0046](0046-the-frontend-cost-is-per-function.md) removed the per-function
tables that made the frontend quadratic, and left the stages within a factor of
two of each other. The plan it recorded was to make each stage per-function and
spread it with `base::for_each`, the way reading files already is.

The first stage tried was the ownership check's final pass. It works, and it is
not worth having: that pass is 19 ms of a 220 ms run, and giving every worker a
scratch sized to the whole program costs 8.4 MB each and 63 MiB of peak. The
pass got 1.9x faster and the run got slower. The measurement is in the plan's
own terms: a stage is worth spreading for what it costs, and this one costs
nothing any more, because making it proportional is what removed its cost.

That left the file reader, which is already spread, and which does not pay
either. Measured on a thousand-module package, interleaved, minimum of five:

| jobs | wall      |
| ---- | --------- |
| 1    | 224 ms    |
| 2    | 216 ms    |
| 4    | 220 ms    |
| 8    | 234 ms    |

The parallel region itself takes 36.8 ms of wall to do 40.3 ms of work. Eight
threads buy 1.1x.

## What the 1.1x was

Two shared atomics inside `mem::ConcurrentArena::alloc`.

Every allocation wrote a bump pointer with a `compare_exchange_weak` loop on a
single shared `size_`, so a parse making six hundred thousand nodes made six
hundred thousand read-modify-writes against one cache line. Every allocation
that landed on a page the last one had not reached also called
`commit_pages` -- an `mprotect` -- and published the new watermark with a
second `compare_exchange_weak` on `committed_size_`. Eight threads crossing
that frontier kept crossing it, and the run spent more time in the kernel than
in the work: 0.22 s of system time to do 0.22 s of work, against 0.03 s on one
thread. Context switches were zero, so this was not blocking; it was the
syscall and the second contended line.

Committing a chunk at a time instead of a page at a time removed that half
([fpag `e19eff3`]). System time came back to 0.05 s, and the run went from 4.5%
slower on eight threads to 0.9% slower.

What remains is the first half: 0.18 s of user time on one thread against
0.31 s on eight, for the same work. That is the bump pointer, and it is not
fixable where it is.

## The constraint that decides the rest

`ConcurrentArena` hands out bytes, and both of alcy's arena users turn those
bytes into an index by dividing the offset:

- `NodeVec<T>::emplace_back` returns `(mem - base_ptr()) / sizeof(T)`, and
  every allocation takes exactly `sizeof(T)` -- a whole number of the node's
  own alignment -- so the bump never rounds and the index is the offset in
  nodes. It says so in a comment, and the ten AST tables depend on it.
- `SpanArena` is the same shape over bytes.

And `ast::verify_file` walks every table densely:

```cpp
for (usize i = 0; i < arena.exprs.size(); ++i) { ... }
for (usize i = 0; i < arena.stmts.size(); ++i) { ... }
```

where `size()` is the bump watermark divided by the node size.

**A per-thread bump region breaks that.** Giving each thread a region of its
own means the arena's used offsets are no longer `[0, size())`: each thread
fills the front of its region and leaves a tail. The index is still a correct
offset for the node that holds it, but `size()` now reaches past the last real
node and every dense walk reads a hole. The walk in `verify_file` would read
uninitialised nodes, and every consumer that iterates a table by index would
too.

So the shared cursor is not an implementation detail that can be left out -- it
is what makes the index the offset, and the index being the offset is what lets
a node read be one base addition. That design was chosen on purpose (ADR-0033's
work on the node tables), and it requires the arena to hand out indices in one
global order.

## Options

**A. Leave it.** `--jobs` is within a percent of neutral. It costs nothing to
keep and nothing to use, and the parallelism is not what makes the frontend
slow.

**B. A cursor that costs one operation instead of a loop.** `alloc` needs a
`compare_exchange` loop because it aligns the bump pointer, and under
contention a failing compare costs several transfers of the line. Where a
caller can promise the invariant both arena users already hold -- the size is a
whole number of the alignment -- alignment cannot move the pointer, so a single
`fetch_add` is equivalent and always succeeds. That is one read-modify-write
where there were several, on the same line. It preserves the contract exactly,
including the `nullptr`-when-full answer, and needs no caller changes beyond
choosing the entry point. Expected: a fraction of the 0.13 s of extra user
time, which is single-digit milliseconds of wall.

**C. One cursor per worker.** This removes the shared cursor rather than
cheapening it: threads bumping different cursors write to different lines. It
is the only option that scales, and the shape it has to take is constrained by
what an index means, so it is written out below.

### C, in the shape the index space allows

An index is an offset into one arena, and every table is walked over
`[0, size())`. Those two facts rule out a cursor per worker *unless* the
cursors are lanes of one reservation in a fixed order, because anything else
makes the used offsets sparse and a dense walk reads holes.

So a lane is a contiguous slice of the one reservation, worker `w` owns slice
`w`, and the node index stays `offset / sizeof(T)` with the same base pointer
and the same `operator[]`. What changes is that a lane has its own cursor and
its own end, so `size()` is no longer one number, and the walks become walks
over lanes.

**The bump itself becomes a plain pointer.** A lane has one writer, so
aligning and bumping need no atomic operation at all, and the primitive for
that already exists: `mem::Arena` is exactly a plain bump with a watermark.
It cannot be used as it stands, because it owns its reservation -- `reserve()`
maps its own pages and the base pointer is private -- and a lane is part of a
reservation that is already mapped. It is the behaviour to have, not the type
to use: either `Arena` grows a way to be built over a region, or the lane
keeps being `ConcurrentArena`'s business, which is where the commit and the
exhaustion answer already are. The second is the smaller change and the one
this would take.

Two things have to exist first, and neither is about the arena:

- **The arena has to expose a lane.** Reserve, commit, the capacity check and
  the `nullptr`-when-full answer are its business today; a caller bumping a
  lane itself would have to reproduce all four, including the chunked commit
  above and the rule that a refusal does not move the count. A lane is a
  cursor the arena hands out, not a pointer a caller derives.
- **A unit of work has to know which worker it is.** `base::for_each` hands
  out index ranges through one counter so that a slow unit cannot strand a
  worker, which is what makes the balance good and what makes the worker
  unknowable in advance. A lane needs the second. Either the primitive hands
  out a worker as well -- one claim per thread, which is one atomic per thread
  and free -- or the parser reads a thread-local lane, which needs no plumbing
  through `parse_one` and the parser and does not share anything.

**What it costs.** A lane that a thread does not fill leaves a hole, so a
table pays up to one lane's worth of unused room per worker, and the room a
table has is the room it had: the reservation is not larger, the usable part
of it is smaller. A lane that fills while other lanes have room is the case to
answer -- the fallback is the shared cursor, which is what the table has now,
so the worst case is today's behaviour and not a refusal.

**What it is worth.** The parse is 39.6 ms of work on one thread and 25.3 ms
of wall on eight, and the two runs execute the same number of instructions
with the same number of page faults and no context switches at all. That is a
shared line and not a shared lock: each thread spends wall clock waiting for a
line it must take to itself before it can hand out the next node. Scaling the
parse from 1.7x to the 5x an eight-thread read of forty milliseconds of work
can reach is 20 ms of a 200 ms run.

**D. Fewer allocations.** The parser makes a node per expression, type, pattern
and statement. Node reads dominate a run, so the tables cannot grow a level of
indirection, but a shape that packs children into their parent would allocate
less. That is a language-design question, not a scheduling one.

## Decision

**B is done.** It removes the retry, which is what the contention was made of:
a run reads a thousand-module package in 0.26 s of user time on eight threads
where it was 0.31 s, executing the same number of instructions as one thread.
`--jobs` goes from 17.2% slower than one thread to 3.9% slower.

**C is built.** A lane is a run of the reservation that one thread bumps with
no atomic operation, cut on the node's stride so that the index -- which is the
offset divided by the stride -- still reads what it wrote. Four fifths of a
table is lanes and the rest is a pool the shared cursor hands out, which is
what the table had before: a lane cannot borrow, and the appends of a package
are not spread the way its room is, so lanes alone refuse a package that fits
with room to spare. Six hundred thousand nodes over a thousand modules is
enough to do it at eight lanes.

The room a table has filled is therefore a set of runs and not one range, and
three things read it that way:

* `bound`, which `verify.cc` asks 82 times to decide whether an index names a
  node. It has to answer exactly: a bound that admitted the room between two
  runs would let the walk that follows read a byte no node was written to.
* `for_each_node`, the seven dense walks over the tables, which visit the nodes
  and not the room between them.
* `nearly_full`, which asks the pool rather than any lane. A lane that is
  nearly full is not a table that is nearly full, because what the lane cannot
  hold the pool can; a table is nearly full when its pool nearly is. Reading it
  per lane is what made the first attempt refuse the package above.

`size()` stopped being a count and became the reach of the last run, which is
an upper bound on the nodes a table holds. Everything else that reads a table
by index goes through `bound` now.

What it bought, measured on the thousand-module package with both binaries
built from the same tree, minimum of five interleaved:

| | parse at `-j 8` | speedup | whole run |
| --- | --- | --- | --- |
| before | 14.5 ms | 1.90x | 177 ms at one job, 185 at eight |
| after | 12.3 ms | **2.10x** | 166 ms at one job, 174 at eight |

Peak memory is flat across the job counts, 248 to 250 MiB, and the diagnostics
are byte for byte the same at one, two, four and eight jobs.

**So `--jobs` still does not pay**, and what is left of its cost is not the
cursor. The parse is a seventh of the run and eight threads take it from 26 ms
to 12, which is 14 ms saved against the 21 ms the stage costs when it is spread
at all: a bag and a reserved arena per file, a thousand of each, and the
threads. That reservation is the next thing to look at, and it is a smaller
change than this one was.

**A** stays the answer for every stage that is not the file reader. The
frontend's cost is proportional to the package, so nothing else is waiting on
this.

## Consequences

The parallelization plan in ADR-0046 has a ceiling that is not about the
stages. Spreading a stage over threads spends an arbitrary number of atomic
operations on one cache line inside the arena, so a stage's speedup is capped
by the arena's cursor, not by its own independence. The stages are still worth
making per-function -- that is what made them proportional, and it is what
makes a later stage spreadable at all -- but the order they can be spread in is
not the order they were made independent in.

`--jobs` is kept. It is within a percent of neutral on the corpus measured
here, its memory is bounded, and it is the switch the next stage will be
measured against.

Two bugs in fpag were paid for first, and only a real caller found either. A
slice cut on the alignment rather than the stride gives every node after the
first an index that reads the one before it, and the failure surfaces as a
garbage span in the parser rather than as anything about an arena. A move that
took the reservation and left the lanes behind handed out cursors into pages
the moved-to arena did not own. Both are fixed, and both are the kind of thing
that a test of the primitive alone does not reach: what found them was putting
the primitive under a caller that reads back what it wrote.

The lesson for the plan in ADR-0046 is the one this note already had, made
sharper: a stage's speedup is capped by what the stage writes through, and
changing that is a change to what an index means rather than to how work is
handed out. The cursor was twenty lines; the meaning was eighty-two call
sites.
