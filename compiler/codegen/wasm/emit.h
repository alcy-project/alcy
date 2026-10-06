// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "codegen/backend.h"
#include "codegen/target.h"
#include "codegen/wasm/module.h"
#include "codegen/wasm/writer.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/operand.h"
#include "ir/storage.h"

namespace codegen::wasm {

// A value's wasm shape: the machine type of each of its words. A scalar
// is one word; `str`, `slice`, and the two-word closure values are two.
struct ValueShape {
  ValType type = ValType::I32;
  u32 words = 0;
};

// Turns the program's reachable IR into one wasm module: the program's
// functions, the `alcy_*` runtime they call, and a WASI `_start` when the
// request owns the entry.
//
// Every IR block becomes a case of a `br_table` selected by a state
// local, so any CFG emits without a relooper. Values live in wasm locals
// (one per SSA register and word); `Alloca` bumps a shadow stack pointer
// that each function restores on return. A construct the emitter does
// not encode is a spanned diagnostic, never a wrong encoding.
class Emitter {
 public:
  explicit Emitter(codegen::EmitRequest request);
  ~Emitter() = default;

  Emitter(const Emitter&) = delete;
  Emitter& operator=(const Emitter&) = delete;

  [[nodiscard]] base::Result<std::vector<u8>, codegen::EmitError> run();

 private:
  using EmitResult = base::Result<void, codegen::EmitError>;

  // The function index space: the two WASI imports, then the runtime this
  // emitter defines, then the program's functions, then `_start`.
  static constexpr u32 FD_WRITE = 0;
  static constexpr u32 PROC_EXIT = 1;
  static constexpr u32 WRITE_ALL = 2;
  static constexpr u32 PRINT = 3;
  static constexpr u32 PRINTLN = 4;
  static constexpr u32 PANIC = 5;
  static constexpr u32 SYS_WRITE = 6;
  static constexpr u32 ALLOC = 7;
  static constexpr u32 DEALLOC = 8;
  // The runtime occupies [0, RUNTIME_FUNCTIONS); the program's functions
  // and `_start` follow it.
  static constexpr u32 RUNTIME_FUNCTIONS = 9;

  // The globals the module defines: the shadow stack pointer and the
  // bump allocator's cursor.
  static constexpr u32 GLOBAL_SP = 0;
  static constexpr u32 GLOBAL_HEAP = 1;
  static constexpr u32 NO_LOCAL = 0xFFFFFFFF;

  // Bytes reserved between the data and the heap for the shadow stack.
  static constexpr u32 STACK_RESERVE = 64 * 1024;
  // The iovec and written-count slot `fd_write` reads and fills.
  static constexpr u32 SCRATCH_BYTES = 16;
  static constexpr u32 PAGE_BYTES = 64 * 1024;

  // --- module phases ---
  void declare_runtime();
  EmitResult choose_roots();
  EmitResult emit_program();
  void emit_runtime_bodies();
  EmitResult emit_start();
  void add_memory_and_globals();

  // --- one function ---
  EmitResult emit_function(ir::FunctionIdx index);
  EmitResult build_locals(const ir::Function& function);
  EmitResult emit_dispatch();
  EmitResult emit_block(ir::BlockIdx block);
  EmitResult emit_instruction(const ir::Instruction& instr);
  EmitResult emit_control(const ir::Instruction& instr);
  EmitResult emit_br(const ir::Instruction& instr);
  EmitResult emit_cond_br(const ir::Instruction& instr);
  EmitResult emit_switch(const ir::Instruction& instr);
  EmitResult emit_call(const ir::Instruction& instr);
  EmitResult emit_indirect_call(const ir::Instruction& instr,
                                const ir::Operand& callee);
  EmitResult emit_ret(const ir::Instruction& instr);
  EmitResult emit_compute(const ir::Instruction& instr);
  EmitResult emit_memory(const ir::Instruction& instr);
  EmitResult emit_elem_offset(const ir::Operand& base,
                              const ir::Operand& index,
                              ir::TypeIdx elem,
                              const ir::Instruction& instr);
  // A field projection over an aggregate: the leading index scales the
  // whole element, a constant field index folds into the offset, and an
  // array walk keeps at most one scaled index.
  EmitResult emit_field_projection(const ir::Operand& base,
                                   ir::TypeIdx elem,
                                   ir::OperandIdxRange indices,
                                   const ir::Instruction& instr);
  // An aggregate field read: fold the indices to an offset and load.
  EmitResult emit_field_load(const ir::Operand& aggregate,
                             ir::OperandIdxRange indices,
                             const ir::Instruction& instr);

