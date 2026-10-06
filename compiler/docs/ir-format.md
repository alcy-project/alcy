# The IR format

alcy's lowered IR has two written forms, decided in
[ADR-0056](../../docs/adr/0056-the-ir-has-a-text-and-a-binary-form.md):

- **text** (`--emit=ir`, `.ir`) is a view for people. It is write-only:
  there is no parser, and nothing reads it back.
- **binary** (`--emit=ir-bc`, `.irb`) is the canonical form. A reader
  rebuilds verified IR from it, and it is the form a build cache or an
  interpreter consumes.

This document is normative for both writers. "MUST"/"MUST NOT" describe
what a writer emits and what a reader accepts; a change to either form
is a change to this document.

## 1. The model both forms carry

Both forms carry one lowered package:

| Piece | Source |
|---|---|
| storage | the `ir::StorageState` tables (functions, blocks, block parameters, instructions, operands, registers, immutables, external functions, types and composites) |
| strings | the `str::StringInterner` the storage's `StringPoolId`s resolve in |
| instruction spans | one `diag::Span` per instruction, or none |
| files | a name (and optionally a size and content hash) per `source::FileId` |
| address names | `(register, name, is_param, is_capture)` per allocated place |
| prelude count | how many storage functions are prelude |

Indices are 32-bit and tables are dense; a range is a `(head, size)`
pair of consecutive indices. Text keeps storage indices (`v12`, `b7`),
so a verifier message and a dump name the same row.

## 2. Text form

### 2.1 Lexical rules

- One declaration or instruction per line. `//` starts a comment that
  runs to the end of the line; the first line is
  `// alcy ir, format 1, pointer width <N>`.
- `vN` names a register (N is its storage index), `bN` a block. Nothing
  else starts with `v` or `b` followed by digits, so no quoting is
  needed.
- `#N` after a name is the storage index of the row being named
  (function, struct, enum, external), written where two rows could
  share a name.
- Strings are printed as alcy string literals (double quotes, the
  escapes alcy accepts: `\n`, `\t`, `\\`, `\"`, `\0`, `\xNN`, `\u{…}`).
- Integers print as decimal, signed tags with their two's-complement
  value. Floats print as the shortest round-trippable decimal, with
  `inf`, `-inf`, and `nan` for the non-finite values. A `bool`
  immediate prints as `true` or `false`. A pointer immediate prints as
  `null` or `0x<hex>`.

### 2.2 Preamble

```text
// alcy ir, format 1, pointer width 64
// prelude: 812 functions

struct Vec<u8>(ptr, u32)          // #12
enum Option<i32> { Some(i32), None }   // #9
extern fn alcy_print(msg: ptr, len: u32)   // #2
```

- One declaration per struct and enum in the storage, in table order,
  each carrying its `#N`. Struct fields are positional: the IR keeps
  declaration order and field types, not field names. Enum variants
  keep their names. Generic arguments print between `<` and `>` after
  the name.
- One `extern fn` line per external function, in table order, with its
  parameter list and return type. `cc=c` is appended when the calling
  convention is not C's.
- The `prelude:` comment appears only when the count is not zero.

### 2.3 Functions

```text
fn add(v1: i32, v2: i32) -> i32 {        // #3 free
b0:
  v0 = v1 + v2                          // main.al:2:9+1
  ret v0
}
```

- The header is `fn <name>#<idx> (<params>) -> <return> {`, then
  `// #<idx> <kind>` on the same line when the index or kind is worth
  showing: the index always, the kind when it is not `free`. The
  parameter list comes from the entry block's block parameters, in
  order, each as `vN: T`. The return type is the function's. A `void`
  return omits `-> T`.
- Function references in instructions print the name; when two dumped
  functions share a name, references print `name#idx` instead.
- A block is `bN:` alone, or `bN (vA: T, vB: T):` when it takes
  parameters. Blocks print in function order, which is storage order.
- Instructions are indented two spaces and print in block order.
- A trailing `// <file>:<offset>+<length>` comment appears when the
  instruction has a span whose file has a name. `<offset>` and
  `<length>` are the span's bytes. When the file has no name, the
  comment is `// ?:<offset>+<length>`.

### 2.4 Types

A type prints as one of:

