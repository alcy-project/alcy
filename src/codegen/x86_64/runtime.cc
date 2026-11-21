// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/runtime.h"

#include <optional>
#include <string>
#include <string_view>

#include "codegen/x86_64/isa.h"
#include "codegen/x86_64/minst.h"
#include "codegen/x86_64/rodata.h"
#include "debug/dcheck.h"
#include "fpag/base/numeric.h"

namespace codegen::x86_64::runtime {

namespace {

// One write(2) of the bytes `buf` and `len` name, to the descriptor `fd`
// names. The count moves first, because the bytes often arrive in the
// register the count is about to take.
void emit_write(Body& body,
                const i64 fd,
                const Operand& buf,
                const Operand& len) {
  body.insts.push_back(make_inst(MOp::Mov, 8, Operand{Reg::Rdx}, len));
  body.insts.push_back(make_inst(MOp::Mov, 8, Operand{Reg::Rsi}, buf));
  body.insts.push_back(
      make_inst(MOp::Mov, 4, Operand{Reg::Rdi}, Operand{Imm{fd}}));
  body.insts.push_back(
      make_inst(MOp::Mov, 4, Operand{Reg::Rax}, Operand{Imm{1}}));
  body.insts.push_back(make_inst(MOp::Syscall, 8));
}

// The bytes one write needs, under a name the data section answers to.
SymbolRef text(Body& body, RoData& rodata, const std::string_view bytes) {
  const std::string& symbol = rodata.intern(bytes);
  for (u32 at = 0; at < body.symbols.size(); ++at) {
    if (body.symbols[at] == symbol) {
      return SymbolRef{at};
    }
  }
  body.symbols.push_back(symbol);
  return SymbolRef{static_cast<u32>(body.symbols.size() - 1)};
}

// The one writer a print family shares: `write` of the two words the
// caller handed over, to the descriptor the family is for. The entry and
// exit sequences are allocation's to put in, the same as a program's own
// functions.
Body build_writer(const std::string_view name,
                  RoData& rodata,
                  const i64 descriptor,
                  const bool newline) {
  Body body;
  (void)name;
  emit_write(body, descriptor, Operand{Reg::Rdi}, Operand{Reg::Rsi});
  if (newline) {
    const SymbolRef newline_ref = text(body, rodata, "\n");
    emit_write(body, descriptor, Operand{SymbolRef{newline_ref.id}},
               Operand{Imm{1}});
  }
  body.insts.push_back(make_inst(MOp::Ret, 8));
  return body;
}

Body build_panic(RoData& rodata) {
  Body body;
  // The message and the place it was written arrive in the argument
  // registers, and both outlive the writes between them, so they move to
  // the registers a call preserves.
  body.insts.push_back(
      make_inst(MOp::Mov, 8, Operand{Reg::Rbx}, Operand{Reg::Rdi}));
  body.insts.push_back(
      make_inst(MOp::Mov, 8, Operand{Reg::R12}, Operand{Reg::Rsi}));
  body.insts.push_back(
      make_inst(MOp::Mov, 8, Operand{Reg::R13}, Operand{Reg::Rdx}));
  body.insts.push_back(
      make_inst(MOp::Mov, 8, Operand{Reg::R14}, Operand{Reg::Rcx}));
  body.insts.push_back(make_inst(MOp::Push, 8, Operand{Reg::Rbx}));
  body.insts.push_back(make_inst(MOp::Push, 8, Operand{Reg::R12}));
  body.insts.push_back(make_inst(MOp::Push, 8, Operand{Reg::R13}));
  body.insts.push_back(make_inst(MOp::Push, 8, Operand{Reg::R14}));

  const SymbolRef prefix = text(body, rodata, "panic at ");
  const SymbolRef colon = text(body, rodata, ": ");
  const SymbolRef newline = text(body, rodata, "\n");

  // The place first, the way the trace reads: `panic at <site>: <msg>`.
  emit_write(body, 2, Operand{prefix}, Operand{Imm{9}});
  emit_write(body, 2, Operand{Reg::R13}, Operand{Reg::R14});
  emit_write(body, 2, Operand{colon}, Operand{Imm{2}});
  emit_write(body, 2, Operand{Reg::Rbx}, Operand{Reg::R12});
  emit_write(body, 2, Operand{newline}, Operand{Imm{1}});

  // A panic ends the process, and nothing past the ask runs.
  body.insts.push_back(
      make_inst(MOp::Mov, 4, Operand{Reg::Rdi}, Operand{Imm{1}}));
  body.insts.push_back(
      make_inst(MOp::Mov, 4, Operand{Reg::Rax}, Operand{Imm{60}}));
  body.insts.push_back(make_inst(MOp::Syscall, 8));
  body.insts.push_back(make_inst(MOp::Ud2, 8));
  return body;
}

// The descriptor calls a program reaches for by name: the call answers
// the byte count, or the negated error number the call reported. The
// three arguments already sit where the kernel reads them.
Body build_syscall_body(const i64 number) {
  Body body;
  body.insts.push_back(
      make_inst(MOp::Mov, 4, Operand{Reg::Rax}, Operand{Imm{number}}));
  body.insts.push_back(make_inst(MOp::Syscall, 8));
  body.insts.push_back(make_inst(MOp::Ret, 8));
  return body;
}

// The name one external callee answers to, in this body's own table.
SymbolRef callee(Body& body, const std::string_view name) {
  for (u32 at = 0; at < body.symbols.size(); ++at) {
    if (body.symbols[at] == name) {
      return SymbolRef{at};
    }
  }
  body.symbols.emplace_back(name);
  return SymbolRef{static_cast<u32>(body.symbols.size() - 1)};
}

// Maps the platform's errno numbers to the portable error kinds the
// std asks for. Linux numbering is the hosted target here.
Body build_error_kind() {
  Body body;
  struct Mapping {
    i32 number;
    i32 kind;
  };
  constexpr Mapping mappings[] = {{4, 1},  {11, 2}, {2, 3},
                                  {13, 4}, {32, 5}, {22, 6}};
  for (const Mapping& mapping : mappings) {
    body.insts.push_back(make_inst(MOp::Cmp, 4, Operand{Reg::Rdi},
                                   Operand{Imm{mapping.number}}));
    const u32 next = body.label_count++;
    body.insts.push_back(make_jcc(Cond::E, Operand{Label{next}}));
    body.insts.push_back(
        make_inst(MOp::Mov, 4, Operand{Reg::Rax}, Operand{Imm{mapping.kind}}));
    body.insts.push_back(make_inst(MOp::Ret, 8));
    MInst at_next = make_inst(MOp::Nop, 8);
    at_next.label = next;
    body.insts.push_back(at_next);
  }
  body.insts.push_back(
      make_inst(MOp::Xor, 4, Operand{Reg::Rax}, Operand{Reg::Rax}));
  body.insts.push_back(make_inst(MOp::Ret, 8));
  return body;
}

// The allocator. The platform's own aligned allocator answers, and a
// block it refuses ends the process the way a panic does; the program
// has nothing to run without the memory it asked for.
[[maybe_unused]] Body build_alloc(RoData& rodata) {
  Body body;
  // posix_memalign(&slot, align, size): the size arrived in `rdi` and
  // the alignment in `rsi`, which is not the order the call reads them.
  // The alignment itself has to reach the pointer's width: the platform
  // refuses a smaller one, where the source's alignment of a word is
  // four and the answer it wants is a block the word fits in.
  body.insts.push_back(
      make_inst(MOp::Mov, 8, Operand{Reg::Rdx}, Operand{Reg::Rdi}));
  body.insts.push_back(
      make_inst(MOp::Cmp, 8, Operand{Reg::Rsi}, Operand{Imm{8}}));
  const u32 wide_enough = body.label_count++;
  body.insts.push_back(make_jcc(Cond::Ae, Operand{Label{wide_enough}}));
  body.insts.push_back(
      make_inst(MOp::Mov, 8, Operand{Reg::Rsi}, Operand{Imm{8}}));
  MInst widened = make_inst(MOp::Nop, 8);
  widened.label = wide_enough;
  body.insts.push_back(widened);
  // The slot the result lands in, reserved below `rsp` for the call:
  // the callee pushes its own frame past it, and the frame the entry
  // sequence made is what the exit restores.
  body.insts.push_back(
      make_inst(MOp::Sub, 8, Operand{Reg::Rsp}, Operand{Imm{16}}));
  body.insts.push_back(make_inst(
      MOp::Lea, 8, Operand{Reg::Rdi},
      Operand{Mem{
          .base = Reg::Rsp, .index = std::nullopt, .scale = 1, .disp = 0}}));
  const SymbolRef memalign = callee(body, "posix_memalign");
  body.insts.push_back(
      make_inst(MOp::Call, 8, Operand{SymbolRef{memalign.id}}));
  // A refusal is the end: there is nothing to answer with.
  body.insts.push_back(
      make_inst(MOp::Cmp, 4, Operand{Reg::Rax}, Operand{Imm{0}}));
  const u32 failed = body.label_count++;
  const u32 ok = body.label_count++;
  MInst to_ok = make_jcc(Cond::E, Operand{Label{ok}});
  body.insts.push_back(to_ok);
  MInst at_failed = make_inst(MOp::Mov, 4, Operand{Reg::Rdi}, Operand{Imm{1}});
  at_failed.label = failed;
  body.insts.push_back(at_failed);
  const SymbolRef abort = callee(body, "abort");
  body.insts.push_back(make_inst(MOp::Call, 8, Operand{SymbolRef{abort.id}}));
  body.insts.push_back(make_inst(MOp::Ud2, 8));
  MInst at_ok = make_inst(
      MOp::Mov, 8, Operand{Reg::Rax},
      Operand{
          Mem{.base = Reg::Rsp, .index = std::nullopt, .scale = 1, .disp = 0}});
  at_ok.label = ok;
  body.insts.push_back(at_ok);
  body.insts.push_back(make_inst(MOp::Ret, 8));
  (void)rodata;
  return body;
}

// The release. The platform's own free answers; a null block was never
// a block, and free reads it the same way.
[[maybe_unused]] Body build_dealloc(RoData& rodata) {
  Body body;
  const SymbolRef free_fn = callee(body, "free");
  body.insts.push_back(make_inst(MOp::Call, 8, Operand{SymbolRef{free_fn.id}}));
  body.insts.push_back(make_inst(MOp::Ret, 8));
  (void)rodata;
  return body;
}

// A freestanding heap asks the kernel for whole pages. The word before
// the block remembers the mapping's length, so release needs only one
// `munmap`. Both paths are plain syscalls: no C library is linked.
Body build_alloc_fs() {
  Body body;
  const auto mov = [&](MOp op, Operand dst, Operand src) {
    body.insts.push_back(make_inst(op, 8, dst, src));
  };
  // length = (size + 16 + 4095) & ~4095
  mov(MOp::Mov, Operand{Reg::Rcx}, Operand{Reg::Rdi});
  mov(MOp::Add, Operand{Reg::Rcx}, Operand{Imm{4111}});
  mov(MOp::And, Operand{Reg::Rcx}, Operand{Imm{~4095LL}});
  // mmap(NULL, length, PROT_READ|PROT_WRITE,
  //      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
  mov(MOp::Mov, Operand{Reg::Rax}, Operand{Imm{9}});
  mov(MOp::Xor, Operand{Reg::Rdi}, Operand{Reg::Rdi});
  mov(MOp::Mov, Operand{Reg::Rsi}, Operand{Reg::Rcx});
  mov(MOp::Mov, Operand{Reg::Rdx}, Operand{Imm{3}});
  mov(MOp::Mov, Operand{Reg::R10}, Operand{Imm{0x22}});
  mov(MOp::Mov, Operand{Reg::R8}, Operand{Imm{-1}});
  mov(MOp::Xor, Operand{Reg::R9}, Operand{Reg::R9});
  body.insts.push_back(make_inst(MOp::Syscall, 8));
  mov(MOp::Cmp, Operand{Reg::Rax}, Operand{Imm{-4095}});
  const u32 ok = body.label_count++;
  // A valid pointer is below -4095 when read unsigned; kernel errors are
  // the values at or above it.
  body.insts.push_back(make_jcc(Cond::B, Operand{Label{ok}}));
  // A refused mapping ends the process with status 1.
  mov(MOp::Mov, Operand{Reg::Rdi}, Operand{Imm{1}});
  mov(MOp::Mov, Operand{Reg::Rax}, Operand{Imm{60}});
  body.insts.push_back(make_inst(MOp::Syscall, 8));
  MInst joined = make_inst(MOp::Nop, 8);
  joined.label = ok;
  body.insts.push_back(joined);
  // A syscall clobbers rcx and r11; the length survives in rsi, where
  // it was handed to mmap as the mapping size.
  mov(MOp::Mov,
      Operand{
          Mem{.base = Reg::Rax, .index = std::nullopt, .scale = 1, .disp = 0}},
      Operand{Reg::Rsi});
  mov(MOp::Add, Operand{Reg::Rax}, Operand{Imm{16}});
  body.insts.push_back(make_inst(MOp::Ret, 8));
  return body;
}

Body build_dealloc_fs() {
  Body body;
  body.insts.push_back(
      make_inst(MOp::Sub, 8, Operand{Reg::Rdi}, Operand{Imm{16}}));
  body.insts.push_back(make_inst(
      MOp::Mov, 8, Operand{Reg::Rsi},
      Operand{Mem{
          .base = Reg::Rdi, .index = std::nullopt, .scale = 1, .disp = 0}}));
  body.insts.push_back(
      make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{Imm{11}}));
  body.insts.push_back(make_inst(MOp::Syscall, 8));
  body.insts.push_back(make_inst(MOp::Ret, 8));
  return body;
}

// The process arguments were saved by the entry wrapper. `alcy_arg`
// answers a borrowed slice into the process's own argument strings.
Body build_arg_count() {
  Body body;
  const SymbolRef argc = callee(body, "alcy_argc");
  body.insts.push_back(
      make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{argc}));
  body.insts.push_back(make_inst(
      MOp::Mov, 8, Operand{Reg::Rax},
      Operand{Mem{
          .base = Reg::Rax, .index = std::nullopt, .scale = 1, .disp = 0}}));
  body.insts.push_back(make_inst(MOp::Ret, 8));
  return body;
}

