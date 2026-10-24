// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Drop elaboration: solve ownership per place over the CFG and place
// each end where the paths still own the value (ADR-0062). Lowering
// journals the end sites and the ownership events; this pass reads
// them, computes the state of every place at every point that decides
// an end, and rewrites the function's blocks to carry the ends.

#include "lowering/drop_elaboration.h"

#include <algorithm>
#include <functional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/types.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "lowering/diag_code.h"
#include "lowering/lowerer.h"

namespace lowering {

namespace {

// A place is a local and a field path from its root.
struct Place {
  u32 local = 0;
  std::vector<u32> path;
};

// Ownership of one place at one point: owned, moved, or a state that
// depends on the path taken (a join of the two, or a loop that carries
// either to the point).
enum class State : u8 { Owned, Moved, Maybe };

State join_state(State a, State b) {
  return a == b ? a : State::Maybe;
}

bool is_prefix(std::span<const u32> prefix, std::span<const u32> of) {
  if (prefix.size() > of.size()) {
    return false;
  }
  for (usize i = 0; i < prefix.size(); ++i) {
    if (prefix[i] != of[i]) {
      return false;
    }
  }
  return true;
}

// One block the pass is building. Blocks are appended as they are
// spawned; `current` names the one being filled.
struct OutBlock {
  ir::BlockIdx block;
  ir::InstrSeq seq;
};

struct Output {
  std::vector<OutBlock> blocks;
  u32 current = 0;
};

}  // namespace

void Lowerer::record_end(u32 mark, diag::Span span) {
  if (failed || mark >= locals.size()) {
    return;
  }
  EndSite site;
  site.block = cur_;
  site.offset = streams_[at(cur_)].size();
  site.seq = record_seq_++;
  for (u32 i = mark; i < static_cast<u32>(locals.size()); ++i) {
    if (i == end_exempt_) {
      continue;
    }
    site.locals.push_back(i);
  }
  if (site.locals.empty()) {
    return;
  }
  site.span = span;
  end_sites_.push_back(std::move(site));
}

void Lowerer::record_event(OwnershipEvent::Kind kind,
                           u32 local,
                           std::vector<u32> path,
                           diag::Span span) {
  if (failed || local >= locals.size()) {
    return;
  }
  OwnershipEvent event;
  event.kind = kind;
  event.local = local;
  event.path = std::move(path);
  event.block = cur_;
  event.offset = streams_[at(cur_)].size();
  event.seq = record_seq_++;
  event.span = span;
  ownership_.push_back(std::move(event));
}

void Lowerer::emit_drop_glue_call(ir::OperandIdx place,
                                  ir::TypeIdx type,
                                  diag::Span span) {
  // A storage copy calls the glue of the type it came from: the
  // lowering interns copies the analyzer's table never measured, and
  // a destructor belongs to the declaration, not to the copy.
  const ir::TypeIdx glue_type = type_origin(type);
  if (!runs_destructor(type) || glue_type.idx >= pkg.drop_glue.size()) {
    return;
  }
  const analyzer::CheckedModule::DropGlue& glue = pkg.drop_glue[glue_type.idx];
  if (glue.index == base::INVALID_IDX) {
    return;
  }
  const analyzer::CheckedModule& def = pkg.modules[glue.module];
  const analyzer::CheckedModule::MethodInfo& info = def.methods[glue.index];
  const ir::FunctionIdx fn =
      fn_index(glue.module, info.item, info.name, info.params, info.ret,
               generic_inst_index(info.self_type), {}, ir::SymbolKind::Method,
               /*extra_generics=*/{}, info.self_type);
  if (!fn.is_valid() || failed) {
    return;
  }
  const ir::RegisterIdx value = emit(ir::Opcode::Load, type, {place});
  emit_void(ir::Opcode::Call,
            {builder.operand(ir::Operand::from_function(
                 fn, builder.primitive(ir::TypeTag::Function))),
             to_operand(value, type)});
  (void)span;
}

void Lowerer::emit_collected_drop(ir::OperandIdx tuple_place,
                                  std::span<const ir::TypeIdx> arg_types,
                                  diag::Span span) {
  for (usize i = 0; i < arg_types.size() && !failed; ++i) {
    const ir::TypeIdx type = arg_types[i];
    const ir::TypeTag tag = tag_of(type);
    if (tag == ir::TypeTag::Void || tag == ir::TypeTag::Error ||
        !runs_destructor(type)) {
      continue;
    }
    const ir::RegisterIdx field =
        emit(ir::Opcode::GetElementPtr, type,
             {tuple_place, zero_i32, index_operand(static_cast<u32>(i))});
    emit_drop_glue_call(to_operand(field, type), type, span);
  }
}

void Lowerer::record_drop_in_place(ir::OperandIdx place,
                                   ir::TypeIdx type,
                                   diag::Span span) {
  if (failed) {
    return;
  }
  OwnershipEvent event;
  event.kind = OwnershipEvent::Kind::DropInPlace;
  event.place = place;
  event.place_type = type;
  // A discarded binding is spent; a value behind a reference or a
  // container's element is not this function's place to retire.
  const ir::Operand& operand = builder.state().operands[place.idx];
  if (operand.is<ir::RegisterIdx>()) {
    u32 local = 0;
    std::vector<u32> path;
    if (place_of_register(operand.as_register().idx, local, path)) {
      event.local = local;
      event.path = std::move(path);
    }
  }
  event.block = cur_;
  event.offset = streams_[at(cur_)].size();
  event.seq = record_seq_++;
  event.span = span;
  ownership_.push_back(std::move(event));
}

namespace {

// The drop pass as one unit: the journals lowering recorded, the
// streams it rewrites, and the caller's facilities, none of them
// reached through the lowerer (ADR-0062).
class DropElaborator {
 public:
  explicit DropElaborator(const DropElaborationInputs& inputs)
      : needs_drop(inputs.needs_drop),
        drop_glue(inputs.drop_glue),
        builder(inputs.builder),
        bag(inputs.bag),
        locals(inputs.locals),
        ownership_(inputs.ownership),
        end_sites_(inputs.end_sites),
        instr_spans_(inputs.instr_spans),
        streams_(inputs.streams),
        fn_blocks_(inputs.blocks),
        base_(inputs.base),
        type_origins_(inputs.type_origins),
        size_one(inputs.size_one),
        zero_i32(inputs.zero_i32),
        width(inputs.width),
        drop_method_(inputs.drop_method),
        reserve_block_(inputs.reserve_block) {}