| Storage | Text |
|---|---|
| `Void` | `()` |
| `I1` | `bool` |
| `I8`…`U64`, `F32`, `F64` | the tag's own spelling (`i8`, `u32`, `f64`) |
| `Str` | `str` |
| `Ptr` | `ptr` (the type-erased pointer; the language has no spelling for it) |
| `Ref(T)` | `&T` |
| `MutRef(T)` | `&mut T` |
| `RawPtr(T)` | `*T` |
| `RawMutPtr(T)` | `*mut T` |
| `Slice(T)` | `[T]` |
| `Array(T, N)` | `[T; N]` |
| `Tuple(A, B)` | `(A, B)`, one element `(A,)`, none `()` |
| `Struct` | the declaration's name and generic arguments |
| `Enum` | the declaration's name and generic arguments |
| `Function` | `fn(A, B) -> R`, or `fn() -> R` |
| `Func` | `(A, B) -> R`, or `() -> R` |
| `Never` | `!` |
| `Error` | `<error>` (never produced by successful lowering) |
| `void` return | omitted from `->` |

### 2.5 Instructions

`dst` is shown as `vN =` when the instruction defines one. Operators
print with alcy's spellings; the operand types determine the signedness
and shift flavour the storage records, so the spelling is derivable.
Named values (`@function`, immutables) print inline.