Body build_arg() {
  Body body;
  const auto mov = [&](MOp op, u8 width, Operand dst, Operand src) {
    body.insts.push_back(make_inst(op, width, dst, src));
  };
  const SymbolRef argv = callee(body, "alcy_argv");
  mov(MOp::Mov, 8, Operand{Reg::Rax}, Operand{argv});
  mov(MOp::Mov, 8, Operand{Reg::Rax},
      Operand{
          Mem{.base = Reg::Rax, .index = std::nullopt, .scale = 1, .disp = 0}});
  mov(MOp::Mov, 8, Operand{Reg::Rcx}, Operand{Reg::Rdi});
  mov(MOp::Lea, 8, Operand{Reg::Rdx},
      Operand{Mem{.base = Reg::Rax, .index = Reg::Rcx, .scale = 8, .disp = 0}});
  mov(MOp::Mov, 8, Operand{Reg::Rdx},
      Operand{
          Mem{.base = Reg::Rdx, .index = std::nullopt, .scale = 1, .disp = 0}});
  mov(MOp::Mov, 8, Operand{Reg::Rsi}, Operand{Reg::Rdx});
  const u32 scan = body.label_count++;
  const u32 done = body.label_count++;
  MInst at_scan = make_inst(
      MOp::Movzx, 1, Operand{Reg::Rcx},
      Operand{
          Mem{.base = Reg::Rsi, .index = std::nullopt, .scale = 1, .disp = 0}});
  at_scan.label = scan;
  body.insts.push_back(at_scan);
  body.insts.push_back(
      make_inst(MOp::Cmp, 1, Operand{Reg::Rcx}, Operand{Imm{0}}));
  body.insts.push_back(make_jcc(Cond::E, Operand{Label{done}}));
  body.insts.push_back(
      make_inst(MOp::Add, 8, Operand{Reg::Rsi}, Operand{Imm{1}}));
  body.insts.push_back(make_inst(MOp::Jmp, 8, Operand{Label{scan}}));
  MInst at_done = make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{Reg::Rdx});
  at_done.label = done;
  body.insts.push_back(at_done);
  body.insts.push_back(
      make_inst(MOp::Sub, 8, Operand{Reg::Rsi}, Operand{Reg::Rax}));
  body.insts.push_back(
      make_inst(MOp::Mov, 8, Operand{Reg::Rdx}, Operand{Reg::Rsi}));
  body.insts.push_back(make_inst(MOp::Ret, 8));
  return body;
}

