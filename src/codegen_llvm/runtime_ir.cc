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
// than compiled per build (see).
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
        comdat_(llvm::Triple(target.triple).isOSBinFormatCOFF()),
        mach_o_(llvm::Triple(target.triple).isOSBinFormatMachO()),
        arch_(target.triple.substr(0, target.triple.find('-'))),
        usize_(target.width == ir::PointerWidth::W64 ? builder_.getInt64Ty()
                                                     : builder_.getInt32Ty()) {}

  void build() {
    empty_text_ = text_constant("");
    newline_text_ = text_constant("\n");
    if (freestanding_ || windows_) {
      // The code generator lowers an aggregate copy and a fill it will
      // not inline to `memcpy` and `memset` calls; no libc answers
      // for either when the kernel is the only library.
      if (freestanding_ && !windows_) {
        build_syscall();
      }
      build_memcpy();
      build_memset();
    }
    // A piece is defined only when the program declares it. A module
    // that never allocates carries no allocator, and a freestanding
    // program that never prints carries no libc call: the declaration
    // is the program's call site, so nothing else can reach it
    const bool print = declared("alcy_print");
    const bool println = declared("alcy_println");
    const bool eprint = declared("alcy_eprint");
    const bool eprintln = declared("alcy_eprintln");
    const bool alloc = declared("alcy_alloc");
    // A checked source operation declares the runtime check it calls,
    // so a declaration of one asks for the panic path too, the way the
    // allocator's failure path does.
    const bool checked =
        declared("alcy_check_overflow") || declared("alcy_check_shift");
    const bool panic = declared("alcy_panic") || alloc || checked;
    const bool sys_write = declared("alcy_sys_write");
    const bool sys_read = declared("alcy_sys_read");
    const bool error_kind = declared("alcy_error_kind");
    if (print || println || eprint || eprintln || panic) {
      build_write_all();
    }
    if (print) {
      build_writer("alcy_print", /*newline=*/false, /*out_fd=*/1);
    }
    if (println) {
      build_writer("alcy_println", /*newline=*/true, /*out_fd=*/1);
    }
    if (eprint) {
      build_writer("alcy_eprint", /*newline=*/false, /*out_fd=*/2);
    }
    if (eprintln) {
      build_writer("alcy_eprintln", /*newline=*/true, /*out_fd=*/2);
    }
    // The entry wrapper stores `argc` and `argv` here, so the
    // globals exist whether or not a count is asked for. The two
    // globals are discardable for the same reason the entry points are:
    // every object carries them, and one copy answers.
    {
      llvm::GlobalVariable* argc =
          module_.getOrInsertGlobal("alcy_argc", builder_.getInt32Ty());
      if (!argc->hasInitializer()) {
        argc->setInitializer(llvm::ConstantInt::get(builder_.getInt32Ty(), 0));
      }
      link_once_odr(*argc);
      llvm::GlobalVariable* argv =
          module_.getOrInsertGlobal("alcy_argv", builder_.getPtrTy());
      if (!argv->hasInitializer()) {
        argv->setInitializer(
            llvm::ConstantPointerNull::get(builder_.getPtrTy()));
      }
      link_once_odr(*argv);
    }
    {
      // The entry wrapper stores the platform's environment table
      // here, whatever shape it has there, so the environment answers
      // whether or not anyone asked for it.
      llvm::GlobalVariable* environGlobal =
          module_.getOrInsertGlobal("alcy_environ", builder_.getPtrTy());
      if (!environGlobal->hasInitializer()) {
        environGlobal->setInitializer(
            llvm::ConstantPointerNull::get(builder_.getPtrTy()));
      }
      link_once_odr(*environGlobal);
    }
    if (windows_) {
      // The scratch the environment answer encodes into, and the one
      // the name rides through: both hold one value at a time and are
      // never borrowed past the call.
      llvm::GlobalVariable* env_buf =
          cast<llvm::GlobalVariable>(module_.getOrInsertGlobal(
              "alcy_env_buf",
              llvm::ArrayType::get(builder_.getInt8Ty(), 262144)));
      env_buf->setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
      if (!env_buf->hasInitializer()) {
        env_buf->setInitializer(
            llvm::ConstantAggregateZero::get(env_buf->getValueType()));
      }
      llvm::GlobalVariable* env_name =
          cast<llvm::GlobalVariable>(module_.getOrInsertGlobal(
              "alcy_env_name",
              llvm::ArrayType::get(builder_.getInt16Ty(), 32768)));
      env_name->setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
      if (!env_name->hasInitializer()) {
        env_name->setInitializer(
            llvm::ConstantAggregateZero::get(env_name->getValueType()));
      }
      // The marker the C runtime defines for a module that does
      // floating-point code; no CRT answers for it now.
      llvm::GlobalVariable* fltused = cast<llvm::GlobalVariable>(
          module_.getOrInsertGlobal("_fltused", builder_.getInt32Ty()));
      if (!fltused->hasInitializer()) {
        fltused->setInitializer(
            llvm::ConstantInt::get(builder_.getInt32Ty(), 0));
      }
      link_once_odr(*fltused);
      build_chkstk();
      build_args_init();
    }
    if (declared("alcy_arg_count")) {
      build_arg_count();
    }
    if (declared("alcy_arg")) {
      build_arg();
    }
    // The environment lookups are defined when something asked for them.
    build_environ();
    if (freestanding_ && !windows_) {
      if (arch_ == "x86_64" || arch_ == "aarch64" || arch_ == "riscv64") {
        if (declared("alcy_file_size")) {
          build_file_size_syscall();
        }
        if (declared("alcy_read_file_into")) {
          build_read_file_into_syscall();
        }
        if (declared("alcy_write_file")) {
          build_write_file_syscall();
        }
      } else {
        // No libc to open a file with; the std still links.
        build_file_stubs();
      }
    } else {
      if (declared("alcy_file_size")) {
        build_file_size();
      }
      if (declared("alcy_read_file_into")) {
        build_read_file_into();
      }
      if (declared("alcy_write_file")) {
        build_write_file();
      }
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
    if (sys_read) {
      build_sys_read();
    }
    if (error_kind) {
      build_error_kind();
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
  // word above it holds the return address, which is the x86-64 SysV
  // and AArch64 layout (Apple spells the latter `arm64`). Windows x64
  // keeps its frame pointer at an RSP-relative point in the fixed frame
  // by design, which is not a chain, so its panic carries its message
  // and site only.
  bool walkable_frames() const {
    if (windows_) {
      return false;
    }
    return arch_ == "x86_64" || arch_ == "aarch64" || arch_ == "arm64";
  }

  // The kernel entry for one architecture: the instruction text and
  // the constraint string that places numbers and arguments. The
  // number rides the register Linux expects it in, and the first
  // argument shares the result register, which the `0` tie says.
  struct SyscallAbi {
    const char* text;
    const char* constraints;
    u64 read;
    u64 write;
    u64 exit;
    u64 mmap;
    u64 munmap;
    u64 openat;
    u64 close;
    u64 lseek;
  };

  bool syscall_abi(SyscallAbi& out) const {
    if (arch_ == "x86_64") {
      out = SyscallAbi{
          .text = "syscall",
          .constraints =
              "={rax},0,{rdi},{rsi},{rdx},{r10},{r8},{r9},~{rcx},~{r11},"
              "~{memory}",
          .read = 0,
          .write = 1,
          .exit = 60,
          .mmap = 9,
          .munmap = 11,
          .openat = 257,
          .close = 3,
          .lseek = 8,
      };
      return true;
    }
    if (arch_ == "aarch64") {
      out = SyscallAbi{
          .text = "svc #0",
          .constraints = "={x0},{x8},0,{x1},{x2},{x3},{x4},{x5},~{memory}",
          .read = 63,
          .write = 64,
          .exit = 93,
          .mmap = 222,
          .munmap = 215,
          .openat = 56,
          .close = 57,
          .lseek = 62,
      };
      return true;
    }
    if (arch_ == "riscv64") {
      out = SyscallAbi{
          .text = "ecall",
          .constraints = "={a0},{a7},0,{a1},{a2},{a3},{a4},{a5},~{memory}",
          .read = 63,
          .write = 64,
          .exit = 93,
          .mmap = 222,
          .munmap = 215,
          .openat = 56,
          .close = 57,
          .lseek = 62,
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
  // one copy and the optimizer drop what no program calls. On COFF a
  // discardable definition also needs a comdat: the object format
  // resolves a weak external through a `.weak.<name>.default.<tag>`
  // copy whose tag names this object's first caller, so two objects of
  // one image conflict (LNK1227) unless the comdat marks them the same
  // definition. Mach-O rejects comdats, and the other formats dedup
  // the plain linkage by symbol.
  void link_once_odr(llvm::GlobalObject& value) {
    value.setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
    if (comdat_) {
      llvm::Comdat* comdat = module_.getOrInsertComdat(value.getName());
      comdat->setSelectionKind(llvm::Comdat::Any);
      value.setComdat(comdat);
    }
  }

  llvm::Function* runtime(std::string_view name, llvm::FunctionType* type) {
    if (llvm::Function* declared = module_.getFunction(name)) {
      DCHECK_MSG(declared->getFunctionType() == type,
                 "runtime signature changed under the program's declaration");
      DCHECK_MSG(declared->isDeclaration(), "runtime defined twice");
      link_once_odr(*declared);
      return declared;
    }
    llvm::Function* function = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage, name, module_);
    link_once_odr(*function);
    return function;
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

  // The kernel32 handle behind a descriptor number: 0 reads stdin,
  // 1 writes stdout, 2 writes stderr, anything else is stderr.
  llvm::Value* std_handle(llvm::Value* fd_value) {
    llvm::Type* i32 = builder_.getInt32Ty();
    llvm::Function* get =
        libc("GetStdHandle",
             llvm::FunctionType::get(builder_.getPtrTy(), {i32}, false));
    llvm::Value* zero = llvm::ConstantInt::get(i32, 0);
    llvm::Value* two = llvm::ConstantInt::get(i32, 2);
    llvm::Value* is_in = builder_.CreateICmpEQ(fd_value, zero, "is_in");
    llvm::Value* is_err = builder_.CreateICmpEQ(fd_value, two, "is_err");
    // STD_INPUT_HANDLE (DWORD)-10, STD_OUTPUT_HANDLE (DWORD)-11,
    // STD_ERROR_HANDLE (DWORD)-12.
    llvm::Value* id = builder_.CreateSelect(
        is_in, llvm::ConstantInt::get(i32, -10, true),
        builder_.CreateSelect(is_err, llvm::ConstantInt::get(i32, -12, true),
                              llvm::ConstantInt::get(i32, -11, true)),
        "id");
    return builder_.CreateCall(get, {id}, "handle");
  }

  // The handle CreateFileW answers with for a path it cannot open, as
  // an integer to compare against.
  llvm::Value* invalid_handle(llvm::Value* handle) {
    llvm::Type* i64 = builder_.getInt64Ty();
    return builder_.CreateICmpEQ(
        builder_.CreatePtrToInt(handle, i64, "handle_int"),
        llvm::ConstantInt::get(i64, 0xffffffffffffffffULL), "invalid");
  }

  // The UTF-16 spelling of a byte path in a slot with the terminator
  // the kernel wants, or null when the bytes do not decode as UTF-8 or
  // the path is too long for a stack slot. The slot does not outlive
  // the call, and the brackets it spans are the caller's to own.
  llvm::Value* wide_path(llvm::Value* data, llvm::Value* len) {
    llvm::Type* i32 = builder_.getInt32Ty();
    llvm::Type* i16 = builder_.getInt16Ty();
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::Function* to_wide = libc(
        "MultiByteToWideChar",
        llvm::FunctionType::get(
            i32, {i32, i32, builder_.getPtrTy(), i32, builder_.getPtrTy(), i32},
            false));
    llvm::LLVMContext& context = module_.getContext();
    llvm::Function* caller = builder_.GetInsertBlock()->getParent();
    llvm::BasicBlock* fail =
        llvm::BasicBlock::Create(context, "path_fail", caller);
    llvm::BasicBlock* measure =
        llvm::BasicBlock::Create(context, "path_measure", caller);
    llvm::BasicBlock* convert =
        llvm::BasicBlock::Create(context, "path_convert", caller);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(context, "path_done", caller);
    llvm::Value* zero32 = llvm::ConstantInt::get(i32, 0);
    llvm::Value* data_len = builder_.CreateZExtOrTrunc(len, i32, "path_len");
    llvm::Value* too_long = builder_.CreateICmpUGT(
        len, llvm::ConstantInt::get(usize_, 0x8000), "too_long");
    builder_.CreateCondBr(too_long, fail, measure);
    builder_.SetInsertPoint(fail);
    builder_.CreateBr(done);
    builder_.SetInsertPoint(measure);
    llvm::Value* units = builder_.CreateCall(
        to_wide,
        {llvm::ConstantInt::get(i32, 65001 /* CP_UTF8 */), zero32, data,
         data_len, llvm::ConstantPointerNull::get(builder_.getPtrTy()), zero32},
        "units");
    llvm::Value* empty = builder_.CreateICmpSLE(units, zero32, "empty");
    builder_.CreateCondBr(empty, fail, convert);
    builder_.SetInsertPoint(convert);
    // A static buffer: the path never lives past the call, and the
    // frame stays under a page, so no stack probe is asked for.
    llvm::GlobalVariable* buffer =
        cast<llvm::GlobalVariable>(module_.getOrInsertGlobal(
            "alcy_wide_path", llvm::ArrayType::get(i16, 32768)));
    buffer->setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
    if (!buffer->hasInitializer()) {
      buffer->setInitializer(
          llvm::ConstantAggregateZero::get(buffer->getValueType()));
    }
    llvm::Value* written =
        builder_.CreateCall(to_wide,
                            {llvm::ConstantInt::get(i32, 65001 /* CP_UTF8 */),
                             zero32, data, data_len, buffer, units},
                            "written");
    llvm::Value* at = builder_.CreateGEP(
        i16, buffer, builder_.CreateZExt(written, i64), "term");
    builder_.CreateStore(llvm::ConstantInt::get(i16, 0), at);
    builder_.CreateBr(done);
    builder_.SetInsertPoint(done);
    llvm::PHINode* result = builder_.CreatePHI(builder_.getPtrTy(), 2, "path");
    result->addIncoming(llvm::ConstantPointerNull::get(builder_.getPtrTy()),
                        fail);
    result->addIncoming(buffer, convert);
    return result;
  }

  // The kernel32 handle for a descriptor, with the kernel's own answer
  // for a failed call left in the thread-local slot. One operation per
  // call: a caller that needs the count re-reads it.
  llvm::Value* descriptor_io(bool is_write,
                             llvm::Value* fd_value,
                             llvm::Value* data,
                             llvm::Value* len) {
    llvm::Type* i32 = builder_.getInt32Ty();
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::LLVMContext& context = module_.getContext();
    llvm::Function* caller = builder_.GetInsertBlock()->getParent();
    llvm::BasicBlock* valid =
        llvm::BasicBlock::Create(context, "valid", caller);
    llvm::BasicBlock* rest = llvm::BasicBlock::Create(context, "rest", caller);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(context, "done", caller);
    // Only the three descriptors exist on this platform; any other
    // number is a descriptor that was never opened.
    llvm::Value* is_zero = builder_.CreateICmpEQ(
        fd_value, llvm::ConstantInt::get(i32, 0), "is_zero");
    llvm::Value* is_one = builder_.CreateICmpEQ(
        fd_value, llvm::ConstantInt::get(i32, 1), "is_one");
    llvm::Value* is_two = builder_.CreateICmpEQ(
        fd_value, llvm::ConstantInt::get(i32, 2), "is_two");
    llvm::Value* known = builder_.CreateOr(
        is_zero, builder_.CreateOr(is_one, is_two, "is_one_or_two"), "known");
    builder_.CreateCondBr(known, valid, rest);
    builder_.SetInsertPoint(rest);
    // EBADF, as a descriptor that was never opened, in the same
    // thread-local slot a failed kernel call leaves behind.
    llvm::Function* set_last_error =
        libc("SetLastError",
             llvm::FunctionType::get(builder_.getVoidTy(), {i32}, false));
    builder_.CreateCall(set_last_error, {llvm::ConstantInt::get(i32, 9)});
    builder_.CreateBr(done);
    builder_.SetInsertPoint(valid);
    llvm::Value* handle = std_handle(fd_value);
    llvm::Value* cap = llvm::ConstantInt::get(usize_, 0xffffffff);
    llvm::Value* clamped = builder_.CreateSelect(
        builder_.CreateICmpUGT(len, cap, "clamp"), cap, len, "clamped");
    llvm::Value* chunk32 = builder_.CreateTrunc(clamped, i32, "chunk");
    llvm::AllocaInst* slot = builder_.CreateAlloca(i32, nullptr, "count_slot");
    builder_.CreateStore(llvm::ConstantInt::get(i32, 0), slot);
    llvm::Function* call = libc(
        is_write ? "WriteFile" : "ReadFile",
        llvm::FunctionType::get(i32,
                                {builder_.getPtrTy(), builder_.getPtrTy(), i32,
                                 builder_.getPtrTy(), builder_.getPtrTy()},
                                false));
    llvm::Value* ok = builder_.CreateCall(
        call,
        {handle, data, chunk32, slot,
         llvm::ConstantPointerNull::get(builder_.getPtrTy())},
        "ok");
    llvm::Value* count = builder_.CreateLoad(i32, slot, "count");
    // A failed call answers `-1`; the thread-local slot keeps the code.
    llvm::Value* failed =
        builder_.CreateICmpEQ(ok, llvm::ConstantInt::get(i32, 0), "failed");
    llvm::Value* wide = builder_.CreateZExt(count, i64, "wide");
    llvm::Value* answer = builder_.CreateSelect(
        failed, llvm::ConstantInt::get(i64, 0xffffffffffffffffULL), wide,
        "answer");
    builder_.CreateBr(done);
    builder_.SetInsertPoint(done);
    llvm::PHINode* joined = builder_.CreatePHI(i64, 2, "joined");
    joined->addIncoming(llvm::ConstantInt::get(i64, 0xffffffffffffffffULL),
                        rest);
    joined->addIncoming(answer, valid);
    return joined;
  }

  // One `write`, with the count the C signature asks for, widened to
  // `usize` so the caller compares it as the signed result it is.
  llvm::Value* write_call(llvm::Value* fd_value,
                          llvm::Value* data,
                          llvm::Value* len) {
    if (freestanding_ && !windows_) {
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
      return builder_.CreateZExtOrTrunc(count, usize_);
    }
    llvm::SmallVector<llvm::Value*, 3> args{fd_value, data};
    if (windows_) {
      llvm::Value* count =
          descriptor_io(/*is_write=*/true, fd_value, data, len);
      if (count->getType() != usize_) {
        count = builder_.CreateSExtOrTrunc(count, usize_, "count.wide");
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
  // symbol must exist whatever the program declares.
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

  // The fill half of the same stand-in: the byte comes from the caller,
  // so a fill the backend keeps as a loop still stores the value it was
  // handed rather than a constant.
  void build_memset() {
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getPtrTy(),
        {builder_.getPtrTy(), builder_.getInt8Ty(), usize_}, false);
    llvm::Function* function = runtime("memset", type);
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* loop =
        llvm::BasicBlock::Create(context, "loop", function);
    llvm::BasicBlock* fill =
        llvm::BasicBlock::Create(context, "fill", function);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(context, "done", function);

    llvm::Value* dst = function->getArg(0);
    llvm::Value* value = function->getArg(1);
    llvm::Value* count = function->getArg(2);

    builder_.SetInsertPoint(entry);
    builder_.CreateBr(loop);

    builder_.SetInsertPoint(loop);
    llvm::PHINode* at = builder_.CreatePHI(usize_, 2, "at");
    at->addIncoming(llvm::ConstantInt::get(usize_, 0), entry);
    llvm::Value* more = builder_.CreateICmpULT(at, count, "more");
    builder_.CreateCondBr(more, fill, done);

    builder_.SetInsertPoint(fill);
    llvm::Value* to = builder_.CreateGEP(builder_.getInt8Ty(), dst, at, "to");
    builder_.CreateStore(value, to);
    llvm::Value* next =
        builder_.CreateAdd(at, llvm::ConstantInt::get(usize_, 1), "next");
    builder_.CreateBr(loop);
    at->addIncoming(next, fill);

    builder_.SetInsertPoint(done);
    builder_.CreateRet(dst);
  }

  // `alcy_print` writes stdout; `alcy_println` adds the newline in a
  // second write, so a failure to flush the text still shows it.
  void build_writer(std::string_view name, bool newline, i32 out_fd) {
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getVoidTy(), {builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime(name, type);
    function->setDoesNotThrow();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(module_.getContext(), "entry", function);

    builder_.SetInsertPoint(entry);
    Guarded text = guard_null(function->getArg(0), function->getArg(1));
    builder_.CreateCall(write_all_, {fd(out_fd), text.text, text.len});
    if (newline) {
      builder_.CreateCall(write_all_, {fd(out_fd), newline_text_,
                                       llvm::ConstantInt::get(usize_, 1)});
    }
    builder_.CreateRetVoid();
  }

  // `alcy_arg_count()` answers `argc` widened to i64. The globals are
  // the entry wrapper's stores; a freestanding program is never given
  // any arguments.
  void build_arg_count() {
    llvm::FunctionType* type =
        llvm::FunctionType::get(builder_.getInt64Ty(), false);
    llvm::Function* function = runtime("alcy_arg_count", type);
    function->setDoesNotThrow();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(module_.getContext(), "entry", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* count = builder_.CreateLoad(
        builder_.getInt32Ty(),
        module_.getOrInsertGlobal("alcy_argc", builder_.getInt32Ty()), "argc");
    builder_.CreateRet(
        builder_.CreateSExt(count, builder_.getInt64Ty(), "wide"));
  }
  // `alcy_arg(i)` answers the i-th argument as a `str` view, or an empty
  // view when `i` is out of range. The bytes belong to the process.
  void build_arg() {
    llvm::StructType* view = llvm::StructType::get(
        module_.getContext(), {builder_.getPtrTy(), usize_});
    llvm::FunctionType* type =
        llvm::FunctionType::get(view, {builder_.getInt64Ty()}, false);
    llvm::Function* function = runtime("alcy_arg", type);
    function->setDoesNotThrow();
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    builder_.SetInsertPoint(entry);
    llvm::BasicBlock* empty =
        llvm::BasicBlock::Create(context, "empty", function);
    llvm::BasicBlock* have =
        llvm::BasicBlock::Create(context, "have", function);
    llvm::BasicBlock* scan =
        llvm::BasicBlock::Create(context, "scan", function);
    llvm::BasicBlock* scan_done =
        llvm::BasicBlock::Create(context, "scan_done", function);
    llvm::Value* index = function->getArg(0);
    llvm::Value* count = builder_.CreateLoad(
        builder_.getInt32Ty(),
        module_.getOrInsertGlobal("alcy_argc", builder_.getInt32Ty()), "argc");
    llvm::Value* wide =
        builder_.CreateSExt(count, builder_.getInt64Ty(), "wide");
    llvm::Value* below = builder_.CreateICmpSGE(
        index, llvm::ConstantInt::get(builder_.getInt64Ty(), 0), "below");
    llvm::Value* inside = builder_.CreateICmpSLT(index, wide, "inside");
    builder_.CreateCondBr(builder_.CreateAnd(below, inside, "ok"), have, empty);
    builder_.SetInsertPoint(empty);
    builder_.CreateRet(llvm::ConstantAggregateZero::get(view));
    builder_.SetInsertPoint(have);
    llvm::Value* argv = builder_.CreateLoad(
        builder_.getPtrTy(),
        module_.getOrInsertGlobal("alcy_argv", builder_.getPtrTy()), "argv");
    llvm::Value* slot =
        builder_.CreateGEP(builder_.getPtrTy(), argv, index, "slot");
    llvm::Value* text = builder_.CreateLoad(builder_.getPtrTy(), slot, "text");
    builder_.CreateBr(scan);
    builder_.SetInsertPoint(scan);
    llvm::PHINode* at = builder_.CreatePHI(usize_, 2, "at");
    llvm::PHINode* length = builder_.CreatePHI(usize_, 2, "length");
    at->addIncoming(llvm::ConstantInt::get(usize_, 0), have);
    length->addIncoming(llvm::ConstantInt::get(usize_, 0), have);
    llvm::Value* byte = builder_.CreateLoad(
        builder_.getInt8Ty(),
        builder_.CreateGEP(builder_.getInt8Ty(), text, at), "byte");
    llvm::Value* is_zero = builder_.CreateICmpEQ(
        byte, llvm::ConstantInt::get(builder_.getInt8Ty(), 0), "zero");
    llvm::Value* next_at =
        builder_.CreateAdd(at, llvm::ConstantInt::get(usize_, 1), "next_at");
    llvm::Value* next_count = builder_.CreateAdd(
        length, llvm::ConstantInt::get(usize_, 1), "next_count");
    at->addIncoming(next_at, scan);
    length->addIncoming(next_count, scan);
    builder_.CreateCondBr(is_zero, scan_done, scan);
    builder_.SetInsertPoint(scan_done);
    llvm::Value* len = length;
    llvm::Value* with_ptr =
        builder_.CreateInsertValue(llvm::UndefValue::get(view), text, 0);
    builder_.CreateRet(builder_.CreateInsertValue(with_ptr, len, 1));
  }
  // A `str` path is not NUL-terminated, so it is copied into a slot
  // with the terminator `fopen` wants.
  llvm::Value* nul_terminated_path(llvm::Value* data, llvm::Value* len) {
    llvm::Value* bytes =
        builder_.CreateAdd(len, llvm::ConstantInt::get(usize_, 1), "path_len");
    llvm::AllocaInst* buf =
        builder_.CreateAlloca(builder_.getInt8Ty(), bytes, "path");
    builder_.CreateMemCpy(buf, llvm::MaybeAlign(1), data, llvm::MaybeAlign(1),
                          len);
    builder_.CreateStore(llvm::ConstantInt::get(builder_.getInt8Ty(), 0),
                         builder_.CreateGEP(builder_.getInt8Ty(), buf, len));
    return buf;
  }

  // `alcy_file_size(path, len)` answers the byte count or -1.
  void build_file_size() {
    if (windows_) {
      build_file_size_kernel32();
      return;
    }
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getInt64Ty(), {builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime("alcy_file_size", type);
    function->setDoesNotThrow();
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* fail =
        llvm::BasicBlock::Create(context, "fail", function);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(context, "done", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* path =
        nul_terminated_path(function->getArg(0), function->getArg(1));
    llvm::Function* open_fn =
        libc("fopen", llvm::FunctionType::get(
                          builder_.getPtrTy(),
                          {builder_.getPtrTy(), builder_.getPtrTy()}, false));
    llvm::Value* file =
        builder_.CreateCall(open_fn, {path, text_constant("rb")}, "file");
    llvm::Value* missing = builder_.CreateICmpEQ(
        file, llvm::ConstantPointerNull::get(builder_.getPtrTy()), "missing");
    builder_.CreateCondBr(missing, fail, done);
    builder_.SetInsertPoint(fail);
    builder_.CreateRet(llvm::ConstantInt::get(builder_.getInt64Ty(), -1));
    builder_.SetInsertPoint(done);
    llvm::Function* seek_fn = libc(
        "fseek",
        llvm::FunctionType::get(
            builder_.getInt32Ty(),
            {builder_.getPtrTy(), builder_.getInt64Ty(), builder_.getInt32Ty()},
            false));
    builder_.CreateCall(seek_fn,
                        {file, llvm::ConstantInt::get(builder_.getInt64Ty(), 0),
                         llvm::ConstantInt::get(builder_.getInt32Ty(), 2)});
    llvm::Function* tell_fn =
        libc("ftell", llvm::FunctionType::get(builder_.getInt64Ty(),
                                              {builder_.getPtrTy()}, false));
    llvm::Value* size = builder_.CreateCall(tell_fn, {file}, "size");
    llvm::Function* close_fn =
        libc("fclose", llvm::FunctionType::get(builder_.getInt32Ty(),
                                               {builder_.getPtrTy()}, false));
    builder_.CreateCall(close_fn, {file});
    builder_.CreateRet(size);
  }

  // `alcy_read_file_into(path, len, out, out_len)` answers the bytes
  // read or -1 when the path cannot be opened.
  void build_read_file_into() {
    if (windows_) {
      build_read_file_into_kernel32();
      return;
    }
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getInt64Ty(),
        {builder_.getPtrTy(), usize_, builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime("alcy_read_file_into", type);
    function->setDoesNotThrow();
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* fail =
        llvm::BasicBlock::Create(context, "fail", function);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(context, "done", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* path =
        nul_terminated_path(function->getArg(0), function->getArg(1));
    llvm::Function* open_fn =
        libc("fopen", llvm::FunctionType::get(
                          builder_.getPtrTy(),
                          {builder_.getPtrTy(), builder_.getPtrTy()}, false));
    llvm::Value* file =
        builder_.CreateCall(open_fn, {path, text_constant("rb")}, "file");
    llvm::Value* missing = builder_.CreateICmpEQ(
        file, llvm::ConstantPointerNull::get(builder_.getPtrTy()), "missing");
    builder_.CreateCondBr(missing, fail, done);
    builder_.SetInsertPoint(fail);
    builder_.CreateRet(llvm::ConstantInt::get(builder_.getInt64Ty(), -1));
    builder_.SetInsertPoint(done);
    llvm::Function* read_fn = libc(
        "fread",
        llvm::FunctionType::get(
            usize_, {builder_.getPtrTy(), usize_, usize_, builder_.getPtrTy()},
            false));
    llvm::Value* count = builder_.CreateCall(
        read_fn,
        {function->getArg(2), llvm::ConstantInt::get(usize_, 1),
         function->getArg(3), file},
        "count");
    llvm::Function* close_fn =
        libc("fclose", llvm::FunctionType::get(builder_.getInt32Ty(),
                                               {builder_.getPtrTy()}, false));
    builder_.CreateCall(close_fn, {file});
    // The count is a `usize`, which a 32-bit target answers in 32 bits;
    // the helper answers `i64` on every target.
    llvm::Value* widened = count;
    if (count->getType() != builder_.getInt64Ty()) {
      widened = builder_.CreateZExt(count, builder_.getInt64Ty(), "wide");
    }
    builder_.CreateRet(widened);
  }

  // `alcy_write_file(path, len, data, data_len)` answers the bytes
  // written or -1 when the path cannot be opened.
  void build_write_file() {
    if (windows_) {
      build_write_file_kernel32();
      return;
    }
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getInt64Ty(),
        {builder_.getPtrTy(), usize_, builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime("alcy_write_file", type);
    function->setDoesNotThrow();
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* fail =
        llvm::BasicBlock::Create(context, "fail", function);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(context, "done", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* path =
        nul_terminated_path(function->getArg(0), function->getArg(1));
    llvm::Function* open_fn =
        libc("fopen", llvm::FunctionType::get(
                          builder_.getPtrTy(),
                          {builder_.getPtrTy(), builder_.getPtrTy()}, false));
    llvm::Value* file =
        builder_.CreateCall(open_fn, {path, text_constant("wb")}, "file");
    llvm::Value* missing = builder_.CreateICmpEQ(
        file, llvm::ConstantPointerNull::get(builder_.getPtrTy()), "missing");
    builder_.CreateCondBr(missing, fail, done);
    builder_.SetInsertPoint(fail);
    builder_.CreateRet(llvm::ConstantInt::get(builder_.getInt64Ty(), -1));
    builder_.SetInsertPoint(done);
    llvm::Function* write_fn = libc(
        "fwrite",
        llvm::FunctionType::get(
            usize_, {builder_.getPtrTy(), usize_, usize_, builder_.getPtrTy()},
            false));
    llvm::Value* count = builder_.CreateCall(
        write_fn,
        {function->getArg(2), llvm::ConstantInt::get(usize_, 1),
         function->getArg(3), file},
        "count");
    llvm::Function* close_fn =
        libc("fclose", llvm::FunctionType::get(builder_.getInt32Ty(),
                                               {builder_.getPtrTy()}, false));
    builder_.CreateCall(close_fn, {file});
    // The count is a `usize`, which a 32-bit target answers in 32 bits;
    // the helper answers `i64` on every target.
    llvm::Value* widened = count;
    if (count->getType() != builder_.getInt64Ty()) {
      widened = builder_.CreateZExt(count, builder_.getInt64Ty(), "wide");
    }
    builder_.CreateRet(widened);
  }
  // `alcy_file_size(path, len)` answers the byte count or -1.
  void build_file_size_kernel32() {
    llvm::Type* i32 = builder_.getInt32Ty();
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::FunctionType* type =
        llvm::FunctionType::get(i64, {builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime("alcy_file_size", type);
    function->setDoesNotThrow();
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* missing =
        llvm::BasicBlock::Create(context, "missing", function);
    llvm::BasicBlock* opened =
        llvm::BasicBlock::Create(context, "opened", function);
    llvm::BasicBlock* sized =
        llvm::BasicBlock::Create(context, "sized", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* path = wide_path(function->getArg(0), function->getArg(1));
    llvm::Value* absent = builder_.CreateICmpEQ(
        path, llvm::ConstantPointerNull::get(builder_.getPtrTy()), "absent");
    builder_.CreateCondBr(absent, missing, opened);
    builder_.SetInsertPoint(missing);
    builder_.CreateRet(llvm::ConstantInt::get(i64, -1));
    builder_.SetInsertPoint(opened);
    llvm::Function* open_fn = libc(
        "CreateFileW", llvm::FunctionType::get(
                           builder_.getPtrTy(),
                           {builder_.getPtrTy(), i32, i32, builder_.getPtrTy(),
                            i32, i32, builder_.getPtrTy()},
                           false));
    // GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
    // FILE_ATTRIBUTE_NORMAL.
    llvm::Value* handle = builder_.CreateCall(
        open_fn,
        {path, llvm::ConstantInt::get(i32, static_cast<u32>(0x80000000)),
         llvm::ConstantInt::get(i32, 1),
         llvm::ConstantPointerNull::get(builder_.getPtrTy()),
         llvm::ConstantInt::get(i32, 3), llvm::ConstantInt::get(i32, 0x80),
         llvm::ConstantPointerNull::get(builder_.getPtrTy())},
        "handle");
    llvm::Value* invalid = invalid_handle(handle);
    llvm::BasicBlock* size_block =
        llvm::BasicBlock::Create(context, "size", function);
    builder_.CreateCondBr(invalid, missing, size_block);
    builder_.SetInsertPoint(size_block);
    llvm::AllocaInst* slot = builder_.CreateAlloca(i64, nullptr, "size_slot");
    builder_.CreateStore(llvm::ConstantInt::get(i64, 0), slot);
    llvm::Function* size_fn =
        libc("GetFileSizeEx",
             llvm::FunctionType::get(
                 i32, {builder_.getPtrTy(), builder_.getPtrTy()}, false));
    llvm::Value* ok = builder_.CreateCall(size_fn, {handle, slot}, "ok");
    llvm::Function* close_fn =
        libc("CloseHandle",
             llvm::FunctionType::get(i32, {builder_.getPtrTy()}, false));
    builder_.CreateCall(close_fn, {handle});
    llvm::Value* size = builder_.CreateLoad(i64, slot, "size");
    llvm::Value* zero32 = llvm::ConstantInt::get(i32, 0);
    llvm::Value* failed = builder_.CreateICmpEQ(ok, zero32, "failed");
    builder_.CreateCondBr(failed, missing, sized);
    builder_.SetInsertPoint(sized);
    builder_.CreateRet(size);
  }

  // `alcy_read_file_into(path, len, out, out_len)` answers the bytes
  // read or -1 when the path cannot be opened.
  void build_read_file_into_kernel32() {
    llvm::Type* i32 = builder_.getInt32Ty();
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::FunctionType* type = llvm::FunctionType::get(
        i64, {builder_.getPtrTy(), usize_, builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime("alcy_read_file_into", type);
    function->setDoesNotThrow();
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* missing =
        llvm::BasicBlock::Create(context, "missing", function);
    llvm::BasicBlock* opened =
        llvm::BasicBlock::Create(context, "opened", function);
    llvm::BasicBlock* read =
        llvm::BasicBlock::Create(context, "read", function);
    llvm::BasicBlock* progress =
        llvm::BasicBlock::Create(context, "progress", function);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(context, "done", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* path = wide_path(function->getArg(0), function->getArg(1));
    llvm::Value* absent = builder_.CreateICmpEQ(
        path, llvm::ConstantPointerNull::get(builder_.getPtrTy()), "absent");
    builder_.CreateCondBr(absent, missing, opened);
    builder_.SetInsertPoint(missing);
    builder_.CreateRet(llvm::ConstantInt::get(i64, -1));
    builder_.SetInsertPoint(opened);
    llvm::Function* open_fn = libc(
        "CreateFileW", llvm::FunctionType::get(
                           builder_.getPtrTy(),
                           {builder_.getPtrTy(), i32, i32, builder_.getPtrTy(),
                            i32, i32, builder_.getPtrTy()},
                           false));
    llvm::Value* handle = builder_.CreateCall(
        open_fn,
        {path, llvm::ConstantInt::get(i32, static_cast<u32>(0x80000000)),
         llvm::ConstantInt::get(i32, 1),
         llvm::ConstantPointerNull::get(builder_.getPtrTy()),
         llvm::ConstantInt::get(i32, 3), llvm::ConstantInt::get(i32, 0x80),
         llvm::ConstantPointerNull::get(builder_.getPtrTy())},
        "handle");
    llvm::Value* invalid = invalid_handle(handle);
    llvm::BasicBlock* loop =
        llvm::BasicBlock::Create(context, "loop", function);
    builder_.CreateCondBr(invalid, missing, loop);
    builder_.SetInsertPoint(loop);
    llvm::PHINode* total = builder_.CreatePHI(usize_, 2, "total");
    llvm::PHINode* into = builder_.CreatePHI(builder_.getPtrTy(), 2, "into");
    llvm::PHINode* left = builder_.CreatePHI(usize_, 2, "left");
    total->addIncoming(llvm::ConstantInt::get(usize_, 0), opened);
    into->addIncoming(function->getArg(2), opened);
    left->addIncoming(function->getArg(3), opened);
    llvm::Value* more =
        builder_.CreateICmpUGT(left, llvm::ConstantInt::get(usize_, 0), "more");
    builder_.CreateCondBr(more, read, done);
    builder_.SetInsertPoint(read);
    llvm::Value* cap = llvm::ConstantInt::get(usize_, 0xffffffff);
    llvm::Value* clamped = builder_.CreateSelect(
        builder_.CreateICmpUGT(left, cap, "clamp"), cap, left, "clamped");
    llvm::Value* chunk32 = builder_.CreateTrunc(clamped, i32, "chunk");
    llvm::AllocaInst* slot = builder_.CreateAlloca(i32, nullptr, "count_slot");
    builder_.CreateStore(llvm::ConstantInt::get(i32, 0), slot);
    llvm::Function* read_fn = libc(
        "ReadFile",
        llvm::FunctionType::get(i32,
                                {builder_.getPtrTy(), builder_.getPtrTy(), i32,
                                 builder_.getPtrTy(), builder_.getPtrTy()},
                                false));
    llvm::Value* ok = builder_.CreateCall(
        read_fn,
        {handle, into, chunk32, slot,
         llvm::ConstantPointerNull::get(builder_.getPtrTy())},
        "ok");
    llvm::Value* count32 = builder_.CreateLoad(i32, slot, "count");
    llvm::Value* zero32 = llvm::ConstantInt::get(i32, 0);
    llvm::Value* call_stopped =
        builder_.CreateICmpEQ(ok, zero32, "call_stopped");
    llvm::Value* no_bytes = builder_.CreateICmpEQ(count32, zero32, "no_bytes");
    llvm::Value* stopped = builder_.CreateOr(call_stopped, no_bytes, "stopped");
    builder_.CreateCondBr(stopped, done, progress);
    builder_.SetInsertPoint(progress);
    llvm::Value* advance =
        builder_.CreateZExtOrTrunc(count32, usize_, "advance");
    llvm::Value* next_total = builder_.CreateAdd(total, advance, "next_total");
    llvm::Value* added =
        builder_.CreateGEP(builder_.getInt8Ty(), into, advance, "added");
    llvm::Value* next_left = builder_.CreateSub(left, advance, "next_left");
    total->addIncoming(next_total, progress);
    into->addIncoming(added, progress);
    left->addIncoming(next_left, progress);
    builder_.CreateBr(loop);
    builder_.SetInsertPoint(done);
    llvm::Function* close_fn =
        libc("CloseHandle",
             llvm::FunctionType::get(i32, {builder_.getPtrTy()}, false));
    builder_.CreateCall(close_fn, {handle});
    builder_.CreateRet(builder_.CreateZExtOrTrunc(total, i64, "widened"));
  }

  // `alcy_write_file(path, len, data, data_len)` answers the bytes
  // written or -1 when the path cannot be opened.
  void build_write_file_kernel32() {
    llvm::Type* i32 = builder_.getInt32Ty();
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::FunctionType* type = llvm::FunctionType::get(
        i64, {builder_.getPtrTy(), usize_, builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime("alcy_write_file", type);
    function->setDoesNotThrow();
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* missing =
        llvm::BasicBlock::Create(context, "missing", function);
    llvm::BasicBlock* opened =
        llvm::BasicBlock::Create(context, "opened", function);
    llvm::BasicBlock* loop =
        llvm::BasicBlock::Create(context, "loop", function);
    llvm::BasicBlock* write =
        llvm::BasicBlock::Create(context, "write", function);
    llvm::BasicBlock* progress =
        llvm::BasicBlock::Create(context, "progress", function);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(context, "done", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* path = wide_path(function->getArg(0), function->getArg(1));
    llvm::Value* absent = builder_.CreateICmpEQ(
        path, llvm::ConstantPointerNull::get(builder_.getPtrTy()), "absent");
    builder_.CreateCondBr(absent, missing, opened);
    builder_.SetInsertPoint(missing);
    builder_.CreateRet(llvm::ConstantInt::get(i64, -1));
    builder_.SetInsertPoint(opened);
    llvm::Function* open_fn = libc(
        "CreateFileW", llvm::FunctionType::get(
                           builder_.getPtrTy(),
                           {builder_.getPtrTy(), i32, i32, builder_.getPtrTy(),
                            i32, i32, builder_.getPtrTy()},
                           false));
    // GENERIC_WRITE, no share mode, CREATE_ALWAYS,
    // FILE_ATTRIBUTE_NORMAL.
    llvm::Value* handle = builder_.CreateCall(
        open_fn,
        {path, llvm::ConstantInt::get(i32, 0x40000000),
         llvm::ConstantInt::get(i32, 0),
         llvm::ConstantPointerNull::get(builder_.getPtrTy()),
         llvm::ConstantInt::get(i32, 2), llvm::ConstantInt::get(i32, 0x80),
         llvm::ConstantPointerNull::get(builder_.getPtrTy())},
        "handle");
    llvm::Value* invalid = invalid_handle(handle);
    builder_.CreateCondBr(invalid, missing, loop);
    builder_.SetInsertPoint(loop);
    llvm::PHINode* total = builder_.CreatePHI(usize_, 2, "total");
    llvm::PHINode* from = builder_.CreatePHI(builder_.getPtrTy(), 2, "from");
    llvm::PHINode* left = builder_.CreatePHI(usize_, 2, "left");
    total->addIncoming(llvm::ConstantInt::get(usize_, 0), opened);
    from->addIncoming(function->getArg(2), opened);
    left->addIncoming(function->getArg(3), opened);
    llvm::Value* more =
        builder_.CreateICmpUGT(left, llvm::ConstantInt::get(usize_, 0), "more");
    builder_.CreateCondBr(more, write, done);
    builder_.SetInsertPoint(write);
    llvm::Value* cap = llvm::ConstantInt::get(usize_, 0xffffffff);
    llvm::Value* clamped = builder_.CreateSelect(
        builder_.CreateICmpUGT(left, cap, "clamp"), cap, left, "clamped");
    llvm::Value* chunk32 = builder_.CreateTrunc(clamped, i32, "chunk");
    llvm::AllocaInst* slot = builder_.CreateAlloca(i32, nullptr, "count_slot");
    builder_.CreateStore(llvm::ConstantInt::get(i32, 0), slot);
    llvm::Function* write_fn = libc(
        "WriteFile",
        llvm::FunctionType::get(i32,
                                {builder_.getPtrTy(), builder_.getPtrTy(), i32,
                                 builder_.getPtrTy(), builder_.getPtrTy()},
                                false));
    llvm::Value* ok = builder_.CreateCall(
        write_fn,
        {handle, from, chunk32, slot,
         llvm::ConstantPointerNull::get(builder_.getPtrTy())},
        "ok");
    llvm::Value* count32 = builder_.CreateLoad(i32, slot, "count");
    llvm::Value* zero32 = llvm::ConstantInt::get(i32, 0);
    llvm::Value* call_stopped =
        builder_.CreateICmpEQ(ok, zero32, "call_stopped");
    llvm::Value* no_bytes = builder_.CreateICmpEQ(count32, zero32, "no_bytes");
    llvm::Value* stopped = builder_.CreateOr(call_stopped, no_bytes, "stopped");
    builder_.CreateCondBr(stopped, done, progress);
    builder_.SetInsertPoint(progress);
    llvm::Value* advance =
        builder_.CreateZExtOrTrunc(count32, usize_, "advance");
    llvm::Value* next_total = builder_.CreateAdd(total, advance, "next_total");
    llvm::Value* added =
        builder_.CreateGEP(builder_.getInt8Ty(), from, advance, "added");
    llvm::Value* next_left = builder_.CreateSub(left, advance, "next_left");
    total->addIncoming(next_total, progress);
    from->addIncoming(added, progress);
    left->addIncoming(next_left, progress);
    builder_.CreateBr(loop);
    builder_.SetInsertPoint(done);
    llvm::Function* close_fn =
        libc("CloseHandle",
             llvm::FunctionType::get(i32, {builder_.getPtrTy()}, false));
    builder_.CreateCall(close_fn, {handle});
    builder_.CreateRet(builder_.CreateZExtOrTrunc(total, i64, "widened"));
  }

  // `__chkstk`, the stack prober the CRT used to provide: every page
  // between the current stack pointer and the next one is touched,
  // so a frame larger than a page faults the guard page in instead of
  // jumping over it. `rax` carries the frame in; the routine is
  // varargs-shaped, keeps `rax` and `rcx`, and never adjusts `rsp`.
  // Sequence after the canonical one in compiler-rt's chkstk.S.
  void build_chkstk() {
    llvm::FunctionType* type =
        llvm::FunctionType::get(builder_.getVoidTy(), false);
    llvm::Function* function = llvm::Function::Create(
        type, llvm::GlobalValue::LinkOnceODRLinkage, "__chkstk", module_);
    function->addFnAttr(llvm::Attribute::Naked);
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(module_.getContext(), "", function);
    builder_.SetInsertPoint(entry);
    const char* sequence = nullptr;
    if (arch_ == "x86_64") {
      sequence =
          "push %rcx\n"
          "push %rax\n"
          "cmp $$4096, %rax\n"
          "lea 24(%rsp), %rcx\n"
          "jb 2f\n"
          "1:\n"
          "sub $$4096, %rcx\n"
          "test %rcx, (%rcx)\n"
          "sub $$4096, %rax\n"
          "cmp $$4096, %rax\n"
          "ja 1b\n"
          "2:\n"
          "sub %rax, %rcx\n"
          "test %rcx, (%rcx)\n"
          "pop %rax\n"
          "pop %rcx\n"
          "ret";
    } else if (arch_ == "aarch64") {
      // The frame arrives in x15 in units of 16 bytes, like the C
      // runtime's; x16 and x17 are the call-clobbered helper pair.
      sequence =
          "lsl x16, x15, #4\n"
          "mov x17, sp\n"
          "1:\n"
          "sub x17, x17, #4096\n"
          "subs x16, x16, #4096\n"
          "ldr xzr, [x17]\n"
          "b.gt 1b\n"
          "ret";
    } else {
      return;
    }
    llvm::InlineAsm* prober = llvm::InlineAsm::get(
        type, sequence, "~{memory}", /*hasSideEffects=*/true,
        /*isAlignStack=*/false, llvm::InlineAsm::AD_ATT);
    builder_.CreateCall(prober);
    builder_.CreateRetVoid();
  }

  // The environment of the process, as the platform handed it out.
  // `alcy_env_exists` answers whether a variable is set;
  // `alcy_env_get` answers its raw bytes, with a null pointer when it
  // is not set. `alcy_env_find` is the one place the platform's
  // layout is read.
  void build_environ() {
    if (!declared("alcy_env_exists") && !declared("alcy_env_get")) {
      return;
    }
    if (mach_o_) {
      build_environ_macho();
      return;
    }
    if (windows_) {
      build_environ_windows();
      return;
    }
    build_environ_linux();
  }

  void build_env_exists(llvm::Function* find, llvm::StructType* view) {
    llvm::FunctionType* type =
        llvm::FunctionType::get(builder_.getInt32Ty(), {view}, false);
    llvm::Function* function = runtime("alcy_env_exists", type);
    function->setDoesNotThrow();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(module_.getContext(), "entry", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* found_view = builder_.CreateCall(find, {function->getArg(0)});
    llvm::Value* ptr = builder_.CreateExtractValue(found_view, 0);
    llvm::Value* is_set = builder_.CreateICmpNE(
        ptr, llvm::ConstantPointerNull::get(builder_.getPtrTy()), "is_set");
    builder_.CreateRet(builder_.CreateSelect(
        is_set, llvm::ConstantInt::get(builder_.getInt32Ty(), 1),
        llvm::ConstantInt::get(builder_.getInt32Ty(), 0)));
  }

  void build_env_get(llvm::Function* find, llvm::StructType* view) {
    llvm::FunctionType* type = llvm::FunctionType::get(view, {view}, false);
    llvm::Function* function = runtime("alcy_env_get", type);
    function->setDoesNotThrow();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(module_.getContext(), "entry", function);
    llvm::BasicBlock* present =
        llvm::BasicBlock::Create(module_.getContext(), "present", function);
    llvm::BasicBlock* missing =
        llvm::BasicBlock::Create(module_.getContext(), "missing", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* found = builder_.CreateCall(find, {function->getArg(0)});
    llvm::Value* ptr = builder_.CreateExtractValue(found, 0);
    llvm::Value* is_set = builder_.CreateICmpNE(
        ptr, llvm::ConstantPointerNull::get(builder_.getPtrTy()), "is_set");
    builder_.CreateCondBr(is_set, present, missing);
    builder_.SetInsertPoint(present);
    builder_.CreateRet(found);
    builder_.SetInsertPoint(missing);
    llvm::Value* empty = llvm::UndefValue::get(view);
    empty = builder_.CreateInsertValue(
        empty, llvm::ConstantPointerNull::get(builder_.getPtrTy()), 0);
    empty =
        builder_.CreateInsertValue(empty, llvm::ConstantInt::get(usize_, 0), 1);
    builder_.CreateRet(empty);
  }

  llvm::Function* new_find(llvm::StructType* view) {
    llvm::FunctionType* type = llvm::FunctionType::get(view, {view}, false);
    llvm::Function* function = llvm::Function::Create(
        type, llvm::GlobalValue::InternalLinkage, "alcy_env_find", module_);
    function->setDoesNotThrow();
    return function;
  }

  llvm::Value* null_view(llvm::StructType* view) {
    llvm::Value* empty = llvm::UndefValue::get(view);
    empty = builder_.CreateInsertValue(
        empty, llvm::ConstantPointerNull::get(builder_.getPtrTy()), 0);
    empty =
        builder_.CreateInsertValue(empty, llvm::ConstantInt::get(usize_, 0), 1);
    return empty;
  }

  void finish_environ(llvm::Function* find, llvm::StructType* view) {
    if (declared("alcy_env_exists")) {
      build_env_exists(find, view);
    }
    if (declared("alcy_env_get")) {
      build_env_get(find, view);
    }
  }

  void build_environ_linux() {
    llvm::LLVMContext& context = module_.getContext();
    llvm::Type* i8 = builder_.getInt8Ty();
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::StructType* view = llvm::StructType::get(context, {ptr, usize_});
    llvm::Function* find = new_find(view);
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", find);
    llvm::BasicBlock* iter = llvm::BasicBlock::Create(context, "iter", find);
    llvm::BasicBlock* compare =
        llvm::BasicBlock::Create(context, "compare", find);
    llvm::BasicBlock* byte_compare =
        llvm::BasicBlock::Create(context, "byte_compare", find);
    llvm::BasicBlock* advance =
        llvm::BasicBlock::Create(context, "advance", find);
    llvm::BasicBlock* at_equals =
        llvm::BasicBlock::Create(context, "at_equals", find);
    llvm::BasicBlock* value_scan =
        llvm::BasicBlock::Create(context, "value_scan", find);
    llvm::BasicBlock* value_scan_loop =
        llvm::BasicBlock::Create(context, "value_scan_loop", find);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(context, "done", find);
    llvm::BasicBlock* next_entry =
        llvm::BasicBlock::Create(context, "next_entry", find);
    llvm::BasicBlock* missing =
        llvm::BasicBlock::Create(context, "missing", find);
    builder_.SetInsertPoint(entry);
    llvm::Value* name = builder_.CreateExtractValue(find->getArg(0), 0);
    llvm::Value* name_len = builder_.CreateExtractValue(find->getArg(0), 1);
    llvm::Value* table = builder_.CreateLoad(
        ptr, module_.getOrInsertGlobal("alcy_environ", ptr), "table");
    builder_.CreateBr(iter);
    builder_.SetInsertPoint(iter);
    llvm::PHINode* at = builder_.CreatePHI(usize_, 2, "at");
    at->addIncoming(llvm::ConstantInt::get(usize_, 0), entry);
    llvm::Value* slot = builder_.CreateGEP(ptr, table, at, "slot");
    llvm::Value* entry_ptr = builder_.CreateLoad(ptr, slot, "entry");
    llvm::Value* is_null = builder_.CreateICmpEQ(
        entry_ptr, llvm::ConstantPointerNull::get(ptr), "is_null");
    builder_.CreateCondBr(is_null, missing, compare);
    builder_.SetInsertPoint(compare);
    llvm::PHINode* j = builder_.CreatePHI(usize_, 2, "j");
    j->addIncoming(llvm::ConstantInt::get(usize_, 0), iter);
    llvm::Value* at_equal = builder_.CreateICmpUGE(j, name_len, "at_equal");
    builder_.CreateCondBr(at_equal, at_equals, byte_compare);
    builder_.SetInsertPoint(byte_compare);
    llvm::Value* nb = builder_.CreateLoad(
        i8, builder_.CreateGEP(i8, entry_ptr, j, "nb_at"), "nb");
    llvm::Value* mb =
        builder_.CreateLoad(i8, builder_.CreateGEP(i8, name, j, "mb_at"), "mb");
    llvm::Value* eq = builder_.CreateICmpEQ(nb, mb, "eq");
    builder_.CreateCondBr(eq, advance, next_entry);
    builder_.SetInsertPoint(advance);
    j->addIncoming(
        builder_.CreateAdd(j, llvm::ConstantInt::get(usize_, 1), "j_next"),
        advance);
    builder_.CreateBr(compare);
    builder_.SetInsertPoint(at_equals);
    llvm::Value* at_byte = builder_.CreateLoad(
        i8, builder_.CreateGEP(i8, entry_ptr, j, "at_byte_at"), "at_byte");
    llvm::Value* is_equals = builder_.CreateICmpEQ(
        at_byte, llvm::ConstantInt::get(i8, '='), "is_equals");
    builder_.CreateCondBr(is_equals, value_scan, next_entry);
    builder_.SetInsertPoint(value_scan);
    llvm::Value* value = builder_.CreateGEP(
        i8, entry_ptr,
        builder_.CreateAdd(j, llvm::ConstantInt::get(usize_, 1), "value_at"),
        "value");
    builder_.CreateBr(value_scan_loop);
    builder_.SetInsertPoint(value_scan_loop);
    llvm::PHINode* loop_at = builder_.CreatePHI(usize_, 2, "loop_at");
    loop_at->addIncoming(llvm::ConstantInt::get(usize_, 0), value_scan);
    llvm::Value* byte = builder_.CreateLoad(
        i8, builder_.CreateGEP(i8, value, loop_at, "byte_at"), "byte");
    llvm::Value* is_zero =
        builder_.CreateICmpEQ(byte, llvm::ConstantInt::get(i8, 0), "is_zero");
    llvm::Value* next_loop = builder_.CreateAdd(
        loop_at, llvm::ConstantInt::get(usize_, 1), "next_loop");
    loop_at->addIncoming(next_loop, value_scan_loop);
    builder_.CreateCondBr(is_zero, done, value_scan_loop);
    builder_.SetInsertPoint(done);
    llvm::Value* found = llvm::UndefValue::get(view);
    found = builder_.CreateInsertValue(found, value, 0);
    found = builder_.CreateInsertValue(found, loop_at, 1);
    builder_.CreateRet(found);
    builder_.SetInsertPoint(next_entry);
    at->addIncoming(
        builder_.CreateAdd(at, llvm::ConstantInt::get(usize_, 1), "at_next"),
        next_entry);
    builder_.CreateBr(iter);
    builder_.SetInsertPoint(missing);
    builder_.CreateRet(null_view(view));
    finish_environ(find, view);
  }

  void build_environ_macho() {
    llvm::LLVMContext& context = module_.getContext();
    llvm::Type* i8 = builder_.getInt8Ty();
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::StructType* view = llvm::StructType::get(context, {ptr, usize_});
    llvm::Function* find = new_find(view);
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", find);
    llvm::BasicBlock* found = llvm::BasicBlock::Create(context, "found", find);
    llvm::BasicBlock* scan = llvm::BasicBlock::Create(context, "scan", find);
    llvm::BasicBlock* getenv_done =
        llvm::BasicBlock::Create(context, "getenv_done", find);
    llvm::BasicBlock* missing =
        llvm::BasicBlock::Create(context, "missing", find);
    builder_.SetInsertPoint(entry);
    llvm::Value* name = builder_.CreateExtractValue(find->getArg(0), 0);
    llvm::Value* name_len = builder_.CreateExtractValue(find->getArg(0), 1);
    llvm::Value* cname = nul_terminated_path(name, name_len);
    llvm::Function* getenv_fn =
        libc("getenv", llvm::FunctionType::get(ptr, {ptr}, false));
    llvm::Value* found_ptr = builder_.CreateCall(getenv_fn, {cname}, "found");
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(found_ptr, llvm::ConstantPointerNull::get(ptr),
                              "is_missing"),
        missing, found);
    builder_.SetInsertPoint(found);
    builder_.CreateBr(scan);
    builder_.SetInsertPoint(scan);
    llvm::PHINode* scan_at = builder_.CreatePHI(usize_, 2, "scan_at");
    scan_at->addIncoming(llvm::ConstantInt::get(usize_, 0), found);
    llvm::Value* byte = builder_.CreateLoad(
        i8, builder_.CreateGEP(i8, found_ptr, scan_at, "byte_at"), "byte");
    llvm::Value* is_zero =
        builder_.CreateICmpEQ(byte, llvm::ConstantInt::get(i8, 0), "is_zero");
    llvm::Value* next_at = builder_.CreateAdd(
        scan_at, llvm::ConstantInt::get(usize_, 1), "next_at");
    scan_at->addIncoming(next_at, scan);
    builder_.CreateCondBr(is_zero, getenv_done, scan);
    builder_.SetInsertPoint(getenv_done);
    llvm::Value* aggregate = llvm::UndefValue::get(view);
    aggregate = builder_.CreateInsertValue(aggregate, found_ptr, 0);
    aggregate = builder_.CreateInsertValue(aggregate, scan_at, 1);
    builder_.CreateRet(aggregate);
    builder_.SetInsertPoint(missing);
    builder_.CreateRet(null_view(view));
    finish_environ(find, view);
  }

  void build_environ_windows() {
    llvm::LLVMContext& context = module_.getContext();
    llvm::Type* i8 = builder_.getInt8Ty();
    llvm::Type* i16 = builder_.getInt16Ty();
    llvm::Type* i32 = builder_.getInt32Ty();
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::StructType* view = llvm::StructType::get(context, {ptr, usize_});
    auto lower = [&](llvm::Value* unit) {
      llvm::Value* is_lower = builder_.CreateAnd(
          builder_.CreateICmpUGE(unit, llvm::ConstantInt::get(i16, 'a')),
          builder_.CreateICmpULE(unit, llvm::ConstantInt::get(i16, 'z')));
      return builder_.CreateSelect(
          is_lower, builder_.CreateSub(unit, llvm::ConstantInt::get(i16, 32)),
          unit);
    };

    llvm::Function* find = new_find(view);
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", find);
    llvm::BasicBlock* prepare_done =
        llvm::BasicBlock::Create(context, "prepare_done", find);
    llvm::BasicBlock* iter = llvm::BasicBlock::Create(context, "iter", find);
    llvm::BasicBlock* unit_compare =
        llvm::BasicBlock::Create(context, "unit_compare", find);
    llvm::BasicBlock* unit_compare_body =
        llvm::BasicBlock::Create(context, "unit_compare_body", find);
    llvm::BasicBlock* at_equals =
        llvm::BasicBlock::Create(context, "at_equals", find);
    llvm::BasicBlock* advance =
        llvm::BasicBlock::Create(context, "advance", find);
    llvm::BasicBlock* encode =
        llvm::BasicBlock::Create(context, "encode", find);
    llvm::BasicBlock* value_scan =
        llvm::BasicBlock::Create(context, "value_scan", find);
    llvm::BasicBlock* value_done =
        llvm::BasicBlock::Create(context, "value_done", find);
    llvm::BasicBlock* encode_body =
        llvm::BasicBlock::Create(context, "encode_body", find);
    llvm::BasicBlock* give_up =
        llvm::BasicBlock::Create(context, "give_up", find);
    llvm::BasicBlock* next_entry =
        llvm::BasicBlock::Create(context, "next_entry", find);
    llvm::BasicBlock* scan_nul =
        llvm::BasicBlock::Create(context, "scan_nul", find);
    llvm::BasicBlock* next_iter =
        llvm::BasicBlock::Create(context, "next_iter", find);
    llvm::BasicBlock* missing =
        llvm::BasicBlock::Create(context, "missing", find);

    builder_.SetInsertPoint(entry);
    llvm::Value* name = builder_.CreateExtractValue(find->getArg(0), 0);
    llvm::Value* name_len = builder_.CreateExtractValue(find->getArg(0), 1);
    llvm::Function* to_wide = libc(
        "MultiByteToWideChar",
        llvm::FunctionType::get(i32, {i32, i32, ptr, i32, ptr, i32}, false));
    // The name as wide units, compared case-insensitively below.
    llvm::Value* nametmp = module_.getOrInsertGlobal(
        "alcy_env_name", llvm::ArrayType::get(i16, 32768));
    (void)nametmp;
    llvm::Value* name_units =
        builder_.CreateCall(to_wide,
                            {llvm::ConstantInt::get(i32, 65001 /* CP_UTF8 */),
                             llvm::ConstantInt::get(i32, 0), name,
                             builder_.CreateTrunc(name_len, i32, "name_len32"),
                             nametmp, llvm::ConstantInt::get(i32, 32768)},
                            "name_units");
    llvm::Value* bad = builder_.CreateICmpSLE(
        name_units, llvm::ConstantInt::get(i32, 0), "bad");
    builder_.CreateCondBr(bad, missing, prepare_done);
    builder_.SetInsertPoint(prepare_done);
    llvm::Value* name_units64 =
        builder_.CreateZExtOrTrunc(name_units, usize_, "name_units64");
    llvm::Value* block = builder_.CreateLoad(
        ptr, module_.getOrInsertGlobal("alcy_environ", ptr), "block");
    builder_.CreateBr(iter);
    builder_.SetInsertPoint(iter);
    llvm::PHINode* at = builder_.CreatePHI(usize_, 2, "at");
    at->addIncoming(llvm::ConstantInt::get(usize_, 0), prepare_done);
    llvm::Value* entry_ptr = builder_.CreateGEP(i16, block, at, "entry");
    llvm::Value* c0 = builder_.CreateLoad(i16, entry_ptr, "c0");
    llvm::Value* end_of_block = builder_.CreateICmpEQ(
        c0, llvm::ConstantInt::get(i16, 0), "end_of_block");
    builder_.CreateCondBr(end_of_block, missing, unit_compare);
    builder_.SetInsertPoint(unit_compare);
    llvm::PHINode* j = builder_.CreatePHI(usize_, 2, "j");
    j->addIncoming(llvm::ConstantInt::get(usize_, 0), iter);
    llvm::Value* at_equal = builder_.CreateICmpUGE(j, name_units64, "at_equal");
    builder_.CreateCondBr(at_equal, at_equals, unit_compare_body);
    builder_.SetInsertPoint(unit_compare_body);
    llvm::Value* a = builder_.CreateLoad(
        i16, builder_.CreateGEP(i16, entry_ptr, j, "a_at"), "a");
    llvm::Value* b = builder_.CreateLoad(
        i16, builder_.CreateGEP(i16, nametmp, j, "b_at"), "b");
    llvm::Value* eq = builder_.CreateICmpEQ(lower(a), lower(b), "eq");
    builder_.CreateCondBr(eq, advance, next_entry);
    builder_.SetInsertPoint(advance);
    j->addIncoming(
        builder_.CreateAdd(j, llvm::ConstantInt::get(usize_, 1), "j_next"),
        advance);
    builder_.CreateBr(unit_compare);
    builder_.SetInsertPoint(at_equals);
    llvm::Value* at_byte = builder_.CreateLoad(
        i16, builder_.CreateGEP(i16, entry_ptr, j, "at_byte_at"), "at_byte");
    llvm::Value* is_equals = builder_.CreateICmpEQ(
        at_byte, llvm::ConstantInt::get(i16, '='), "is_equals");
    builder_.CreateCondBr(is_equals, encode, next_entry);
    builder_.SetInsertPoint(encode);
    llvm::Value* value_ptr = builder_.CreateGEP(
        i16, entry_ptr,
        builder_.CreateAdd(j, llvm::ConstantInt::get(usize_, 1), "value_at"),
        "value");
    builder_.CreateBr(value_scan);
    builder_.SetInsertPoint(value_scan);
    llvm::PHINode* k = builder_.CreatePHI(usize_, 2, "k");
    k->addIncoming(llvm::ConstantInt::get(usize_, 0), encode);
    llvm::Value* u = builder_.CreateLoad(
        i16, builder_.CreateGEP(i16, value_ptr, k, "u_at"), "u");
    llvm::Value* is_zero =
        builder_.CreateICmpEQ(u, llvm::ConstantInt::get(i16, 0), "is_zero");
    llvm::Value* next_k =
        builder_.CreateAdd(k, llvm::ConstantInt::get(usize_, 1), "next_k");
    k->addIncoming(next_k, value_scan);
    builder_.CreateCondBr(is_zero, value_done, value_scan);
    builder_.SetInsertPoint(value_done);
    llvm::Function* to_multi =
        libc("WideCharToMultiByte",
             llvm::FunctionType::get(
                 i32, {i32, i32, ptr, i32, ptr, i32, ptr, ptr}, false));
    llvm::Value* value_len32 = builder_.CreateTrunc(k, i32, "value_len32");
    llvm::Value* measure = builder_.CreateCall(
        to_multi,
        {llvm::ConstantInt::get(i32, 65001 /* CP_UTF8 */),
         llvm::ConstantInt::get(i32, 0), value_ptr, value_len32,
         llvm::ConstantPointerNull::get(ptr), llvm::ConstantInt::get(i32, 0),
         llvm::ConstantPointerNull::get(ptr),
         llvm::ConstantPointerNull::get(ptr)},
        "measure");
    llvm::Value* too_big = builder_.CreateICmpUGT(
        measure, llvm::ConstantInt::get(i32, 262144), "too_big");
    builder_.CreateCondBr(too_big, give_up, encode_body);
    builder_.SetInsertPoint(encode_body);
    llvm::Value* buf = module_.getOrInsertGlobal(
        "alcy_env_buf", llvm::ArrayType::get(i8, 262144));
    (void)buf;
    llvm::Value* encoded = builder_.CreateCall(
        to_multi,
        {llvm::ConstantInt::get(i32, 65001 /* CP_UTF8 */),
         llvm::ConstantInt::get(i32, 0), value_ptr, value_len32, buf, measure,
         llvm::ConstantPointerNull::get(ptr),
         llvm::ConstantPointerNull::get(ptr)},
        "encoded");
    llvm::Value* aggregate = llvm::UndefValue::get(view);
    aggregate = builder_.CreateInsertValue(aggregate, buf, 0);
    aggregate = builder_.CreateInsertValue(
        aggregate, builder_.CreateZExtOrTrunc(encoded, usize_, "encoded_wide"),
        1);
    builder_.CreateRet(aggregate);
    builder_.SetInsertPoint(give_up);
    builder_.CreateRet(null_view(view));
    builder_.SetInsertPoint(next_entry);
    builder_.CreateBr(scan_nul);
    llvm::BasicBlock* scan_nul_incr =
        llvm::BasicBlock::Create(context, "scan_nul_incr", find);
    builder_.SetInsertPoint(scan_nul);
    llvm::PHINode* k_next = builder_.CreatePHI(usize_, 2, "k_next");
    k_next->addIncoming(llvm::ConstantInt::get(usize_, 0), next_entry);
    llvm::Value* u2 = builder_.CreateLoad(
        i16, builder_.CreateGEP(i16, entry_ptr, k_next, "u2_at"), "u2");
    llvm::Value* is_zero2 =
        builder_.CreateICmpEQ(u2, llvm::ConstantInt::get(i16, 0), "is_zero2");
    llvm::Value* k_plus =
        builder_.CreateAdd(k_next, llvm::ConstantInt::get(usize_, 1), "k_plus");
    builder_.CreateCondBr(is_zero2, next_iter, scan_nul_incr);
    builder_.SetInsertPoint(scan_nul_incr);
    k_next->addIncoming(k_plus, scan_nul_incr);
    builder_.CreateBr(scan_nul);
    builder_.SetInsertPoint(next_iter);
    at->addIncoming(
        builder_.CreateAdd(builder_.CreateAdd(at, k_next, "at_plus_k"),
                           llvm::ConstantInt::get(usize_, 1), "at_next"),
        next_iter);
    builder_.CreateBr(iter);
    builder_.SetInsertPoint(missing);
    builder_.CreateRet(null_view(view));
    finish_environ(find, view);
  }

  // The arguments the process was started with, gathered the way the
  // C runtime hands them out: the command line as the kernel gave it,
  // parsed the way the command line is documented to parse -
  // backslashes are literal except before a quote, an even run of them
  // collapses, the quote itself toggles or escapes - and checked as
  // UTF-8 out of the kernel's wide string. The argv bytes are owned by
  // a static arena and handed out as NUL-terminated strings, so the
  // descriptor scan answers unchanged.
  void build_args_init() {
    llvm::Type* i32 = builder_.getInt32Ty();
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::Type* i16 = builder_.getInt16Ty();
    llvm::Type* i8 = builder_.getInt8Ty();
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::LLVMContext& context = module_.getContext();

    llvm::Function* get_command_line =
        libc("GetCommandLineW", llvm::FunctionType::get(ptr, false));
    llvm::Function* to_multi_byte =
        libc("WideCharToMultiByte",
             llvm::FunctionType::get(
                 i32, {i32, i32, ptr, i32, ptr, i32, ptr, ptr}, false));

    llvm::GlobalVariable* arena =
        cast<llvm::GlobalVariable>(module_.getOrInsertGlobal(
            "alcy_args_arena", llvm::ArrayType::get(i8, 262144)));
    arena->setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
    if (!arena->hasInitializer()) {
      arena->setInitializer(
          llvm::ConstantAggregateZero::get(arena->getValueType()));
    }
    llvm::GlobalVariable* table =
        cast<llvm::GlobalVariable>(module_.getOrInsertGlobal(
            "alcy_args_table", llvm::ArrayType::get(ptr, 1024)));
    table->setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
    if (!table->hasInitializer()) {
      table->setInitializer(
          llvm::ConstantAggregateZero::get(table->getValueType()));
    }
    llvm::GlobalVariable* temp =
        cast<llvm::GlobalVariable>(module_.getOrInsertGlobal(
            "alcy_args_temp", llvm::ArrayType::get(i16, 32768)));
    temp->setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
    if (!temp->hasInitializer()) {
      temp->setInitializer(
          llvm::ConstantAggregateZero::get(temp->getValueType()));
    }

    llvm::FunctionType* type =
        llvm::FunctionType::get(builder_.getVoidTy(), false);
    llvm::Function* function = runtime("alcy_init_args", type);
    function->setDoesNotThrow();

    const auto block = [&](const char* name) {
      return llvm::BasicBlock::Create(context, name, function);
    };
    llvm::BasicBlock* entry = block("entry");
    llvm::BasicBlock* scan = block("scan");
    llvm::BasicBlock* scan_done = block("scan_done");
    llvm::BasicBlock* skip_ws = block("skip_ws");
    llvm::BasicBlock* have_arg = block("have_arg");
    llvm::BasicBlock* arg_start = block("arg_start");
    llvm::BasicBlock* char_loop = block("char_loop");
    llvm::BasicBlock* read_char = block("read_char");
    llvm::BasicBlock* bslash = block("bslash");
    llvm::BasicBlock* not_bslash = block("not_bslash");
    llvm::BasicBlock* quote = block("quote");
    llvm::BasicBlock* quote_paired = block("quote_paired");
    llvm::BasicBlock* quote_toggle = block("quote_toggle");
    llvm::BasicBlock* quote_literal = block("quote_literal");
    llvm::BasicBlock* quote_single = block("quote_single");
    llvm::BasicBlock* quote_drop_one = block("quote_drop_one");
    llvm::BasicBlock* quote_zero = block("quote_zero");
    llvm::BasicBlock* open_quote = block("open_quote");
    llvm::BasicBlock* in_quote_qv = block("in_quote_qv");
    llvm::BasicBlock* in_quote_doubled = block("in_quote_doubled");
    llvm::BasicBlock* quote_zero_single = block("quote_zero_single");
    llvm::BasicBlock* not_quote = block("not_quote");
    llvm::BasicBlock* space = block("space");
    llvm::BasicBlock* space_keep = block("space_keep");
    llvm::BasicBlock* ordinary = block("ordinary");
    llvm::BasicBlock* arg_end = block("arg_end");
    llvm::BasicBlock* flush = block("flush");
    llvm::BasicBlock* flush_replacement = block("flush_replacement");
    llvm::BasicBlock* flush_copy = block("flush_copy");
    llvm::BasicBlock* flush_join = block("flush_join");
    llvm::BasicBlock* flush_check = block("flush_check");
    llvm::BasicBlock* args_done = block("args_done");

    // How many arguments an argv the runtime built carries, for the
    // `alcy_arg` descriptor scan. The kernel hands the entry no
    // arguments on this platform, so the runtime gathers them.
    builder_.SetInsertPoint(entry);
    llvm::Function* get_env_strings =
        libc("GetEnvironmentStringsW", llvm::FunctionType::get(ptr, false));
    llvm::Value* env_block =
        builder_.CreateCall(get_env_strings, {}, "env_block");
    builder_.CreateStore(env_block,
                         module_.getOrInsertGlobal("alcy_environ", ptr));
    llvm::Value* wcmd = builder_.CreateCall(get_command_line, {}, "wcmd");
    builder_.CreateBr(scan);

    builder_.SetInsertPoint(scan);
    llvm::PHINode* at = builder_.CreatePHI(i64, 2, "at");
    at->addIncoming(llvm::ConstantInt::get(i64, 0), entry);
    llvm::Value* c = builder_.CreateLoad(
        i16, builder_.CreateGEP(i16, wcmd, at, "at_arg"), "c");
    llvm::Value* next =
        builder_.CreateAdd(at, llvm::ConstantInt::get(i64, 1), "next");
    llvm::Value* done =
        builder_.CreateICmpEQ(c, llvm::ConstantInt::get(i16, 0), "done");
    at->addIncoming(next, scan);
    builder_.CreateCondBr(done, scan_done, scan);

    builder_.SetInsertPoint(scan_done);
    llvm::PHINode* n = builder_.CreatePHI(i64, 1, "n");
    n->addIncoming(at, scan);
    builder_.CreateBr(skip_ws);

    // Skip leading whitespace: the position of the next argument, or
    // the end of the command line.
    builder_.SetInsertPoint(skip_ws);
    llvm::PHINode* outer_i = builder_.CreatePHI(i64, 3, "i");
    llvm::PHINode* arena_pos = builder_.CreatePHI(i64, 3, "arena_pos");
    llvm::PHINode* argc = builder_.CreatePHI(i64, 3, "argc");
    outer_i->addIncoming(llvm::ConstantInt::get(i64, 0), scan_done);
    arena_pos->addIncoming(llvm::ConstantInt::get(i64, 0), scan_done);
    argc->addIncoming(llvm::ConstantInt::get(i64, 0), scan_done);
    llvm::Value* past_end = builder_.CreateICmpUGE(outer_i, n, "past_end");
    builder_.CreateCondBr(past_end, args_done, have_arg);
    builder_.SetInsertPoint(have_arg);
    llvm::Value* outer_c = builder_.CreateLoad(
        i16, builder_.CreateGEP(i16, wcmd, outer_i, "have_at"), "outer_c");
    llvm::Value* is_space0 = builder_.CreateOr(
        builder_.CreateICmpEQ(outer_c, llvm::ConstantInt::get(i16, 32)),
        builder_.CreateICmpEQ(outer_c, llvm::ConstantInt::get(i16, 9)),
        "is_space0");
    llvm::BasicBlock* to_next_block =
        llvm::BasicBlock::Create(context, "to_next", function);
    builder_.CreateCondBr(is_space0, to_next_block, arg_start);
    builder_.SetInsertPoint(to_next_block);
    llvm::PHINode* to_space = builder_.CreatePHI(i64, 1, "to_space");
    to_space->addIncoming(outer_i, have_arg);
    llvm::Value* to_next =
        builder_.CreateAdd(to_space, llvm::ConstantInt::get(i64, 1), "to_next");
    outer_i->addIncoming(to_next, to_next_block);
    arena_pos->addIncoming(arena_pos, to_next_block);
    argc->addIncoming(argc, to_next_block);
    builder_.CreateBr(skip_ws);

    // One argument: a fresh segment, unquoted, no pending backslashes.
    builder_.SetInsertPoint(arg_start);
    builder_.CreateBr(char_loop);

    builder_.SetInsertPoint(char_loop);
    llvm::PHINode* seg_i = builder_.CreatePHI(i64, 9, "i");
    llvm::PHINode* seg = builder_.CreatePHI(i64, 9, "seg");
    llvm::PHINode* in_quotes =
        builder_.CreatePHI(builder_.getInt1Ty(), 9, "in_quotes");
    llvm::PHINode* bs = builder_.CreatePHI(i64, 9, "bs");
    llvm::PHINode* started =
        builder_.CreatePHI(builder_.getInt1Ty(), 9, "started");
    seg_i->addIncoming(outer_i, arg_start);
    seg->addIncoming(llvm::ConstantInt::get(i64, 0), arg_start);
    in_quotes->addIncoming(llvm::ConstantInt::getFalse(context), arg_start);
    bs->addIncoming(llvm::ConstantInt::get(i64, 0), arg_start);
    started->addIncoming(llvm::ConstantInt::getFalse(context), arg_start);
    llvm::Value* at_end_now = builder_.CreateICmpUGE(seg_i, n, "at_end_now");
    builder_.CreateCondBr(at_end_now, arg_end, read_char);

    builder_.SetInsertPoint(read_char);
    llvm::Value* read = builder_.CreateLoad(
        i16, builder_.CreateGEP(i16, wcmd, seg_i, "read_at"), "read");
    llvm::Value* is_bslash = builder_.CreateICmpEQ(
        read, llvm::ConstantInt::get(i16, 92), "is_bslash");
    builder_.CreateCondBr(is_bslash, bslash, not_bslash);

    builder_.SetInsertPoint(bslash);
    // A backslash is copied now - it is literal until a quote or
    // whitespace decides it is not; the quote case rewinds the run.
    llvm::Value* temp_at_bslash = builder_.CreateGEP(i16, temp, seg, "temp_at");
    builder_.CreateStore(llvm::ConstantInt::get(i16, 92), temp_at_bslash);
    llvm::Value* bs_next =
        builder_.CreateAdd(bs, llvm::ConstantInt::get(i64, 1), "bs_next");
    llvm::Value* seg_b =
        builder_.CreateAdd(seg, llvm::ConstantInt::get(i64, 1), "seg_b");
    llvm::Value* i_b =
        builder_.CreateAdd(seg_i, llvm::ConstantInt::get(i64, 1), "i_b");
    seg_i->addIncoming(i_b, bslash);
    seg->addIncoming(seg_b, bslash);
    in_quotes->addIncoming(in_quotes, bslash);
    bs->addIncoming(bs_next, bslash);
    started->addIncoming(llvm::ConstantInt::getTrue(context), bslash);
    builder_.CreateBr(char_loop);

    builder_.SetInsertPoint(not_bslash);
    llvm::Value* is_quote = builder_.CreateICmpEQ(
        read, llvm::ConstantInt::get(i16, 34), "is_quote");
    builder_.CreateCondBr(is_quote, quote, not_quote);

    builder_.SetInsertPoint(quote);
    llvm::Value* bs_ge_2 =
        builder_.CreateICmpUGE(bs, llvm::ConstantInt::get(i64, 2), "bs_ge_2");
    builder_.CreateCondBr(bs_ge_2, quote_paired, quote_single);

    builder_.SetInsertPoint(quote_paired);
    // An even run collapses to half literal backslashes, and the quote
    // toggles; an odd run collapses to half backslashes, and the quote
    // is literal.
    llvm::Value* half =
        builder_.CreateLShr(bs, llvm::ConstantInt::get(i64, 1), "half");
    llvm::Value* whole = builder_.CreateSub(bs, half, "whole");
    llvm::Value* seg_after = builder_.CreateSub(seg, whole, "seg_after");
    llvm::Value* is_odd =
        builder_.CreateAnd(bs, llvm::ConstantInt::get(i64, 1), "is_odd");
    llvm::Value* odd =
        builder_.CreateICmpNE(is_odd, llvm::ConstantInt::get(i64, 0), "odd");
    builder_.CreateCondBr(odd, quote_literal, quote_toggle);

    builder_.SetInsertPoint(quote_toggle);
    llvm::Value* toggled = builder_.CreateXor(
        in_quotes, llvm::ConstantInt::getTrue(context), "toggled");
    llvm::Value* i_toggle =
        builder_.CreateAdd(seg_i, llvm::ConstantInt::get(i64, 1), "i_toggle");
    seg_i->addIncoming(i_toggle, quote_toggle);
    seg->addIncoming(seg_after, quote_toggle);
    in_quotes->addIncoming(toggled, quote_toggle);
    bs->addIncoming(llvm::ConstantInt::get(i64, 0), quote_toggle);
    started->addIncoming(llvm::ConstantInt::getTrue(context), quote_toggle);
    builder_.CreateBr(char_loop);

    builder_.SetInsertPoint(quote_literal);
    llvm::Value* temp_at_lit =
        builder_.CreateGEP(i16, temp, seg_after, "temp_at_lit");
    builder_.CreateStore(llvm::ConstantInt::get(i16, 34), temp_at_lit);
    llvm::Value* seg_lit = builder_.CreateAdd(
        seg_after, llvm::ConstantInt::get(i64, 1), "seg_lit");
    llvm::Value* i_lit =
        builder_.CreateAdd(seg_i, llvm::ConstantInt::get(i64, 1), "i_lit");
    seg_i->addIncoming(i_lit, quote_literal);
    seg->addIncoming(seg_lit, quote_literal);
    in_quotes->addIncoming(in_quotes, quote_literal);
    bs->addIncoming(llvm::ConstantInt::get(i64, 0), quote_literal);
    started->addIncoming(llvm::ConstantInt::getTrue(context), quote_literal);
    builder_.CreateBr(char_loop);

    builder_.SetInsertPoint(quote_single);
    llvm::Value* bs_one =
        builder_.CreateICmpEQ(bs, llvm::ConstantInt::get(i64, 1), "bs_one");
    builder_.CreateCondBr(bs_one, quote_drop_one, quote_zero);

    builder_.SetInsertPoint(quote_drop_one);
    llvm::Value* seg_drop =
        builder_.CreateSub(seg, llvm::ConstantInt::get(i64, 1), "seg_drop");
    llvm::Value* temp_at_drop =
        builder_.CreateGEP(i16, temp, seg_drop, "temp_at_drop");
    builder_.CreateStore(llvm::ConstantInt::get(i16, 34), temp_at_drop);
    llvm::Value* seg_dropped = builder_.CreateAdd(
        seg_drop, llvm::ConstantInt::get(i64, 1), "seg_dropped");
    llvm::Value* i_drop =
        builder_.CreateAdd(seg_i, llvm::ConstantInt::get(i64, 1), "i_drop");
    seg_i->addIncoming(i_drop, quote_drop_one);
    seg->addIncoming(seg_dropped, quote_drop_one);
    in_quotes->addIncoming(in_quotes, quote_drop_one);
    bs->addIncoming(llvm::ConstantInt::get(i64, 0), quote_drop_one);
    started->addIncoming(llvm::ConstantInt::getTrue(context), quote_drop_one);
    builder_.CreateBr(char_loop);

    builder_.SetInsertPoint(quote_zero);
    builder_.CreateCondBr(in_quotes, in_quote_qv, open_quote);

    builder_.SetInsertPoint(open_quote);
    llvm::Value* i_open =
        builder_.CreateAdd(seg_i, llvm::ConstantInt::get(i64, 1), "i_open");
    seg_i->addIncoming(i_open, open_quote);
    seg->addIncoming(seg, open_quote);
    in_quotes->addIncoming(llvm::ConstantInt::getTrue(context), open_quote);
    bs->addIncoming(llvm::ConstantInt::get(i64, 0), open_quote);
    started->addIncoming(llvm::ConstantInt::getTrue(context), open_quote);
    builder_.CreateBr(char_loop);

    builder_.SetInsertPoint(in_quote_qv);
    // A doubled quote inside a quoted section is one literal quote; a
    // lone quote ends the section.
    llvm::Value* next_i =
        builder_.CreateAdd(seg_i, llvm::ConstantInt::get(i64, 1), "next_i");
    llvm::Value* next_c = builder_.CreateLoad(
        i16, builder_.CreateGEP(i16, wcmd, next_i, "next_at"), "next_c");
    llvm::Value* doubled = builder_.CreateICmpEQ(
        next_c, llvm::ConstantInt::get(i16, 34), "doubled");
    builder_.CreateCondBr(doubled, in_quote_doubled, quote_zero_single);

    builder_.SetInsertPoint(in_quote_doubled);
    llvm::Value* temp_at_dbl =
        builder_.CreateGEP(i16, temp, seg, "temp_at_dbl");
    builder_.CreateStore(llvm::ConstantInt::get(i16, 34), temp_at_dbl);
    llvm::Value* seg_dbl =
        builder_.CreateAdd(seg, llvm::ConstantInt::get(i64, 1), "seg_dbl");
    llvm::Value* i_dbl =
        builder_.CreateAdd(seg_i, llvm::ConstantInt::get(i64, 2), "i_dbl");
    seg_i->addIncoming(i_dbl, in_quote_doubled);
    seg->addIncoming(seg_dbl, in_quote_doubled);
    in_quotes->addIncoming(llvm::ConstantInt::getTrue(context),
                           in_quote_doubled);
    bs->addIncoming(llvm::ConstantInt::get(i64, 0), in_quote_doubled);
    started->addIncoming(llvm::ConstantInt::getTrue(context), in_quote_doubled);
    builder_.CreateBr(char_loop);

    builder_.SetInsertPoint(quote_zero_single);
    llvm::Value* i_qzs =
        builder_.CreateAdd(seg_i, llvm::ConstantInt::get(i64, 1), "i_qzs");
    seg_i->addIncoming(i_qzs, quote_zero_single);
    seg->addIncoming(seg, quote_zero_single);
    in_quotes->addIncoming(llvm::ConstantInt::getFalse(context),
                           quote_zero_single);
    bs->addIncoming(llvm::ConstantInt::get(i64, 0), quote_zero_single);
    started->addIncoming(llvm::ConstantInt::getTrue(context),
                         quote_zero_single);
    builder_.CreateBr(char_loop);

    builder_.SetInsertPoint(not_quote);
    llvm::Value* is_space = builder_.CreateOr(
        builder_.CreateICmpEQ(read, llvm::ConstantInt::get(i16, 32)),
        builder_.CreateICmpEQ(read, llvm::ConstantInt::get(i16, 9)),
        "is_space");
    builder_.CreateCondBr(is_space, space, ordinary);

    builder_.SetInsertPoint(space);
    builder_.CreateCondBr(in_quotes, space_keep, arg_end);

    builder_.SetInsertPoint(space_keep);
    llvm::Value* temp_at_space = builder_.CreateGEP(i16, temp, seg, "temp_at");
    builder_.CreateStore(read, temp_at_space);
    llvm::Value* seg_sp =
        builder_.CreateAdd(seg, llvm::ConstantInt::get(i64, 1), "seg_sp");
    llvm::Value* i_sp =
        builder_.CreateAdd(seg_i, llvm::ConstantInt::get(i64, 1), "i_sp");
    seg_i->addIncoming(i_sp, space_keep);
    seg->addIncoming(seg_sp, space_keep);
    in_quotes->addIncoming(in_quotes, space_keep);
    bs->addIncoming(llvm::ConstantInt::get(i64, 0), space_keep);
    // Whitespace inside quotes is content of the argument.
    started->addIncoming(llvm::ConstantInt::getTrue(context), space_keep);
    builder_.CreateBr(char_loop);

    builder_.SetInsertPoint(ordinary);
    llvm::Value* temp_at_ord = builder_.CreateGEP(i16, temp, seg, "temp_at");
    builder_.CreateStore(read, temp_at_ord);
    llvm::Value* seg_ord =
        builder_.CreateAdd(seg, llvm::ConstantInt::get(i64, 1), "seg_ord");
    llvm::Value* i_ord =
        builder_.CreateAdd(seg_i, llvm::ConstantInt::get(i64, 1), "i_ord");
    seg_i->addIncoming(i_ord, ordinary);
    seg->addIncoming(seg_ord, ordinary);
    in_quotes->addIncoming(in_quotes, ordinary);
    bs->addIncoming(llvm::ConstantInt::get(i64, 0), ordinary);
    started->addIncoming(llvm::ConstantInt::getTrue(context), ordinary);
    builder_.CreateBr(char_loop);

    builder_.SetInsertPoint(arg_end);
    builder_.CreateCondBr(started, flush, flush_check);

    builder_.SetInsertPoint(flush);
    llvm::Value* seg32 = builder_.CreateTrunc(seg, i32, "seg32");
    llvm::Value* bytes = builder_.CreateCall(
        to_multi_byte,
        {llvm::ConstantInt::get(i32, 65001 /* CP_UTF8 */),
         llvm::ConstantInt::get(i32, 0), temp, seg32,
         llvm::ConstantPointerNull::get(ptr), llvm::ConstantInt::get(i32, 0),
         llvm::ConstantPointerNull::get(ptr),
         llvm::ConstantPointerNull::get(ptr)},
        "bytes");
    llvm::Value* bad =
        builder_.CreateICmpSLE(bytes, llvm::ConstantInt::get(i32, 0), "bad");
    builder_.CreateCondBr(bad, flush_replacement, flush_copy);

    builder_.SetInsertPoint(flush_replacement);
    llvm::Value* bad_at = builder_.CreateGEP(i8, arena, arena_pos, "bad_at");
    builder_.CreateStore(llvm::ConstantInt::get(i8, 0xef), bad_at);
    builder_.CreateStore(
        llvm::ConstantInt::get(i8, 0xbf),
        builder_.CreateGEP(i8, bad_at, llvm::ConstantInt::get(i64, 1)));
    builder_.CreateStore(
        llvm::ConstantInt::get(i8, 0xbd),
        builder_.CreateGEP(i8, bad_at, llvm::ConstantInt::get(i64, 2)));
    builder_.CreateStore(
        llvm::ConstantInt::get(i8, 0),
        builder_.CreateGEP(i8, bad_at, llvm::ConstantInt::get(i64, 3)));
    builder_.CreateBr(flush_join);

    builder_.SetInsertPoint(flush_copy);
    llvm::Value* bytes64 = builder_.CreateZExt(bytes, i64, "bytes64");
    llvm::Value* dest = builder_.CreateGEP(i8, arena, arena_pos, "dest");
    builder_.CreateCall(to_multi_byte,
                        {llvm::ConstantInt::get(i32, 65001 /* CP_UTF8 */),
                         llvm::ConstantInt::get(i32, 0), temp, seg32, dest,
                         builder_.CreateTrunc(bytes64, i32, "bytes32"),
                         llvm::ConstantPointerNull::get(ptr),
                         llvm::ConstantPointerNull::get(ptr)});
    builder_.CreateStore(llvm::ConstantInt::get(i8, 0),
                         builder_.CreateGEP(i8, dest, bytes64, "dest_end"));
    llvm::Value* advance =
        builder_.CreateAdd(bytes64, llvm::ConstantInt::get(i64, 1), "advance");
    builder_.CreateBr(flush_join);

    builder_.SetInsertPoint(flush_join);
    llvm::PHINode* dest_phi = builder_.CreatePHI(ptr, 2, "dest_phi");
    llvm::PHINode* advance_phi = builder_.CreatePHI(i64, 2, "advance_phi");
    dest_phi->addIncoming(bad_at, flush_replacement);
    dest_phi->addIncoming(dest, flush_copy);
    advance_phi->addIncoming(llvm::ConstantInt::get(i64, 4), flush_replacement);
    advance_phi->addIncoming(advance, flush_copy);
    // The argv bytes are permanent: the slot table borrows from the
    // same arena.
    llvm::Value* slot = builder_.CreateGEP(ptr, table, argc, "slot");
    builder_.CreateStore(dest_phi, slot);
    llvm::Value* arena_next =
        builder_.CreateAdd(arena_pos, advance_phi, "arena_pos_next");
    llvm::Value* argc_next =
        builder_.CreateAdd(argc, llvm::ConstantInt::get(i64, 1), "argc_next");
    builder_.CreateBr(flush_check);

    builder_.SetInsertPoint(flush_check);
    llvm::PHINode* check_i = builder_.CreatePHI(i64, 2, "i");
    llvm::PHINode* check_arena = builder_.CreatePHI(i64, 2, "arena_pos");
    llvm::PHINode* check_argc = builder_.CreatePHI(i64, 2, "argc");
    // The not-started path arrives here with the pre-flush values; the
    // flushed path arrives with the grown ones.
    check_i->addIncoming(seg_i, arg_end);
    check_arena->addIncoming(arena_pos, arg_end);
    check_argc->addIncoming(argc, arg_end);
    check_i->addIncoming(seg_i, flush_join);
    check_arena->addIncoming(arena_next, flush_join);
    check_argc->addIncoming(argc_next, flush_join);
    llvm::Value* full = builder_.CreateICmpUGE(
        check_argc, llvm::ConstantInt::get(i64, 1024), "full");
    llvm::Value* exhausted = builder_.CreateICmpUGT(
        check_arena, llvm::ConstantInt::get(i64, 262144 - 4), "exhausted");
    llvm::Value* stop = builder_.CreateOr(full, exhausted, "stop");
    builder_.CreateCondBr(stop, args_done, skip_ws);

    // The parse continues at the whitespace the argument ended on; the
    // position arrives from the flush.
    builder_.SetInsertPoint(skip_ws);
    outer_i->addIncoming(check_i, flush_check);
    arena_pos->addIncoming(check_arena, flush_check);
    argc->addIncoming(check_argc, flush_check);

    builder_.SetInsertPoint(args_done);
    llvm::Value* argc32 = builder_.CreateTrunc(argc, i32, "argc32");
    llvm::Value* argc_glob = module_.getOrInsertGlobal("alcy_argc", i32);
    builder_.CreateStore(argc32, argc_glob);
    llvm::Value* argv_glob = module_.getOrInsertGlobal("alcy_argv", ptr);
    builder_.CreateStore(table, argv_glob);
    builder_.CreateRetVoid();
  }

  // openat(2) with `AT_FDCWD` for the current directory. The path is
  // copied into a slot with the terminator the syscall wants.
  static constexpr i64 AT_FDCWD = -100;
  static constexpr i64 O_RDONLY = 0;
  // The low 12 bits are the access mode; `O_CREAT` and `O_TRUNC` are
  // bits 6 and 9 on Linux.
  static constexpr i64 O_WRONLY = 1;
  static constexpr i64 O_CREAT = 0x40;
  static constexpr i64 O_TRUNC = 0x200;
  static constexpr i64 SEEK_TO_THE_END = 2;

  void build_file_size_syscall() {
    SyscallAbi abi{};
    CHECK_MSG(syscall_abi(abi),
              "freestanding runtime on an unsupported target");
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getInt64Ty(), {builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime("alcy_file_size", type);
    function->setDoesNotThrow();
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* missing =
        llvm::BasicBlock::Create(context, "missing", function);
    llvm::BasicBlock* opened =
        llvm::BasicBlock::Create(context, "opened", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* path =
        nul_terminated_path(function->getArg(0), function->getArg(1));
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::Value* zero = llvm::ConstantInt::get(i64, 0);
    llvm::Value* fd_value =
        syscall({llvm::ConstantInt::get(i64, abi.openat),
                 llvm::ConstantInt::get(i64, static_cast<u64>(AT_FDCWD)),
                 builder_.CreatePtrToInt(path, i64),
                 llvm::ConstantInt::get(i64, O_RDONLY), zero, zero, zero});
    llvm::Value* negative = builder_.CreateICmpSLT(fd_value, zero, "negative");
    builder_.CreateCondBr(negative, missing, opened);
    builder_.SetInsertPoint(missing);
    builder_.CreateRet(llvm::ConstantInt::get(builder_.getInt64Ty(), -1));
    builder_.SetInsertPoint(opened);
    llvm::Value* size = syscall(
        {llvm::ConstantInt::get(i64, abi.lseek), fd_value, zero,
         llvm::ConstantInt::get(i64, SEEK_TO_THE_END), zero, zero, zero});
    syscall({llvm::ConstantInt::get(i64, abi.close), fd_value, zero, zero, zero,
             zero, zero});
    builder_.CreateRet(size);
  }

  void build_read_file_into_syscall() {
    SyscallAbi abi{};
    CHECK_MSG(syscall_abi(abi),
              "freestanding runtime on an unsupported target");
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getInt64Ty(),
        {builder_.getPtrTy(), usize_, builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime("alcy_read_file_into", type);
    function->setDoesNotThrow();
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* missing =
        llvm::BasicBlock::Create(context, "missing", function);
    llvm::BasicBlock* opened =
        llvm::BasicBlock::Create(context, "opened", function);
    llvm::BasicBlock* loop =
        llvm::BasicBlock::Create(context, "loop", function);
    llvm::BasicBlock* read =
        llvm::BasicBlock::Create(context, "read", function);
    llvm::BasicBlock* progress =
        llvm::BasicBlock::Create(context, "progress", function);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(context, "done", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* path =
        nul_terminated_path(function->getArg(0), function->getArg(1));
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::Value* zero = llvm::ConstantInt::get(i64, 0);
    llvm::Value* fd_value =
        syscall({llvm::ConstantInt::get(i64, abi.openat),
                 llvm::ConstantInt::get(i64, static_cast<u64>(AT_FDCWD)),
                 builder_.CreatePtrToInt(path, i64),
                 llvm::ConstantInt::get(i64, O_RDONLY), zero, zero, zero});
    llvm::Value* negative = builder_.CreateICmpSLT(fd_value, zero, "negative");
    builder_.CreateCondBr(negative, missing, opened);
    builder_.SetInsertPoint(missing);
    builder_.CreateRet(llvm::ConstantInt::get(builder_.getInt64Ty(), -1));
    builder_.SetInsertPoint(opened);
    builder_.CreateBr(loop);
    builder_.SetInsertPoint(loop);
    llvm::PHINode* total = builder_.CreatePHI(usize_, 2, "total");
    llvm::PHINode* into = builder_.CreatePHI(builder_.getPtrTy(), 2, "into");
    llvm::PHINode* left = builder_.CreatePHI(usize_, 2, "left");
    total->addIncoming(llvm::ConstantInt::get(usize_, 0), opened);
    into->addIncoming(function->getArg(2), opened);
    left->addIncoming(function->getArg(3), opened);
    llvm::Value* more =
        builder_.CreateICmpUGT(left, llvm::ConstantInt::get(usize_, 0), "more");
    builder_.CreateCondBr(more, read, done);
    builder_.SetInsertPoint(read);
    llvm::Value* remaining = left;
    llvm::Value* count =
        syscall({llvm::ConstantInt::get(i64, abi.read), fd_value,
                 builder_.CreatePtrToInt(into, i64),
                 // The length rides the ABI's full-word register.
                 builder_.CreateZExtOrTrunc(remaining, i64), zero, zero, zero});
    llvm::Value* stopped = builder_.CreateICmpSLE(count, zero, "stopped");
    builder_.CreateCondBr(stopped, done, progress);
    builder_.SetInsertPoint(progress);
    llvm::Value* advance = builder_.CreateZExtOrTrunc(count, usize_, "advance");
    llvm::Value* next_total = builder_.CreateAdd(total, advance, "next_total");
    llvm::Value* added =
        builder_.CreateGEP(builder_.getInt8Ty(), into, advance, "added");
    llvm::Value* next_left = builder_.CreateSub(left, advance, "next_left");
    total->addIncoming(next_total, progress);
    into->addIncoming(added, progress);
    left->addIncoming(next_left, progress);
    builder_.CreateBr(loop);
    builder_.SetInsertPoint(done);
    syscall({llvm::ConstantInt::get(i64, abi.close), fd_value, zero, zero, zero,
             zero, zero});
    builder_.CreateRet(
        builder_.CreateZExtOrTrunc(total, builder_.getInt64Ty(), "widened"));
  }

  void build_write_file_syscall() {
    SyscallAbi abi{};
    CHECK_MSG(syscall_abi(abi),
              "freestanding runtime on an unsupported target");
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getInt64Ty(),
        {builder_.getPtrTy(), usize_, builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime("alcy_write_file", type);
    function->setDoesNotThrow();
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock* missing =
        llvm::BasicBlock::Create(context, "missing", function);
    llvm::BasicBlock* opened =
        llvm::BasicBlock::Create(context, "opened", function);
    llvm::BasicBlock* loop =
        llvm::BasicBlock::Create(context, "loop", function);
    llvm::BasicBlock* write =
        llvm::BasicBlock::Create(context, "write", function);
    llvm::BasicBlock* progress =
        llvm::BasicBlock::Create(context, "progress", function);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(context, "done", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* path =
        nul_terminated_path(function->getArg(0), function->getArg(1));
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::Value* zero = llvm::ConstantInt::get(i64, 0);
    llvm::Value* fd_value =
        syscall({llvm::ConstantInt::get(i64, abi.openat),
                 llvm::ConstantInt::get(i64, static_cast<u64>(AT_FDCWD)),
                 builder_.CreatePtrToInt(path, i64),
                 llvm::ConstantInt::get(i64, O_WRONLY | O_CREAT | O_TRUNC),
                 // 0666 octal = 438 decimal.
                 llvm::ConstantInt::get(i64, 438), zero, zero});
    llvm::Value* negative = builder_.CreateICmpSLT(fd_value, zero, "negative");
    builder_.CreateCondBr(negative, missing, opened);
    builder_.SetInsertPoint(missing);
    builder_.CreateRet(llvm::ConstantInt::get(builder_.getInt64Ty(), -1));
    builder_.SetInsertPoint(opened);
    builder_.CreateBr(loop);
    builder_.SetInsertPoint(loop);
    llvm::PHINode* total = builder_.CreatePHI(usize_, 2, "total");
    llvm::PHINode* from = builder_.CreatePHI(builder_.getPtrTy(), 2, "from");
    llvm::PHINode* left = builder_.CreatePHI(usize_, 2, "left");
    total->addIncoming(llvm::ConstantInt::get(usize_, 0), opened);
    from->addIncoming(function->getArg(2), opened);
    left->addIncoming(function->getArg(3), opened);
    llvm::Value* more =
        builder_.CreateICmpUGT(left, llvm::ConstantInt::get(usize_, 0), "more");
    builder_.CreateCondBr(more, write, done);
    builder_.SetInsertPoint(write);
    llvm::Value* count =
        syscall({llvm::ConstantInt::get(i64, abi.write), fd_value,
                 builder_.CreatePtrToInt(from, i64), left, zero, zero, zero});
    llvm::Value* stopped = builder_.CreateICmpSLE(count, zero, "stopped");
    builder_.CreateCondBr(stopped, done, progress);
    builder_.SetInsertPoint(progress);
    llvm::Value* advance = builder_.CreateZExtOrTrunc(count, usize_, "advance");
    llvm::Value* next_total = builder_.CreateAdd(total, advance, "next_total");
    llvm::Value* advanced =
        builder_.CreateGEP(builder_.getInt8Ty(), from, advance, "advanced");
    llvm::Value* next_left = builder_.CreateSub(left, advance, "next_left");
    total->addIncoming(next_total, progress);
    from->addIncoming(advanced, progress);
    left->addIncoming(next_left, progress);
    builder_.CreateBr(loop);
    builder_.SetInsertPoint(done);
    syscall({llvm::ConstantInt::get(i64, abi.close), fd_value, zero, zero, zero,
             zero, zero});
    builder_.CreateRet(
        builder_.CreateZExtOrTrunc(total, builder_.getInt64Ty(), "widened"));
  }

  // A freestanding program has no libc to open a file with, so the
  // file answers are the error and the std still links.
  void build_file_stubs() {
    if (declared("alcy_file_size")) {
      llvm::FunctionType* type = llvm::FunctionType::get(
          builder_.getInt64Ty(), {builder_.getPtrTy(), usize_}, false);
      llvm::Function* function = runtime("alcy_file_size", type);
      function->setDoesNotThrow();
      llvm::BasicBlock* entry =
          llvm::BasicBlock::Create(module_.getContext(), "entry", function);
      builder_.SetInsertPoint(entry);
      builder_.CreateRet(llvm::ConstantInt::get(builder_.getInt64Ty(), -1));
    }
    if (declared("alcy_read_file_into")) {
      llvm::FunctionType* type = llvm::FunctionType::get(
          builder_.getInt64Ty(),
          {builder_.getPtrTy(), usize_, builder_.getPtrTy(), usize_}, false);
      llvm::Function* function = runtime("alcy_read_file_into", type);
      function->setDoesNotThrow();
      llvm::BasicBlock* entry =
          llvm::BasicBlock::Create(module_.getContext(), "entry", function);
      builder_.SetInsertPoint(entry);
      builder_.CreateRet(llvm::ConstantInt::get(builder_.getInt64Ty(), -1));
    }
    if (declared("alcy_write_file")) {
      llvm::FunctionType* type = llvm::FunctionType::get(
          builder_.getInt64Ty(),
          {builder_.getPtrTy(), usize_, builder_.getPtrTy(), usize_}, false);
      llvm::Function* function = runtime("alcy_write_file", type);
      function->setDoesNotThrow();
      llvm::BasicBlock* entry =
          llvm::BasicBlock::Create(module_.getContext(), "entry", function);
      builder_.SetInsertPoint(entry);
      builder_.CreateRet(llvm::ConstantInt::get(builder_.getInt64Ty(), -1));
    }
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
    if (freestanding_ && !windows_) {
      // No libc `abort`; exit with the status a hosted abort reports.
      exit_now(134);
      return;
    }
    if (windows_) {
      llvm::Function* quit =
          libc("ExitProcess",
               llvm::FunctionType::get(builder_.getVoidTy(),
                                       {builder_.getInt32Ty()}, false));
      builder_.CreateCall(quit,
                          {llvm::ConstantInt::get(builder_.getInt32Ty(), 134)});
      builder_.CreateUnreachable();
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

  // One table lookup: the name, length and start of the entry with the
  // greatest start below `pc` and in range, or null and zero. Builds
  // its own scan loop and leaves the insert point past it. The caller
  // only looks up a table it defined beside the function table, so
  // entries is never null where this runs. With `lowest` set, only the
  // entries whose start is at or above it count: a call site lies in the
  // text of the function that holds the call, so the site a frame reads
  // is above the start of the function the frame matched.
  struct TableMatch {
    llvm::Value* name = nullptr;
    llvm::Value* len = nullptr;
    llvm::BasicBlock* matched = nullptr;
    llvm::Value* start = nullptr;
  };

  TableMatch build_table_lookup(llvm::Value* pc,
                                llvm::Value* entries,
                                llvm::Value* lowest = nullptr) {
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

    // The scan keeps the match with the greatest start below the
    // return address. The comparison is strict: a call that ends its
    // function returns to the address the next function begins at, and
    // a frame belongs to the function that made the call. Entries are
    // not sorted, and an entry whose start is too far below the address
    // (libc, another mapping) is not a match.
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
    llvm::Value* after = builder_.CreateICmpULT(start_word, pc_word, "after");
    llvm::Value* near = builder_.CreateICmpULT(
        builder_.CreateSub(pc_word, start_word),
        llvm::ConstantInt::get(usize_, MAX_BACKTRACE_DISTANCE), "near");
    llvm::Value* candidate = builder_.CreateAnd(after, near, "candidate");
    if (lowest != nullptr) {
      llvm::Value* above = builder_.CreateICmpUGE(
          start_word, builder_.CreatePtrToInt(lowest, usize_, "low_word"),
          "above");
      candidate = builder_.CreateAnd(candidate, above, "candidate");
    }
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
    return {best_name, best_len, matched, best_start};
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

    // The frame reads only the calls its own function made. The site
    // table spans the module, and the nearest entry below a return
    // address can be a call another function made, so the lookup starts
    // no lower than the function the frame matched.
    builder_.SetInsertPoint(site);
    TableMatch at = build_table_lookup(pc, site_entries, frame.start);
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
    build_faithful_io("alcy_sys_write", /*is_write=*/true);
  }

  void build_sys_read() {
    build_faithful_io("alcy_sys_read", /*is_write=*/false);
  }

  void build_error_kind() {
    llvm::FunctionType* type = llvm::FunctionType::get(
        builder_.getInt32Ty(), {builder_.getInt32Ty()}, false);
    llvm::Function* function = runtime("alcy_error_kind", type);
    function->setDoesNotThrow();
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* code = function->getArg(0);
    i32 e_intr = 4;
    i32 e_again = 11;
    i32 e_noent = 2;
    i32 e_acces = 13;
    i32 e_pipe = 32;
    i32 e_inval = 22;
    if (mach_o_) {
      // BSD errno numbering: only EAGAIN differs from the Linux table.
      e_intr = 4;
      e_again = 35;
      e_noent = 2;
      e_acces = 13;
      e_pipe = 32;
      e_inval = 22;
    }
    struct Mapping {
      i32 number;
      i32 kind;
    };
    const Mapping mappings[] = {{e_intr, 1},  {e_again, 2}, {e_noent, 3},
                                {e_acces, 4}, {e_pipe, 5},  {e_inval, 6}};
    llvm::BasicBlock* current = entry;
    for (const Mapping& mapping : mappings) {
      llvm::BasicBlock* matched =
          llvm::BasicBlock::Create(context, "matched", function);
      llvm::BasicBlock* next =
          llvm::BasicBlock::Create(context, "next", function);
      builder_.SetInsertPoint(current);
      llvm::Value* equal = builder_.CreateICmpEQ(
          code, llvm::ConstantInt::get(builder_.getInt32Ty(), mapping.number),
          "equal");
      builder_.CreateCondBr(equal, matched, next);
      builder_.SetInsertPoint(matched);
      builder_.CreateRet(
          llvm::ConstantInt::get(builder_.getInt32Ty(), mapping.kind));
      current = next;
    }
    builder_.SetInsertPoint(current);
    builder_.CreateRet(llvm::ConstantInt::get(builder_.getInt32Ty(), 0));
  }

  // The faithful descriptor call: one `write(2)`/`read(2)`,
  // answering the bytes or `-(errno)`. A short write and an interrupted
  // call are the caller's to see; the retrying loops stay with the
  // print hooks and the std's own helpers.
  void build_faithful_io(std::string_view name, bool is_write) {
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::FunctionType* type = llvm::FunctionType::get(
        i64, {builder_.getInt32Ty(), builder_.getPtrTy(), usize_}, false);
    llvm::Function* function = runtime(name, type);
    function->setDoesNotThrow();
    llvm::LLVMContext& context = module_.getContext();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(context, "entry", function);
    builder_.SetInsertPoint(entry);
    llvm::Value* fd_value = function->getArg(0);
    llvm::Value* data = function->getArg(1);
    llvm::Value* len = function->getArg(2);
    if (freestanding_ && !windows_) {
      // The kernel answers `-errno` itself; there is nothing to fetch.
      SyscallAbi abi{};
      CHECK_MSG(syscall_abi(abi),
                "freestanding runtime on an unsupported target");
      llvm::Value* zero = llvm::ConstantInt::get(i64, 0);
      llvm::Value* count = syscall({
          llvm::ConstantInt::get(i64, is_write ? abi.write : abi.read),
          builder_.CreateSExt(fd_value, i64),
          builder_.CreatePtrToInt(data, i64),
          builder_.CreateZExtOrTrunc(len, i64),
          zero,
          zero,
          zero,
      });
      builder_.CreateRet(count);
      return;
    }
    llvm::Value* count = io_call(is_write, fd_value, data, len);
    llvm::BasicBlock* failed =
        llvm::BasicBlock::Create(context, "failed", function);
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(context, "ok", function);
    llvm::Value* zero = llvm::ConstantInt::get(count->getType(), 0);
    llvm::Value* refused = builder_.CreateICmpSLT(count, zero, "refused");
    builder_.CreateCondBr(refused, failed, ok);

    builder_.SetInsertPoint(failed);
    llvm::Value* negated =
        builder_.CreateNeg(builder_.CreateSExt(errno_code(), i64), "errno");
    builder_.CreateRet(negated);

    builder_.SetInsertPoint(ok);
    builder_.CreateRet(builder_.CreateSExtOrTrunc(count, i64, "count"));
  }

  // One libc call, with the raw signed result the caller compares; the
  // Windows entry points take and return the narrower count.
  llvm::Value* io_call(bool is_write,
                       llvm::Value* fd_value,
                       llvm::Value* data,
                       llvm::Value* len) {
    if (windows_) {
      return descriptor_io(is_write, fd_value, data, len);
    }
    llvm::Function* function =
        libc(is_write ? "write" : "read",
             llvm::FunctionType::get(
                 usize_, {builder_.getInt32Ty(), builder_.getPtrTy(), usize_},
                 false));
    return builder_.CreateCall(function, {fd_value, data, len}, "count");
  }

  // The number the last failed call left, read from the C library's
  // thread-local slot.
  llvm::Value* errno_code() {
    if (windows_) {
      // GetLastError: the last kernel call leaves a number; nothing
      // here interprets it.
      llvm::Function* slot =
          libc("GetLastError",
               llvm::FunctionType::get(builder_.getInt32Ty(), {}, false));
      return builder_.CreateCall(slot, {}, "errno");
    }
    const char* slot_name = mach_o_ ? "__error" : "__errno_location";
    llvm::Function* slot = libc(
        slot_name, llvm::FunctionType::get(builder_.getPtrTy(), {}, false));
    return builder_.CreateLoad(builder_.getInt32Ty(),
                               builder_.CreateCall(slot, {}, "slot"), "errno");
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

  // The largest object the address space can hold. A request past it is
  // refused here rather than handed to the host allocator, which cannot
  // answer for it: `posix_memalign` aborts inside a sanitizer instead of
  // reporting failure, and the freestanding path's mapping length adds
  // the alignment and the header to the size, a sum that would wrap into
  // a block smaller than the caller asked for.
  llvm::Value* unrepresentable_request(llvm::Value* size, llvm::Value* align) {
    llvm::Constant* max_object = llvm::ConstantInt::get(
        usize_, llvm::APInt::getSignedMaxValue(usize_->getBitWidth()));
    llvm::Value* size_too_large =
        builder_.CreateICmpUGT(size, max_object, "size_too_large");
    llvm::Value* align_too_large =
        builder_.CreateICmpUGT(align, max_object, "align_too_large");
    return builder_.CreateOr(size_too_large, align_too_large,
                             "unrepresentable");
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
    if (freestanding_ && !windows_) {
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
    llvm::Value* invalid = builder_.CreateOr(
        builder_.CreateOr(no_align, not_power_of_two, "bad_alignment"),
        unrepresentable_request(size, align), "invalid");
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
      // VirtualAlloc gives page-granular blocks, which answer every
      // alignment the page covers. A request past it is refused: the
      // kernel has nothing to answer with.
      llvm::Value* over_page = builder_.CreateICmpUGT(
          align, llvm::ConstantInt::get(usize_, 4096), "over_page");
      llvm::BasicBlock* page_ok =
          llvm::BasicBlock::Create(context, "page_ok", function);
      builder_.CreateCondBr(over_page, reject, page_ok);
      builder_.SetInsertPoint(page_ok);
      // A zero-size request still returns a distinct, freeable block,
      // the same as the POSIX path.
      llvm::Value* at_least_one = builder_.CreateSelect(
          builder_.CreateICmpEQ(size, zero, "size_is_zero"),
          llvm::ConstantInt::get(usize_, 1), size, "at_least_one");
      llvm::Function* valloc = libc(
          "VirtualAlloc", llvm::FunctionType::get(
                              builder_.getPtrTy(),
                              {builder_.getPtrTy(), usize_,
                               builder_.getInt32Ty(), builder_.getInt32Ty()},
                              false));
      llvm::Value* block = builder_.CreateCall(
          valloc,
          {null_pointer(), at_least_one,
           // MEM_COMMIT | MEM_RESERVE
           llvm::ConstantInt::get(builder_.getInt32Ty(), 0x3000),
           // PAGE_READWRITE
           llvm::ConstantInt::get(builder_.getInt32Ty(), 0x04)},
          "block");
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
    if (freestanding_ && !windows_) {
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

    if (windows_) {
      release->eraseFromParent();
      // MEM_RELEASE: the pointer is the whole reservation, so the size
      // the caller carries is not needed and the kernel wants none.
      builder_.SetInsertPoint(entry);
      llvm::Value* is_null =
          builder_.CreateICmpEQ(block, null_pointer(), "is_null");
      llvm::BasicBlock* release_win =
          llvm::BasicBlock::Create(context, "release_win", function);
      builder_.CreateCondBr(is_null, done, release_win);
      builder_.SetInsertPoint(release_win);
      llvm::Function* vfree = libc(
          "VirtualFree",
          llvm::FunctionType::get(
              builder_.getInt32Ty(),
              {builder_.getPtrTy(), usize_, builder_.getInt32Ty()}, false));
      // 0x8000 is MEM_RELEASE; the size must be 0 with it.
      builder_.CreateCall(
          vfree, {block, llvm::ConstantInt::get(usize_, 0),
                  llvm::ConstantInt::get(builder_.getInt32Ty(), 0x8000)});
      builder_.CreateBr(done);
      builder_.SetInsertPoint(done);
      builder_.CreateRetVoid();
      return;
    }

    builder_.SetInsertPoint(entry);
    llvm::Value* is_null =
        builder_.CreateICmpEQ(block, null_pointer(), "is_null");
    builder_.CreateCondBr(is_null, done, release);

    builder_.SetInsertPoint(release);
    builder_.CreateCall(
        libc("free", llvm::FunctionType::get(builder_.getVoidTy(),
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
    llvm::Value* invalid = builder_.CreateOr(
        builder_.CreateOr(no_align, not_power_of_two, "bad_alignment"),
        unrepresentable_request(size, align), "invalid");
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
  // Whether a discardable definition needs a comdat to dedup by name:
  // only COFF resolves weak externals through per-object defaults.
  bool comdat_;
  // Whether the C library's error slot is spelled `__error` (Mach-O)
  // rather than `__errno_location`.
  bool mach_o_;
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
