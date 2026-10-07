// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/runtime_ir.h"

#include <string>
#include <string_view>

#include "codegen_llvm/common.h"
#include "codegen_llvm/target.h"
#include "debug/check.h"
#include "debug/dcheck.h"
#include "fpag/base/numeric.h"
#include "ir/type.h"
#include "llvm/IR/ConstantFolder.h"
#include "llvm/IR/InlineAsm.h"

namespace codegen_llvm {
namespace {

using Builder =
    llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter>;

// The program runtime: the `alcy_*` functions a program's module
// declares, libc calls and platform split included, built as IR rather
// than compiled per build (see `docs/adr/0023-program-runtime-in-process.md`).
class RuntimeBuilder {
 public:
  RuntimeBuilder(llvm::Module& module, const Target& target, bool freestanding)
      : module_(module),
        builder_(module.getContext()),
        windows_(target.is_windows()),
        freestanding_(freestanding),
        arch_(target.triple.substr(0, target.triple.find('-'))),
        usize_(target.width == ir::PointerWidth::W64 ? builder_.getInt64Ty()
                                                     : builder_.getInt32Ty()) {}

  void build() {
    empty_text_ = text_constant("");
    newline_text_ = text_constant("\n");
    if (freestanding_) {
      build_syscall();
    }
    // A piece is defined only when the program declares it. A module
    // that never allocates carries no allocator, and a freestanding
    // program that never prints carries no libc call: the declaration
    // is the program's call site, so nothing else can reach it
    // (ADR-0052).
    const bool print = declared("alcy_print");
    const bool println = declared("alcy_println");
    const bool panic = declared("alcy_panic");
    const bool sys_write = declared("alcy_sys_write");
    if (print || println || panic || sys_write) {
      build_write_all();
    }
    if (print) {
      build_writer("alcy_print", /*newline=*/false);
    }
    if (println) {
      build_writer("alcy_println", /*newline=*/true);
    }
    if (panic) {
      build_panic();
    }
    if (sys_write) {
      build_sys_write();
    }
    if (declared("alcy_alloc")) {
      build_alloc();
    }
    if (declared("alcy_dealloc")) {
      build_dealloc();
    }
  }

 private:
  bool declared(std::string_view name) const {
    return module_.getFunction(name) != nullptr;
  }

  // The kernel entry for one architecture: the instruction text and
  // the constraint string that places numbers and arguments. The
  // number rides the register Linux expects it in, and the first
  // argument shares the result register, which the `0` tie says.
  struct SyscallAbi {
    const char* text;
    const char* constraints;
    u64 write;
    u64 exit;
    u64 mmap;
    u64 munmap;
  };

  bool syscall_abi(SyscallAbi& out) const {
    if (arch_ == "x86_64") {
      out = SyscallAbi{
          .text = "syscall",
          .constraints =
              "={rax},0,{rdi},{rsi},{rdx},{r10},{r8},{r9},~{rcx},~{r11},"
              "~{memory}",
          .write = 1,
          .exit = 60,
          .mmap = 9,
          .munmap = 11,
      };
      return true;
    }
    if (arch_ == "aarch64") {
      out = SyscallAbi{
          .text = "svc #0",
          .constraints = "={x0},{x8},0,{x1},{x2},{x3},{x4},{x5},~{memory}",
          .write = 64,
          .exit = 93,
          .mmap = 222,
          .munmap = 215,
      };
      return true;
    }
    if (arch_ == "riscv64") {
      out = SyscallAbi{
          .text = "ecall",
          .constraints = "={a0},{a7},0,{a1},{a2},{a3},{a4},{a5},~{memory}",
          .write = 64,
          .exit = 93,
          .mmap = 222,
          .munmap = 215,
      };
      return true;
    }
    // The pipeline refuses a freestanding target with no sequence, so
    // this is unreachable by construction.
    return false;
  }