// Copy a borrowed path into a NUL-terminated stack buffer and return the
// buffer's address in `Reg::Rdi`, ready for the `openat` syscall.
void path_buffer(Body& body, u8 width) {
  const auto mov = [&](MOp op, Operand dst, Operand src) {
    body.insts.push_back(make_inst(op, 8, dst, src));
  };
  // rdi = path bytes, rsi = byte count
  body.insts.push_back(
      make_inst(MOp::Sub, 8, Operand{Reg::Rsp}, Operand{Imm{4096}}));
  mov(MOp::Mov, Operand{Reg::Rdx}, Operand{Reg::Rsi});
  mov(MOp::Mov, Operand{Reg::Rcx}, Operand{Reg::Rsi});
  mov(MOp::Mov, Operand{Reg::Rsi}, Operand{Reg::Rdi});
  mov(MOp::Mov, Operand{Reg::Rdi}, Operand{Reg::Rsp});
  body.insts.push_back(make_inst(MOp::RepMovsb, 8));
  body.insts.push_back(make_inst(
      MOp::Mov, 1,
      Operand{Mem{.base = Reg::Rsp, .index = Reg::Rdx, .scale = 1, .disp = 0}},
      Operand{Imm{0}}));
  mov(MOp::Mov, Operand{Reg::Rsi}, Operand{Reg::Rsp});
  (void)width;
}

