# ADR-0141: The shadow stack reserves the frames the program declares

- Subject: the compiler
- Status: Accepted
- Date: 2026-11-06

## Context

The direct-wasm backend runs every function on a shadow stack the
emitter places between the data segment and the heap. The layout
published a fixed reserve of 64 KiB, and a frame prologue compared the
shadow stack pointer's end against the reserve's end, which is where the
heap starts. A program whose frames did not fit therefore panicked
`stack overflow` on wasm while native ran it. Five `exe-large-aggregate-*`
cases are exactly that shape: a 70000-byte array as a parameter, as a
result, or as a binding's storage.

The entry wrapper took its own shadow-stack bytes with no check at all.
A two-variant `main` writes its result through a leading pointer into a
slot the wrapper allocates before the call, and that slot's size is the
result type's size, so `fn main() -> Result<(), [u8; 70000]>` moved the
shadow stack pointer 70000 bytes past a 65536-byte reserve and left it
there. The first allocation then came from the base of the heap the slot
had already crossed. A variable-length `Alloca` bumped the pointer
unchecked for the same reason.

The frames are small in ordinary programs: `fib` sums to 64 bytes,
`format` to 832, and `hash-parity` to 6656. The aggregate cases sum to
580832 and 630036 bytes.

## Decision

The reserve is the sum of the frame bytes the emitted functions take,
plus the slot the entry wrapper takes for a two-variant result, rounded
to a 16-byte multiple, not below 64 KiB and not above 64 MiB. The sum
bounds a chain because a call chain visits each function at most once
unless it recurses, so a program that does not recurse cannot outgrow
it. The floor keeps a small program's layout where it was, and the
ceiling keeps the arithmetic in a word.

Every shadow-stack bump compares the pointer it would leave against the
reserve and panics with `stack overflow`: the frame prologue, the entry
wrapper's result slot, and a variable-length `Alloca`. Nothing takes
shadow stack without answering the bound first.

## Consequences

A program with frames past 64 KiB runs under direct-wasm as it does
natively. The reserve grows with the program's frames, over-fitting what
no single chain visits: functions that never appear on one chain each
contribute their frame to the sum. A program whose frames sum past
64 MiB keeps the panic.

Recursion still meets the reserve. A chain deeper than the reserve holds
panics with `stack overflow` (the `stack-limit` case), which remains a
per-target limit native does not share and no spec states.