  // Builds `alcy_syscall(number, a1..a6) -> isize`, one internal
  // function per module whose body is the target's system-call
  // instruction. Every freestanding piece calls through it.
  void build_syscall() {
    SyscallAbi abi{};
    if (!syscall_abi(abi)) {
      DCHECK_MSG(false, "freestanding runtime on an unsupported target");
      return;
    }
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::FunctionType* type = llvm::FunctionType::get(
        i64, {i64, i64, i64, i64, i64, i64, i64}, false);
    syscall_ = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                      "alcy_syscall", module_);
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(module_.getContext(), "entry", syscall_);
    builder_.SetInsertPoint(entry);
    llvm::InlineAsm* call_as =
        llvm::InlineAsm::get(type, abi.text, abi.constraints,
                             /*hasSideEffects=*/true);
    llvm::SmallVector<llvm::Value*, 7> args;
    for (u32 i = 0; i < 7; ++i) {
      args.push_back(syscall_->getArg(i));
    }
    llvm::Value* result = builder_.CreateCall(call_as, args, "result");
    builder_.CreateRet(result);
  }

  llvm::Value* syscall(llvm::ArrayRef<llvm::Value*> args) {
    DCHECK_MSG(syscall_ != nullptr, "syscall helper not built");
    return builder_.CreateCall(syscall_, args, "syscall");
  }

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
    if (freestanding_) {
      SyscallAbi abi{};
      CHECK_MSG(syscall_abi(abi),
                "freestanding runtime on an unsupported target");
      llvm::Type* i64 = builder_.getInt64Ty();
      llvm::Value* zero = llvm::ConstantInt::get(i64, 0);
      llvm::Value* count = syscall({
          llvm::ConstantInt::get(i64, abi.write),
          builder_.CreateSExt(fd_value, i64),
          builder_.CreatePtrToInt(data, i64),
          builder_.CreateZExtOrTrunc(len, i64),
          zero,
          zero,
          zero,
      });
      // The kernel reports an error as a small negative value; the
      // caller's signed compare reads the bits, and widening keeps
      // them, so the loop stops on an error exactly as it does here.
      return builder_.CreateZExt(count, usize_);
    }
    llvm::SmallVector<llvm::Value*, 3> args{fd_value, data};
    if (windows_) {
      // `_write` takes and returns the narrower count.
      llvm::Function* write_function = libc(
          "_write",
          llvm::FunctionType::get(builder_.getInt32Ty(),
                                  {builder_.getInt32Ty(), builder_.getPtrTy(),
                                   builder_.getInt32Ty()},
                                  false));
      args.push_back(builder_.CreateTrunc(len, builder_.getInt32Ty()));
      llvm::Value* count = builder_.CreateCall(write_function, args, "count");
      if (count->getType() != usize_) {
        count = builder_.CreateSExt(count, usize_, "count.wide");
      }
      return count;
    }
    llvm::Function* write_function =
        libc("write",
             llvm::FunctionType::get(
                 usize_, {builder_.getInt32Ty(), builder_.getPtrTy(), usize_},
                 false));
    args.push_back(len);
    return builder_.CreateCall(write_function, args, "count");
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
    if (freestanding_) {
      // No libc `abort`; exit with the status a hosted abort reports.
      exit_now(134);
      return;
    }
    builder_.CreateCall(
        libc("abort", llvm::FunctionType::get(builder_.getVoidTy(), false)));
    builder_.CreateRetVoid();
  }