// openat(AT_FDCWD, path, flags, mode) -> fd in rax, or a negative errno.
void open_path(Body& body, i64 flags) {
  const auto mov = [&](MOp op, Operand dst, Operand src) {
    body.insts.push_back(make_inst(op, 8, dst, src));
  };
  mov(MOp::Mov, Operand{Reg::Rax}, Operand{Imm{257}});
  mov(MOp::Mov, Operand{Reg::Rdi}, Operand{Imm{-100}});
  mov(MOp::Mov, Operand{Reg::Rdx}, Operand{Imm{flags}});
  mov(MOp::Mov, Operand{Reg::R10}, Operand{Imm{0}});
  body.insts.push_back(make_inst(MOp::Syscall, 8));
}

// The shared tail: close the fd in r12 and keep rax as the answer.
[[maybe_unused]] void close_file(Body& body) {
  const auto mov = [&](MOp op, Operand dst, Operand src) {
    body.insts.push_back(make_inst(op, 8, dst, src));
  };
  mov(MOp::Mov, Operand{Reg::R12}, Operand{Reg::Rax});
  mov(MOp::Mov, Operand{Reg::Rdi}, Operand{Reg::Rax});
  mov(MOp::Mov, Operand{Reg::Rax}, Operand{Imm{3}});
  body.insts.push_back(make_inst(MOp::Syscall, 8));
  mov(MOp::Mov, Operand{Reg::Rax}, Operand{Reg::R12});
}

