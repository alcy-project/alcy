// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/wasm/emit.h"

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "codegen/backend.h"
#include "codegen/diag_code.h"
#include "codegen/target.h"
#include "codegen/wasm/module.h"
#include "codegen/wasm/opcodes.h"
#include "codegen/wasm/reach.h"
#include "codegen/wasm/writer.h"
#include "config/build_config.h"
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "fpag/str/string_pool_id.h"
#include "i18n/messages.h"
#include "ir/block.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/type_util.h"

namespace codegen::wasm {
namespace {

using op::OP_BLOCK;
using op::OP_BLOCK_VOID;
using op::OP_BR;
using op::OP_BR_IF;
using op::OP_BR_TABLE;
using op::OP_CALL;
using op::OP_DROP;
using op::OP_ELSE;
using op::OP_END;
using op::OP_F32_CONST;
using op::OP_F64_CONST;
using op::OP_GLOBAL_GET;
using op::OP_GLOBAL_SET;
using op::OP_I32_ADD;
using op::OP_I32_AND;
using op::OP_I32_CONST;
using op::OP_I32_EQ;
using op::OP_I32_EQZ;
using op::OP_I32_GE_U;
using op::OP_I32_GT_U;
using op::OP_I32_LE_S;
using op::OP_I32_SHL;
using op::OP_I32_SHR_U;
using op::OP_I32_SUB;
using op::OP_I64_CONST;
using op::OP_I64_EQ;
using op::OP_IF;
using op::OP_LOCAL_GET;
using op::OP_LOCAL_SET;
using op::OP_LOOP;
using op::OP_MEMORY_GROW;
using op::OP_MEMORY_SIZE;
using op::OP_RETURN;
using op::OP_SELECT;
using op::OP_UNREACHABLE;

#if BUILD_FLAG(IS_DEBUG)
// A definition whose aggregate result has to live somewhere the frame
// owns: a call's sret destination, a load's copy target, an aggregate
// built in place.
bool needs_slot(const ir::Instruction& instr) {
  using O = ir::Opcode;
  return instr.op == O::Call || instr.op == O::Load ||
         instr.op == O::InsertValue;
}

bool is_aggregate_tag(ir::TypeTag tag) {
  using T = ir::TypeTag;
  return tag == T::Struct || tag == T::Tuple || tag == T::Enum ||
         tag == T::Array;
}

bool is_terminator(ir::Opcode op) {
  using O = ir::Opcode;
  return op == O::Br || op == O::CondBr || op == O::Switch || op == O::Ret ||
         op == O::Unreachable;
}
#endif

// The `main` a binary runs, matching what the analyzer admits and the
// LLVM backend's wrapper expects.
bool is_entry_candidate(const ir::Storage& storage,
                        str::StringInterner& strings,
                        const ir::Function& function) {
  if (function.meta.kind != ir::SymbolKind::Free) {
    return false;
  }
  if (strings.get(function.meta.name) != "main") {
    return false;
  }
  if (!function.meta.param_types.empty()) {
    return false;
  }
  const ir::TypeTag ret = storage.types()[function.meta.return_type].tag;
  return ret == ir::TypeTag::Void || ret == ir::TypeTag::I32 ||
         ret == ir::TypeTag::Never || ret == ir::TypeTag::Enum;
}

}  // namespace

Emitter::Emitter(codegen::EmitRequest request)
    : storage_(std::move(request.storage).unwrap()),
      spans_(request.instr_spans),
      strings_(request.strings),
      bag_(request.bag),
      target_(request.target),
      emit_entry_(request.emit_entry) {}

base::Result<std::vector<u8>, codegen::EmitError> emit_module(
    codegen::EmitRequest request) {
  Emitter emitter(std::move(request));
  return emitter.run();
}

Emitter::EmitResult Emitter::unsupported(diag::Span span,
                                         std::string_view what) {
  if (!failed_) {
    (void)bag_->emit<i18n::Key::CodegenWasmUnsupported>(
        diag::Severity::Error, diag::Stage::CodegenNative,
        codegen::DiagCode::Unsupported, span, what);
    failed_ = true;
  }
  return base::make_err(codegen::EmitError::Unsupported);
}

diag::Span Emitter::span_of(ir::InstructionIdx index) const {
  return index.idx < spans_.size() ? spans_[index.idx] : diag::Span{};
}

base::Result<std::vector<u8>, codegen::EmitError> Emitter::run() {
  // The memory section is finalized once every data segment is placed;
  // adding data needs a memory declared, so it starts as one page.
  builder_.set_memory(1);
  // Address zero is the runtime's null, so nothing the program owns is
  // placed there; the strings start past a small gap.
  constexpr u8 ZERO_GAP[16] = {};
  (void)builder_.add_data(ZERO_GAP, 16);
  data_end_ = sizeof(ZERO_GAP);
  declare_runtime();
  if (choose_roots().is_err()) {
    return base::make_err(codegen::EmitError::Unsupported);
  }
  if (emit_program().is_err()) {
    return base::make_err(codegen::EmitError::Unsupported);
  }
  // The runtime's newline is data too, and the scratch slot the layout
  // publishes sits after every string; both must be placed before the
  // runtime bodies bake the addresses in.
  (void)newline_offset();
  add_memory_and_globals();
  emit_runtime_bodies();
  if (emit_start().is_err()) {
    return base::make_err(codegen::EmitError::Unsupported);
  }
  builder_.export_memory("memory");
  if (start_index_ != NO_LOCAL) {
    builder_.export_function("_start", start_index_);
  }
  return base::make_ok(builder_.finish());
}

// The type section and the function index space's runtime half. The
// emitter's index constants are this function's to keep.
void Emitter::declare_runtime() {
  const u32 fd_write = builder_.add_type(
      FuncType{{ValType::I32, ValType::I32, ValType::I32, ValType::I32},
               {ValType::I32}});
  const u32 proc_exit = builder_.add_type(FuncType{{ValType::I32}, {}});
  const u32 text =
      builder_.add_type(FuncType{{ValType::I32, ValType::I32}, {}});
  const u32 write_all = builder_.add_type(
      FuncType{{ValType::I32, ValType::I32, ValType::I32}, {}});
  const u32 alloc =
      builder_.add_type(FuncType{{ValType::I32, ValType::I32}, {ValType::I32}});

  // The builder calls run in every build; the DCHECKs only pin the indexes.
  const u32 fd_write_index =
      builder_.add_import("wasi_snapshot_preview1", "fd_write", fd_write);
  DCHECK_EQ(fd_write_index, FD_WRITE);
  const u32 proc_exit_index =
      builder_.add_import("wasi_snapshot_preview1", "proc_exit", proc_exit);
  DCHECK_EQ(proc_exit_index, PROC_EXIT);
  const u32 write_all_index = builder_.add_function(write_all);
  DCHECK_EQ(write_all_index, WRITE_ALL);
  const u32 print_index = builder_.add_function(text);
  DCHECK_EQ(print_index, PRINT);
  const u32 println_index = builder_.add_function(text);
  DCHECK_EQ(println_index, PRINTLN);
  const u32 panic_index = builder_.add_function(text);
  DCHECK_EQ(panic_index, PANIC);
  const u32 sys_write_index = builder_.add_function(write_all);
  DCHECK_EQ(sys_write_index, SYS_WRITE);
  const u32 alloc_index = builder_.add_function(alloc);
  DCHECK_EQ(alloc_index, ALLOC);
  const u32 dealloc_index = builder_.add_function(write_all);
  DCHECK_EQ(dealloc_index, DEALLOC);
}

Emitter::EmitResult Emitter::choose_roots() {
  function_index_.assign(storage_.functions().size(), NO_LOCAL);
  std::vector<ir::FunctionIdx> roots;
  if (emit_entry_) {
    for (const ir::FunctionIdx index : storage_.functions().idx_range()) {
      if (is_entry_candidate(storage_, *strings_,
                             storage_.functions()[index])) {
        entry_ = index;
        break;
      }
    }
    if (!entry_.is_valid()) {
      return unsupported(diag::Span{}, "main");
    }
    roots.push_back(entry_);
  } else {
    // A library has no entry to prune from, so every free function is part
    // of its surface. The exported subset narrows when visibility reaches
    // the IR.
    for (const ir::FunctionIdx index : storage_.functions().idx_range()) {
      if (storage_.functions()[index].meta.kind == ir::SymbolKind::Free) {
        roots.push_back(index);
      }
    }
  }
  reachable_ = reachable_functions(storage_, roots);
  // Every reachable function takes its index before any body is written:
  // a call may name a function that comes later in the module, and the
  // emitter writes call operands as fixed indices.
  u32 next = RUNTIME_FUNCTIONS;
  for (const ir::FunctionIdx index : storage_.functions().idx_range()) {
    if (reachable_[index.idx]) {
      function_index_[index.idx] = next++;
    }
  }
  return base::make_ok();
}

Emitter::EmitResult Emitter::emit_program() {
  for (const ir::FunctionIdx index : storage_.functions().idx_range()) {
    if (!reachable_[index.idx]) {
      continue;
    }
    if (emit_function(index).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
  }
  return base::make_ok();
}

std::optional<ValueShape> Emitter::shape_of(ir::TypeIdx type) const {
  using T = ir::TypeTag;
  const ir::TypeNode& node = storage_.types()[type];
  switch (node.tag) {
    case T::Void:
    case T::Never:
    case T::Error: return ValueShape{ValType::I32, 0};
    case T::I1:
    case T::I8:
    case T::I16:
    case T::I32:
    case T::U8:
    case T::U16:
    case T::U32:
    case T::Ptr:
    case T::Function: return ValueShape{ValType::I32, 1};
    case T::I64:
    case T::U64: return ValueShape{ValType::I64, 1};
    case T::F32: return ValueShape{ValType::F32, 1};
    case T::F64: return ValueShape{ValType::F64, 1};
    case T::Str:
    case T::Slice:
    case T::Func: return ValueShape{ValType::I32, 2};
    case T::Ref:
    case T::MutRef: {
      const ir::TypeIdx pointee = storage_.ref_types()[node.as_ref()].pointee;
      if (storage_.types()[pointee].tag == T::Slice) {
        return ValueShape{ValType::I32, 2};
      }
      return ValueShape{ValType::I32, 1};
    }
    // An aggregate value rides as the address of its words in memory:
    // the frame slot a result owns, or the pointer a parameter borrowed.
    case T::Struct:
    case T::Tuple:
    case T::Enum:
    case T::Array: return ValueShape{ValType::I32, 1};
    default: return std::nullopt;
  }
}

std::optional<ValueShape> Emitter::shape_of_definition(
    const ir::Instruction& instr) const {
  if (instr.op == ir::Opcode::Alloca) {
    // The destination names the element type; the value is its address.
    return ValueShape{ValType::I32, 1};
  }
  if (!instr.dst.is_valid()) {
    return ValueShape{ValType::I32, 0};
  }
  return shape_of(storage_.registers()[instr.dst].type);
}

u32 Emitter::allocate_local(ValType type) {
  const u32 index = param_words_ + static_cast<u32>(locals_.size());
  locals_.push_back(type);
  return index;
}

void Emitter::begin_body(u32 param_words) {
  param_words_ = param_words;
  body_ = BinaryWriter{};
  locals_.clear();
}

void Emitter::finish_body(u32 function) {
  FuncBody body;
  body.locals = std::move(locals_);
  body.code = std::move(body_);
  builder_.set_body(function, std::move(body));
}

Emitter::EmitResult Emitter::emit_function(ir::FunctionIdx index) {
  const ir::Function& function = storage_.functions()[index];

  const std::optional<ValueShape> result = shape_of(function.meta.return_type);
  if (!result.has_value()) {
    return unsupported(
        diag::Span{},
        ir::type_to_str(storage_.types()[function.meta.return_type].tag));
  }
  sret_ = is_aggregate_tag(storage_.types()[function.meta.return_type].tag) &&
          result->words == 1;
  std::vector<ValType> params;
  u32 param_words = 0;
  if (sret_) {
    // The caller brings the storage for the result as a leading pointer.
    params.push_back(ValType::I32);
    ++param_words;
  }
  for (const ir::TypeIdx param : function.meta.param_types) {
    const std::optional<ValueShape> shape = shape_of(param);
    if (!shape.has_value()) {
      return unsupported(diag::Span{},
                         ir::type_to_str(storage_.types()[param].tag));
    }
    params.insert(params.end(), shape->words, shape->type);
    param_words += shape->words;
  }
  std::vector<ValType> results;
  if (!sret_) {
    results.assign(result->words, result->type);
  }

  const u32 type = builder_.add_type(FuncType{params, results});
  const u32 wasm = builder_.add_function(type);
  // The index was assigned when the roots were chosen, so a forward call
  // already knows it; adding in order is what keeps the two equal.
  DCHECK_EQ(wasm, function_index_[index.idx]);

  begin_body(param_words);
  if (build_locals(function).is_err()) {
    return base::make_err(codegen::EmitError::Unsupported);
  }

  // The shadow stack pointer is restored on every return, so a loop that
  // allocates does not walk the stack up.
  global_get(GLOBAL_SP);
  local_set(frame_local_);
  // The frame's own slots sit above the saved pointer; the shadow stack
  // moves past them once, and a return restores it.
  local_get(frame_local_);
  i32_const(static_cast<i32>(frame_bytes_));
  op(OP_I32_ADD);
  global_set(GLOBAL_SP);
  set_state(0);
  if (emit_dispatch().is_err()) {
    return base::make_err(codegen::EmitError::Unsupported);
  }
  finish_body(wasm);
  return base::make_ok();
}

Emitter::EmitResult Emitter::build_locals(const ir::Function& function) {
  reg_base_.assign(storage_.registers().size(), NO_LOCAL);
  reg_shape_.assign(storage_.registers().size(), ValueShape{});
  reg_slot_.assign(storage_.registers().size(), NO_LOCAL);
  frame_bytes_ = 0;
  alloca_elem_.assign(storage_.registers().size(), ir::TypeIdx::invalid());
  blocks_.clear();
  blocks_.reserve(function.blocks.size());
  for (const ir::BlockIdx block : function.blocks) {
    blocks_.push_back(block);
  }
  block_case_.assign(storage_.blocks().size(), NO_LOCAL);
  for (u32 case_index = 0; case_index < blocks_.size(); ++case_index) {
    block_case_[blocks_[case_index].idx] = case_index;
  }

  frame_local_ = allocate_local(ValType::I32);
  state_local_ = allocate_local(ValType::I32);
  scratch0_ = allocate_local(ValType::I32);
  scratch1_ = allocate_local(ValType::I32);

  // The entry block's parameters are the function's parameters; the wasm
  // parameters are already locals 0..param_words-1, so those registers
  // share them instead of taking fresh slots.
  const ir::Block& entry = storage_.blocks()[function.blocks.head()];
  u32 word = sret_ ? 1 : 0;
  u32 param = 0;
  for (const ir::TypeIdx type : function.meta.param_types) {
    if (param >= entry.block_params.size()) {
      return unsupported(diag::Span{}, "function parameters");
    }
    const std::optional<ValueShape> shape = shape_of(type);
    DCHECK(shape.has_value());
    const ir::RegisterIdx reg =
        storage_.block_params()[entry.block_params[param]].reg;
    reg_base_[reg.idx] = word;
    reg_shape_[reg.idx] = *shape;
    word += shape->words;
    ++param;
  }
  if (word != param_words_) {
    return unsupported(diag::Span{}, "function parameters");
  }

  for (const ir::BlockIdx block_index : blocks_) {
    if (block_index.idx == function.blocks.head().idx) {
      continue;
    }
    const ir::Block& block = storage_.blocks()[block_index];
    for (const ir::BlockParamIdx param_index : block.block_params) {
      const ir::BlockParam& block_param = storage_.block_params()[param_index];
      const std::optional<ValueShape> shape = shape_of(block_param.type);
      if (!shape.has_value()) {
        return unsupported(
            diag::Span{},
            ir::type_to_str(storage_.types()[block_param.type].tag));
      }
      reg_shape_[block_param.reg.idx] = *shape;
      reg_base_[block_param.reg.idx] = NO_LOCAL;
      for (u32 word_index = 0; word_index < shape->words; ++word_index) {
        const u32 local = allocate_local(shape->type);
        if (word_index == 0) {
          reg_base_[block_param.reg.idx] = local;
        }
      }
    }
  }

  for (const ir::BlockIdx block_index : blocks_) {
    const ir::Block& block = storage_.blocks()[block_index];
    for (const ir::InstructionIdx instr_index : block.instrs) {
      const ir::Instruction& instr = storage_.instrs()[instr_index];
      if (!instr.dst.is_valid() || reg_base_[instr.dst.idx] != NO_LOCAL) {
        continue;
      }
      const std::optional<ValueShape> shape = shape_of_definition(instr);
      if (!shape.has_value()) {
        const ir::TypeIdx type = storage_.registers()[instr.dst].type;
        return unsupported(span_of(instr_index),
                           ir::type_to_str(storage_.types()[type].tag));
      }
      if (shape->words == 0) {
        continue;
      }
      reg_shape_[instr.dst.idx] = *shape;
      for (u32 word_index = 0; word_index < shape->words; ++word_index) {
        const u32 local = allocate_local(shape->type);
        if (word_index == 0) {
          reg_base_[instr.dst.idx] = local;
        }
      }
    }
  }

  // An aggregate produced by a call or a load owns a slot the frame
  // reserves here; the def writes its address into the register and the
  // words through it. Reserving at entry, not at the def, is what keeps
  // a loop from walking the shadow stack up.
  for (const ir::BlockIdx block_index : blocks_) {
    const ir::Block& block = storage_.blocks()[block_index];
    for (const ir::InstructionIdx instr_index : block.instrs) {
      const ir::Instruction& instr = storage_.instrs()[instr_index];
      if (!instr.dst.is_valid() || reg_slot_[instr.dst.idx] != NO_LOCAL) {
        continue;
      }
      if (!needs_slot(instr)) {
        continue;
      }
      const ir::TypeIdx type = storage_.registers()[instr.dst].type;
      const ir::TypeLayout layout = storage_.layout_of(type, target_.width);
      frame_bytes_ = static_cast<u32>(
          ir::align_up(frame_bytes_, layout.align == 0 ? 1 : layout.align));
      reg_slot_[instr.dst.idx] = frame_bytes_;
      frame_bytes_ += static_cast<u32>(layout.size == 0 ? 1 : layout.size);
    }
  }
  frame_bytes_ = static_cast<u32>(ir::align_up(frame_bytes_, 16));
  return base::make_ok();
}

Emitter::EmitResult Emitter::emit_dispatch() {
  const u32 cases = static_cast<u32>(blocks_.size());
  DCHECK(cases > 0);
  loop_void();
  for (u32 i = 0; i < cases; ++i) {
    block_void();
  }
  local_get(state_local_);
  op(OP_BR_TABLE);
  body_.u32_leb(cases);
  for (u32 i = 0; i < cases; ++i) {
    body_.u32_leb(i);
  }
  body_.u32_leb(cases - 1);
  for (u32 k = 0; k < cases; ++k) {
    end_op();  // Closes case k; its body follows, inside the outer cases.
    dispatch_depth_ = cases - 1 - k;
    if (emit_block(blocks_[k]).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
  }
  end_op();  // Closes the dispatch loop.
  op(OP_UNREACHABLE);
  return base::make_ok();
}

Emitter::EmitResult Emitter::emit_block(ir::BlockIdx block_index) {
  const ir::Block& block = storage_.blocks()[block_index];
  for (const ir::InstructionIdx instr_index : block.instrs) {
    current_span_ = span_of(instr_index);
    const ir::Instruction& instr = storage_.instrs()[instr_index];
    if (emit_instruction(instr).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
  }
#if BUILD_FLAG(IS_DEBUG)
  DCHECK(block.instrs.empty() ||
         is_terminator(
             storage_.instrs()[block.instrs[block.instrs.size() - 1]].op));
#endif
  return base::make_ok();
}

Emitter::EmitResult Emitter::emit_instruction(const ir::Instruction& instr) {
  using O = ir::Opcode;
  switch (instr.op) {
    case O::Noop: return base::make_ok();

    case O::IntAdd:
    case O::IntSub:
    case O::IntMul:
    case O::IntDiv:
    case O::UintDiv:
    case O::IntRem:
    case O::UintRem:
    case O::FAdd:
    case O::FSub:
    case O::FMul:
    case O::FDiv:
    case O::And:
    case O::Or:
    case O::Xor:
    case O::ShiftLeft:
    case O::ArithmeticShiftRight:
    case O::LogicalShiftRight:
    case O::Not:
    case O::BitReverse:
    case O::Eq:
    case O::Ne:
    case O::Le:
    case O::Lt:
    case O::Ge:
    case O::Gt:
    case O::TypeCast:
    case O::TypeSizeOf:
    case O::TypeAlignOf:
    case O::Select:
    case O::Move:
    case O::Borrow:
    case O::Drop: return emit_compute(instr);

    case O::Alloca:
    case O::Load:
    case O::Store:
    case O::Memcopy:
    case O::GetElementPtr:
    case O::ElemOffset:
    case O::ExtractValue:
    case O::InsertValue: return emit_memory(instr);

    case O::Br:
    case O::CondBr:
    case O::Switch:
    case O::Call:
    case O::Ret:
    case O::Unreachable: return emit_control(instr);

    default: return unsupported(current_span_, ir::opcode_to_str(instr.op));
  }
}

Emitter::EmitResult Emitter::emit_control(const ir::Instruction& instr) {
  using O = ir::Opcode;
  switch (instr.op) {
    case O::Br: return emit_br(instr);
    case O::CondBr: return emit_cond_br(instr);
    case O::Switch: return emit_switch(instr);
    case O::Call: return emit_call(instr);
    case O::Ret: return emit_ret(instr);
    case O::Unreachable: op(OP_UNREACHABLE); return base::make_ok();
    default: return unsupported(current_span_, ir::opcode_to_str(instr.op));
  }
}

u32 Emitter::case_of(ir::BlockIdx block) const {
  const u32 case_index = block_case_[block.idx];
  DCHECK(case_index != NO_LOCAL);
  return case_index;
}

void Emitter::set_state(u32 block) {
  i32_const(static_cast<i32>(block));
  local_set(state_local_);
}

Emitter::EmitResult Emitter::branch_to(ir::BlockIdx target,
                                       ir::OperandIdxRange args) {
  const ir::Block& target_block = storage_.blocks()[target];
  if (args.size() != target_block.block_params.size()) {
    return unsupported(current_span_, "block parameters");
  }
  for (u32 position = 0; position < target_block.block_params.size();
       ++position) {
    const ir::BlockParam& param =
        storage_.block_params()[target_block.block_params[position]];
    const ir::Operand& arg = storage_.operands()[args[position]];
    const ValueShape shape = reg_shape_[param.reg.idx];
    for (u32 word = 0; word < shape.words; ++word) {
      if (push_word(arg, word, shape.type).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      local_set(reg_base_[param.reg.idx] + word);
    }
  }
  set_state(case_of(target));
  br(dispatch_depth_);
  return base::make_ok();
}

Emitter::EmitResult Emitter::emit_br(const ir::Instruction& instr) {
  const ir::OperandIdxRange ops = instr.operands;
  DCHECK(!ops.empty());
  const ir::BlockIdx target = storage_.operands()[ops.head()].as_block();
  return branch_to(target, ir::OperandIdxRange{ops.head() + 1, ops.size() - 1});
}

Emitter::EmitResult Emitter::emit_cond_br(const ir::Instruction& instr) {
  const ir::OperandIdxRange ops = instr.operands;
  DCHECK_EQ(ops.size(), 3u);
  const ir::Operand& cond = storage_.operands()[ops.head()];
  const ir::BlockIdx yes = storage_.operands()[ops.head() + 1].as_block();
  const ir::BlockIdx no = storage_.operands()[ops.head() + 2].as_block();
  if (push_word(cond, 0, ValType::I32).is_err()) {
    return base::make_err(codegen::EmitError::Unsupported);
  }
  if_void();
  set_state(case_of(yes));
  else_op();
  set_state(case_of(no));
  end_op();
  br(dispatch_depth_);
  return base::make_ok();
}

Emitter::EmitResult Emitter::emit_switch(const ir::Instruction& instr) {
  const ir::OperandIdxRange ops = instr.operands;
  DCHECK(ops.size() >= 2);

  const ir::Operand& value = storage_.operands()[ops.head()];
  const ir::TypeTag tag = storage_.types()[value.type].tag;
  const bool wide = tag == ir::TypeTag::I64 || tag == ir::TypeTag::U64;
  const ir::BlockIdx fallback = storage_.operands()[ops.head() + 1].as_block();
  set_state(case_of(fallback));

  for (u32 index = 2; index + 1 < ops.size(); index += 2) {
    const ir::Operand& case_value = storage_.operands()[ops.head() + index];
    const ir::BlockIdx target =
        storage_.operands()[ops.head() + index + 1].as_block();
    DCHECK(case_value.is<ir::ImmutableIdx>());
    const ir::Immutable& immutable =
        storage_.immutables()[case_value.as_immutable()];
    const ir::TypeTag case_tag = storage_.types()[immutable.type].tag;
    const u64 raw = immutable.as_u64_integer(case_tag);

    // state = value == case ? target : state, folded through the operand
    // stack's select so no branch nests.
    i32_const(static_cast<i32>(case_of(target)));
    local_get(state_local_);
    if (wide) {
      i64_const(static_cast<i64>(raw));
      if (push_word(value, 0, ValType::I64).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      op(OP_I64_EQ);
    } else {
      i32_const(static_cast<i32>(raw));
      if (push_word(value, 0, ValType::I32).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      op(OP_I32_EQ);
    }
    op(OP_SELECT);
    local_set(state_local_);
  }
  br(dispatch_depth_);
  return base::make_ok();
}

Emitter::EmitResult Emitter::emit_call(const ir::Instruction& instr) {
  const ir::OperandIdxRange ops = instr.operands;
  DCHECK(!ops.empty());
  const ir::Operand& callee = storage_.operands()[ops.head()];
  if (!callee.is<ir::FunctionIdx>() && !callee.is<ir::ExternalFunctionIdx>()) {
    return unsupported(current_span_, "a call through a function value");
  }

  u32 callee_index = NO_LOCAL;
  if (callee.is<ir::FunctionIdx>()) {
    callee_index = function_index_[callee.as_function().idx];
    DCHECK(callee_index != NO_LOCAL);
  } else {
    const ir::ExternalFunction& external =
        storage_.external_functions()[callee.as_external_function()];
    const std::string_view name = strings_->get(external.meta.name);
    if (name == "alcy_print") {
      callee_index = PRINT;
    } else if (name == "alcy_println") {
      callee_index = PRINTLN;
    } else if (name == "alcy_panic") {
      callee_index = PANIC;
    } else if (name == "alcy_sys_write") {
      callee_index = SYS_WRITE;
    } else if (name == "alcy_alloc") {
      callee_index = ALLOC;
    } else if (name == "alcy_dealloc") {
      callee_index = DEALLOC;
    } else {
      return unsupported(current_span_, name);
    }
  }

  const bool aggregate_result =
      callee.is<ir::FunctionIdx>() &&
      is_aggregate_tag(storage_
                           .types()[storage_.functions()[callee.as_function()]
                                        .meta.return_type]
                           .tag);
  u32 slot = NO_LOCAL;
  if (aggregate_result) {
    DCHECK(instr.dst.is_valid());
    slot = reg_slot_[instr.dst.idx];
    DCHECK(slot != NO_LOCAL);
    // The result's storage leads the arguments, the way the callee's
    // signature declares it.
    local_get(frame_local_);
    i32_const(static_cast<i32>(slot));
    op(OP_I32_ADD);
  }

  for (u32 index = 1; index < ops.size(); ++index) {
    if (push_operand(storage_.operands()[ops.head() + index]).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
  }
  call(callee_index);

  if (aggregate_result) {
    local_get(frame_local_);
    i32_const(static_cast<i32>(slot));
    op(OP_I32_ADD);
    local_set(reg_base_[instr.dst.idx]);
  } else if (instr.dst.is_valid() && reg_shape_[instr.dst.idx].words > 0) {
    pop_words(reg_base_[instr.dst.idx], reg_shape_[instr.dst.idx].words);
  }
  return base::make_ok();
}

Emitter::EmitResult Emitter::emit_ret(const ir::Instruction& instr) {
  const ir::OperandIdxRange ops = instr.operands;
  DCHECK(ops.size() <= 1);
  // The frame slot holds the stack pointer the function started with, so
  // an allocation in a loop does not walk the stack up across returns.
  local_get(frame_local_);
  global_set(GLOBAL_SP);
  if (sret_ && !ops.empty()) {
    // The result goes to the caller's storage, which is the leading
    // parameter. The frame is already restored; the copy reads the
    // value's own slot, which the caller's frame also holds.
    const ir::Operand& value = storage_.operands()[ops.head()];
    const u32 size =
        static_cast<u32>(storage_.layout_of(value.type, target_.width).size);
    local_get(0);
    if (push_word(value, 0, ValType::I32).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
    i32_const(static_cast<i32>(size));
    body_.bytes(MEMORY_COPY);
    op(OP_RETURN);
    return base::make_ok();
  }
  if (!ops.empty()) {
    if (push_operand(storage_.operands()[ops.head()]).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
  }
  op(OP_RETURN);
  return base::make_ok();
}

Emitter::EmitResult Emitter::push_operand(const ir::Operand& operand) {
  const std::optional<ValueShape> shape = shape_of(operand.type);
  if (!shape.has_value()) {
    return unsupported(current_span_,
                       ir::type_to_str(storage_.types()[operand.type].tag));
  }
  for (u32 word = 0; word < shape->words; ++word) {
    if (push_word(operand, word, shape->type).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
  }
  return base::make_ok();
}

Emitter::EmitResult Emitter::push_word(const ir::Operand& operand,
                                       u32 word,
                                       ValType type) {
  (void)type;
  using Payload = ir::Operand::Payload;
  switch (operand.tag()) {
    case Payload::TagOf<ir::RegisterIdx>: {
      const u32 base = reg_base_[operand.as_register().idx];
      if (base == NO_LOCAL) {
        return unsupported(current_span_, "an undefined value");
      }
      local_get(base + word);
      return base::make_ok();
    }
    case Payload::TagOf<ir::ImmutableIdx>:
      return push_immutable(storage_.immutables()[operand.as_immutable()],
                            word);
    default: return unsupported(current_span_, "this operand");
  }
}

Emitter::EmitResult Emitter::push_immutable(const ir::Immutable& immutable,
                                            u32 word) {
  const ir::TypeTag tag = storage_.types()[immutable.type].tag;
  if (ir::is_integer_type(tag)) {
    const u64 raw = immutable.as_u64_integer(tag);
    if (word == 0 && (tag == ir::TypeTag::I64 || tag == ir::TypeTag::U64)) {
      i64_const(static_cast<i64>(raw));
    } else {
      i32_const(static_cast<i32>(static_cast<u32>(raw)));
    }
    return base::make_ok();
  }
  if (ir::is_float_type(tag)) {
    if (tag == ir::TypeTag::F32) {
      f32_const(static_cast<f32>(immutable.as_f64_fp(tag)));
    } else {
      f64_const(immutable.as_f64_fp(tag));
    }
    return base::make_ok();
  }
  if (tag == ir::TypeTag::Str) {
    const std::string_view text = string_text(immutable.data.str_id_value);
    if (word == 0) {
      i32_const(static_cast<i32>(data_for(immutable.data.str_id_value)));
      return base::make_ok();
    }
    i32_const(static_cast<i32>(text.size()));
    return base::make_ok();
  }
  if (tag == ir::TypeTag::Ptr || tag == ir::TypeTag::Function ||
      tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
    i32_const(static_cast<i32>(static_cast<u32>(immutable.data.ptr)));
    return base::make_ok();
  }
  return unsupported(current_span_, ir::type_to_str(tag));
}

Emitter::EmitResult Emitter::copy_operand(const ir::Operand& operand,
                                          u32 dst_base,
                                          ValueShape shape) {
  for (u32 word = 0; word < shape.words; ++word) {
    if (push_word(operand, word, shape.type).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
    local_set(dst_base + word);
  }
  return base::make_ok();
}

void Emitter::pop_words(u32 base, u32 words) {
  for (u32 word = words; word > 0; --word) {
    local_set(base + word - 1);
  }
}

void Emitter::copy_locals(u32 src_base, u32 dst_base, u32 words) {
  for (u32 word = 0; word < words; ++word) {
    local_get(src_base + word);
    local_set(dst_base + word);
  }
}

std::string_view Emitter::string_text(str::StringPoolId id) const {
  return strings_->get(id);
}

u32 Emitter::data_for(std::string_view text) {
  const u32 offset = data_end_;
  builder_.add_data(std::span<const u8>(
      reinterpret_cast<const u8*>(text.data()), text.size()));
  data_end_ = offset + static_cast<u32>(text.size());
  return offset;
}

u32 Emitter::data_for(str::StringPoolId id) {
  const auto found = string_data_.find(id.offset);
  if (found != string_data_.end()) {
    return found->second;
  }
  const u32 offset = data_for(string_text(id));
  string_data_.emplace(id.offset, offset);
  return offset;
}

u32 Emitter::newline_offset() {
  if (newline_ == NO_LOCAL) {
    newline_ = data_for("\n");
  }
  return newline_;
}

void Emitter::add_memory_and_globals() {
  scratch_ = static_cast<u32>(ir::align_up(data_end_, SCRATCH_BYTES));
  stack_base_ = scratch_ + SCRATCH_BYTES;
  heap_base_ = stack_base_ + STACK_RESERVE;
  // One page of heap beyond the cursor keeps the first small allocation
  // from having to grow the memory at all.
  const u32 pages = (heap_base_ + PAGE_BYTES) / PAGE_BYTES;
  builder_.set_memory(pages == 0 ? 1 : pages);
  builder_.add_global(ValType::I32, true, static_cast<i32>(stack_base_));
  builder_.add_global(ValType::I32, true, static_cast<i32>(heap_base_));
}

// The runtime the program's `alcy_*` declarations resolve to, written
// directly: a retry loop around `fd_write` and a bump allocator.
void Emitter::emit_runtime_bodies() {
  {
    // alcy_write_all(fd, ptr, len)
    begin_body(3);
    const u32 written = allocate_local(ValType::I32);
    const u32 count = allocate_local(ValType::I32);
    i32_const(0);
    local_set(written);
    block_void();
    loop_void();
    local_get(written);
    local_get(2);
    op(OP_I32_GE_U);
    op(OP_BR_IF);  // $done
    body_.u32_leb(1);
    i32_const(static_cast<i32>(scratch_));
    local_get(1);
    local_get(written);
    op(OP_I32_ADD);
    i32_store(body_, 2, 0);
    i32_const(static_cast<i32>(scratch_ + 4));
    local_get(2);
    local_get(written);
    op(OP_I32_SUB);
    i32_store(body_, 2, 0);
    local_get(0);
    i32_const(static_cast<i32>(scratch_));
    i32_const(1);
    i32_const(static_cast<i32>(scratch_ + 8));
    call(FD_WRITE);
    op(OP_BR_IF);  // A nonzero errno ends the loop.
    body_.u32_leb(1);
    i32_const(static_cast<i32>(scratch_ + 8));
    i32_load(body_, 2, 0);
    local_set(count);
    local_get(count);
    i32_const(0);
    op(OP_I32_LE_S);
    op(OP_BR_IF);
    body_.u32_leb(1);
    local_get(written);
    local_get(count);
    op(OP_I32_ADD);
    local_set(written);
    br(0);
    end_op();
    end_op();
    finish_body(WRITE_ALL);
  }
  {
    // alcy_print(ptr, len)
    begin_body(2);
    i32_const(1);
    local_get(0);
    guard_null_len(0);
    call(WRITE_ALL);
    finish_body(PRINT);
  }
  {
    // alcy_println(ptr, len)
    begin_body(2);
    i32_const(1);
    local_get(0);
    guard_null_len(0);
    call(WRITE_ALL);
    i32_const(1);
    i32_const(static_cast<i32>(newline_offset()));
    i32_const(1);
    call(WRITE_ALL);
    finish_body(PRINTLN);
  }
  {
    // alcy_sys_write(fd, ptr, len)
    begin_body(3);
    local_get(0);
    local_get(1);
    guard_null_len(1);
    call(WRITE_ALL);
    finish_body(SYS_WRITE);
  }
  {
    // alcy_panic(ptr, len)
    begin_body(2);
    i32_const(2);
    local_get(0);
    guard_null_len(0);
    call(WRITE_ALL);
    i32_const(1);
    call(PROC_EXIT);
    op(OP_UNREACHABLE);
    finish_body(PANIC);
  }
  {
    // alcy_alloc(size, align)
    begin_body(2);
    const u32 block = allocate_local(ValType::I32);
    const u32 end = allocate_local(ValType::I32);
    // block = align_up(heap, align)
    global_get(GLOBAL_HEAP);
    local_get(1);
    i32_const(1);
    op(OP_I32_SUB);
    op(OP_I32_ADD);
    i32_const(0);
    local_get(1);
    op(OP_I32_SUB);
    op(OP_I32_AND);
    local_set(block);
    // end = block + max(size, 1)
    local_get(block);
    i32_const(1);
    local_get(0);
    local_get(0);
    op(OP_I32_EQZ);
    op(OP_SELECT);
    op(OP_I32_ADD);
    local_set(end);
    // Grow when the cursor runs past the memory that exists.
    local_get(end);
    op(OP_MEMORY_SIZE);
    body_.byte(0x00);
    i32_const(16);
    op(OP_I32_SHL);
    op(OP_I32_GT_U);
    if_void();
    local_get(end);
    op(OP_MEMORY_SIZE);
    body_.byte(0x00);
    i32_const(16);
    op(OP_I32_SHL);
    op(OP_I32_SUB);
    i32_const(0xFFFF);
    op(OP_I32_ADD);
    i32_const(16);
    op(OP_I32_SHR_U);
    op(OP_MEMORY_GROW);
    body_.byte(0x00);
    op(OP_DROP);
    end_op();
    local_get(end);
    global_set(GLOBAL_HEAP);
    local_get(block);
    finish_body(ALLOC);
  }
  {
    // alcy_dealloc(ptr, size, align): the bump allocator frees nothing.
    begin_body(3);
    finish_body(DEALLOC);
  }
}

void Emitter::guard_null_len(u32 ptr_local) {
  // The `(fd, ptr, ...)` operands are already pushed; only the length is
  // pushed here: zero when the pointer is null, the local otherwise.
  i32_const(0);
  local_get(ptr_local + 1);
  local_get(ptr_local);
  op(OP_I32_EQZ);
  op(OP_SELECT);
}

Emitter::EmitResult Emitter::emit_start() {
  if (!emit_entry_) {
    return base::make_ok();
  }
  const ir::TypeTag ret =
      storage_.types()[storage_.functions()[entry_].meta.return_type].tag;
  if (ret == ir::TypeTag::Enum) {
    return unsupported(diag::Span{}, "a main that returns an enum");
  }
  const u32 type = builder_.add_type(FuncType{{}, {}});
  start_index_ = builder_.add_function(type);
  begin_body(0);
  call(function_index_[entry_.idx]);
  if (ret == ir::TypeTag::I32) {
    call(PROC_EXIT);
  } else if (ret == ir::TypeTag::Never) {
    op(OP_UNREACHABLE);
  }
  finish_body(start_index_);
  return base::make_ok();
}

// --- raw encoders ---

void Emitter::op(u8 byte) {
  body_.byte(byte);
}

void Emitter::i32_const(i32 value) {
  op(OP_I32_CONST);
  body_.i32_leb(value);
}

void Emitter::i64_const(i64 value) {
  op(OP_I64_CONST);
  body_.i64_leb(value);
}

void Emitter::f32_const(f32 value) {
  op(OP_F32_CONST);
  body_.f32_bits(value);
}

void Emitter::f64_const(f64 value) {
  op(OP_F64_CONST);
  body_.f64_bits(value);
}

void Emitter::local_get(u32 local) {
  op(OP_LOCAL_GET);
  body_.u32_leb(local);
}

void Emitter::local_set(u32 local) {
  op(OP_LOCAL_SET);
  body_.u32_leb(local);
}

void Emitter::global_get(u32 global) {
  op(OP_GLOBAL_GET);
  body_.u32_leb(global);
}

void Emitter::global_set(u32 global) {
  op(OP_GLOBAL_SET);
  body_.u32_leb(global);
}

void Emitter::call(u32 function) {
  op(OP_CALL);
  body_.u32_leb(function);
}

void Emitter::if_void() {
  op(OP_IF);
  op(OP_BLOCK_VOID);
}

void Emitter::else_op() {
  op(OP_ELSE);
}

void Emitter::end_op() {
  op(OP_END);
}

void Emitter::block_void() {
  op(OP_BLOCK);
  op(OP_BLOCK_VOID);
}

void Emitter::loop_void() {
  op(OP_LOOP);
  op(OP_BLOCK_VOID);
}

void Emitter::br(u32 depth) {
  op(OP_BR);
  body_.u32_leb(depth);
}

}  // namespace codegen::wasm
