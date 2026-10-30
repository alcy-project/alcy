# ADR-0117: The platform floor is `sys`

- Subject: the language
- Status: Accepted
- Date: 2026-10-30

## Context

`core`'s manifest calls it "Language primitives, with no dependency
on any other package", but `mem.al` holds six declarations that are
not language primitives: `sys_write`, `file_size`, `read_file_into`,
`write_file_all`, `arg_count`, and `arg`. They are the compiler's
platform facilities - a descriptor write, three libc-backed file
helpers, and two declarations for the process's arguments - and five
of them are
re-exported by `core`'s prelude, so a program that asks for the
language's base receives the operating system with it. (`sys_write`
is not re-exported, and nothing in `lib/std` calls it; a program
reaches it only by declaring the intrinsic itself.)

The obvious home was `io` (ADR-0016's split makes io the I/O floor
and `network → io`), but io is not a low package: it depends on
`alloc`, `arch`, `fmt`, `text`, and `unicode`. With no feature flags
(ADR-0098), selection names a package and a package's dependencies
are its own, so "a descriptor write without a heap or a formatter"
has no spelling while the calls live in io. The freestanding work
(ADR-0052) wants exactly that spelling.

Two smaller things point the same way. `sys_write`'s declared meaning
is "write all of `buf`", which hides a short write and an interrupted
call that a faithful layer must show (ADR-0023's runtime loop keeps
the write-all behavior where it belongs, under the print hooks). And
keeping OS facilities in `core` makes the suite's one intact rule
("core is the language") the exception.

## Decision

**A suite member `sys`, depending on `core` alone, is the platform
floor: the calls a program makes to the system.**

- `sys` declares `Fd` (a nominal handle rather than a bare `i32`),
  `STDIN`, `STDOUT`, `STDERR`, and the faithful calls on a
  descriptor:
  - `read(fd: Fd, out: &mut [u8]) -> Result<usize, OsError>` and
    `write(fd: Fd, bytes: str) -> Result<usize, OsError>`, one
    `read(2)`/`write(2)` each.
  - A short read or write is reported as itself, zero bytes read is
    the end of input, an interrupted call is `Err` with the
    interrupted kind, and nothing is retried, buffered, translated,
    or allocated.
  - `read` and `write` are safe: a bad descriptor is an error, not
    undefined behavior. Opening and closing descriptors is `unsafe`
    and lands with its own slice, because ownership of a descriptor
    is a contract the type system does not carry yet.
- The error is the call's own: the runtime piece answers `-errno`,
  and `sys::OsError` carries the raw number with a mapped kind. The
  number-to-kind table is per-target and documented; if targets
  disagree the normalization moves into the runtime, which the first
  target does not need.
- The implementation seam is the existing one: intrinsic declarations
  in `sys` and a runtime piece per declaration (LLVM over libc's
  `read`/`write`, wasm over WASI's `fd_read`/`fd_write`, freestanding
  over raw syscalls when that work lands, ADR-0052).
- `sys` also takes the process's arguments (`arg_count`, `arg`): the
  platform gave them, and reading them needs no heap. `io::args`
  stays as the wrapper that copies them into `String`s.
- **The reorg**: `core` keeps only language primitives (memory,
  views, `panic`, `size_of`/`align_of`, the wrapping forms, and the
  intrinsic set the language owns). `sys_write` moves to `sys` and
  becomes the faithful `write`; the whole-file helpers move to `io`,
  which owns "the whole file as a `String`"; and the five `core`
  prelude re-exports retire. Intrinsics are recognized by name, so
  the compiler does not change; `docs/spec/items.md`'s list and its
  "user code cannot name descriptors" wording do.
- ADR-0016's split gains the row `sys → core`; `io → core, sys,
  alloc, arch` plus the `fmt`, `text`, and `unicode` it actually
  uses; and `network`'s dependencies are revisited when that package
  is built, since its current manifest pulls io's whole closure as a
  leftover of the pre-seeded shape.

## Consequences

- Raw I/O is selectable without the heap, the formatter, or text:
  `core` and `sys`, and nothing else. The freestanding path has its
  floor, and io builds its buffered reader, scanner, and printing
  above it (ADR-0118).
- `core` becomes what its manifest says, and the intrinsic home rule
  is one sentence: the package that abstracts the facility declares
  it (alloc already declares its own).
- The user-visible change is small: nothing in `lib/std` calls
  `sys_write` today, and `read`/`write` are new names rather than
  replacements for the library's.
- The cost is one more suite member, and a manifest line for the
  programs that want the floor alone.

## Alternatives considered

- **A raw module inside `io`.** Rejected: io is not a low package,
  and with feature flags banned (ADR-0098) a module buys no
  dependency granularity. It would also take a core-only program's
  descriptor write away when `sys_write` moved.
- **The name `os`.** Not taken: in Rust's family `std::os` means
  platform-specific extensions, and this layer is the call surface;
  `arch` already owns target-specific operations. `sys` says "the
  calls the system offers".
- **Leaving the calls in `core`.** Rejected: they are not language
  primitives, and the write-all meaning is the wrong shape for the
  floor (the faithful call must expose short and interrupted).
- **The whole-file helpers in `sys`.** Not taken: they are io's
  convenience (a path in, the whole content out) over libc's stdio,
  not a kernel call; the abstraction is io's.

## Deferred

- `open`, `close`, and `seek`: descriptor ownership, flags, and the
  `unsafe` boundary.
- The freestanding raw-syscall implementations of the same
  declarations.
- A global input lock and a global buffered stdout (the `thread`
  package is empty; the story lands with it).
- `OsError`'s kind table beyond the first targets, and whether kinds
  are normalized in the runtime.