Body build_file_size() {
  Body body;
  const auto mov = [&](MOp op, Operand dst, Operand src) {
    body.insts.push_back(make_inst(op, 8, dst, src));
  };
  path_buffer(body, 8);
  open_path(body, 0);  // O_RDONLY
  body.insts.push_back(
      make_inst(MOp::Cmp, 8, Operand{Reg::Rax}, Operand{Imm{-4095}}));
  const u32 ok = body.label_count++;
  // Linux returns negative errno values: read unsigned, a valid fd or
  // pointer is below the -4095 threshold and an error is at or above it.
  body.insts.push_back(make_jcc(Cond::B, Operand{Label{ok}}));
  body.insts.push_back(
      make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{Imm{-1}}));
  body.insts.push_back(make_inst(MOp::Ret, 8));
  MInst joined = make_inst(MOp::Nop, 8);
  joined.label = ok;
  body.insts.push_back(joined);
  mov(MOp::Mov, Operand{Reg::R12}, Operand{Reg::Rax});  // keep fd
  // fstat: rax=5, rdi=fd, rsi=stat buffer. st_size is at offset 48.
  body.insts.push_back(
      make_inst(MOp::Sub, 8, Operand{Reg::Rsp}, Operand{Imm{256}}));
  mov(MOp::Mov, Operand{Reg::Rdi}, Operand{Reg::R12});
  mov(MOp::Mov, Operand{Reg::Rsi}, Operand{Reg::Rsp});
  mov(MOp::Mov, Operand{Reg::Rax}, Operand{Imm{5}});
  body.insts.push_back(make_inst(MOp::Syscall, 8));
  mov(MOp::Mov, Operand{Reg::Rax},
      Operand{Mem{
          .base = Reg::Rsp, .index = std::nullopt, .scale = 1, .disp = 48}});
  mov(MOp::Mov, Operand{Reg::R13}, Operand{Reg::Rax});  // keep size
  // close(fd)
  mov(MOp::Mov, Operand{Reg::Rdi}, Operand{Reg::R12});
  mov(MOp::Mov, Operand{Reg::Rax}, Operand{Imm{3}});
  body.insts.push_back(make_inst(MOp::Syscall, 8));
  mov(MOp::Mov, Operand{Reg::Rax}, Operand{Reg::R13});
  body.insts.push_back(make_inst(MOp::Ret, 8));
  return body;
}

