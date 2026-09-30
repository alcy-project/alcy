// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/runtime_ir.h"

#include <string_view>

#include "codegen_llvm/common.h"
#include "config/build_config.h"
#include "debug/dcheck.h"
#include "ir/type.h"

namespace codegen_llvm {
namespace {

using Builder =
    llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter>;

// The program runtime: the `alcy_*` functions a program's module
// declares, libc calls and platform split included, built as IR rather
// than compiled per build (see `docs/adr/0023`).
class RuntimeBuilder {
 public:
  RuntimeBuilder(llvm::Module& module, ir::PointerWidth width)
      : module_(module),
        builder_(module.getContext()),
        usize_(width == ir::PointerWidth::W64 ? builder_.getInt64Ty()
                                              : builder_.getInt32Ty()) {}

  void build() {
    empty_text_ = text_constant("");
    newline_text_ = text_constant("\n");
    build_write_all();
    build_writer("alcy_print", /*newline=*/false);
    build_writer("alcy_println", /*newline=*/true);
    build_panic();
    build_sys_write();
    build_alloc();
    build_dealloc();
  }

 private:
  // A null message carries no bytes, so the length must drop with it;
  // passing the caller's length would read past the empty literal.
  struct Guarded {
    llvm::Value* text;
    llvm::Value* len;
  };

  Guarded guard_null(llvm::Value* text, llvm::Value* len) {
    llvm::Value* is_null =
        builder_.CreateICmpEQ(text, null_pointer(), "is_null");
    return {builder_.CreateSelect(is_null, empty_text_, text, "text"),
            builder_.CreateSelect(is_null, llvm::ConstantInt::get(usize_, 0),
                                  len, "len")};
  }

  llvm::Constant* text_constant(std::string_view bytes) {
    return builder_.CreateGlobalString(llvm::StringRef(bytes),
                                       "alcy.runtime.text", 0, &module_);
  }

  // Every entry point is `linkonce_odr`: an object alcy writes carries
  // the whole runtime, so two of them in one image would each define
  // `alcy_*`, and a discardable definition is what lets the linker keep
  // one copy and the optimizer drop what no program calls.
  llvm::Function* runtime(std::string_view name, llvm::FunctionType* type) {
    if (llvm::Function* declared = module_.getFunction(name)) {
      DCHECK_MSG(declared->getFunctionType() == type,
                 "runtime signature changed under the program's declaration");
      DCHECK_MSG(declared->isDeclaration(), "runtime defined twice");
      declared->setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
      return declared;
    }
    return llvm::Function::Create(type, llvm::GlobalValue::LinkOnceODRLinkage,
                                  name, module_);
  }

  llvm::Function* libc(std::string_view name, llvm::FunctionType* type) {
    return llvm::cast<llvm::Function>(
        module_.getOrInsertFunction(name, type).getCallee());
  }

  llvm::Value* fd(i32 number) {
    return llvm::ConstantInt::get(builder_.getInt32Ty(), number);
  }

  llvm::Constant* null_pointer() {
    return llvm::ConstantPointerNull::get(builder_.getPtrTy());
  }

  // One `write`, with the count the C signature asks for, widened to
  // `usize` so the caller compares it as the signed result it is.
  llvm::Value* write_call(llvm::Value* fd_value,
                          llvm::Value* data,
                          llvm::Value* len) {
    llvm::SmallVector<llvm::Value*, 3> args{fd_value, data};
#if BUILD_FLAG(IS_OS_WIN)
    // `_write` takes and returns the narrower count.
    llvm::Function* write_function = libc(
        "_write",
        llvm::FunctionType::get(
            builder_.getInt32Ty(),
            {builder_.getInt32Ty(), builder_.getPtrTy(), builder_.getInt32Ty()},
            false));
    args.push_back(builder_.CreateTrunc(len, builder_.getInt32Ty()));
    llvm::Value* count = builder_.CreateCall(write_function, args, "count");
    if (count->getType() != usize_) {
      count = builder_.CreateSExt(count, usize_, "count.wide");
    }
#else
    llvm::Function* write_function =
        libc("write",
             llvm::FunctionType::get(
                 usize_, {builder_.getInt32Ty(), builder_.getPtrTy(), usize_},
                 false));
    args.push_back(len);
    llvm::Value* count = builder_.CreateCall(write_function, args, "count");
#endif
    return count;
  }

