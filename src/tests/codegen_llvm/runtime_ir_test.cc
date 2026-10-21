// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/runtime_ir.h"

#include <string>
#include <string_view>

#include "codegen_llvm/common.h"
#include "codegen_llvm/target.h"
#include "config/build_config.h"
#include "doctest/doctest.h"
#include "ir/type.h"

namespace codegen_llvm {

// The runtime keys its libc bindings on the target's OS and its
// pointer-sized types on the width; a case varies the width.
codegen_llvm::Target target_of(ir::PointerWidth width) {
  return codegen_llvm::Target{codegen_llvm::host_triple(), width};
}

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

// Whether a runtime built for this target carries the panic walk: the
// walk reads a frame layout, so the answer follows the machine rather
// than the host the test runs on.
bool builds_panic_walk(const codegen_llvm::Target& target) {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_walk_test", context);
  const auto usize = llvm::Type::getInt64Ty(context);
  const auto void_ty = llvm::Type::getVoidTy(context);
  const auto pointer = opaque_pointer(context);
  declare(module, "alcy_panic",
          signature(void_ty, {pointer, usize, pointer, usize}));
  add_runtime_definitions(module, target,
                          /*freestanding=*/false, /*backtrace=*/true);
  return module_text(module).find("llvm.frameaddress") != std::string::npos;
}

}  // namespace

