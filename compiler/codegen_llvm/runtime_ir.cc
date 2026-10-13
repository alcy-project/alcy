// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/runtime_ir.h"

#include <optional>
#include <string>
#include <string_view>

#include "codegen_llvm/common.h"
#include "codegen_llvm/target.h"
#include "debug/check.h"
#include "debug/dcheck.h"
#include "fpag/base/numeric.h"
#include "ir/type.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/ConstantFolder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

namespace codegen_llvm {
namespace {

using Builder =
    llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter>;

// The program runtime: the `alcy_*` functions a program's module
// declares, libc calls and platform split included, built as IR rather
// than compiled per build (see `docs/adr/0023-program-runtime-in-process.md`).
class RuntimeBuilder {
 public:
  RuntimeBuilder(llvm::Module& module,
                 const Target& target,
                 bool freestanding,
                 bool backtrace)
      : module_(module),
        builder_(module.getContext()),
        windows_(target.is_windows()),
        freestanding_(freestanding),
        backtrace_(backtrace),
        arch_(target.triple.substr(0, target.triple.find('-'))),
        usize_(target.width == ir::PointerWidth::W64 ? builder_.getInt64Ty()
                                                     : builder_.getInt32Ty()) {}

  void build() {
    empty_text_ = text_constant("");
    newline_text_ = text_constant("\n");
    if (freestanding_) {
      build_syscall();
      // The backend lowers aggregate copies to `memcpy` calls, and a
      // freestanding program has no libc to resolve them (ADR-0052).
      build_memcpy();
    }
    // A piece is defined only when the program declares it. A module
    // that never allocates carries no allocator, and a freestanding
    // program that never prints carries no libc call: the declaration
    // is the program's call site, so nothing else can reach it
    // (ADR-0052).
    const bool print = declared("alcy_print");
    const bool println = declared("alcy_println");
    const bool alloc = declared("alcy_alloc");
    // A checked source operation declares the runtime check it calls,
    // so a declaration of one asks for the panic path too, the way the
    // allocator's failure path does.
    const bool checked =
        declared("alcy_check_overflow") || declared("alcy_check_shift");
    const bool panic = declared("alcy_panic") || alloc || checked;
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
    if (declared("alcy_check_overflow")) {
      build_check("alcy_check_overflow", "integer overflow");
    }
    if (declared("alcy_check_shift")) {
      build_check("alcy_check_shift", "shift out of range");
    }
    if (sys_write) {
      build_sys_write();
    }
    if (alloc) {
      build_alloc();
    }
    if (declared("alcy_dealloc")) {
      build_dealloc();
    }
  }

 private:
  // A panic trace stops after this many frames, and a frame only
  // matches an entry whose start is this close to the return address.
  // The window keeps a libc or runtime address from being blamed on
  // whichever program function happens to sit below it.
  static constexpr u64 MAX_BACKTRACE_FRAMES = 64;
  static constexpr u64 MAX_BACKTRACE_DISTANCE = u64{1} << 24;

  bool declared(std::string_view name) const {
    return module_.getFunction(name) != nullptr;
  }