  // Writes the whole buffer, retrying short writes and giving up when
  // the descriptor stops accepting bytes.
  void build_write_all() {
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getVoidTy(),
        {builder_.getInt32Ty(), builder_.getPtrTy(), usize_}, false);
    write_all_ = llvm::Function::Create(
        type, llvm::GlobalValue::InternalLinkage, "alcy_write_all", module_);
    llvm::Value* fd_value = write_all_->getArg(0);
    llvm::Value* data = write_all_->getArg(1);
    llvm::Value* len = write_all_->getArg(2);

    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", write_all_);
    llvm::BasicBlock* loop =
        llvm::BasicBlock::Create(context, "loop", write_all_);
    llvm::BasicBlock* attempt =
        llvm::BasicBlock::Create(context, "attempt", write_all_);
    llvm::BasicBlock* advance =
        llvm::BasicBlock::Create(context, "advance", write_all_);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(context, "done", write_all_);

    builder_.SetInsertPoint(entry);
    builder_.CreateBr(loop);

    builder_.SetInsertPoint(loop);
    llvm::PHINode* written = builder_.CreatePHI(usize_, 2, "written");
    written->addIncoming(llvm::ConstantInt::get(usize_, 0), entry);
    llvm::Value* remaining = builder_.CreateSub(len, written, "remaining");
    llvm::Value* more = builder_.CreateICmpULT(written, len, "more");
    builder_.CreateCondBr(more, attempt, done);

    builder_.SetInsertPoint(attempt);
    llvm::Value* at =
        builder_.CreateGEP(builder_.getInt8Ty(), data, written, "at");
    llvm::Value* count = write_call(fd_value, at, remaining);
    llvm::Value* progress = builder_.CreateICmpSGT(
        count, llvm::ConstantInt::get(usize_, 0), "progress");
    builder_.CreateCondBr(progress, advance, done);

    builder_.SetInsertPoint(advance);
    llvm::Value* next = builder_.CreateAdd(written, count, "next");
    builder_.CreateBr(loop);
    written->addIncoming(next, advance);

    builder_.SetInsertPoint(done);
    builder_.CreateRetVoid();
  }