  // Ends the program through the exit syscall. Only a freestanding
  // runtime takes this path; a hosted one returns to its crt.
  void exit_now(u64 code) {
    SyscallAbi abi{};
    CHECK_MSG(syscall_abi(abi),
              "freestanding runtime on an unsupported target");
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::Value* zero = llvm::ConstantInt::get(i64, 0);
    syscall({
        llvm::ConstantInt::get(i64, abi.exit),
        llvm::ConstantInt::get(i64, code),
        zero,
        zero,
        zero,
        zero,
        zero,
    });
    builder_.CreateUnreachable();
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
    if (freestanding_) {
      build_alloc_mmap(function);
      return;
    }
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
    // `posix_memalign` writes its result here; `_aligned_malloc` returns
    // it directly, so the slot exists only where it is read.
    llvm::AllocaInst* slot = nullptr;
    if (!windows_) {
      slot = builder_.CreateAlloca(builder_.getPtrTy(), nullptr, "slot");
    }
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
    llvm::Value* effective = align;
    if (!windows_) {
      // POSIX allocators want at least pointer-sized alignment; Windows
      // takes the requested alignment as it is.
      llvm::Value* min_align =
          llvm::ConstantInt::get(usize_, usize_->getBitWidth() / 8);
      llvm::Value* too_small =
          builder_.CreateICmpULT(align, min_align, "too_small");
      effective =
          builder_.CreateSelect(too_small, min_align, align, "effective");
    }
    llvm::Value* size_is_zero =
        builder_.CreateICmpEQ(size, zero, "size_is_zero");
    llvm::Value* requested =
        builder_.CreateSelect(size_is_zero, effective, size, "requested");

    if (windows_) {
      // `_aligned_malloc` returns null on failure, so there is no result
      // slot to read back and nothing to check.
      llvm::Function* aligned = libc(
          "_aligned_malloc", llvm::FunctionType::get(builder_.getPtrTy(),
                                                     {usize_, usize_}, false));
      builder_.CreateRet(
          builder_.CreateCall(aligned, {requested, align}, "block"));
      return;
    }
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
  }

  void build_dealloc() {
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getVoidTy(), {builder_.getPtrTy(), usize_, usize_}, false);
    llvm::Function* function = runtime("alcy_dealloc", type);
    if (freestanding_) {
      build_dealloc_mmap(function);
      return;
    }
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
    builder_.CreateCall(
        libc(windows_ ? "_aligned_free" : "free",
             llvm::FunctionType::get(builder_.getVoidTy(),
                                     {builder_.getPtrTy()}, false)),
        {block});
    builder_.CreateBr(done);