  void run();
  ir::RegisterIdx pass_emit(ir::InstrSeq& seq,
                            ir::Opcode op,
                            ir::TypeIdx type,
                            const std::vector<ir::OperandIdx>& ops,
                            diag::Span span);
  void pass_emit_void(ir::InstrSeq& seq,
                      ir::Opcode op,
                      const std::vector<ir::OperandIdx>& ops,
                      diag::Span span);
  void pass_copy(ir::InstrSeq& seq, ir::InstructionIdx idx);

 private:
  [[nodiscard]] ir::RegisterIdx claim_reg() const {
    return ir::RegisterIdx(static_cast<u32>(builder.state().registers.size()));
  }
  [[nodiscard]] ir::OperandIdx to_operand(ir::RegisterIdx reg,
                                          ir::TypeIdx type) const {
    return builder.operand(ir::Operand::from_register(reg, type));
  }
  [[nodiscard]] ir::OperandIdx to_operand(ir::ImmutableIdx imm,
                                          ir::TypeIdx type) const {
    return builder.operand(ir::Operand::from_immutable(imm, type));
  }
  [[nodiscard]] bool runs_destructor(ir::TypeIdx type) const {
    return lowering::runs_destructor(needs_drop, builder, type_origins_, type);
  }
  [[nodiscard]] ir::BlockIdx reserve_block() { return reserve_block_(); }
  [[nodiscard]] usize at(ir::BlockIdx block) const { return block.idx - base_; }
  [[nodiscard]] ir::TypeTag tag_of(ir::TypeIdx idx) const {
    return builder.state().types[idx].tag;
  }
  [[nodiscard]] ir::TypeIdx type_origin(ir::TypeIdx type) const {
    const auto found = type_origins_.find(type.idx);
    return found == type_origins_.end() ? type : ir::TypeIdx(found->second);
  }
  [[nodiscard]] ir::TypeIdx usize_type() const {
    return builder.primitive(width == ir::PointerWidth::W64 ? ir::TypeTag::U64
                                                            : ir::TypeTag::U32);
  }
  [[nodiscard]] ir::TypeIdxRange variant_fields(ir::TypeIdx enum_type,
                                                u32 variant) const {
    const ir::EnumType& enum_ty =
        builder.state().enum_types[builder.state().types[enum_type].as_enum()];
    const u32 at = enum_ty.variants.head().idx + variant;
    return builder.state()
        .enum_variant_types[ir::EnumVariantTypeIdx(at)]
        .fields;
  }
  [[nodiscard]] ir::OperandIdx imm_from_u64(ir::TypeTag tag,
                                            ir::TypeIdx type,
                                            u64 value) const {
    return lowering::imm_from_u64(builder, tag, type, value);
  }
  [[nodiscard]] ir::OperandIdx index_operand(u32 index) const {
    return imm_from_u64(ir::TypeTag::I32, builder.primitive(ir::TypeTag::I32),
                        index);
  }
  [[nodiscard]] ir::OperandIdx bool_operand(bool value) const {
    return imm_from_u64(ir::TypeTag::I1, builder.primitive(ir::TypeTag::I1),
                        value ? 1 : 0);
  }
  [[nodiscard]] ir::TypeIdx type_at_path(ir::TypeIdx type,
                                         std::span<const u32> path) const {
    return lowering::type_at_path(builder, type_origins_, type, path);
  }
  [[nodiscard]] ir::FunctionIdx drop_method(u32 module, u32 method) const {
    return drop_method_(module, method);
  }

