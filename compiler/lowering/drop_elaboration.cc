// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Drop elaboration: solve ownership per place over the CFG and place
// each end where the paths still own the value (ADR-0062). Lowering
// journals the end sites and the ownership events; this pass reads
// them, computes the state of every place at every point that decides
// an end, and rewrites the function's blocks to carry the ends.

#include <algorithm>
#include <functional>
#include <span>
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

ir::RegisterIdx Lowerer::pass_emit(ir::InstrSeq& seq,
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

void Lowerer::pass_emit_void(ir::InstrSeq& seq,
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

void Lowerer::pass_copy(ir::InstrSeq& seq, ir::InstructionIdx idx) {
  const ir::Instruction& old = builder.state().instrs[idx.idx];
  const ir::InstructionIdx copy = builder.instr({.op = old.op,
                                                 .flags = old.flags,
                                                 .dst = old.dst,
                                                 .measure = old.measure,
                                                 .operands = old.operands});
  seq.push(copy);
  instr_spans_.push_back(instr_spans_[idx.idx]);
}

void Lowerer::place_ends() {
  if (failed || (ownership_.empty() && end_sites_.empty())) {
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
    }
    return state;
  };
  auto step_point = [&](State state, const Point& point, const Place& place) {
    if (point.kind == Point::Kind::Event) {
      const OwnershipEvent& event = ownership_[point.index];
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
    if (type.idx < pkg.drop_glue.size()) {
      const analyzer::CheckedModule::DropGlue& glue = pkg.drop_glue[type.idx];
      if (glue.index != base::INVALID_IDX) {
        const analyzer::CheckedModule& def = pkg.modules[glue.module];
        const analyzer::CheckedModule::MethodInfo& info =
            def.methods[glue.index];
        ir::FunctionIdx fn = fn_index(
            glue.module, info.item, info.name, info.params, info.ret,
            generic_inst_index(info.self_type), {}, ir::SymbolKind::Method);
        if (!fn.is_valid()) {
          return;
        }
        pass_emit_void(seq_of(out), ir::Opcode::Drop, {place}, span);
        const ir::RegisterIdx value =
            pass_emit(seq_of(out), ir::Opcode::Load, type, {place}, span);
        pass_emit_void(seq_of(out), ir::Opcode::Move, {place}, span);
        pass_emit_void(seq_of(out), ir::Opcode::Call,
                       {builder.operand(ir::Operand::from_function(
                            fn, builder.primitive(ir::TypeTag::Function))),
                        to_operand(value, type)},
                       span);
        return;
      }
    }
    const ir::TypeNode& node = builder.types()[type];
    if (node.tag != ir::TypeTag::Struct) {
      const u32 index = bag.emit<i18n::Key::LowerDestructorNotRun>(
          diag::Severity::Warning, diag::Stage::Lowering,
          DiagCode::DropUnplaced, span, locals[local].name);
      (void)index;
      return;
    }
    const ir::StructType& fields = builder.struct_types()[node.as_struct()];
    for (u32 i = 0; i < fields.fields.size(); ++i) {
      const ir::TypeIdx field = fields.fields[i];
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
    enum class Kind : u8 { Site, Bind, Move, StoreEnd, StoreClear };
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

}  // namespace lowering