  // Whether the panic walker can read this target's frames: the walk
  // assumes the frame pointer addresses the caller's frame and the
  // word above it holds the return address, which is the x86-64 and
  // AArch64 layout. Anywhere else the walk stays off and the panic
  // carries its message and site only.
  bool walkable_frames() const {
    return arch_ == "x86_64" || arch_ == "aarch64";
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
    write_all_->setDoesNotThrow();
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

  // A byte loop stands in for the libc routine a freestanding program
  // cannot link: LLVM synthesizes `memcpy` for aggregate copies, so the
  // symbol must exist whatever the program declares (ADR-0052).
  void build_memcpy() {
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getPtrTy(), {builder_.getPtrTy(), builder_.getPtrTy(), usize_},
        false);
    llvm::Function* function = runtime("memcpy", type);
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* loop =
        llvm::BasicBlock::Create(context, "loop", function);
    llvm::BasicBlock* copy =
        llvm::BasicBlock::Create(context, "copy", function);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(context, "done", function);

    llvm::Value* dst = function->getArg(0);
    llvm::Value* src = function->getArg(1);
    llvm::Value* count = function->getArg(2);

    builder_.SetInsertPoint(entry);
    builder_.CreateBr(loop);

    builder_.SetInsertPoint(loop);
    llvm::PHINode* at = builder_.CreatePHI(usize_, 2, "at");
    at->addIncoming(llvm::ConstantInt::get(usize_, 0), entry);
    llvm::Value* more = builder_.CreateICmpULT(at, count, "more");
    builder_.CreateCondBr(more, copy, done);

    builder_.SetInsertPoint(copy);
    llvm::Value* from =
        builder_.CreateGEP(builder_.getInt8Ty(), src, at, "from");
    llvm::Value* byte = builder_.CreateLoad(builder_.getInt8Ty(), from, "byte");
    llvm::Value* to = builder_.CreateGEP(builder_.getInt8Ty(), dst, at, "to");
    builder_.CreateStore(byte, to);
    llvm::Value* next =
        builder_.CreateAdd(at, llvm::ConstantInt::get(usize_, 1), "next");
    builder_.CreateBr(loop);
    at->addIncoming(next, copy);

    builder_.SetInsertPoint(done);
    builder_.CreateRet(dst);
  }