    builder_.SetInsertPoint(done);
    builder_.CreateRetVoid();
  }

  // A freestanding allocator over `mmap`: one anonymous mapping per
  // block, with two words of header before the returned address
  // holding the mapping's base and length, so `munmap` releases
  // exactly what the kernel mapped.
  void build_alloc_mmap(llvm::Function* function) {
    SyscallAbi abi{};
    CHECK_MSG(syscall_abi(abi),
              "freestanding runtime on an unsupported target");
    llvm::LLVMContext& context = module_.getContext();
    llvm::Type* i64 = builder_.getInt64Ty();
    const auto constant = [&](u64 value) {
      return llvm::ConstantInt::get(i64, value);
    };
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* reject =
        llvm::BasicBlock::Create(context, "reject", function);
    llvm::BasicBlock* mapped =
        llvm::BasicBlock::Create(context, "mapped", function);
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(context, "ok", function);

    llvm::Value* size = function->getArg(0);
    llvm::Value* align = function->getArg(1);

    builder_.SetInsertPoint(entry);
    // The alignment contract matches the hosted allocator's: a power
    // of two, at least one word. `align_of` always answers one.
    llvm::Value* no_align =
        builder_.CreateICmpEQ(align, constant(0), "no_align");
    llvm::Value* lower = builder_.CreateSub(align, constant(1), "lower");
    llvm::Value* masked = builder_.CreateAnd(align, lower, "masked");
    llvm::Value* not_power_of_two =
        builder_.CreateICmpNE(masked, constant(0), "not_power_of_two");
    llvm::Value* invalid =
        builder_.CreateOr(no_align, not_power_of_two, "invalid");
    builder_.CreateCondBr(invalid, reject, mapped);

    builder_.SetInsertPoint(reject);
    builder_.CreateRet(null_pointer());

    builder_.SetInsertPoint(mapped);
    llvm::Value* effective =
        builder_.CreateSelect(builder_.CreateICmpULT(align, constant(8)),
                              constant(8), align, "effective");
    llvm::Value* total = builder_.CreateAdd(
        builder_.CreateAdd(size, effective, "room"), constant(16), "total");
    llvm::Value* base = syscall({
        constant(abi.mmap),
        constant(0),
        total,
        constant(3),     // PROT_READ | PROT_WRITE
        constant(0x22),  // MAP_PRIVATE | MAP_ANONYMOUS
        llvm::ConstantInt::getSigned(i64, -1),
        constant(0),
    });
    // A returned address is positive; an error comes back as a small
    // negative value. Zero is treated as failure too: `mmap` never
    // returns it for a live mapping.
    llvm::Value* failed = builder_.CreateICmpSLE(base, constant(0), "failed");
    builder_.CreateCondBr(failed, reject, ok);

    builder_.SetInsertPoint(ok);
    llvm::Value* effective_lower =
        builder_.CreateSub(effective, constant(1), "effective_lower");
    llvm::Value* room = builder_.CreateAdd(base, constant(16), "header_end");
    llvm::Value* rounded = builder_.CreateAdd(room, effective_lower, "rounded");
    llvm::Value* mask = builder_.CreateNot(effective_lower, "mask");
    llvm::Value* raw = builder_.CreateAnd(rounded, mask, "raw");
    llvm::Value* block = builder_.CreateIntToPtr(raw, builder_.getPtrTy());
    llvm::Value* base_slot = builder_.CreateGEP(
        builder_.getInt8Ty(), block, llvm::ConstantInt::getSigned(i64, -16));
    builder_.CreateStore(base, base_slot);
    llvm::Value* total_slot = builder_.CreateGEP(
        builder_.getInt8Ty(), block, llvm::ConstantInt::getSigned(i64, -8));
    builder_.CreateStore(total, total_slot);
    builder_.CreateRet(block);
  }

  void build_dealloc_mmap(llvm::Function* function) {
    SyscallAbi abi{};
    CHECK_MSG(syscall_abi(abi),
              "freestanding runtime on an unsupported target");
    llvm::LLVMContext& context = module_.getContext();
    llvm::Type* i64 = builder_.getInt64Ty();
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
    llvm::Value* base_slot = builder_.CreateGEP(
        builder_.getInt8Ty(), block, llvm::ConstantInt::getSigned(i64, -16));
    llvm::Value* base = builder_.CreateLoad(i64, base_slot, "base");
    llvm::Value* total_slot = builder_.CreateGEP(
        builder_.getInt8Ty(), block, llvm::ConstantInt::getSigned(i64, -8));
    llvm::Value* total = builder_.CreateLoad(i64, total_slot, "total");
    llvm::Value* zero = llvm::ConstantInt::get(i64, 0);
    syscall({
        llvm::ConstantInt::get(i64, abi.munmap),
        base,
        total,
        zero,
        zero,
        zero,
        zero,
    });
    builder_.CreateBr(done);

    builder_.SetInsertPoint(done);
    builder_.CreateRetVoid();
  }

  llvm::Module& module_;
  Builder builder_;
  bool windows_;
  bool freestanding_;
  std::string arch_;
  llvm::IntegerType* usize_;
  llvm::Constant* empty_text_ = nullptr;
  llvm::Constant* newline_text_ = nullptr;
  llvm::Function* write_all_ = nullptr;
  llvm::Function* syscall_ = nullptr;
};

}  // namespace

void add_runtime_definitions(llvm::Module& module,
                             const Target& target,
                             bool freestanding) {
  RuntimeBuilder(module, target, freestanding).build();
}

}  // namespace codegen_llvm