| Opcode | Text |
|---|---|
| `Noop` | `nop` |
| `Alloca` | `v = alloca T` (T is the destination register's type) |
| `Load` | `v = *p` |
| `Store` | `*p = v` |
| `GetElementPtr` | `v = addr(base, i, …)` |
| `ElemOffset` | `v = &base[i]` |
| `ExtractValue` | `v = agg.0.1` |
| `InsertValue` | `v = insert(agg, x, 0, 1)` |
| `Memcopy` | `memcopy(dst, src, len)` |
| `IntAdd`/`IntSub`/`IntMul` | `a + b`, `a - b`, `a * b` |
| `IntDiv`/`UintDiv` | `a / b` |
| `IntRem`/`UintRem` | `a % b` |
| `FAdd`/`FSub`/`FMul`/`FDiv` | `a + b`, `a - b`, `a * b`, `a / b` |
| `And`/`Or`/`Xor` | `a & b`, `a \| b`, `a ^ b` |
| `ShiftLeft` | `a << b` |
| `ArithmeticShiftRight`/`LogicalShiftRight` | `a >> b` |
| `Not` | `!a` when the value is a `bool`, else `~a` |
| `BitReverse` | `bit_reverse(a)` |
| `Eq`/`Ne`/`Le`/`Lt`/`Ge`/`Gt` | `a == b`, `a != b`, `a <= b`, `a < b`, `a >= b`, `a > b` |
| `TypeCast` | `v as T` |
| `TypeSizeOf`/`TypeAlignOf` | `size_of(T)`, `align_of(T)` |
| `Select` | `v = select(c, a, b)` |
| `Br` | `br bN(v, …)` |
| `CondBr` | `condbr c, bT, bF` |
| `Switch` | `switch v, [i -> bN, …], default bN` |
| `Call` | `v = callee(args)`; `callee(args)` when the value is discarded |
| `Ret` | `ret` or `ret v` |
| `Unreachable` | `unreachable` |
| `AtomicLoad` | `v = atomic.load(p)` |
| `AtomicStore` | `atomic.store(p, v)` |
| `AtomicRmw` | `v = atomic.rmw.<op>(p, v)` with `add`, `sub`, `and`, `or`, `xor`, `xchg` |
| `AtomicCompareExchange` | `v = atomic.cas(p, cmp, new)` |
| `Fence` | `fence` |
| `Move` | `v = move x` |
| `Drop` | `drop x` |
| `Borrow` | `v = &place` or `v = &mut place`, by the destination's type |

Operands: a register is `vN`, a block `bN`, an immutable is inline, a
function is its name (or `name#idx`), an external is its name, an
integer immediate inside `ExtractValue`/`InsertValue`/`Switch`/`Alloca`
prints as a bare number.

## 3. Binary form

### 3.1 Primitives and framing

All integers are little-endian and unsigned unless noted. The file is:

```text
magic           u8[4] = "ALIR"
major           u16
minor           u16
flags           u32      bit 0 must be set (little-endian payload)
width           u32      32 or 64, the pointer width lowering ran with
version_len     u32
version         u8[version_len]   the compiler's version string
section_count   u64
sections        section_count × { kind u32, flags u32 (0), offset u64, size u64 }
payload         sections, each 8-byte aligned, non-overlapping, ascending
footer          u64      FNV-1a 64 of every byte before it
```

A reader MUST reject a missing or wrong magic, a clear flags bit 0, an
unknown major version, a section that overlaps another, and a mismatch
between the footer and the bytes.

### 3.2 Versioning

- A change to the header, to an existing section's meaning, or to a
  record's layout is a **major** bump.
- A new section kind is a **minor** bump. A reader skips any kind it
  does not know and MUST NOT guess what it contained.
- Kind numbers are never reused for a different meaning; a retired
  section's number stays reserved.
- The `width` and `version` fields are what a cache compares before
  deciding a stored package still applies.

### 3.3 Strings

`STRINGS` is `u32 count` followed by `count` entries of
`u32 length, u8[length]`. Every `StringPoolId` in the storage is
written as the index of its bytes in this table, in ascending pool
offset order for the strings the package references. A reader interns
the entries in order and remaps every reference; the pool offsets of
the writer are not preserved and MUST NOT be written.

### 3.4 Sections

`count` below is a `u32`. A range is `head u32, size u32`.

**STRINGS** as above.

**TYPES**: `count` × `{ tag u8, pad u8[3], payload u32 }`. `payload`
is the row in the tag's composite table, or 0 for a primitive tag.

**STRUCTS**: `{ name u32 (string), fields_head u32, fields_size u32,
params_head u32, params_size u32 }`.

**ARRAYS**: `{ element u32, pad u32, count u64 }`.

**SLICES**: `{ element u32 }`.

**ENUMS**: `{ name u32, variants_head u32, variants_size u32,
params_head u32, params_size u32 }`.

**ENUM_VARIANTS**: `{ name u32, fields_head u32, fields_size u32 }`.

**REFS**: `{ pointee u32 }`.

**TUPLES**: `{ elements_head u32, elements_size u32 }`.

**FUNCS**: `{ ret u32, params_head u32, params_size u32 }`.

**FUNCTIONS**: `{ return_type u32, param_head u32, param_size u32,
name u32, path u32, kind u8, pad u8[3], generics_head u32,
generics_size u32, blocks_head u32, blocks_size u32 }`.

**BLOCKS**: `{ instrs_head u32, instrs_size u32, params_head u32,
params_size u32 }`.

**BLOCK_PARAMS**: `{ type u32, reg u32 }`.

**REGISTERS**: `{ type u32, def u32 }` where `def` is `U32_MAX` when the
register has no defining instruction.

**INSTRUCTIONS**: `{ op u8, flags u8, pad u8[2], dst u32, measure u32,
operands_head u32, operands_size u32 }`. `flags` keeps
`InstructionFlags`' bit layout: bits 0–2 `rmw_op`, bit 3 the boolean.

**OPERANDS**: `{ tag u8, pad u8[3], payload u32, type u32 }`. `tag`
matches `ir::Operand::Payload`'s order; `payload` is the row index in
the table that tag names.

**IMMUTABLES**: `{ type u32, aux u32, value u64 }`. For a `Str`
immutable, `aux` is the string index and `value` is 0; for every other
tag, `aux` is 0 and `value` is the union's bytes.

**EXTERNALS**: `{ return_type u32, param_head u32, param_size u32,
name u32, path u32, kind u8, cc u8, pad u8[2], generics_head u32,
generics_size u32 }`.

**FILES**: `{ name u32, pad u32, size u64, hash u64 }`, one per file the
spans name, in `FileId` order. `hash` is 0 when the writer did not
compute one.

**SPANS**: `count` × `{ file u32, offset u32, length u32 }`, parallel to
the instruction table. `file` is `U32_MAX` for a span with no file.
The section is absent or empty when the package carries no spans.

**ADDR_NAMES**: `count` × `{ reg u32, name u32, flags u8, pad u8[3] }`,
`flags` bit 0 `is_param`, bit 1 `is_capture`.

### 3.5 Reading

A reader MUST build a `StorageState`, re-intern `STRINGS`, remap every
string reference, and return only what `StorageBuilder::build()`
verifies. Truncation, an out-of-range index, a range that leaves its
table, a wrong section size, a span count that is neither zero nor the
instruction count, and a duplicate register definition are errors, and
no partially built storage is handed out.

## 4. Determinism

The same package MUST serialize to the same bytes, and the same package
MUST dump to the same text: tables in index order, declarations in
table order, sections in ascending kind order, strings in pool offset
order, padding zeroed, and no dependence on pointer values or on any
unordered container's iteration order.
