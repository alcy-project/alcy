# ADR-0143: Small allocations come from per-thread size classes

- Subject: the compiler
- Status: Accepted
- Date: 2026-11-03

## Context

ADR-0021 measures the compiler by the instructions it retires, so who
serves `operator new` is a question about how much of a run goes to
something other than the work the run is named for. On the 1200-module
corpus the allocator was the largest such cost: about 515,000 requests
cost 99.2 M of the run's 659.7 M instructions, 86.4 M of them inside
glibc's `malloc`, `free`, and their chunk bookkeeping. That is more
than the analyzer's largest single routine, and the analyzer is what
the corpus is shaped to stress.

The requests are the pattern a size-class cache serves. They are small,
most of them a few dozen bytes, mostly short-lived, and repeated in the
same handful of sizes as a container grows, shrinks, and is rebuilt one
module after another. glibc answers a repeated request from a thread
cache already, but a miss pays a bin search, and its free path decides
whether the chunk coalesces with a neighbour. `base::Vec` and the
arenas above it decide when allocation happens; nothing in `src/base`
decided how it was served.

## Decision

**The process allocator is the compiler's. `src/base/allocator.cc`
replaces the global `operator new`, `operator new[]`, and their
`delete` forms with a per-thread size-class cache.**

- A block carries a sixteen byte header naming the class it was cut
  for, so a payload handed back reaches its free list through one
  subtraction, and the payload after the header keeps the
  `alignof(std::max_align_t)` the default operator new promises.
- Classes are sixteen byte steps to 256 bytes and 64 byte steps to
  4 KiB. A request above the largest class comes from the system
  allocator with the same header and a class that says so, so both
  paths deallocate through the same arithmetic and a block never has
  to be traced back to the path that produced it.
- Free lists, the bump region, and the chunks it is cut from are per
  thread, and a chunk is never handed back. A block freed on one
  thread joins that thread's list for its class without reading the
  memory of the thread that cut it, and neither path takes a lock.
- A build under AddressSanitizer or ThreadSanitizer keeps the system
  allocator: the file compiles to nothing when either is defined.
- A request for over-aligned storage keeps the system allocator's
  aligned pair, which is separate from the one replaced here, so the
  two never free each other's blocks.
- Containers, arenas, and every diagnostic that reports its own
  accounting keep their interfaces. The change sits below them.

## Consequences

- Allocation itself costs 99.2 M -> 24.2 M instructions on the
  1200-module corpus, 15.0% -> 4.2% of the run.
- Per-shape instruction counts on the 1200-module corpus fall 7% to
  13%: closure 324.5 -> 282.6 M, generic 649.4 -> 568.9 M, adapt
  471.8 -> 417.2 M, print 231.9 -> 207.7 M, drop 610.4 -> 547.5 M,
  plain 704.5 -> 641.7 M, comp 564.2 -> 524.5 M. The formatter gains
  most, because a whole module's document tree is short-lived:
  `fmt --check` over 1200 modules 614.8 -> 501.2 M.
- A free list holds what a phase released until a later phase asks for
  the same size again, so peak RSS grows by that pool: +0.8 MB over
  100 modules, +2.1 MB over 300, +4.3 MB over 600, +8.0 MB over 1200
  (116.1 -> 124.1 MB, 6.9%), and +3.5 MB over 2400. The pool is the
  run's peak live set of each size class, so it is a few per cent of
  the peak and it follows what a phase holds, not the shape of the
  source program.
- A request occupies its class' block plus the header: 24 bytes take
  48 where the system allocator would cut a 32 byte chunk.
- Leak and use-after-free evidence moves to the sanitizer builds. A
  freed block is reachable from a free list, so a valgrind run reports
  pooled memory as reachable instead of lost, and a small overwrite no
  longer meets the allocator's own neighbour.
- Chunks belong to the thread that cut them and are never returned, so
  a thread that allocates and exits leaves its chunks to the process.
  The compiler's worker threads live for the run (ADR-0048), and a
  chunk is 64 KiB.
- `std::malloc` and `std::free` reach the system only from this file;
  the `no-c-heap` rule keeps that true and the three call sites carry
  its ignore.
