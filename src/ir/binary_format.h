// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>

#include "fpag/base/numeric.h"

namespace ir::binary {

// What `--emit=ir-bc` writes, as src/docs/ir-format.md defines it.
// One place for both writers of the form: the reader lives in
// deserialize.h and names the same constants.
inline constexpr u8 MAGIC[4] = {'A', 'L', 'I', 'R'};
inline constexpr u16 MAJOR = 5;
// Minor 1 adds the specialization and synthetic function lists.
inline constexpr u16 MINOR = 1;
// Bit 0: the payload is little-endian. Always set; a reader rejects a
// file that clears it.
inline constexpr u32 FLAG_LITTLE_ENDIAN = 1;
// Sections start on this boundary.
inline constexpr usize SECTION_ALIGN = 8;
// One parametric-callee row (ADR-0079): a kind byte, three reserved
// bytes, the function or the spec and method, and the argument range.
// The section's record count divides by it, so the writer's row and the
// reader's stride must move together.
inline constexpr usize PARAMETRIC_CALLEE_RECORD_SIZE = 24;

// The trailing footer: the XXH3-128 digest of every byte before it.
// The writer and the reader share this, so a cache that handed back
// different bytes than were written is refused rather than read.

// Section kinds, ascending in the order a writer emits them. A kind is
// never reused for a different meaning.
enum class Section : u8 {
  Strings = 1,
  Types = 2,
  Structs = 3,
  Arrays = 4,
  Slices = 5,
  Enums = 6,
  EnumVariants = 7,
  Refs = 8,
  Tuples = 9,
  Funcs = 10,
  Functions = 11,
  Blocks = 12,
  BlockParams = 13,
  Registers = 14,
  Instructions = 15,
  Operands = 16,
  Immutables = 17,
  Externals = 18,
  Files = 19,
  Spans = 20,
  AddrNames = 21,
  // The package's prelude count, a single u32. Absent means zero.
  Prelude = 22,
  // Compile-time value arguments; one u64 per row. Absent means none.
  Consts = 23,
  // The build identity `identity_digest` computes over the write input;
  // 16 bytes. A file written before this kind existed has none.
  Identity = 24,
  // The package's public surface, so a dependent can resolve against it
  // without the source (ADR-0078). Rows: a u32 count, then per row a
  // function index (invalid for a declaration with no code), a string
  // index for the source name, the symbol kind, one flags byte, and two
  // reserved bytes. Absent means the package carries no summary.
  Summary = 25,
  // The callees a parametric body cannot name (ADR-0079, ADR-0080).
  // Rows: a u32 count, then per row a kind byte, three reserved bytes,
  // the function (Generic) or the spec (Dispatch), the method's string
  // index (Dispatch), and the argument range. Absent means none.
  ParametricCallees = 26,
  // The functions emitted once in their parametric form (ADR-0079).
  // Rows: a u32 count, then that many function indexes. Absent means
  // none.
  ParametricFunctions = 27,
  // The specializations of staged items (ADR-0079). Rows: a u32 count,
  // then that many function indexes. Absent means none.
  PreludeSpecializations = 28,
  // The bodies the specialization pass synthesized (ADR-0079). Rows: a
  // u32 count, then that many function indexes. Absent means none.
  SyntheticFunctions = 29,
};

}  // namespace ir::binary