  // --- values ---
  [[nodiscard]] std::optional<ValueShape> shape_of(ir::TypeIdx type) const;
  [[nodiscard]] std::optional<ValueShape> shape_of_definition(
      const ir::Instruction& instr) const;
  u32 allocate_local(ValType type);
  EmitResult push_operand(const ir::Operand& operand);
  EmitResult push_word(const ir::Operand& operand, u32 word, ValType type);
  EmitResult push_immutable(const ir::Immutable& immutable, u32 word);
  EmitResult copy_operand(const ir::Operand& operand,
                          u32 dst_base,
                          ValueShape shape);
  void pop_words(u32 base, u32 words);
  void copy_locals(u32 src_base, u32 dst_base, u32 words);
  void set_state(u32 block);
  EmitResult branch_to(ir::BlockIdx target, ir::OperandIdxRange args);
  [[nodiscard]] u32 case_of(ir::BlockIdx block) const;

  // --- strings and data ---
  u32 data_for(std::string_view text);
  u32 data_for(str::StringPoolId id);
  [[nodiscard]] std::string_view string_text(str::StringPoolId id) const;
  u32 newline_offset();

  // --- diagnostics ---
  EmitResult unsupported(diag::Span span, std::string_view what);
  [[nodiscard]] diag::Span span_of(ir::InstructionIdx index) const;

  // --- raw opcodes ---
  void op(u8 byte);
  void i32_const(i32 value);
  void i64_const(i64 value);
  void f32_const(f32 value);
  void f64_const(f64 value);
  void local_get(u32 local);
  void local_set(u32 local);
  void global_get(u32 global);
  void global_set(u32 global);
  void call(u32 function);
  void if_void();
  void else_op();
  void end_op();
  void block_void();
  void loop_void();
  void br(u32 depth);
  void begin_body(u32 param_words);
  void finish_body(u32 function);
  // The length half of the runtime's null guard, for the `(ptr, len)`
  // locals `ptr_local` and `ptr_local + 1`: zero when the pointer is
  // null, the length otherwise.
  void guard_null_len(u32 ptr_local);

  // --- module state ---
  ir::Storage storage_;
  std::span<const diag::Span> spans_;
  str::StringInterner* strings_;
  diag::DiagBag* bag_;
  codegen::Target target_;
  bool emit_entry_;

  ModuleBuilder builder_;
  std::vector<bool> reachable_;
  std::vector<u32> function_index_;  // FunctionIdx -> wasm index, NO_LOCAL
  ir::FunctionIdx entry_ = ir::FunctionIdx::invalid();
  u32 start_index_ = NO_LOCAL;
  std::unordered_map<u32, u32> string_data_;  // StringPoolId offset -> data
  u32 data_end_ = 0;
  u32 scratch_ = 0;
  u32 stack_base_ = 0;
  u32 heap_base_ = 0;
  u32 newline_ = NO_LOCAL;

  // --- per-function state, reset by emit_function ---
  BinaryWriter body_;
  std::vector<ValType> locals_;
  std::vector<u32> reg_base_;
  std::vector<ValueShape> reg_shape_;
  // Aggregate-valued results live in the frame: the register holds the
  // address, and this is its byte offset from the frame base.
  std::vector<u32> reg_slot_;
  u32 frame_bytes_ = 0;
  bool sret_ = false;
  std::vector<ir::TypeIdx> alloca_elem_;
  std::vector<ir::BlockIdx> blocks_;
  std::vector<u32> block_case_;
  u32 param_words_ = 0;
  u32 frame_local_ = NO_LOCAL;
  u32 state_local_ = NO_LOCAL;
  u32 scratch0_ = NO_LOCAL;
  u32 scratch1_ = NO_LOCAL;
  u32 dispatch_depth_ = 0;
  diag::Span current_span_;
  bool failed_ = false;
};

// The whole backend: one module from one request.
[[nodiscard]] base::Result<std::vector<u8>, codegen::EmitError> emit_module(
    codegen::EmitRequest request);

}  // namespace codegen::wasm