  // `alcy_print` writes stdout; `alcy_println` adds the newline in a
  // second write, so a failure to flush the text still shows it.
  void build_writer(std::string_view name, bool newline) {
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getVoidTy(), {builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime(name, type);
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(module_.getContext(), "entry", function);

    builder_.SetInsertPoint(entry);
    Guarded text = guard_null(function->getArg(0), function->getArg(1));
    builder_.CreateCall(write_all_, {fd(1), text.text, text.len});
    if (newline) {
      builder_.CreateCall(write_all_, {fd(1), newline_text_,
                                       llvm::ConstantInt::get(usize_, 1)});
    }
    builder_.CreateRetVoid();
  }

  void build_panic() {
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getVoidTy(), {builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime("alcy_panic", type);
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(module_.getContext(), "entry", function);

    builder_.SetInsertPoint(entry);
    Guarded text = guard_null(function->getArg(0), function->getArg(1));
    builder_.CreateCall(write_all_, {fd(2), text.text, text.len});
    builder_.CreateCall(
        libc("abort", llvm::FunctionType::get(builder_.getVoidTy(), false)));
    builder_.CreateRetVoid();
  }

  void build_sys_write() {
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getVoidTy(),
        {builder_.getInt32Ty(), builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime("alcy_sys_write", type);
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(module_.getContext(), "entry", function);

    builder_.SetInsertPoint(entry);
    Guarded text = guard_null(function->getArg(1), function->getArg(2));
    builder_.CreateCall(write_all_, {function->getArg(0), text.text, text.len});
    builder_.CreateRetVoid();
  }

  // The requested alignment must be a power of two. POSIX allocators
  // require at least pointer-sized alignment, so small alignments are
  // promoted before `posix_memalign`. A zero-size request still returns
  // a distinct, freeable pointer so callers can round-trip it.
  void build_alloc() {
    llvm::FunctionType* type =
        llvm::FunctionType::get(builder_.getPtrTy(), {usize_, usize_}, false);
    llvm::Function* function = runtime("alcy_alloc", type);
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* reject =
        llvm::BasicBlock::Create(context, "reject", function);
    llvm::BasicBlock* prepare =
        llvm::BasicBlock::Create(context, "prepare", function);

    llvm::Value* size = function->getArg(0);
    llvm::Value* align = function->getArg(1);

    builder_.SetInsertPoint(entry);
#if !BUILD_FLAG(IS_OS_WIN)
    // `posix_memalign` writes its result here; `_aligned_malloc`
    // returns it directly.
    llvm::AllocaInst* slot =
        builder_.CreateAlloca(builder_.getPtrTy(), nullptr, "slot");
#endif
    llvm::Value* zero = llvm::ConstantInt::get(usize_, 0);
    llvm::Value* one = llvm::ConstantInt::get(usize_, 1);
    llvm::Value* no_align = builder_.CreateICmpEQ(align, zero, "no_align");
    llvm::Value* lower = builder_.CreateSub(align, one, "lower");
    llvm::Value* masked = builder_.CreateAnd(align, lower, "masked");
    llvm::Value* not_power_of_two =
        builder_.CreateICmpNE(masked, zero, "not_power_of_two");
    llvm::Value* invalid =
        builder_.CreateOr(no_align, not_power_of_two, "invalid");
    builder_.CreateCondBr(invalid, reject, prepare);

    builder_.SetInsertPoint(reject);
    builder_.CreateRet(null_pointer());

    builder_.SetInsertPoint(prepare);
#if BUILD_FLAG(IS_OS_WIN)
    // Windows takes the requested alignment as it is.
    llvm::Value* effective = align;
#else
    // POSIX allocators want at least pointer-sized alignment.
    llvm::Value* min_align =
        llvm::ConstantInt::get(usize_, usize_->getBitWidth() / 8);
    llvm::Value* too_small =
        builder_.CreateICmpULT(align, min_align, "too_small");
    llvm::Value* effective =
        builder_.CreateSelect(too_small, min_align, align, "effective");
#endif
    llvm::Value* size_is_zero =
        builder_.CreateICmpEQ(size, zero, "size_is_zero");
    llvm::Value* requested =
        builder_.CreateSelect(size_is_zero, effective, size, "requested");

#if BUILD_FLAG(IS_OS_WIN)
    // `_aligned_malloc` returns null on failure and is released with
    // `_aligned_free`, so there is no result slot to read back.
    llvm::Function* aligned = libc(
        "_aligned_malloc",
        llvm::FunctionType::get(builder_.getPtrTy(), {usize_, usize_}, false));
    builder_.CreateRet(
        builder_.CreateCall(aligned, {requested, align}, "block"));
#else
    builder_.CreateStore(null_pointer(), slot);
    llvm::Function* memalign = libc(
        "posix_memalign",
        llvm::FunctionType::get(builder_.getInt32Ty(),
                                {builder_.getPtrTy(), usize_, usize_}, false));
    llvm::Value* code =
        builder_.CreateCall(memalign, {slot, effective, requested}, "code");
    llvm::Value* failed = builder_.CreateICmpNE(
        code, llvm::ConstantInt::get(builder_.getInt32Ty(), 0), "failed");
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(context, "ok", function);
    builder_.CreateCondBr(failed, reject, ok);

    builder_.SetInsertPoint(ok);
    builder_.CreateRet(builder_.CreateLoad(builder_.getPtrTy(), slot, "block"));
#endif
  }

  void build_dealloc() {
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getVoidTy(), {builder_.getPtrTy(), usize_, usize_}, false);
    llvm::Function* function = runtime("alcy_dealloc", type);
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* release =
        llvm::BasicBlock::Create(context, "release", function);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(context, "done", function);

    llvm::Value* block = function->getArg(0);

    builder_.SetInsertPoint(entry);
    llvm::Value* is_null =
        builder_.CreateICmpEQ(block, null_pointer(), "is_null");
    builder_.CreateCondBr(is_null, done, release);

    builder_.SetInsertPoint(release);
#if BUILD_FLAG(IS_OS_WIN)
    builder_.CreateCall(
        libc("_aligned_free",
             llvm::FunctionType::get(builder_.getVoidTy(),
                                     {builder_.getPtrTy()}, false)),
        {block});
#else
    builder_.CreateCall(
        libc("free", llvm::FunctionType::get(builder_.getVoidTy(),
                                             {builder_.getPtrTy()}, false)),
        {block});
#endif
    builder_.CreateBr(done);

    builder_.SetInsertPoint(done);
    builder_.CreateRetVoid();
  }

  llvm::Module& module_;
  Builder builder_;
  llvm::IntegerType* usize_;
  llvm::Constant* empty_text_ = nullptr;
  llvm::Constant* newline_text_ = nullptr;
  llvm::Function* write_all_ = nullptr;
};

}  // namespace

void add_runtime_definitions(llvm::Module& module, ir::PointerWidth width) {
  RuntimeBuilder(module, width).build();
}

}  // namespace codegen_llvm
