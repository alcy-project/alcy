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

* `NodeVec<T>::emplace_back` returns `(mem - base_ptr()) / sizeof(T)`, and
  every allocation takes exactly `sizeof(T)` -- a whole number of the node's
  own alignment -- so the bump never rounds and the index is the offset in
  nodes. It says so in a comment, and the ten AST tables depend on it.
* `SpanArena` is the same shape over bytes.

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

**C. One arena per worker.** This removes the shared cursor rather than
cheapening it: threads writing to different arenas write to different lines.
It is the only option that scales, and it is a change to what a node index
means, because an index is currently an offset into one arena. It needs the
identity of a node to say which arena holds it -- an extra field, or a
per-arena dense walk everywhere a table is walked today, which means every
consumer of node indices including `verify_file`, the desugarer, and the
analyzer. It also needs each arena's reservation, or one reservation
partitioned up front, and the partition has to be sized from the input.

**D. Fewer allocations.** The parser makes a node per expression, type, pattern
and statement. Node reads dominate a run, so the tables cannot grow a level of
indirection, but a shape that packs children into their parent would allocate
less. That is a language-design question, not a scheduling one.

## Decision

Measure **B**, because it is small, it preserves the contract both callers
already rely on, and it is the last thing that can be done to the cursor
without changing what an index means. Do **C** only if a stage that costs real
time needs it, and treat it as a redesign of the AST's addressing rather than
as a change to fpag.

**A is the answer for now**, and it is a better answer than it looks: the
frontend's cost is proportional to the package again, so nothing is waiting on
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