  std::span<const u8> needs_drop;
  std::span<const analyzer::CheckedModule::DropGlue> drop_glue;
  ir::StorageBuilder& builder;
  diag::DiagBag& bag;
  std::span<const Local> locals;
  std::span<const OwnershipEvent> ownership_;
  std::span<const EndSite> end_sites_;
  std::vector<diag::Span>& instr_spans_;
  std::vector<ir::InstrSeq>& streams_;
  std::vector<ir::BlockIdx>& fn_blocks_;
  u32 base_ = 0;
  const std::unordered_map<u32, u32>& type_origins_;
  ir::OperandIdx size_one = ir::OperandIdx(0);
  ir::OperandIdx zero_i32 = ir::OperandIdx(0);
  ir::PointerWidth width = ir::PointerWidth::W64;
  std::function<ir::FunctionIdx(u32, u32)> drop_method_;
  std::function<ir::BlockIdx()> reserve_block_;
};

}  // namespace

ir::RegisterIdx DropElaborator::pass_emit(
    ir::InstrSeq& seq,
    ir::Opcode op,
    ir::TypeIdx type,
    const std::vector<ir::OperandIdx>& ops,
    diag::Span span) {
  const ir::RegisterIdx dst = claim_reg();
  const ir::OperandIdx head =
      ir::OperandIdx(static_cast<u32>(builder.state().operands.size()));
  for (ir::OperandIdx op_idx : ops) {
    builder.operand(ir::Operand(builder.state().operands[op_idx.idx]));
  }
  const ir::InstructionIdx instr =
      builder.instr({.op = op,
                     .flags = {},
                     .dst = dst,
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {head, static_cast<u32>(ops.size())}});
  builder.reg({.type = type, .def_idx = instr});
  seq.push(instr);
  instr_spans_.push_back(span);
  return dst;
}

void DropElaborator::pass_emit_void(ir::InstrSeq& seq,
                                    ir::Opcode op,
                                    const std::vector<ir::OperandIdx>& ops,
                                    diag::Span span) {
  const ir::OperandIdx head =
      ir::OperandIdx(static_cast<u32>(builder.state().operands.size()));
  for (ir::OperandIdx op_idx : ops) {
    builder.operand(ir::Operand(builder.state().operands[op_idx.idx]));
  }
  const ir::InstructionIdx instr =
      builder.instr({.op = op,
                     .flags = {},
                     .dst = ir::RegisterIdx(base::INVALID_IDX),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {head, static_cast<u32>(ops.size())}});
  seq.push(instr);
  instr_spans_.push_back(span);
}

void DropElaborator::pass_copy(ir::InstrSeq& seq, ir::InstructionIdx idx) {
  const ir::Instruction& old = builder.state().instrs[idx.idx];
  const ir::InstructionIdx copy = builder.instr({.op = old.op,
                                                 .flags = old.flags,
                                                 .dst = old.dst,
                                                 .measure = old.measure,
                                                 .operands = old.operands});
  seq.push(copy);
  instr_spans_.push_back(instr_spans_[idx.idx]);
}

void DropElaborator::run() {
  if (ownership_.empty() && end_sites_.empty()) {
    return;
  }

  // Places: every (local, path) an event names, and the root of every
  // local an end site names.
  std::vector<Place> places;
  auto place_of = [&](u32 local, const std::vector<u32>& path) -> u32 {
    for (u32 i = 0; i < places.size(); ++i) {
      if (places[i].local == local && places[i].path == path) {
        return i;
      }
    }
    places.push_back(Place{local, path});
    return static_cast<u32>(places.size() - 1);
  };
  for (const OwnershipEvent& event : ownership_) {
    // A place a drop ends is tracked like any other: the owner's own
    // drop still has to skip it. Only an untracked place (a temporary,
    // an element) names no local to hold the state.
    if (event.kind == OwnershipEvent::Kind::DropInPlace &&
        event.local == base::INVALID_IDX) {
      continue;
    }
    place_of(event.local, event.path);
  }
  for (const EndSite& site : end_sites_) {
    for (u32 local : site.locals) {
      place_of(local, {});
    }
  }
  const u32 place_count = static_cast<u32>(places.size());

  // Blocks in lowering order, with their events and end sites in
  // (offset, seq) order. A site is a move: the end consumes the value,
  // so a later site sees it moved.
  struct Point {
    enum class Kind : u8 { Event, Site };
    u32 offset = 0;
    u32 seq = 0;
    Kind kind = Kind::Event;
    u32 index = 0;
  };
  const u32 block_count = static_cast<u32>(fn_blocks_.size());
  std::vector<std::vector<Point>> block_points(block_count);
  for (u32 i = 0; i < ownership_.size(); ++i) {
    const OwnershipEvent& event = ownership_[i];
    block_points[at(event.block)].push_back(
        Point{event.offset, event.seq, Point::Kind::Event, i});
  }
  for (u32 i = 0; i < end_sites_.size(); ++i) {
    const EndSite& site = end_sites_[i];
    block_points[at(site.block)].push_back(
        Point{site.offset, site.seq, Point::Kind::Site, i});
  }
  for (std::vector<Point>& list : block_points) {
    std::sort(list.begin(), list.end(), [](const Point& a, const Point& b) {
      if (a.offset != b.offset) {
        return a.offset < b.offset;
      }
      return a.seq < b.seq;
    });
  }

  // Predecessors, from each block's terminator.
  std::vector<std::vector<u32>> preds(block_count);
  for (u32 b = 0; b < block_count; ++b) {
    const base::IdxRange<ir::InstructionIdx> range = streams_[b].finish();
    if (range.size() == 0) {
      continue;
    }
    const ir::InstructionIdx last = range[range.size() - 1];
    for (ir::OperandIdx o : builder.state().instrs[last.idx].operands) {
      const ir::Operand& operand = builder.state().operands[o.idx];
      if (operand.is<ir::BlockIdx>()) {
        const u32 target = at(operand.as_block());
        if (target < block_count) {
          preds[target].push_back(b);
        }
      }
    }
  }

  auto applies = [&](const Place& place, const OwnershipEvent& event) {
    return event.local == place.local && is_prefix(event.path, place.path);
  };
  auto step = [&](State state, const OwnershipEvent& event) {
    switch (event.kind) {
      case OwnershipEvent::Kind::Bind:
      case OwnershipEvent::Kind::Store: return State::Owned;
      case OwnershipEvent::Kind::Move: return State::Moved;
      case OwnershipEvent::Kind::DropInPlace: return state;
    }
    return state;
  };
  auto step_point = [&](State state, const Point& point, const Place& place) {
    if (point.kind == Point::Kind::Event) {
      const OwnershipEvent& event = ownership_[point.index];
      if (event.kind == OwnershipEvent::Kind::DropInPlace) {
        // A discarded binding is spent; a value behind a reference or a
        // container's element names no tracked place.
        if (event.local == base::INVALID_IDX) {
          return state;
        }
        return applies(place, event) ? State::Moved : state;
      }
      return applies(place, event) ? step(state, event) : state;
    }
    // Ending a value consumes it, so a site is a move for every local
    // it names.
    const EndSite& site = end_sites_[point.index];
    for (u32 local : site.locals) {
      if (local == place.local) {
        return State::Moved;
      }
    }
    return state;
  };

  // Forward dataflow per place: in-states join the predecessors'
  // out-states, and the block's events carry the state forward.
  std::vector<std::vector<State>> in(
      place_count, std::vector<State>(block_count, State::Moved));
  std::vector<std::vector<State>> out(
      place_count, std::vector<State>(block_count, State::Moved));
  for (u32 pi = 0; pi < place_count; ++pi) {
    const Place& place = places[pi];
    bool changed = true;
    u32 rounds = 0;
    while (changed && rounds <= block_count + 1) {
      changed = false;
      ++rounds;
      for (u32 b = 0; b < block_count; ++b) {
        State state = State::Moved;
        if (!preds[b].empty()) {
          state = out[pi][preds[b][0]];
          for (usize k = 1; k < preds[b].size(); ++k) {
            state = join_state(state, out[pi][preds[b][k]]);
          }
        }
        if (state != in[pi][b]) {
          in[pi][b] = state;
          changed = true;
        }
        State after = state;
        for (const Point& point : block_points[b]) {
          after = step_point(after, point, place);
        }
        if (after != out[pi][b]) {
          out[pi][b] = after;
          changed = true;
        }
      }
    }
  }

  auto place_index = [&](u32 local, const std::vector<u32>& path) -> u32 {
    for (u32 i = 0; i < place_count; ++i) {
      if (places[i].local == local && places[i].path == path) {
        return i;
      }
    }
    return base::INVALID_IDX;
  };
  auto state_at = [&](u32 local, const std::vector<u32>& path, u32 block,
                      u32 offset, u32 seq) -> State {
    const u32 pi = place_index(local, path);
    if (pi == base::INVALID_IDX) {
      return State::Owned;
    }
    State state = in[pi][block];
    for (const Point& point : block_points[block]) {
      if (point.offset > offset ||
          (point.offset == offset && point.seq >= seq)) {
        break;
      }
      state = step_point(state, point, places[pi]);
    }
    return state;
  };

  // A place needs a run-time flag when a decision point sees it as
  // maybe. Decisions are the end sites and the ends a store places over
  // the value it replaces. Flags are created where the place binds,
  // which always precedes every use of it in lowering order.
  const ir::TypeIdx i1 = builder.primitive(ir::TypeTag::I1);
  std::vector<bool> flag_needed(place_count, false);
  std::vector<ir::RegisterIdx> flag(place_count,
                                    ir::RegisterIdx(base::INVALID_IDX));
  std::function<void(u32, const std::vector<u32>&, ir::TypeIdx, u32, u32, u32)>
      mark_maybe;
  mark_maybe = [&](u32 local, const std::vector<u32>& path, ir::TypeIdx type,
                   u32 block, u32 offset, u32 seq) {
    const State state = state_at(local, path, block, offset, seq);
    if (state == State::Moved) {
      return;
    }
    if (state == State::Maybe) {
      const u32 pi = place_index(local, path);
      if (pi != base::INVALID_IDX) {
        flag_needed[pi] = true;
      }
    }
    const ir::TypeNode& node = builder.types()[type];
    if (node.tag != ir::TypeTag::Struct) {
      return;
    }
    const ir::StructType& fields = builder.struct_types()[node.as_struct()];
    for (u32 i = 0; i < fields.fields.size(); ++i) {
      std::vector<u32> child = path;
      child.push_back(i);
      mark_maybe(local, child, fields.fields[i], block, offset, seq);
    }
  };
  for (const EndSite& site : end_sites_) {
    for (u32 local : site.locals) {
      if (!runs_destructor(locals[local].type)) {
        continue;
      }
      mark_maybe(local, {}, locals[local].type, at(site.block), site.offset,
                 site.seq);
    }
  }
  for (const OwnershipEvent& event : ownership_) {
    if (event.kind != OwnershipEvent::Kind::Store) {
      continue;
    }
    const Local& local = locals[event.local];
    const ir::TypeIdx type = type_at_path(local.type, event.path);
    if (!runs_destructor(type)) {
      continue;
    }
    mark_maybe(event.local, event.path, type, at(event.block), event.offset - 1,
               event.seq);
  }

  // Emit the ends. Each block rebuilds its instruction list with the
  // actions that land on it; a guarded end splits the block into the
  // test, the drop, and the continuation.
  auto seq_of = [&](Output& out) -> ir::InstrSeq& {
    return out.blocks[out.current].seq;
  };
  auto spawn = [&](Output& out) -> u32 {
    const ir::BlockIdx block = reserve_block();
    out.blocks.push_back(OutBlock{block, ir::InstrSeq{}});
    return static_cast<u32>(out.blocks.size() - 1);
  };
  const ir::TypeIdx void_ty = builder.primitive(ir::TypeTag::Void);
  auto emit_guard = [&](Output& out, ir::RegisterIdx reg, diag::Span span,
                        const std::function<void(Output&)>& body) {
    const ir::RegisterIdx loaded = pass_emit(seq_of(out), ir::Opcode::Load, i1,
                                             {to_operand(reg, i1)}, span);
    const u32 drop_index = spawn(out);
    const u32 after_index = spawn(out);
    pass_emit_void(seq_of(out), ir::Opcode::CondBr,
                   {to_operand(loaded, i1),
                    builder.operand(ir::Operand::from_block(
                        out.blocks[after_index].block, void_ty)),
                    builder.operand(ir::Operand::from_block(
                        out.blocks[drop_index].block, void_ty))},
                   span);
    out.current = drop_index;
    body(out);
    pass_emit_void(seq_of(out), ir::Opcode::Br,
                   {builder.operand(ir::Operand::from_block(
                       out.blocks[after_index].block, void_ty))},
                   span);
    out.current = after_index;
  };

  // The end of `place`, which the walk reached knowing it is owned on
  // the path that arrives here. Children a path moved are skipped; a
  // child a path may have moved is ended behind its own flag.
  std::function<void(
      Output&, u32, const std::vector<u32>&, ir::TypeIdx, ir::OperandIdx,
      const std::function<State(const std::vector<u32>&)>&, diag::Span)>
      emit_end;
  emit_end = [&](Output& out, u32 local, const std::vector<u32>& path,
                 ir::TypeIdx type, ir::OperandIdx place,
                 const std::function<State(const std::vector<u32>&)>& state_of,
                 diag::Span span) {
    if (!runs_destructor(type)) {
      return;
    }
    // The same origin rule the collected call follows: a copy's
    // glue is the one the type it came from declared.
    const ir::TypeIdx glue_type = type_origin(type);
    if (glue_type.idx < drop_glue.size()) {
      const analyzer::CheckedModule::DropGlue& glue = drop_glue[glue_type.idx];
      if (glue.index != base::INVALID_IDX) {
        const ir::FunctionIdx fn = drop_method(glue.module, glue.index);
        if (!fn.is_valid()) {
          return;
        }
        pass_emit_void(seq_of(out), ir::Opcode::Drop, {place}, span);
        const ir::RegisterIdx value =
            pass_emit(seq_of(out), ir::Opcode::Load, type, {place}, span);
        // A binding's end marks the place moved; an explicit end does
        // not, because the reference names a value borrow should not
        // retire for the rest of the buffer.
        if (local < locals.size()) {
          pass_emit_void(seq_of(out), ir::Opcode::Move, {place}, span);
        }
        pass_emit_void(seq_of(out), ir::Opcode::Call,
                       {builder.operand(ir::Operand::from_function(
                            fn, builder.primitive(ir::TypeTag::Function))),
                        to_operand(value, type)},
                       span);
        return;
      }
    }
    const ir::TypeNode& node = builder.types()[type];
    if (node.tag == ir::TypeTag::Enum) {
      // The discriminant selects the live variant; only that variant's
      // destructible fields are there to end.
      const ir::EnumType& shape = builder.enum_types()[node.as_enum()];
      const ir::TypeIdx i32_ty = builder.primitive(ir::TypeTag::I32);
      const ir::RegisterIdx disc_addr =
          pass_emit(seq_of(out), ir::Opcode::GetElementPtr, i32_ty,
                    {place, zero_i32, index_operand(0)}, span);
      const ir::RegisterIdx disc =
          pass_emit(seq_of(out), ir::Opcode::Load, i32_ty,
                    {to_operand(disc_addr, i32_ty)}, span);
      const u32 exit_index = spawn(out);
      std::vector<u32> variant_blocks(shape.variants.size(), exit_index);
      for (u32 v = 0; v < shape.variants.size(); ++v) {
        const ir::TypeIdxRange fields = variant_fields(type, v);
        bool any = false;
        for (ir::TypeIdx field : fields) {
          if (runs_destructor(field)) {
            any = true;
            break;
          }
        }
        if (any) {
          variant_blocks[v] = spawn(out);
        }
      }
      std::vector<ir::OperandIdx> ops;
      ops.push_back(to_operand(disc, i32_ty));
      ops.push_back(builder.operand(
          ir::Operand::from_block(out.blocks[exit_index].block, void_ty)));
      for (u32 v = 0; v < shape.variants.size(); ++v) {
        ops.push_back(imm_from_u64(ir::TypeTag::I32, i32_ty, v));
        ops.push_back(builder.operand(ir::Operand::from_block(
            out.blocks[variant_blocks[v]].block, void_ty)));
      }
      pass_emit_void(seq_of(out), ir::Opcode::Switch, ops, span);
      const ir::TypeIdx byte = builder.primitive(ir::TypeTag::U8);
      for (u32 v = 0; v < shape.variants.size(); ++v) {
        if (variant_blocks[v] == exit_index) {
          continue;
        }
        out.current = variant_blocks[v];
        const ir::TypeIdxRange fields = variant_fields(type, v);
        const ir::RegisterIdx area =
            pass_emit(seq_of(out), ir::Opcode::GetElementPtr, byte,
                      {place, zero_i32, index_operand(1)}, span);
        for (u32 fi = 0; fi < fields.size(); ++fi) {
          const ir::TypeIdx field = fields[fi];
          if (!runs_destructor(field)) {
            continue;
          }
          // The address carries the field's own type, not the byte
          // it was reached through: a projection from it recovers the
          // element type from the register, and a byte register would
          // index the element type itself.
          const ir::RegisterIdx field_addr =
              pass_emit(seq_of(out), ir::Opcode::GetElementPtr, field,
                        {to_operand(area, byte),
                         index_operand(static_cast<u32>(ir::field_offset(
                             builder.state(), fields, fi, width)))},
                        span);
          emit_end(
              out, local, path, field, to_operand(field_addr, field),
              [](const std::vector<u32>&) { return State::Owned; }, span);
        }
        pass_emit_void(seq_of(out), ir::Opcode::Br,
                       {builder.operand(ir::Operand::from_block(
                           out.blocks[exit_index].block, void_ty))},
                       span);
      }
      out.current = exit_index;
      return;
    }
    if (node.tag == ir::TypeTag::Array) {
      // Every element is there to end, walked with a counter.
      const ir::ArrayType& shape = builder.array_types()[node.as_array()];
      if (shape.count == 0) {
        return;
      }
      const ir::TypeIdx usize_ty = usize_type();
      const ir::TypeTag usize_tag = tag_of(usize_ty);
      const ir::RegisterIdx counter = pass_emit(seq_of(out), ir::Opcode::Alloca,
                                                usize_ty, {size_one}, span);
      pass_emit_void(
          seq_of(out), ir::Opcode::Store,
          {imm_from_u64(usize_tag, usize_ty, 0), to_operand(counter, usize_ty)},
          span);
      const u32 header_index = spawn(out);
      const u32 body_index = spawn(out);
      const u32 exit_index = spawn(out);
      pass_emit_void(seq_of(out), ir::Opcode::Br,
                     {builder.operand(ir::Operand::from_block(
                         out.blocks[header_index].block, void_ty))},
                     span);
      out.current = header_index;
      const ir::RegisterIdx at =
          pass_emit(seq_of(out), ir::Opcode::Load, usize_ty,
                    {to_operand(counter, usize_ty)}, span);
      const ir::TypeIdx i1 = builder.primitive(ir::TypeTag::I1);
      const ir::RegisterIdx more =
          pass_emit(seq_of(out), ir::Opcode::Lt, i1,
                    {to_operand(at, usize_ty),
                     imm_from_u64(usize_tag, usize_ty, shape.count)},
                    span);
      pass_emit_void(seq_of(out), ir::Opcode::CondBr,
                     {to_operand(more, i1),
                      builder.operand(ir::Operand::from_block(
                          out.blocks[body_index].block, void_ty)),
                      builder.operand(ir::Operand::from_block(
                          out.blocks[exit_index].block, void_ty))},
                     span);
      out.current = body_index;
      const ir::RegisterIdx index =
          pass_emit(seq_of(out), ir::Opcode::Load, usize_ty,
                    {to_operand(counter, usize_ty)}, span);
      const ir::RegisterIdx element =
          pass_emit(seq_of(out), ir::Opcode::GetElementPtr, shape.element,
                    {place, zero_i32, to_operand(index, usize_ty)}, span);
      emit_end(
          out, local, path, shape.element, to_operand(element, shape.element),
          [](const std::vector<u32>&) { return State::Owned; }, span);
      const ir::RegisterIdx next = pass_emit(
          seq_of(out), ir::Opcode::IntAdd, usize_ty,
          {to_operand(index, usize_ty), imm_from_u64(usize_tag, usize_ty, 1)},
          span);
      pass_emit_void(
          seq_of(out), ir::Opcode::Store,
          {to_operand(next, usize_ty), to_operand(counter, usize_ty)}, span);
      pass_emit_void(seq_of(out), ir::Opcode::Br,
                     {builder.operand(ir::Operand::from_block(
                         out.blocks[header_index].block, void_ty))},
                     span);
      out.current = exit_index;
      return;
    }
    if (node.tag != ir::TypeTag::Struct && node.tag != ir::TypeTag::Tuple) {
      // A place no walk reaches; the caller's value is left alone. The
      // sites name a binding, the explicit end does not.
      if (local < locals.size()) {
        const u32 index = bag.emit<i18n::Key::LowerDestructorNotRun>(
            diag::Severity::Warning, diag::Stage::Lowering,
            DiagCode::DropUnplaced, span, locals[local].name);
        (void)index;
      }
      return;
    }
    const ir::TypeIdxRange fields =
        node.tag == ir::TypeTag::Struct
            ? builder.struct_types()[node.as_struct()].fields
            : builder.tuple_types()[node.as_tuple()].elements;
    for (u32 i = 0; i < fields.size(); ++i) {
      const ir::TypeIdx field = fields[i];
      if (!runs_destructor(field)) {
        continue;
      }
      std::vector<u32> child = path;
      child.push_back(i);
      const State state = state_of(child);
      if (state == State::Moved) {
        continue;
      }
      const ir::RegisterIdx gep =
          pass_emit(seq_of(out), ir::Opcode::GetElementPtr, field,
                    {place, zero_i32, index_operand(i)}, span);
      const ir::OperandIdx child_place = to_operand(gep, field);
      if (state == State::Maybe) {
        const u32 pi = place_index(local, child);
        if (pi != base::INVALID_IDX && flag[pi].is_valid()) {
          emit_guard(out, flag[pi], span, [&](Output& drop_out) {
            emit_end(drop_out, local, child, field, child_place, state_of,
                     span);
          });
          continue;
        }
      }
      emit_end(out, local, child, field, child_place, state_of, span);
    }
  };

  // One action at one boundary of a block.
  struct Action {
    enum class Kind : u8 { Site, Bind, Move, StoreEnd, StoreClear, DropHere };
    u32 offset = 0;
    u32 seq = 0;
    Kind kind = Kind::Site;
    u32 index = 0;
  };
  std::vector<std::vector<Action>> actions(block_count);
  for (u32 si = 0; si < end_sites_.size(); ++si) {
    const EndSite& site = end_sites_[si];
    actions[at(site.block)].push_back(
        Action{site.offset, site.seq, Action::Kind::Site, si});
  }
  for (u32 ei = 0; ei < ownership_.size(); ++ei) {
    const OwnershipEvent& event = ownership_[ei];
    const u32 b = at(event.block);
    switch (event.kind) {
      case OwnershipEvent::Kind::Bind:
        actions[b].push_back(
            Action{event.offset, event.seq, Action::Kind::Bind, ei});
        break;
      case OwnershipEvent::Kind::Move:
        actions[b].push_back(
            Action{event.offset, event.seq, Action::Kind::Move, ei});
        break;
      case OwnershipEvent::Kind::Store:
        actions[b].push_back(
            Action{event.offset - 1, event.seq, Action::Kind::StoreEnd, ei});
        actions[b].push_back(
            Action{event.offset, event.seq, Action::Kind::StoreClear, ei});
        break;
      case OwnershipEvent::Kind::DropInPlace:
        actions[b].push_back(
            Action{event.offset, event.seq, Action::Kind::DropHere, ei});
        break;
    }
  }
  for (std::vector<Action>& list : actions) {
    std::sort(list.begin(), list.end(), [](const Action& a, const Action& b) {
      if (a.offset != b.offset) {
        return a.offset < b.offset;
      }
      if (a.seq != b.seq) {
        return a.seq < b.seq;
      }
      return static_cast<u8>(a.kind) < static_cast<u8>(b.kind);
    });
  }

  auto create_flag = [&](u32 pi, diag::Span span) {
    const ir::RegisterIdx reg = claim_reg();
    const ir::OperandIdx alloca_head =
        ir::OperandIdx(static_cast<u32>(builder.state().operands.size()));
    builder.operand(ir::Operand(builder.state().operands[size_one.idx]));
    const ir::InstructionIdx alloca =
        builder.instr({.op = ir::Opcode::Alloca,
                       .flags = {},
                       .dst = reg,
                       .measure = ir::TypeIdx::invalid(),
                       .operands = {alloca_head, 1}});
    builder.reg({.type = i1, .def_idx = alloca});
    instr_spans_.push_back(span);
    const ir::OperandIdx value = bool_operand(false);
    const ir::OperandIdx ptr = to_operand(reg, i1);
    const ir::OperandIdx store_head =
        ir::OperandIdx(static_cast<u32>(builder.state().operands.size()));
    builder.operand(ir::Operand(builder.state().operands[value.idx]));
    builder.operand(ir::Operand(builder.state().operands[ptr.idx]));
    const ir::InstructionIdx clear =
        builder.instr({.op = ir::Opcode::Store,
                       .flags = {},
                       .dst = ir::RegisterIdx(base::INVALID_IDX),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = {store_head, 2}});
    instr_spans_.push_back(span);
    flag[pi] = reg;
    return std::pair<ir::InstructionIdx, ir::InstructionIdx>{alloca, clear};
  };

  auto run_site = [&](u32 si, Output& out) {
    const EndSite& site = end_sites_[si];
    for (usize k = site.locals.size(); k > 0; --k) {
      const u32 local = site.locals[k - 1];
      const Local& info = locals[local];
      if (!runs_destructor(info.type)) {
        continue;
      }
      auto state_of = [&](const std::vector<u32>& path) {
        return state_at(local, path, at(site.block), site.offset, site.seq);
      };
      const State root =
          state_at(local, {}, at(site.block), site.offset, site.seq);
      if (root == State::Moved) {
        continue;
      }
      const ir::OperandIdx place = to_operand(info.addr, info.type);
      if (root == State::Maybe) {
        const u32 pi = place_index(local, {});
        if (pi != base::INVALID_IDX && flag[pi].is_valid()) {
          emit_guard(out, flag[pi], site.span, [&](Output& drop_out) {
            emit_end(drop_out, local, {}, info.type, place, state_of,
                     site.span);
          });
          continue;
        }
      }
      emit_end(out, local, {}, info.type, place, state_of, site.span);
    }
  };

  auto run_store_end = [&](u32 ei, Output& out) {
    const OwnershipEvent& event = ownership_[ei];
    const Local& info = locals[event.local];
    const ir::TypeIdx type = type_at_path(info.type, event.path);
    if (!runs_destructor(type)) {
      return;
    }
    auto state_of = [&](const std::vector<u32>& path) {
      return state_at(event.local, path, at(event.block), event.offset - 1,
                      event.seq);
    };
    const State state = state_of(event.path);
    if (state == State::Moved) {
      return;
    }
    ir::OperandIdx place = to_operand(info.addr, info.type);
    ir::TypeIdx walked = info.type;
    for (u32 step : event.path) {
      const ir::TypeNode& node = builder.types()[walked];
      if (node.tag != ir::TypeTag::Struct) {
        return;
      }
      const ir::TypeIdx field =
          builder.struct_types()[node.as_struct()].fields[step];
      const ir::RegisterIdx gep =
          pass_emit(seq_of(out), ir::Opcode::GetElementPtr, field,
                    {place, zero_i32, index_operand(step)}, event.span);
      place = to_operand(gep, field);
      walked = field;
    }
    if (state == State::Maybe) {
      const u32 pi = place_index(event.local, event.path);
      if (pi != base::INVALID_IDX && flag[pi].is_valid()) {
        emit_guard(out, flag[pi], event.span, [&](Output& drop_out) {
          emit_end(drop_out, event.local, event.path, type, place, state_of,
                   event.span);
        });
        return;
      }
    }
    emit_end(out, event.local, event.path, type, place, state_of, event.span);
  };

  std::vector<Output> outputs(block_count);
  for (u32 b = 0; b < block_count; ++b) {
    Output& out = outputs[b];
    out.blocks.push_back(OutBlock{fn_blocks_[b], ir::InstrSeq{}});
    out.current = 0;
    const base::IdxRange<ir::InstructionIdx> range = streams_[b].finish();
    const u32 count = range.size();
    usize action_at = 0;
    for (u32 boundary = 0; boundary <= count; ++boundary) {
      while (action_at < actions[b].size() &&
             actions[b][action_at].offset == boundary) {
        const Action& action = actions[b][action_at];
        ++action_at;
        switch (action.kind) {
          case Action::Kind::Site: run_site(action.index, out); break;
          case Action::Kind::Bind: {
            const OwnershipEvent& event = ownership_[action.index];
            for (u32 pi = 0; pi < place_count; ++pi) {
              if (places[pi].local != event.local || !flag_needed[pi] ||
                  flag[pi].is_valid()) {
                continue;
              }
              const std::pair<ir::InstructionIdx, ir::InstructionIdx> pair =
                  create_flag(pi, event.span);
              seq_of(out).push(pair.first);
              seq_of(out).push(pair.second);
            }
            break;
          }
          case Action::Kind::Move: {
            const OwnershipEvent& event = ownership_[action.index];
            const u32 pi = place_index(event.local, event.path);
            if (pi != base::INVALID_IDX && flag[pi].is_valid()) {
              pass_emit_void(seq_of(out), ir::Opcode::Store,
                             {bool_operand(true), to_operand(flag[pi], i1)},
                             event.span);
            }
            break;
          }
          case Action::Kind::StoreEnd: run_store_end(action.index, out); break;
          case Action::Kind::DropHere: {
            const OwnershipEvent& event = ownership_[action.index];
            const auto owned = [](const std::vector<u32>&) {
              return State::Owned;
            };
            emit_end(out, event.local, event.path, event.place_type,
                     event.place, owned, event.span);
            break;
          }
          case Action::Kind::StoreClear: {
            const OwnershipEvent& event = ownership_[action.index];
            for (u32 pi = 0; pi < place_count; ++pi) {
              if (places[pi].local == event.local && flag[pi].is_valid() &&
                  is_prefix(event.path, places[pi].path)) {
                pass_emit_void(seq_of(out), ir::Opcode::Store,
                               {bool_operand(false), to_operand(flag[pi], i1)},
                               event.span);
              }
            }
            break;
          }
        }
      }
      if (boundary == count) {
        break;
      }
      pass_copy(seq_of(out), range[boundary]);
    }
  }

  for (u32 b = 0; b < block_count; ++b) {
    for (OutBlock& block : outputs[b].blocks) {
      streams_[at(block.block)] = block.seq;
    }
  }
}

void Lowerer::place_ends() {
  if (failed || (ownership_.empty() && end_sites_.empty())) {
    return;
  }
  const DropElaborationInputs inputs{
      .needs_drop = pkg.needs_drop,
      .drop_glue = pkg.drop_glue,
      .builder = builder,
      .bag = bag,
      .locals = locals,
      .ownership = ownership_,
      .end_sites = end_sites_,
      .instr_spans = instr_spans_,
      .streams = streams_,
      .blocks = fn_blocks_,
      .base = fn_block_base_,
      .type_origins = type_origins_,
      .size_one = size_one,
      .zero_i32 = zero_i32,
      .width = width,
      .drop_method =
          [this](u32 mod, u32 method) {
            const analyzer::CheckedModule& def = pkg.modules[mod];
            const analyzer::CheckedModule::MethodInfo& info =
                def.methods[method];
            return fn_index(mod, info.item, info.name, info.params, info.ret,
                            generic_inst_index(info.self_type), {},
                            ir::SymbolKind::Method, /*extra_generics=*/{},
                            info.self_type);
          },
      .reserve_block = [this] { return reserve_block(); },
  };
  elaborate_drops(inputs);
}

void elaborate_drops(const DropElaborationInputs& inputs) {
  DropElaborator pass(inputs);
  pass.run();
}

ir::TypeIdx type_at_path(ir::StorageBuilder& builder,
                         const std::unordered_map<u32, u32>& type_origins,
                         ir::TypeIdx type,
                         std::span<const u32> path) {
  const auto origin_of = [&](ir::TypeIdx idx) {
    const auto found = type_origins.find(idx.idx);
    return found == type_origins.end() ? idx : ir::TypeIdx(found->second);
  };
  ir::TypeIdx current = origin_of(type);
  for (u32 index : path) {
    const ir::TypeNode& node = builder.types()[current];
    if (node.tag == ir::TypeTag::Struct) {
      const ir::StructType& fields = builder.struct_types()[node.as_struct()];
      if (index >= fields.fields.size()) {
        return builder.error_type();
      }
      current = origin_of(fields.fields[index]);
    } else if (node.tag == ir::TypeTag::Tuple) {
      const ir::TupleType& tuple = builder.tuple_types()[node.as_tuple()];
      if (index >= tuple.elements.size()) {
        return builder.error_type();
      }
      current = origin_of(tuple.elements[index]);
    } else {
      return builder.error_type();
    }
  }
  return current;
}

ir::OperandIdx imm_from_u64(ir::StorageBuilder& builder,
                            ir::TypeTag tag,
                            ir::TypeIdx type,
                            u64 value) {
  ir::Immutable imm{.type = type, .data = {}};
  switch (tag) {
    case ir::TypeTag::I1: imm.data.i1_value = value != 0; break;
    case ir::TypeTag::I8: imm.data.i8_value = static_cast<i8>(value); break;
    case ir::TypeTag::I16: imm.data.i16_value = static_cast<i16>(value); break;
    case ir::TypeTag::I32: imm.data.i32_value = static_cast<i32>(value); break;
    case ir::TypeTag::I64: imm.data.i64_value = static_cast<i64>(value); break;
    case ir::TypeTag::U8: imm.data.u8_value = static_cast<u8>(value); break;
    case ir::TypeTag::U16: imm.data.u16_value = static_cast<u16>(value); break;
    case ir::TypeTag::U32: imm.data.u32_value = static_cast<u32>(value); break;
    default: imm.data.u64_value = value; break;
  }
  return builder.operand(
      ir::Operand::from_immutable(builder.immutable(imm), type));
}

bool runs_destructor(std::span<const u8> needs_drop,
                     const ir::StorageBuilder& builder,
                     const std::unordered_map<u32, u32>& type_origins,
                     ir::TypeIdx type) {
  if (type.idx < needs_drop.size()) {
    return needs_drop[type.idx] != 0;
  }
  // A type interned after the analyzer sized the table - the lowering
  // builds tuple and reference types, a closure's environment among
  // them - answers from its structure. A storage copy follows the
  // copy back to the declaration the analyzer measured; the fields'
  // entries are in the table; a reference owns nothing; and a value
  // cycle cannot reach here because the checker rejects one before
  // lowering runs.
  const auto found = type_origins.find(type.idx);
  const ir::TypeIdx origin =
      found == type_origins.end() ? type : ir::TypeIdx(found->second);
  if (origin.idx != type.idx) {
    return runs_destructor(needs_drop, builder, type_origins, origin);
  }
  const ir::TypeNode& node = builder.types()[type];
  switch (node.tag) {
    case ir::TypeTag::Tuple: {
      const ir::TupleType& tuple = builder.tuple_types()[node.as_tuple()];
      for (ir::TypeIdx element : tuple.elements) {
        if (runs_destructor(needs_drop, builder, type_origins, element)) {
          return true;
        }
      }
      return false;
    }
    case ir::TypeTag::Array:
      return runs_destructor(needs_drop, builder, type_origins,
                             builder.array_types()[node.as_array()].element);
    default: return false;
  }
}

}  // namespace lowering
