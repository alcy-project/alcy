// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "analyzer/types.h"
#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/seq_builder.h"
#include "ir/type.h"

namespace diag {
class DiagBag;
}  // namespace diag

namespace ir {
class StorageBuilder;
}  // namespace ir

namespace lowering {

// One local the lowering keeps: the name it binds, the alloca that
// holds it, and its type. The drop pass reads the table to ask what a
// place holds and what to call it in a diagnostic.
struct Local {
  Local(std::string_view name, ir::RegisterIdx addr, ir::TypeIdx type)
      : name(name), addr(addr), type(type) {}

  std::string_view name;
  ir::RegisterIdx addr;
  ir::TypeIdx type;
  // The locals a reference held here may point into: `p := &n` names n,
  // and a binding from an alias copies what the alias named. Empty when
  // the reference came from a parameter or from a call that kept its
  // input to itself.
  std::vector<u32> referents;
};

// One ownership event the drop pass reads (ADR-0062), in lowering
// order. `offset` is the instruction count of the block at the moment
// the event was recorded: a Bind is after the binding's store, a Move
// after the marker, a Store after the store.
struct OwnershipEvent {
  enum class Kind : u8 { Bind, Move, Store, StoreElement, DropInPlace };
  Kind kind = Kind::Bind;
  u32 local = base::INVALID_IDX;
  std::vector<u32> path;
  ir::BlockIdx block = ir::BlockIdx(0);
  u32 offset = 0;
  u32 seq = 0;
  diag::Span span;
  // DropInPlace and StoreElement: the value at this pointer is ended
  // where it stands. A store's place is the address it wrote over, which
  // no path names: the element an index reached, or a slot of it.
  ir::OperandIdx place = ir::OperandIdx::invalid();
  ir::TypeIdx place_type = ir::TypeIdx(base::INVALID_IDX);
};

// Where a place goes out of scope: a block and a position in it, and
// the locals the scope declared, innermost last. The pass ends them.
struct EndSite {
  ir::BlockIdx block = ir::BlockIdx(0);
  u32 offset = 0;
  u32 seq = 0;
  std::vector<u32> locals;
  diag::Span span;
};

// What the drop pass reads, rewrites, and asks of its caller: the
// journals, the local table, the streams of the function being
// lowered, and nothing of the lowerer's own (ADR-0062). The pass owns
// the solving; the caller owns the bookkeeping that the block numbers
// and the instruction spans need.
struct DropElaborationInputs {
  // The two analyzer tables the pass reads: whether a type ends with
  // code, and the glue method it ends through.
  std::span<const u8> needs_drop;
  // Types that are never moved (ADR-0085): their end only ends.
  std::span<const u8> not_movable;
  std::span<const analyzer::CheckedModule::DropGlue> drop_glue;
  ir::StorageBuilder& builder;
  diag::DiagBag& bag;
  std::span<const Local> locals;
  // The receiver of a destructor. Its end belongs to the destructor's
  // own body, so the walk leaves it alone: there is no way to spell a
  // value that has been partly ended, and the body is where a value
  // that has been is written (ADR-0137). No local when the function is
  // not a destructor.
  u32 end_exempt = base::INVALID_IDX;
  std::span<const OwnershipEvent> ownership;
  std::span<const EndSite> end_sites;
  // One span per instruction of the function being lowered; the pass
  // appends one for each instruction it emits.
  std::vector<diag::Span>& instr_spans;
  // The streams of the function's blocks, in block order, and the
  // blocks they belong to. The pass rewrites the streams in place.
  std::vector<ir::InstrSeq>& streams;
  std::vector<ir::BlockIdx>& blocks;
  // The number `blocks.front()` carries: a block maps to its stream by
  // the offset of its number from the base.
  u32 base = 0;
  // Where each type copy came from (the lowering's origin links).
  const std::unordered_map<u32, u32>& type_origins;
  // Values the caller already interned: its `1` and `0` operands and
  // the pointer width the target chose.
  ir::OperandIdx size_one;
  ir::OperandIdx zero_i32;
  ir::PointerWidth width;
  // The function the lowering interns for a destructor method; the
  // pass names the module and method the glue table recorded.
  std::function<ir::FunctionIdx(u32 module, u32 method)> drop_method;
  // Reserves a fresh block in the function being lowered; the caller
  // keeps the block bookkeeping in step.
  std::function<ir::BlockIdx()> reserve_block;
};

// The tables the pass builds for one function: its places, blocks,
// points, states, and actions. Every one of them is refilled from empty,
// so the set outlives a single function and is cleared instead of
// rebuilt, which carries each table's capacity across the functions that
// come after it.
class DropWorkingSet {
 public:
  // The rows themselves. Callers hold the set and never read it.
  struct Tables;

  DropWorkingSet();
  ~DropWorkingSet();
  DropWorkingSet(const DropWorkingSet&) = delete;
  DropWorkingSet& operator=(const DropWorkingSet&) = delete;
  DropWorkingSet(DropWorkingSet&&) = delete;
  DropWorkingSet& operator=(DropWorkingSet&&) = delete;

  [[nodiscard]] Tables& tables() { return *tables_; }

 private:
  std::unique_ptr<Tables> tables_;
};

// Solves ownership per place over the function's CFG and places each
// end where the paths still own the value (ADR-0062). It rewrites the
// streams the caller handed over, before their blocks are published,
// and reports what it cannot place through the caller's diagnostics.
void elaborate_drops(const DropElaborationInputs& inputs,
                     DropWorkingSet& working);
void elaborate_drops(const DropElaborationInputs& inputs);

// The type a place path names, following the field and element slots
// the path walks. A path that leaves the shape answers the error type.
[[nodiscard]] ir::TypeIdx type_at_path(
    ir::StorageBuilder& builder,
    const std::unordered_map<u32, u32>& type_origins,
    ir::TypeIdx type,
    std::span<const u32> path);

// Whether a value of the type ends with code: the analyzer's table
// answers for the types it sized, and a type the lowering interned
// answers from its structure, following a storage copy back to the
// declaration it came from.
[[nodiscard]] bool runs_destructor(
    std::span<const u8> needs_drop,
    const ir::StorageBuilder& builder,
    const std::unordered_map<u32, u32>& type_origins,
    ir::TypeIdx type);

}  // namespace lowering
