// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "codegen_llvm/declaration.h"
#include "codegen_llvm/llvm_ir_storage.h"
#include "codegen_llvm/target.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/profiler/profiler.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"

namespace llvm {
class Function;
class Module;
}  // namespace llvm

namespace codegen_llvm {

// One program function of the runtime backtrace table the unwinder
// reads: the LLVM function and the source spelling a frame shows
// (ADR-0063).
struct BacktraceName {
  llvm::Function* function = nullptr;
  std::string name;
};

// Fills the table the panic walker dereferences, for a module built with
// backtrace on: every defined function gets an entry, the program's with
// the display name `visible` carries and every other one (the runtime,
// the entry thunk) with a null name, which the walker skips. Called
// after add_runtime_definitions and before optimization.
void define_backtrace_table(llvm::Module& module,
                            const std::vector<BacktraceName>& visible);

class LlvmIrEmitter {
 public:
  using IRBuilder =
      llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter>;

  // `storage` carries its verification proof: the emitter never
  // sees unverified IR. Unreachable opcode and shape cases below
  // rely on this; the only way to build a proof is
  // StorageBuilder::build.
  LlvmIrEmitter(llvm::Module* module,
                ir::VerifiedStorage storage,
                ir::SymbolTable* interner,
                const Target& target,
                bool emit_entry,
                bool freestanding = false,
                debug::Profiler* profiler = nullptr,
                bool backtrace = false);
  ~LlvmIrEmitter() = default;

  LlvmIrEmitter(const LlvmIrEmitter&) = delete;
  LlvmIrEmitter& operator=(const LlvmIrEmitter&) = delete;

  LlvmIrEmitter(LlvmIrEmitter&&) noexcept = default;
  LlvmIrEmitter& operator=(LlvmIrEmitter&&) noexcept = default;

  // Emits every function body. Returns the program functions and the
  // names a runtime backtrace shows for them, empty unless the emitter
  // was built with backtrace on.
  std::vector<BacktraceName> emit() && noexcept;

 private:
  void check_state();

  llvm::Type* type(ir::TypeIdx idx) const;
  llvm::Type* build_type(ir::TypeIdx idx) const;
  llvm::Type* enum_payload_area_type(ir::TypeIdx idx) const;

  void emit_function(llvm::Function* llvm_function, const ir::Function& func);
  void emit_block(const ir::Block& block);
  void emit_instruction(const ir::Instruction& instr);

  // Per-category instruction emission (see llvm_ir_emitter_compute.cc and
  // llvm_ir_emitter_memory.cc).
  void emit_compute(const ir::Instruction& instr);
  void emit_memory(const ir::Instruction& instr);
  void emit_control(const ir::Instruction& instr);

  llvm::Function* create_function(const ir::FunctionMeta& function_meta) const;
  std::string linkable_name(const ir::FunctionMeta& function_meta) const;
  // The source spelling of a function for a backtrace frame: its module
  // path and name, without the signature the linkable name encodes.
  std::string display_name(const ir::FunctionMeta& function_meta) const;

  llvm::Value* resolve_operand_value(const ir::Operand& operand) const;
  llvm::Function* resolve_operand_function(const ir::Operand& operand) const;

  void setup_immutables();
  void setup_external_functions();

  // True for a zero-parameter `main` whose return form maps to an
  // exit code: `()`, `i32`, or a two-variant enum whose first variant
  // holds `()` (see docs/adr/0009-result-option-library-enums.md).
  bool is_entry_candidate(const ir::Function& function) const;
  // Emits the C-ABI `main` wrapper around a renamed user entry.
  void emit_entry(llvm::Function* entry_function, ir::TypeTag ret);
  // Ends a freestanding program through the target's exit syscall.
  // Answers false for a target with no sequence here, which the
  // pipeline refuses before emission.
  bool emit_exit(llvm::Value* code32);

  llvm::Module* module_;
  ir::VerifiedStorage storage_;
  std::unique_ptr<IRBuilder> builder_;
  ir::SymbolTable* interner_;
  ir::PointerWidth width_;
  // The target's triple, kept for the freestanding exit sequence.
  std::string triple_;
  // Where the trace events go, or nothing. The function bodies emit one
  // region each under "emit-fn".
  debug::Profiler* profiler_ = nullptr;
  // Entry synthesis belongs to binaries; a library's `main` stays an
  // ordinary item, wrapped in nothing.
  bool emit_entry_;
  // Whether the entry is `_start`, ending through the exit syscall
  // instead of returning to a C runtime (ADR-0052).
  bool freestanding_ = false;
  // Whether the panic walker gets a table to read; the emitter creates
  // the slot it lands in, and define_backtrace_table fills it once the
  // runtime exists (ADR-0063).
  bool backtrace_ = false;
  std::vector<BacktraceName> visible_functions_;
  LlvmIrStorage values_;
  // LLVM types are immutable once built, and one shape is requested for
  // every register, operand, and signature that names it, so each is
  // constructed once and shared.
  mutable std::vector<llvm::Type*> type_cache_;

  static constexpr usize FUNCTION_ARGS_SOO_SIZE = 8;
};

}  // namespace codegen_llvm
