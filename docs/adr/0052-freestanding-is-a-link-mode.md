# ADR-0052: Freestanding is a link mode

- Subject: the toolchain
- Status: Accepted
- Date: 2026-10-06

## Context

The C FFI slice made libc callable, which is one half of the
baremetal story. The other half is a program that runs without it:
the roadmap's `_start`, raw syscalls, and an allocator that does not
call `malloc`. Three questions had to be answered before any of that
could land.

Who decides a build is freestanding? A language construct would make
checking depend on the link, and the link is a goal-package choice
that already lives in `.alcy/toolchain.toml`. What replaces crt1's
job? Today the backend emits a `main` the C runtime calls; with no
runtime, the kernel jumps to `_start` and no one returns. And what
happens to the program runtime? It was defined in every module
regardless of use, so an unallocated program still carried a
`posix_memalign` reference in `.text` and could not link without
libc even when it called nothing.

## Decision

**The goal package's toolchain file says `freestanding = true`.** It
is a build mode, not a language mode: checking is unchanged, `fn
main` is still the entry the analyzer validates, and a library built
without an entry is unaffected. The switch travels with the link
options and the run context, so the backend and the linker agree by
construction.

**The compiler owns `_start`.** In a freestanding build the backend
emits `_start` as the executable's entry instead of a C `main`; it
calls the alcy entry and ends through the target's exit syscall. The
status is `main`'s `i32`, zero for `()`, and one for an enum `main`
whose variant is not the first. The sequences are written for
x86-64, aarch64, and riscv64, and a target outside them is refused
at the pipeline with its name rather than miscompiled.

**The runtime is defined per declaration.** A runtime piece is built
only when the program's module declares it, so a module that never
allocates carries no allocator and a freestanding build carries no
libc call it does not make. This also shrinks hosted objects, and it
is what makes the freestanding link resolvable at all: an
unreferenced runtime function kept its libc references alive
because `.text` is one section, so section GC had nothing to bite
on.

**The link skips the C runtime.** Embedded lld gets no crt1, crti,
or crtn, no dynamic linker, and no `-lc`, with `-e _start` naming
the entry; the system driver path gets `-nostdlib -static
-Wl,-e,_start`. The user's own link arguments still apply, which is
how a freestanding program names whatever libraries it does want.

## Consequences

A freestanding binary is static, has no interpreter, and runs on the
kernel alone; the exit status is the program's answer. The exe
harness still runs such a case but skips its sanitized pass, because
the sanitizer's own crt and libraries cannot join a program that
defines `_start`.

What a freestanding program can do is still thin: it computes and
exits. Two follow-ups are next in the same roadmap slice: raw
syscalls, so it can read and write without libc, and a libc-free
allocator behind `alloc`/`dealloc`. Until the allocator lands, a
freestanding program that allocates fails at the link with the
missing libc symbol, which is honest but not the destination.