TEST_CASE("Runtime defines every entry point") {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_test", context);
  const auto usize = llvm::Type::getInt64Ty(context);
  const auto i32 = llvm::Type::getInt32Ty(context);
  const auto void_ty = llvm::Type::getVoidTy(context);
  const auto pointer = opaque_pointer(context);

  // The program's call sites hold the declarations; the runtime
  // defines only what was declared, so this case declares them all.
  declare(module, "alcy_print", signature(void_ty, {pointer, usize}));
  declare(module, "alcy_println", signature(void_ty, {pointer, usize}));
  declare(module, "alcy_panic",
          signature(void_ty, {pointer, usize, pointer, usize}));
  declare(module, "alcy_sys_write", signature(void_ty, {i32, pointer, usize}));
  declare(module, "alcy_alloc", signature(pointer, {usize, usize}));
  declare(module, "alcy_dealloc", signature(void_ty, {pointer, usize, usize}));

  add_runtime_definitions(module, target_of(ir::PointerWidth::W64));

  CHECK(!llvm::verifyModule(module));

  check_definition(module, "alcy_print", signature(void_ty, {pointer, usize}));
  check_definition(module, "alcy_println",
                   signature(void_ty, {pointer, usize}));
  check_definition(module, "alcy_panic",
                   signature(void_ty, {pointer, usize, pointer, usize}));
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

TEST_CASE("Runtime defines only what the program declares") {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_test", context);
  const auto usize = llvm::Type::getInt64Ty(context);
  const auto void_ty = llvm::Type::getVoidTy(context);
  const auto pointer = opaque_pointer(context);

  llvm::Function* print =
      declare(module, "alcy_print", signature(void_ty, {pointer, usize}));
  add_runtime_definitions(module, target_of(ir::PointerWidth::W64));

  CHECK(!print->isDeclaration());
  // No allocator is declared, so none is defined: a freestanding
  // program that never allocates carries no libc call.
  CHECK(module.getFunction("alcy_alloc") == nullptr);
  CHECK(module.getFunction("alcy_dealloc") == nullptr);
  CHECK(!llvm::verifyModule(module));
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
  declare(module, "alcy_panic",
          signature(void_ty, {pointer, usize, pointer, usize}));

  add_runtime_definitions(module, target_of(ir::PointerWidth::W64));

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

  declare(module, "alcy_print", signature(void_ty, {pointer, usize}));
  declare(module, "alcy_alloc", signature(pointer, {usize, usize}));

  add_runtime_definitions(module, target_of(ir::PointerWidth::W32));

  CHECK(!llvm::verifyModule(module));

  check_definition(module, "alcy_print", signature(void_ty, {pointer, usize}));
  check_definition(module, "alcy_alloc", signature(pointer, {usize, usize}));
}

TEST_CASE("The file helpers answer i64 on a 32-bit target") {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_test", context);
  const auto i64 = llvm::Type::getInt64Ty(context);
  const auto usize32 = llvm::Type::getInt32Ty(context);
  const auto pointer = opaque_pointer(context);

  // The lowering declares these with the intrinsic's own i64 shape,
  // and the answer cannot follow the pointer's width: the size_t a
  // 32-bit fread/fwrite answers is widened inside the runtime.
  declare(module, "alcy_file_size", signature(i64, {pointer, usize32}));
  declare(module, "alcy_read_file_into",
          signature(i64, {pointer, usize32, pointer, usize32}));
  declare(module, "alcy_write_file",
          signature(i64, {pointer, usize32, pointer, usize32}));

  add_runtime_definitions(module, target_of(ir::PointerWidth::W32));

  CHECK(!llvm::verifyModule(module));

  check_definition(module, "alcy_file_size",
                   signature(i64, {pointer, usize32}));
  check_definition(module, "alcy_read_file_into",
                   signature(i64, {pointer, usize32, pointer, usize32}));
  check_definition(module, "alcy_write_file",
                   signature(i64, {pointer, usize32, pointer, usize32}));
}

TEST_CASE("A freestanding runtime calls no libc") {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_test", context);
  const auto usize = llvm::Type::getInt64Ty(context);
  const auto void_ty = llvm::Type::getVoidTy(context);
  const auto pointer = opaque_pointer(context);

  // Every piece a program could use, so the whole runtime is built
  // and any libc reference would show.
  declare(module, "alcy_print", signature(void_ty, {pointer, usize}));
  declare(module, "alcy_println", signature(void_ty, {pointer, usize}));
  declare(module, "alcy_panic",
          signature(void_ty, {pointer, usize, pointer, usize}));
  declare(
      module, "alcy_sys_write",
      signature(void_ty, {llvm::Type::getInt32Ty(context), pointer, usize}));
  declare(module, "alcy_alloc", signature(pointer, {usize, usize}));
  declare(module, "alcy_dealloc", signature(void_ty, {pointer, usize, usize}));

  // The syscall sequence belongs to an architecture, and a wasm test
  // build's host is not one of them; the case names the x86-64 Linux
  // one, which needs no target machine to build IR for.
  const codegen_llvm::Target linux_x64{"x86_64-unknown-linux-gnu",
                                       ir::PointerWidth::W64};
  add_runtime_definitions(module, linux_x64, /*freestanding=*/true);

  CHECK(!llvm::verifyModule(module));
  CHECK(module.getFunction("write") == nullptr);
  CHECK(module.getFunction("posix_memalign") == nullptr);
  CHECK(module.getFunction("free") == nullptr);
  CHECK(module.getFunction("abort") == nullptr);
  // The pieces reach the kernel through the syscall helper instead.
  CHECK(module.getFunction("alcy_syscall") != nullptr);
  const std::string text = module_text(module);
  CHECK(text.find("asm sideeffect") != std::string::npos);
}

TEST_CASE("A failed allocation panics instead of returning null") {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_test", context);
  const auto usize = llvm::Type::getInt64Ty(context);
  const auto pointer = opaque_pointer(context);

  // The program asks for the allocator; a failed allocation has to
  // abort, because every caller writes through the pointer it gets back
  // and safe code has no null to check.
  declare(module, "alcy_alloc", signature(pointer, {usize, usize}));

  add_runtime_definitions(module, target_of(ir::PointerWidth::W64));

  CHECK(!llvm::verifyModule(module));

  // Asking for the allocator asks for the panic path it aborts through.
  llvm::Function* panic = module.getFunction("alcy_panic");
  CHECK(panic != nullptr);
  if (panic != nullptr) {
    CHECK(!panic->isDeclaration());
  }
  const std::string text = module_text(module);
  CHECK(text.find("allocation failed") != std::string::npos);
  CHECK(text.find("call void @alcy_panic") != std::string::npos);
  // The old failure path returned null, which a safe write followed.
  CHECK(text.find("ret ptr null") == std::string::npos);
}

TEST_CASE("A panic writes the site it was handed") {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_test", context);
  const auto usize = llvm::Type::getInt64Ty(context);
  const auto void_ty = llvm::Type::getVoidTy(context);
  const auto pointer = opaque_pointer(context);

  declare(module, "alcy_panic",
          signature(void_ty, {pointer, usize, pointer, usize}));
  add_runtime_definitions(module, target_of(ir::PointerWidth::W64));

  CHECK(!llvm::verifyModule(module));

  const std::string text = module_text(module);
  // The chrome around the message, and the branch that leaves it out
  // when the caller had no site to name.
  CHECK(text.find("c\"panic\\00\"") != std::string::npos);
  CHECK(text.find("c\" at \\00\"") != std::string::npos);
  CHECK(text.find("c\": \\00\"") != std::string::npos);
  CHECK(text.find("has_where") != std::string::npos);
  // No backtrace asked, so no frame walk is built in.
  CHECK(text.find("c\"  at \\00\"") == std::string::npos);
  CHECK(text.find("llvm.frameaddress") == std::string::npos);
}

TEST_CASE("A panic with backtrace walks the frame chain") {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_test", context);
  const auto usize = llvm::Type::getInt64Ty(context);
  const auto void_ty = llvm::Type::getVoidTy(context);
  const auto pointer = opaque_pointer(context);

  // The walk reads one frame layout, so the case names its machine
  // rather than the host's: under a wasm test runner the host triple
  // is not a machine with frames to walk.
  const codegen_llvm::Target target{"x86_64-unknown-linux-gnu",
                                    ir::PointerWidth::W64};
  declare(module, "alcy_panic",
          signature(void_ty, {pointer, usize, pointer, usize}));
  add_runtime_definitions(module, target,
                          /*freestanding=*/false, /*backtrace=*/true);

  CHECK(!llvm::verifyModule(module));

  const std::string text = module_text(module);
  // The walk reads the frame chain and prints one line per named frame.
  CHECK(text.find("llvm.frameaddress") != std::string::npos);
  CHECK(text.find("c\"  at \\00\"") != std::string::npos);
  // A frame with a site prints it after the name.
  CHECK(text.find("c\" (\\00\"") != std::string::npos);
  CHECK(text.find("c\")\\00\"") != std::string::npos);
  // The walk starts from the panic's own frame, so the panic stays a
  // function of its own rather than folding into its caller.
  llvm::Function* panic = module.getFunction("alcy_panic");
  CHECK(panic != nullptr);
  if (panic != nullptr) {
    CHECK(panic->hasFnAttribute(llvm::Attribute::NoInline));
  }

  // AArch64's frame layout is the pair the walk reads, on Apple too,
  // which spells the machine `arm64`.
  CHECK(builds_panic_walk({"arm64-apple-darwin", ir::PointerWidth::W64}));
}

TEST_CASE("A panic without walkable frames carries no walk") {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_test", context);
  const auto usize = llvm::Type::getInt32Ty(context);
  const auto void_ty = llvm::Type::getVoidTy(context);
  const auto pointer = opaque_pointer(context);

  // The walk reads the x86-64 and AArch64 frame layout; anywhere else
  // backtrace on still means the message and its site only.
  const codegen_llvm::Target target{"wasm32-unknown-emscripten",
                                    ir::PointerWidth::W32};
  declare(module, "alcy_panic",
          signature(void_ty, {pointer, usize, pointer, usize}));
  add_runtime_definitions(module, target,
                          /*freestanding=*/false, /*backtrace=*/true);

  CHECK(!llvm::verifyModule(module));

  const std::string text = module_text(module);
  CHECK(text.find("llvm.frameaddress") == std::string::npos);
  CHECK(text.find("c\"  at \\00\"") == std::string::npos);

  // Windows x64 is an x86-64 machine whose frame pointer is an
  // RSP-relative point in the fixed frame by design, so the layout the
  // walk reads is not there and its panic carries the message and its
  // site only.
  CHECK_FALSE(
      builds_panic_walk({"x86_64-pc-windows-msvc", ir::PointerWidth::W64}));
}

TEST_CASE("Runtime reaches libc the way the platform expects") {
  llvm::LLVMContext context;
  llvm::Module module("runtime_ir_test", context);
  const auto usize = llvm::Type::getInt64Ty(context);
  const auto void_ty = llvm::Type::getVoidTy(context);
  const auto pointer = opaque_pointer(context);

  // The allocator, the free path, and the panic path are the pieces
  // that reach libc, so all of them are declared here.
  declare(module, "alcy_alloc", signature(pointer, {usize, usize}));
  declare(module, "alcy_dealloc", signature(void_ty, {pointer, usize, usize}));
  declare(module, "alcy_panic",
          signature(void_ty, {pointer, usize, pointer, usize}));

  add_runtime_definitions(module, target_of(ir::PointerWidth::W64));

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