// read(fd, buf, count) or write(fd, buf, count); the fd is in rdi and
// the count in rdx when called. Kept in r12 across the close.
Body build_file_io(bool write) {
  Body body;
  const auto mov = [&](MOp op, Operand dst, Operand src) {
    body.insts.push_back(make_inst(op, 8, dst, src));
  };
  const auto slot = [](i32 disp) {
    return Operand{
        Mem{.base = Reg::Rbp, .index = std::nullopt, .scale = 1, .disp = disp}};
  };
  // Source args: rdi=path, rsi=len, rdx=buffer, rcx=count. Keep the
  // latter two in the frame: r10/r11 do not survive a syscall.
  mov(MOp::Mov, slot(-8), Operand{Reg::Rdx});
  mov(MOp::Mov, slot(-16), Operand{Reg::Rcx});
  path_buffer(body, 8);
  open_path(body, write ? 577 : 0);  // O_WRONLY|O_CREAT|O_TRUNC or O_RDONLY
  body.insts.push_back(
      make_inst(MOp::Cmp, 8, Operand{Reg::Rax}, Operand{Imm{-4095}}));
  const u32 ok = body.label_count++;
  // Linux returns negative errno values: read unsigned, a valid fd or
  // pointer is below the -4095 threshold and an error is at or above it.
  body.insts.push_back(make_jcc(Cond::B, Operand{Label{ok}}));
  mov(MOp::Mov, Operand{Reg::Rax}, Operand{Imm{-1}});
  body.insts.push_back(make_inst(MOp::Ret, 8));
  MInst joined = make_inst(MOp::Nop, 8);
  joined.label = ok;
  body.insts.push_back(joined);
  mov(MOp::Mov, slot(-24), Operand{Reg::Rax});  // fd
  mov(MOp::Mov, Operand{Reg::Rdi}, slot(-24));
  mov(MOp::Mov, Operand{Reg::Rsi}, slot(-8));
  mov(MOp::Mov, Operand{Reg::Rdx}, slot(-16));
  mov(MOp::Mov, Operand{Reg::Rax}, Operand{Imm{write ? 1 : 0}});
  body.insts.push_back(make_inst(MOp::Syscall, 8));
  mov(MOp::Mov, slot(-32), Operand{Reg::Rax});  // bytes moved
  mov(MOp::Mov, Operand{Reg::Rdi}, slot(-24));
  mov(MOp::Mov, Operand{Reg::Rax}, Operand{Imm{3}});
  body.insts.push_back(make_inst(MOp::Syscall, 8));
  mov(MOp::Mov, Operand{Reg::Rax}, slot(-32));
  body.insts.push_back(make_inst(MOp::Ret, 8));
  return body;
}

}  // namespace

