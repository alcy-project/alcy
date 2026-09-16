// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <memory>

#include "codegen_llvm/declaration.h"
#include "debug/dcheck.h"
#include "fpag/base/numeric.h"
#include "fpag/base/vec.h"
#include "ir/common.h"

namespace codegen_llvm {

// Index-keyed maps from ir entities to emitted LLVM values. Grows alongside
// ir::Storage during emission; each map entry is written exactly once.
class LlvmIrStorage {
 public:
  LlvmIrStorage() = default;

  void resize_functions(usize size) { functions_.resize(size, nullptr); }
  void resize_registers(usize size) { registers_.resize(size, nullptr); }
  void resize_blocks(usize size) { blocks_.resize(size, nullptr); }
  void resize_immutables(usize size) { immutables_.resize(size, nullptr); }
  void resize_external_functions(usize size) {
    external_functions_.resize(size, nullptr);
  }
  void resize_alloca_types(usize size) { alloca_types_.resize(size, nullptr); }

  void add_function(ir::FunctionIdx id, llvm::Function* function) {
    DCHECK_MSG(function, "Function is null");
    functions_[id] = function;
  }

  void add_register(ir::RegisterIdx id, llvm::Value* value) {
    DCHECK_MSG(value, "Value is null");
    registers_[id] = value;
  }

  void add_block(ir::BlockIdx id, llvm::BasicBlock* block) {
    DCHECK_MSG(block, "Block is null");
    blocks_[id] = block;
  }

  void add_immutable(ir::ImmutableIdx id, llvm::Constant* immutable) {
    DCHECK_MSG(immutable, "Immutable is null");
    immutables_[id] = immutable;
  }

  void add_external_function(ir::ExternalFunctionIdx id,
                             llvm::Function* ex_function) {
    DCHECK_MSG(ex_function, "External function is null");
    external_functions_[id] = ex_function;
  }

  // Records the allocated element type for an Alloca destination, used by
  // GetElementPtr which only sees opaque pointers otherwise.
  void add_alloca_type(ir::RegisterIdx id, llvm::Type* type) {
    DCHECK_MSG(type, "Alloca element type is null");
    alloca_types_[id] = type;
  }

  llvm::Function* function(ir::FunctionIdx id) const { return functions_[id]; }
  llvm::Value* register_value(ir::RegisterIdx id) const {
    return registers_[id];
  }
  llvm::BasicBlock* block(ir::BlockIdx id) const { return blocks_[id]; }
  llvm::Constant* immutable(ir::ImmutableIdx id) const {
    return immutables_[id];
  }
  llvm::Function* external_function(ir::ExternalFunctionIdx id) const {
    return external_functions_[id];
  }
  llvm::Type* alloca_type(ir::RegisterIdx id) const {
    return alloca_types_[id];
  }

 private:
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
  base::Vec<llvm::Type*, ir::RegisterIdx, Alloc<llvm::Type*>> alloca_types_;
};

}  // namespace codegen_llvm
