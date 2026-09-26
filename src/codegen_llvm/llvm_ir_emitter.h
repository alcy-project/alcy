// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <string>

#include "codegen_llvm/declaration.h"
#include "codegen_llvm/llvm_ir_storage.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_interner.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/storage.h"
#include "ir/type.h"

namespace codegen_llvm {

class LlvmIrEmitter {
 public:
  using IRBuilder =
      llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter>;

  LlvmIrEmitter(llvm::Module* module,
                ir::Storage&& storage,
                str::StringInterner* interner,
                ir::PointerWidth width);
  ~LlvmIrEmitter() = default;

  LlvmIrEmitter(const LlvmIrEmitter&) = delete;
  LlvmIrEmitter& operator=(const LlvmIrEmitter&) = delete;

  LlvmIrEmitter(LlvmIrEmitter&&) noexcept = default;
  LlvmIrEmitter& operator=(LlvmIrEmitter&&) noexcept = default;

  void emit() && noexcept;

 private:
  void check_state();

  llvm::Type* type(ir::TypeIdx idx) const;
  // The payload half of an enum slot: a byte area on a carrier that
  // carries the alignment ir::type_layout published.
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
  // The linker-visible name of a function.
  std::string linkable_name(const ir::FunctionMeta& function_meta) const;

  llvm::Value* resolve_operand_value(const ir::Operand& operand) const;
  llvm::Function* resolve_operand_function(const ir::Operand& operand) const;

  void setup_immutables();
  void setup_external_functions();

  // True for a zero-parameter `main` whose return form maps to an
  // exit code: `()`, `i32`, or a two-variant enum whose first variant
  // holds `()` (see docs/adr/0009).
  bool is_entry_candidate(const ir::Function& function) const;
  // Emits the C-ABI `main` wrapper around a renamed user entry.
  void emit_entry(llvm::Function* entry_function, ir::TypeTag ret);

  llvm::Module* module_;
  ir::Storage storage_;
  std::unique_ptr<IRBuilder> builder_;
  str::StringInterner* interner_;
  ir::PointerWidth width_;
  LlvmIrStorage values_;

  static constexpr usize kFunctionArgsSooSize = 8;
};

}  // namespace codegen_llvm
