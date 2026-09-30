// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/runtime_ir.h"

#include <string>
#include <string_view>

#include "codegen_llvm/common.h"
#include "config/build_config.h"
#include "doctest/doctest.h"
#include "ir/type.h"

namespace codegen_llvm {
namespace {

llvm::PointerType* opaque_pointer(llvm::LLVMContext& context) {
  return llvm::PointerType::get(context, 0);
}

llvm::FunctionType* signature(llvm::Type* return_type,
                              llvm::ArrayRef<llvm::Type*> params) {
  return llvm::FunctionType::get(return_type, params, false);
}

// Checks one entry point is defined with the expected signature. The
// call sites the program emitted hold the declaration, so a mismatch
// here would only surface as a verifier error much later. The linkage is
// part of the contract for the same reason: an object alcy writes
// carries the whole runtime, so a second object in one image defines
// these again unless each definition is discardable.
void check_definition(llvm::Module& module,
                      std::string_view name,
                      llvm::FunctionType* expected) {
  llvm::Function* function = module.getFunction(name);
  CHECK(function != nullptr);
  if (function == nullptr) {
    return;
  }
  CHECK(!function->isDeclaration());
  CHECK(function->hasLinkOnceODRLinkage());
  CHECK(function->getFunctionType() == expected);
}

// Declares one runtime function the way the emitter does, so the
// runtime has something to fill in instead of create.
llvm::Function* declare(llvm::Module& module,
                        std::string_view name,
                        llvm::FunctionType* type) {
  return llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, name,
                                module);
}

std::string module_text(const llvm::Module& module) {
  std::string text;
  llvm::raw_string_ostream os(text);
  module.print(os, nullptr);
  return text;
}

}  // namespace

TEST_CASE("Runtime defines every entry point") {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_test", context);
  const auto usize = llvm::Type::getInt64Ty(context);
  const auto i32 = llvm::Type::getInt32Ty(context);
  const auto void_ty = llvm::Type::getVoidTy(context);
  const auto pointer = opaque_pointer(context);

  add_runtime_definitions(module, ir::PointerWidth::W64);

  CHECK(!llvm::verifyModule(module));

  check_definition(module, "alcy_print", signature(void_ty, {pointer, usize}));
  check_definition(module, "alcy_println",
                   signature(void_ty, {pointer, usize}));
  check_definition(module, "alcy_panic", signature(void_ty, {pointer, usize}));
  check_definition(module, "alcy_sys_write",
                   signature(void_ty, {i32, pointer, usize}));
  check_definition(module, "alcy_alloc", signature(pointer, {usize, usize}));
  check_definition(module, "alcy_dealloc",
                   signature(void_ty, {pointer, usize, usize}));

  llvm::Function* write_all = module.getFunction("alcy_write_all");
  CHECK(write_all != nullptr);
  if (write_all != nullptr) {
    CHECK(write_all->hasLocalLinkage());
  }
}

TEST_CASE("Runtime fills in the program's declarations") {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_test", context);
  const auto usize = llvm::Type::getInt64Ty(context);
  const auto void_ty = llvm::Type::getVoidTy(context);
  const auto pointer = opaque_pointer(context);

  llvm::Function* print =
      declare(module, "alcy_print", signature(void_ty, {pointer, usize}));
  llvm::Function* alloc =
      declare(module, "alcy_alloc", signature(pointer, {usize, usize}));

  add_runtime_definitions(module, ir::PointerWidth::W64);

  // The program's call sites hold these functions, so the definitions
  // must land in place rather than in a second symbol, carrying the
  // linkage the runtime defines everything else with.
  CHECK(module.getFunction("alcy_print") == print);
  CHECK(module.getFunction("alcy_alloc") == alloc);
  CHECK(!print->isDeclaration());
  CHECK(!alloc->isDeclaration());
  CHECK(print->hasLinkOnceODRLinkage());
  CHECK(alloc->hasLinkOnceODRLinkage());
  CHECK(!llvm::verifyModule(module));

  const std::string text = module_text(module);
  CHECK(text.find("call void @alcy_write_all") != std::string::npos);
  CHECK(text.find("call void @abort") != std::string::npos);
}

TEST_CASE("Runtime follows the target width") {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_test", context);
  const auto usize = llvm::Type::getInt32Ty(context);
  const auto void_ty = llvm::Type::getVoidTy(context);
  const auto pointer = opaque_pointer(context);

  add_runtime_definitions(module, ir::PointerWidth::W32);

  CHECK(!llvm::verifyModule(module));

  check_definition(module, "alcy_print", signature(void_ty, {pointer, usize}));
  check_definition(module, "alcy_alloc", signature(pointer, {usize, usize}));
}

TEST_CASE("Runtime reaches libc the way the platform expects") {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_test", context);

  add_runtime_definitions(module, ir::PointerWidth::W64);

  CHECK(!llvm::verifyModule(module));

  if (BUILD_FLAG(IS_OS_WIN)) {
    CHECK(module.getFunction("_write") != nullptr);
    CHECK(module.getFunction("_aligned_malloc") != nullptr);
    CHECK(module.getFunction("_aligned_free") != nullptr);
  } else {
    CHECK(module.getFunction("write") != nullptr);
    CHECK(module.getFunction("posix_memalign") != nullptr);
    CHECK(module.getFunction("free") != nullptr);
  }
}

}  // namespace codegen_llvm