bool defines(const std::string_view name) {
  return name == "alcy_print" || name == "alcy_println" ||
         name == "alcy_eprint" || name == "alcy_eprintln" ||
         name == "alcy_panic" || name == "alcy_sys_write" ||
         name == "alcy_sys_read" || name == "alcy_error_kind" ||
         name == "alcy_arg_count" || name == "alcy_arg" ||
         name == "alcy_file_size" || name == "alcy_read_file_into" ||
         name == "alcy_write_file" || name == "alcy_alloc" ||
         name == "alcy_dealloc";
}

Body build(const std::string_view name, RoData& rodata, bool /*freestanding*/) {
  if (name == "alcy_print") {
    return build_writer(name, rodata, 1, /*newline=*/false);
  }
  if (name == "alcy_println") {
    return build_writer(name, rodata, 1, /*newline=*/true);
  }
  if (name == "alcy_eprint") {
    return build_writer(name, rodata, 2, /*newline=*/false);
  }
  if (name == "alcy_eprintln") {
    return build_writer(name, rodata, 2, /*newline=*/true);
  }
  if (name == "alcy_panic") {
    return build_panic(rodata);
  }
  if (name == "alcy_sys_write") {
    return build_syscall_body(1);
  }
  if (name == "alcy_sys_read") {
    return build_syscall_body(0);
  }
  if (name == "alcy_error_kind") {
    return build_error_kind();
  }
  if (name == "alcy_arg_count") {
    return build_arg_count();
  }
  if (name == "alcy_arg") {
    return build_arg();
  }
  if (name == "alcy_file_size") {
    return build_file_size();
  }
  if (name == "alcy_read_file_into") {
    return build_file_io(false);
  }
  if (name == "alcy_write_file") {
    return build_file_io(true);
  }
  if (name == "alcy_alloc") {
    return build_alloc_fs();
  }
  if (name == "alcy_dealloc") {
    return build_dealloc_fs();
  }
  DCHECK_MSG(false, "the runtime was asked for a name it does not define");
  return Body{};
}

}  // namespace codegen::x86_64::runtime