  // `alcy_print` writes stdout; `alcy_println` adds the newline in a
  // second write, so a failure to flush the text still shows it.
  void build_writer(std::string_view name, bool newline) {
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getVoidTy(), {builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime(name, type);
    function->setDoesNotThrow();
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
        builder_.getVoidTy(),
        {builder_.getPtrTy(), usize_, builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime("alcy_panic", type);
    function->setDoesNotReturn();
    function->setDoesNotThrow();
    if (backtrace_) {
      // The walk starts from this frame; inlining it into the caller
      // would start the walk one frame too deep and fold the walk into
      // code the table cannot name back.
      function->addFnAttr(llvm::Attribute::NoInline);
    }
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(module_.getContext(), "entry", function);
    llvm::BasicBlock* located =
        llvm::BasicBlock::Create(module_.getContext(), "located", function);
    llvm::BasicBlock* text =
        llvm::BasicBlock::Create(module_.getContext(), "text", function);
    llvm::BasicBlock* end =
        llvm::BasicBlock::Create(module_.getContext(), "end", function);

    builder_.SetInsertPoint(entry);
    Guarded message = guard_null(function->getArg(0), function->getArg(1));
    Guarded where = guard_null(function->getArg(2), function->getArg(3));
    // `panic at <site>: <message>`, and `panic: <message>` when the call
    // site is unknown (the entry thunk and the allocator's failure have
    // no source position).
    builder_.CreateCall(write_all_, {fd(2), text_constant("panic"),
                                     llvm::ConstantInt::get(usize_, 5)});
    llvm::Value* has_where = builder_.CreateICmpNE(
        where.len, llvm::ConstantInt::get(usize_, 0), "has_where");
    builder_.CreateCondBr(has_where, located, text);

    builder_.SetInsertPoint(located);
    builder_.CreateCall(write_all_, {fd(2), text_constant(" at "),
                                     llvm::ConstantInt::get(usize_, 4)});
    builder_.CreateCall(write_all_, {fd(2), where.text, where.len});
    builder_.CreateBr(text);

    builder_.SetInsertPoint(text);
    builder_.CreateCall(write_all_, {fd(2), text_constant(": "),
                                     llvm::ConstantInt::get(usize_, 2)});
    builder_.CreateCall(write_all_, {fd(2), message.text, message.len});
    // The report ends its line whether or not frames follow: a message
    // glued to whatever prints next reads as a bug of its own.
    builder_.CreateCall(
        write_all_, {fd(2), newline_text_, llvm::ConstantInt::get(usize_, 1)});
    // The walk needs frames it can read; without them the message and
    // its site are the whole report.
    if (backtrace_ && walkable_frames()) {
      build_panic_walk(end);
    } else {
      builder_.CreateBr(end);
    }

    builder_.SetInsertPoint(end);
    if (freestanding_) {
      // No libc `abort`; exit with the status a hosted abort reports.
      exit_now(134);
      return;
    }
    builder_.CreateCall(
        libc("abort", llvm::FunctionType::get(builder_.getVoidTy(), false)));
    builder_.CreateUnreachable();
  }

  // Loads the address table `slot_name` holds, or null when no table
  // was defined. The tables are the emitter's to define; a runtime
  // without one walks nothing.
  llvm::Value* load_entries(std::string_view slot_name) {
    llvm::GlobalVariable* slot = module_.getNamedGlobal(slot_name);
    if (slot == nullptr) {
      return llvm::ConstantPointerNull::get(builder_.getPtrTy());
    }
    return builder_.CreateLoad(builder_.getPtrTy(), slot, "entries");
  }

  // One table lookup: the name and length of the entry with the
  // greatest start at or below `pc` and in range, or null and zero.
  // Builds its own scan loop and leaves the insert point past it. The
  // caller only looks up a table it defined beside the function table,
  // so entries is never null where this runs.
  struct TableMatch {
    llvm::Value* name = nullptr;
    llvm::Value* len = nullptr;
    llvm::BasicBlock* matched = nullptr;
  };

  TableMatch build_table_lookup(llvm::Value* pc, llvm::Value* entries) {
    llvm::LLVMContext& context = module_.getContext();
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::StructType* entry_type =
        llvm::StructType::get(context, {ptr, ptr, usize_});
    llvm::BasicBlock* here = builder_.GetInsertBlock();
    llvm::Function* function = here->getParent();
    llvm::BasicBlock* scan =
        llvm::BasicBlock::Create(context, "scan", function);
    llvm::BasicBlock* advance =
        llvm::BasicBlock::Create(context, "advance", function);
    llvm::BasicBlock* matched =
        llvm::BasicBlock::Create(context, "matched", function);
    llvm::Constant* nil = llvm::ConstantPointerNull::get(builder_.getPtrTy());
    builder_.CreateBr(scan);

    // The scan keeps the match with the greatest start at or below the
    // return address. Entries are not sorted, and an entry whose start
    // is too far below the address (libc, another mapping) is not a
    // match.
    builder_.SetInsertPoint(scan);
    llvm::PHINode* best_start = builder_.CreatePHI(ptr, 2, "best_start");
    llvm::PHINode* best_name = builder_.CreatePHI(ptr, 2, "best_name");
    llvm::PHINode* best_len = builder_.CreatePHI(usize_, 2, "best_len");
    llvm::PHINode* index = builder_.CreatePHI(usize_, 2, "index");
    best_start->addIncoming(nil, here);
    best_name->addIncoming(nil, here);
    best_len->addIncoming(llvm::ConstantInt::get(usize_, 0), here);
    index->addIncoming(llvm::ConstantInt::get(usize_, 0), here);
    llvm::Value* entry =
        builder_.CreateGEP(entry_type, entries, {index}, "entry");
    llvm::Value* start = builder_.CreateLoad(
        ptr, builder_.CreateStructGEP(entry_type, entry, 0), "start");
    llvm::Value* last = builder_.CreateICmpEQ(start, nil, "last");
    builder_.CreateCondBr(last, matched, advance);

    builder_.SetInsertPoint(advance);
    llvm::Value* name = builder_.CreateLoad(
        ptr, builder_.CreateStructGEP(entry_type, entry, 1), "name");
    llvm::Value* len = builder_.CreateLoad(
        usize_, builder_.CreateStructGEP(entry_type, entry, 2), "len");
    llvm::Value* pc_word = builder_.CreatePtrToInt(pc, usize_, "pc_word");
    llvm::Value* start_word =
        builder_.CreatePtrToInt(start, usize_, "start_word");
    llvm::Value* after = builder_.CreateICmpULE(start_word, pc_word, "after");
    llvm::Value* near = builder_.CreateICmpULT(
        builder_.CreateSub(pc_word, start_word),
        llvm::ConstantInt::get(usize_, MAX_BACKTRACE_DISTANCE), "near");
    llvm::Value* candidate = builder_.CreateAnd(after, near, "candidate");
    llvm::Value* closer = builder_.CreateICmpUGT(
        start_word, builder_.CreatePtrToInt(best_start, usize_, "best_word"),
        "closer");
    llvm::Value* take = builder_.CreateAnd(candidate, closer, "take");
    best_start->addIncoming(builder_.CreateSelect(take, start, best_start),
                            advance);
    best_name->addIncoming(builder_.CreateSelect(take, name, best_name),
                           advance);
    best_len->addIncoming(builder_.CreateSelect(take, len, best_len), advance);
    index->addIncoming(
        builder_.CreateAdd(index, llvm::ConstantInt::get(usize_, 1)), advance);
    builder_.CreateBr(scan);

    builder_.SetInsertPoint(matched);
    return {best_name, best_len, matched};
  }

  // Walks the frame chain outward from `alcy_panic` and prints one line
  // per program frame: `  at <name>`, with the call site appended as
  // `  at <name> (<site>)` when the site table names one. The runtime's
  // own frames carry no name and print nothing.
  void build_panic_walk(llvm::BasicBlock* end) {
    llvm::LLVMContext& context = module_.getContext();
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::BasicBlock* from = builder_.GetInsertBlock();
    llvm::Function* function = from->getParent();

    llvm::BasicBlock* walk =
        llvm::BasicBlock::Create(context, "walk", function);
    llvm::BasicBlock* look =
        llvm::BasicBlock::Create(context, "look", function);
    llvm::BasicBlock* site =
        llvm::BasicBlock::Create(context, "site", function);
    llvm::BasicBlock* print =
        llvm::BasicBlock::Create(context, "print", function);
    llvm::BasicBlock* print_site =
        llvm::BasicBlock::Create(context, "print_site", function);
    llvm::BasicBlock* print_end =
        llvm::BasicBlock::Create(context, "print_end", function);
    llvm::BasicBlock* next_frame =
        llvm::BasicBlock::Create(context, "next_frame", function);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(context, "done", function);

    llvm::Value* entries = load_entries("alcy_backtrace_entries");
    llvm::Value* site_entries = load_entries("alcy_callsite_entries");
    llvm::Value* no_entries = builder_.CreateICmpEQ(
        entries, llvm::ConstantPointerNull::get(builder_.getPtrTy()),
        "no_entries");
    // `[fp]` is the caller's frame and `[fp + word]` its return address
    // in the x86-64 and AArch64 layout the walk is enabled for; the
    // chain starts at this frame.
    llvm::Function* frame_address = llvm::Intrinsic::getOrInsertDeclaration(
        &module_, llvm::Intrinsic::frameaddress, {ptr});
    llvm::Value* fp0 =
        builder_.CreateCall(frame_address, {builder_.getInt32(0)}, "frame");
    builder_.CreateCondBr(no_entries, done, walk);

    builder_.SetInsertPoint(walk);
    llvm::PHINode* fp = builder_.CreatePHI(ptr, 2, "fp");
    llvm::PHINode* depth = builder_.CreatePHI(usize_, 2, "depth");
    fp->addIncoming(fp0, from);
    depth->addIncoming(llvm::ConstantInt::get(usize_, 0), from);
    llvm::Value* next_fp = builder_.CreateLoad(ptr, fp, "next_fp");
    llvm::Value* pc_offset =
        llvm::ConstantInt::get(usize_, usize_->getBitWidth() / 8);
    llvm::Value* at_pc =
        builder_.CreateGEP(builder_.getInt8Ty(), fp, {pc_offset}, "at_pc");
    llvm::Value* pc = builder_.CreateLoad(ptr, at_pc, "pc");
    llvm::Value* no_pc = builder_.CreateICmpEQ(
        pc, llvm::ConstantPointerNull::get(builder_.getPtrTy()), "no_pc");
    llvm::Value* too_deep = builder_.CreateICmpUGE(
        depth, llvm::ConstantInt::get(usize_, MAX_BACKTRACE_FRAMES),
        "too_deep");
    // The stack grows down, so a frame that did not move up ends the
    // walk.
    llvm::Value* not_up =
        builder_.CreateICmpULE(builder_.CreatePtrToInt(next_fp, usize_),
                               builder_.CreatePtrToInt(fp, usize_), "not_up");
    llvm::Value* stop = builder_.CreateOr(no_pc, too_deep, "stop");
    stop = builder_.CreateOr(stop, not_up);
    // The depth the next frame runs at, computed here so it dominates
    // the blocks that carry it forward.
    llvm::Value* increased = builder_.CreateAdd(
        depth, llvm::ConstantInt::get(usize_, 1), "increased");
    builder_.CreateCondBr(builder_.CreateNot(stop, "go"), look, done);

    // A frame shows exactly when the function table names it; the name
    // is null for an unmatched address and for the runtime's own
    // frames, and both read the same way.
    builder_.SetInsertPoint(look);
    TableMatch frame = build_table_lookup(pc, entries);
    llvm::Value* show = builder_.CreateICmpNE(
        frame.name, llvm::ConstantPointerNull::get(builder_.getPtrTy()),
        "show");
    builder_.CreateCondBr(show, site, next_frame);

    builder_.SetInsertPoint(site);
    TableMatch at = build_table_lookup(pc, site_entries);
    builder_.CreateBr(print);

    builder_.SetInsertPoint(print);
    builder_.CreateCall(write_all_, {fd(2), text_constant("  at "),
                                     llvm::ConstantInt::get(usize_, 5)});
    builder_.CreateCall(write_all_, {fd(2), frame.name, frame.len});
    llvm::Value* has_site = builder_.CreateICmpNE(
        at.name, llvm::ConstantPointerNull::get(builder_.getPtrTy()),
        "has_site");
    builder_.CreateCondBr(has_site, print_site, print_end);

    builder_.SetInsertPoint(print_site);
    builder_.CreateCall(write_all_, {fd(2), text_constant(" ("),
                                     llvm::ConstantInt::get(usize_, 2)});
    builder_.CreateCall(write_all_, {fd(2), at.name, at.len});
    builder_.CreateCall(write_all_, {fd(2), text_constant(")"),
                                     llvm::ConstantInt::get(usize_, 1)});
    builder_.CreateBr(print_end);

    builder_.SetInsertPoint(print_end);
    builder_.CreateCall(
        write_all_, {fd(2), newline_text_, llvm::ConstantInt::get(usize_, 1)});
    builder_.CreateBr(next_frame);

    builder_.SetInsertPoint(next_frame);
    llvm::PHINode* parent = builder_.CreatePHI(ptr, 2, "parent");
    llvm::PHINode* deeper = builder_.CreatePHI(usize_, 2, "deeper");
    parent->addIncoming(next_fp, frame.matched);
    parent->addIncoming(next_fp, print_end);
    deeper->addIncoming(increased, frame.matched);
    deeper->addIncoming(increased, print_end);
    builder_.CreateBr(walk);
    fp->addIncoming(parent, next_frame);
    depth->addIncoming(deeper, next_frame);

    builder_.SetInsertPoint(done);
    builder_.CreateBr(end);
  }

  // One checked-arithmetic abort: the emitter's call carries the
  // operation's overflow condition and the site it names, a set
  // condition prints the message and takes the panic path, and a clear
  // one returns so the operation proceeds.
  void build_check(std::string_view name, std::string_view message) {
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getVoidTy(),
        {builder_.getInt1Ty(), builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime(name, type);
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(module_.getContext(), "entry", function);
    llvm::BasicBlock* fail =
        llvm::BasicBlock::Create(module_.getContext(), "fail", function);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(module_.getContext(), "done", function);

    builder_.SetInsertPoint(entry);
    builder_.CreateCondBr(function->getArg(0), fail, done);
    builder_.SetInsertPoint(fail);
    llvm::Function* panic_function = module_.getFunction("alcy_panic");
    DCHECK_MSG(panic_function != nullptr, "checked arithmetic has no panic");
    builder_.CreateCall(
        panic_function,
        {text_constant(message), llvm::ConstantInt::get(usize_, message.size()),
         function->getArg(1), function->getArg(2)});
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(done);
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
    function->setDoesNotThrow();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(module_.getContext(), "entry", function);

    builder_.SetInsertPoint(entry);
    Guarded text = guard_null(function->getArg(1), function->getArg(2));
    builder_.CreateCall(write_all_, {function->getArg(0), text.text, text.len});
    builder_.CreateRetVoid();
  }

  // Allocation failure is a defined abort, matching the panic path:
  // every caller writes through the returned pointer and has no null to
  // check.
  void fail_allocation() {
    constexpr std::string_view MESSAGE = "allocation failed";
    llvm::Function* panic_function = module_.getFunction("alcy_panic");
    DCHECK_MSG(panic_function != nullptr, "allocation failure has no panic");
    builder_.CreateCall(
        panic_function,
        {text_constant(MESSAGE), llvm::ConstantInt::get(usize_, MESSAGE.size()),
         empty_text_, llvm::ConstantInt::get(usize_, 0)});
    builder_.CreateUnreachable();
  }

  // The requested alignment must be a power of two. POSIX allocators
  // require at least pointer-sized alignment, so small alignments are
  // promoted before `posix_memalign`. A zero-size request still returns
  // a distinct, freeable pointer so callers can round-trip it.
  void build_alloc() {
    llvm::FunctionType* type =
        llvm::FunctionType::get(builder_.getPtrTy(), {usize_, usize_}, false);
    llvm::Function* function = runtime("alcy_alloc", type);
    // The result is a fresh block or the process ends; saying so lets
    // the optimizer pair an allocation with its deallocation, promote
    // one that never escapes, or drop it.
    function->setDoesNotThrow();
    function->setReturnDoesNotAlias();
    function->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(
        module_.getContext(), 0, std::nullopt));
    function->addFnAttr(llvm::Attribute::getWithAllocKind(
        module_.getContext(),
        llvm::AllocFnKind::Alloc | llvm::AllocFnKind::Uninitialized));
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
    fail_allocation();

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
      // `_aligned_malloc` returns null on failure, so the result is
      // checked here rather than read back from a slot.
      llvm::Function* aligned = libc(
          "_aligned_malloc", llvm::FunctionType::get(builder_.getPtrTy(),
                                                     {usize_, usize_}, false));
      llvm::Value* block =
          builder_.CreateCall(aligned, {requested, align}, "block");
      llvm::Value* failed =
          builder_.CreateICmpEQ(block, null_pointer(), "failed");
      llvm::BasicBlock* ok = llvm::BasicBlock::Create(context, "ok", function);
      builder_.CreateCondBr(failed, reject, ok);
      builder_.SetInsertPoint(ok);
      builder_.CreateRet(block);
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
    function->setDoesNotThrow();
    function->addFnAttr(llvm::Attribute::getWithAllocKind(
        module_.getContext(), llvm::AllocFnKind::Free));
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
    fail_allocation();

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
  // Whether `alcy_panic` walks the frame chain before it aborts.
  bool backtrace_;
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
                             bool freestanding,
                             bool backtrace) {
  RuntimeBuilder(module, target, freestanding, backtrace).build();
}

}  // namespace codegen_llvm
