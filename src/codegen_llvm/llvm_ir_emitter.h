// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <memory>

#include "codegen_llvm/declaration.h"
#include "fpag/base/numeric.h"
#include "fpag/base/vec.h"
#include "fpag/str/string_interner.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/storage.h"

namespace codegen_llvm {

class LlvmIrEmitter {
 public:
  using IRBuilder =
      llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter>;
  // using ValueIdx = base::Idx<llvm::Value, base::IdxBaseType>;

  LlvmIrEmitter(llvm::Module* module,
                ir::Storage&& storage,
                str::StringInterner* interner);
  ~LlvmIrEmitter() = default;

  LlvmIrEmitter(const LlvmIrEmitter&) = delete;
  LlvmIrEmitter& operator=(const LlvmIrEmitter&) = delete;

  LlvmIrEmitter(LlvmIrEmitter&&) noexcept = default;
  LlvmIrEmitter& operator=(LlvmIrEmitter&&) noexcept = default;

  void emit() && noexcept;

 private:
  void init_value_map();

  void check_state();

  llvm::Type* type(ir::Type type) const;

  void emit_function(llvm::Function* llvm_function, const ir::Function& func);
  void emit_block(const ir::Block& block);
  void emit_instruction(const ir::Instruction& instr);

  llvm::Function* create_function(const ir::FunctionMeta& function_meta) const;

  llvm::Value* resolve_operand_value(const ir::Operand& operand) const;
  llvm::Function* resolve_operand_function(const ir::Operand& operand) const;
  void add_function(ir::FunctionIdx id, llvm::Function* function);
  void add_register(ir::RegisterIdx id, llvm::Value* value);
  void add_block(ir::BlockIdx id, llvm::BasicBlock* block);
  void add_immutable(ir::ImmutableIdx id, llvm::Constant* immutable);
  void add_external_function(ir::ExternalFunctionIdx id,
                             llvm::Function* ex_function);

  void setup_immutables();
  void setup_external_functions();

  llvm::Module* module_;
  ir::Storage storage_;
  std::unique_ptr<IRBuilder> builder_;
  str::StringInterner* interner_;

  // TODO: Move these fields to LlvmIrStorage.
  template <typename T>
  using Alloc = std::allocator<T>;

  base::Vec<llvm::Function*, ir::FunctionIdx, Alloc<llvm::Function*>>
      functions_;
  base::Vec<llvm::Value*, ir::RegisterIdx, Alloc<llvm::Value*>> registers_;
  base::Vec<llvm::BasicBlock*, ir::BlockIdx, Alloc<llvm::BasicBlock*>> blocks_;
  base::Vec<llvm::Constant*, ir::ImmutableIdx, Alloc<llvm::Constant*>>
      immutables_;
  base::Vec<llvm::Function*, ir::ExternalFunctionIdx, Alloc<llvm::Function*>>
      external_functions_;

  static constexpr usize kFunctionArgsSooSize = 8;
};

}  // namespace codegen_llvm
