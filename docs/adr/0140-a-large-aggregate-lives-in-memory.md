# ADR-0140: A large aggregate lives in memory, not in a value

- Subject: the compiler
- Status: Accepted
- Date: 2026-11-05

## Context

Every aggregate crossed the code generator as one SSA value: a
by-value parameter, a returned aggregate, a whole-struct store, and a
binding copy each became a load, a call result, or an insert of the
aggregate's own type. LLVM's instruction selector builds one DAG node
per value and its operand list is bounded, so a value past 65536 bytes
aborted the compiler inside SelectionDAG
(`NumValues wasn't wide enough for its operands`) and the process
died with a signal rather than a diagnostic. Emission had succeeded:
`--emit=llvm-ir` printed the module, so the module held a value the
target could not select.

The reproduction is a program shape, not an exotic one. Any of these
aborted at 70000 bytes and compiled at 65536:

```alcy
fn last(a: [u8; 70000]) -> u8 { ret a[69999] }      // by-value parameter
fn make(fill: u8) -> [u8; 70000] { ret [fill; 70000] }  // result
struct Big { buf: [u8; 70000], tag: i32 }           // whole-struct store
mut b := a                                          // binding copy
[[u8; 70000]; 2]                                    // nested aggregate
```

A fixed array's length is its element's size times its length
(ADR-0136), so a size a program may name has no upper bound below
`isize`, and the compiler must carry every one of them.

## Decision

**An aggregate past the direct limit is a place, and a register of
that type names its address.**

- `IN_MEMORY_AGGREGATE_BYTES` is the limit: a type whose size reaches
  it is never an SSA value. Its register holds a pointer to a frame
  slot, recorded as that register's slot type, and a read materializes
  the load. `Move` and `Borrow` of such a register alias the address,
  which is what an alias between registers already meant.
- A whole-aggregate load or store past the limit becomes a copy of the
  bytes, into a fresh slot and into the destination's slot
  respectively, so no instruction names the width.
- A parameter of such a type crosses as a pointer to the caller's
  slot. A result of such a type crosses in a slot the caller names as
  argument zero, and the signature returns `void`. A call through a
  function value spells the same signature at the site, so a closure
  that takes or returns a large aggregate needs no other rule.
- The entry block copies an incoming slot into a frame slot of its
  own, which keeps the callee's copy its own. A large parameter of a
  block that is not the entry has no phi: its slot is filled by the
  branch that reaches the block, so a join of two arms costs one copy
  per arm and no value.
- The threshold is 4096 bytes, not the 65536 the selector happens to
  refuse. The failing bound belongs to one target's instruction
  selection, holds no meaning for the language, and would move with
  the target; a bound far below it keeps the memory path one shape
  and keeps the values that stay in registers small.
- Observable behaviour does not change. A copy still copies bytes
  (`mut a := [7u8; 5000]; b := a; a[0] = 9` leaves `b[0]` at 7), a
  callee still receives its own copy, and no extern surface moves:
  ADR-0051 already keeps a by-value aggregate out of `extern "C"`, so
  the internal form is the only one that changes.

## Consequences

- A program that moves an aggregate of any size compiles and runs,
  and the shapes in the context above are cases
  (`exe-large-aggregate-param`, `-return`, `-copy`, `-join`,
  `-main-ok`, `-main-err`).
- The entry wrapper supplies the slot for a large result too, since
  `main` may return a two-variant enum whose payload is large
  (`fn main() -> Result<(), [u8; 70000]>`), and the exit code reads
  the discriminant out of that slot.
- An aggregate between the limit and 64 KiB now copies through memory
  where it used to ride registers. That is a cost in the middle of the
  range and the price of one rule instead of two; it is invisible in
  programs that keep their aggregates small, which is where the
  register path still applies.
- A new operation that produces or consumes an aggregate asks the same
  question. A register whose type is past the limit is read through
  the slot, and assigning directly to `values_.register_value` for
  such a type is what a memory-backed value must not do.
