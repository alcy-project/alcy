# IR construction guide

How to build well-formed `ir::Storage` (`src/ir/`). The verifier
(`verify_storage`) is authoritative; this guide explains the conventions it
enforces so frontend code gets them right the first time.

## Type currency

Every type position (`Register::type`, `Operand::type`, `FunctionMeta`,
block parameters, immediates) holds a `TypeIdx`, never a bare tag.

- Primitives: `builder.primitive(TypeTag::I32)`. Tags below `Struct` plus
  `Function` are pre-interned in tag order (O(1), no allocation). In
  constexpr contexts use `primitive_idx(TypeTag::...)` instead.
- Structs: `builder.struct_type(name, fields)` where `fields` is a
  `TypeIdxRange` of already-interned types. Arrays:
  `builder.array_type(element, count)`.
- Reuse: `builder.ref_type(idx)` appends a copy of an existing entry
  (needed for field/parameter lists that mention one type twice).

## Operands

Construct operands only through `Operand::from_register`,
`from_function`, `from_block`, `from_immutable`, `from_external_function`,
or `Operand::invalid()`. Never hand-assemble tag and payload: the payload
is an `AutoTaggedUnion`, so a factory-selected tag always matches its data.

- Inspect with `is<T>()`; extract with `as_register()`,
  `as_immutable()`, etc. (checked in debug builds).
- Dispatch on tags with `Operand::TagOf<T>` as switch labels, not on a
  parallel enum.

## Ranges and ordering

Range members (`operands`, `instrs`, `block_params`, function `blocks`,
parameter/field lists) must reference **consecutive** vector entries.

- Use `SeqBuilder<T>` (`OperandSeq`, `InstrSeq`, `BlockSeq`, …) for every
  range of two or more entries. It records the head on first push and
  verifies contiguity on every push (debug builds).
- Single-entry ranges (`{idx, 1}`) use braced literals directly.
- Blocks with forward references (branch targets) need backpatching: LLVM
  requires the entry block first in vector order, but branch targets must
  exist before the branching instruction references them. Declare the entry
  block empty, build the targets, then fill it with `set_block_instrs` /
  `set_block_params`.

## Verification

Call `verify_storage(storage)` before consuming IR. It returns
`base::Result<void, VerifyError>` covering index bounds, single-definition
of registers (block parameters count as definitions), terminator placement
(last instruction only), callee/branch shapes, call arity, and the
control/memory shapes below. `LlvmIrEmitter::emit()` runs it in debug
builds ahead of LLVM's own verification.

## Discarded values

An invalid destination register (`RegisterIdx(kInvalidIdx)`) means
"discard": void calls, `Store`, `Drop`, and `Fence` use it, and the emitter
skips value binding for any producer with an invalid `dst`.

## Opcode conventions

| Category | Layout |
|---|---|
| `Call` | `operands[0]` is the callee (`Function`/`ExternalFunction`); the rest are arguments, count-checked against the signature. |
| `Br` | `operands[0]` is the target block; `operands[1..]` fill its block parameters positionally. |
| `CondBr` | Exactly `[cond(i1), true_block, false_block]`. Targets take no block parameters in MVP. |
| `Switch` | `[value(integer), default_block, (case_imm, case_block)...]`; case values are integer immediates; targets take no block parameters in MVP. |
| `Ret` | One operand, or none for `void` (`CreateRetVoid`). |
| `Alloca` | The allocated element type comes from the destination register's type; the operand is the array size. |
| `Load` / `Store` | `[ptr]` / `[value, ptr]`; the loaded type comes from the destination register. |
| `GetElementPtr` | `[base_ptr(register), integer index...]`. The element type is recovered from the base pointer's `Alloca` site, tracked by the emitter; pointers from elsewhere are unsupported in MVP. |
| `ExtractValue` / `InsertValue` | Aggregate first, then integer-immediate indexes (`InsertValue` takes the field value second). |
| `TypeCast` | Determined by source/destination tags: int resizing by width and signedness, int<->float, float resizing, int<->pointer. |
| Comparisons | Signedness follows the operand type (`I*` signed, `U*` unsigned); floats use ordered predicates; pointers support `Eq`/`Ne` only. |
| Atomics | `AtomicLoad [ptr]`, `AtomicStore [value, ptr]`, `AtomicRmw [ptr, value]` (operation from `InstructionFlags::rmw_op`), `AtomicCompareExchange [ptr, cmp, new]`, `Fence`. All sequentially consistent in MVP. |
| `Move` / `Drop` | Register alias / ownership marker; no code emitted. |
| `Select` | `[cond, true_value, false_value]`. |

Aggregate-typed immediates and `Str`/`Function`-typed lowering are not
supported; struct/array values travel through calls, allocas, and
extract/insert only.
