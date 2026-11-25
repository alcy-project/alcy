// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/runtime.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "codegen/x86_64/isa.h"
#include "codegen/x86_64/minst.h"
#include "codegen/x86_64/rodata.h"
#include "debug/dcheck.h"
#include "fpag/base/numeric.h"

namespace codegen::x86_64::runtime {

namespace {

// write(2) every byte `buf` and `len` name, to `fd`. The cursor and
// remainder live in stack slots: every general register is used by a
// caller, and a syscall clobbers rcx/r11. A short write advances them
// and retries; a failed write stops.
void emit_write(Body& body,
                const i64 fd,
                const Operand& buf,
                const Operand& len) {
  body.insts.push_back(
      make_inst(MOp::Sub, 8, Operand{Reg::Rsp}, Operand{Imm{16}}));
  body.insts.push_back(make_inst(MOp::Mov, 8, Operand{Reg::Rax}, buf));
  body.insts.push_back(make_inst(
      MOp::Mov, 8,
      Operand{
          Mem{.base = Reg::Rsp, .index = std::nullopt, .scale = 1, .disp = 0}},
      Operand{Reg::Rax}));
  body.insts.push_back(make_inst(MOp::Mov, 8, Operand{Reg::Rax}, len));
  body.insts.push_back(make_inst(
      MOp::Mov, 8,
      Operand{
          Mem{.base = Reg::Rsp, .index = std::nullopt, .scale = 1, .disp = 8}},
      Operand{Reg::Rax}));
  const u32 loop = body.label_count++;
  const u32 done = body.label_count++;
  MInst at_loop = make_inst(MOp::Nop, 8);
  at_loop.label = loop;
  body.insts.push_back(at_loop);
  body.insts.push_back(make_inst(
      MOp::Mov, 8, Operand{Reg::Rsi},
      Operand{Mem{
          .base = Reg::Rsp, .index = std::nullopt, .scale = 1, .disp = 0}}));
  body.insts.push_back(make_inst(
      MOp::Mov, 8, Operand{Reg::Rdx},
      Operand{Mem{
          .base = Reg::Rsp, .index = std::nullopt, .scale = 1, .disp = 8}}));
  body.insts.push_back(
      make_inst(MOp::Mov, 4, Operand{Reg::Rdi}, Operand{Imm{fd}}));
  body.insts.push_back(
      make_inst(MOp::Mov, 4, Operand{Reg::Rax}, Operand{Imm{1}}));
  body.insts.push_back(make_inst(MOp::Syscall, 8));
  body.insts.push_back(
      make_inst(MOp::Test, 8, Operand{Reg::Rax}, Operand{Reg::Rax}));
  body.insts.push_back(make_jcc(Cond::Le, Operand{Label{done}}));
  // Rax holds the byte count returned by the syscall. Move the stack
  // slots through r11: the ALU forms need register operands.
  body.insts.push_back(make_inst(
      MOp::Mov, 8, Operand{Reg::R11},
      Operand{Mem{
          .base = Reg::Rsp, .index = std::nullopt, .scale = 1, .disp = 0}}));
  body.insts.push_back(
      make_inst(MOp::Add, 8, Operand{Reg::R11}, Operand{Reg::Rax}));
  body.insts.push_back(make_inst(
      MOp::Mov, 8,
      Operand{
          Mem{.base = Reg::Rsp, .index = std::nullopt, .scale = 1, .disp = 0}},
      Operand{Reg::R11}));
  body.insts.push_back(make_inst(
      MOp::Mov, 8, Operand{Reg::R11},
      Operand{Mem{
          .base = Reg::Rsp, .index = std::nullopt, .scale = 1, .disp = 8}}));
  body.insts.push_back(
      make_inst(MOp::Sub, 8, Operand{Reg::R11}, Operand{Reg::Rax}));
  body.insts.push_back(make_inst(
      MOp::Mov, 8,
      Operand{
          Mem{.base = Reg::Rsp, .index = std::nullopt, .scale = 1, .disp = 8}},
      Operand{Reg::R11}));
  body.insts.push_back(make_inst(
      MOp::Mov, 8, Operand{Reg::Rax},
      Operand{Mem{
          .base = Reg::Rsp, .index = std::nullopt, .scale = 1, .disp = 8}}));
  body.insts.push_back(
      make_inst(MOp::Test, 8, Operand{Reg::Rax}, Operand{Reg::Rax}));
  body.insts.push_back(make_jcc(Cond::G, Operand{Label{loop}}));
  MInst at_done = make_inst(MOp::Nop, 8);
  at_done.label = done;
  body.insts.push_back(at_done);
  body.insts.push_back(
      make_inst(MOp::Add, 8, Operand{Reg::Rsp}, Operand{Imm{16}}));
}

// The bytes one write needs, under a name the data section answers to.
SymbolRef text(Body& body, RoData& rodata, const std::string_view bytes) {
  const std::string& symbol = rodata.name(rodata.intern(bytes));
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

// Appends the stack walk to a panic body. The two tables are arrays of
// {address, text, length}; the scan keeps the entry whose address is the
// greatest one below a frame's return address, which names the frame's
// function and the call it made.
void emit_backtrace_walk(Body& body, RoData& rodata) {
  const auto push = [&](MOp op, u8 width, Operand dst, Operand src = {}) {
    if (std::holds_alternative<std::monostate>(src)) {
      body.insts.push_back(make_inst(op, width, dst));
    } else {
      body.insts.push_back(make_inst(op, width, dst, src));
    }
  };
  const auto label = [&]() { return body.label_count++; };
  const auto jcc = [&](Cond cond, u32 target) {
    body.insts.push_back(make_jcc(cond, Operand{Label{target}}));
  };
  const auto at_label = [&](u32 id) {
    MInst inst = make_inst(MOp::Nop, 8);
    inst.label = id;
    body.insts.push_back(inst);
  };
  const auto symbol = [&](std::string_view name) {
    body.symbols.emplace_back(name);
    return static_cast<u32>(body.symbols.size() - 1);
  };

  const SymbolRef at_text = text(body, rodata, "  at ");
  const SymbolRef open_text = text(body, rodata, " (");
  const SymbolRef close_text = text(body, rodata, ")");
  const SymbolRef newline = text(body, rodata, "\n");
  const u32 funcs = symbol("alcy_bt_funcs_array");
  const u32 sites = symbol("alcy_bt_sites_array");

  // One lookup: scan `table` for the greatest address not past rcx,
  // leaving the matched text pointer in r9 and its length in r10. r8
  // holds the best address and r11 the scan cursor.
  const auto lookup = [&](u32 table) {
    push(MOp::Mov, 8, Operand{Reg::R11}, Operand{SymbolRef{table}});
    push(MOp::Xor, 8, Operand{Reg::R8}, Operand{Reg::R8});
    push(MOp::Xor, 8, Operand{Reg::R9}, Operand{Reg::R9});
    push(MOp::Xor, 8, Operand{Reg::R10}, Operand{Reg::R10});
    const u32 scan = label();
    const u32 next = label();
    const u32 done = label();
    at_label(scan);
    push(MOp::Mov, 8, Operand{Reg::Rdx},
         Operand{Mem{
             .base = Reg::R11, .index = std::nullopt, .scale = 1, .disp = 0}});
    push(MOp::Test, 8, Operand{Reg::Rdx}, Operand{Reg::Rdx});
    jcc(Cond::E, done);
    push(MOp::Cmp, 8, Operand{Reg::Rdx}, Operand{Reg::Rcx});
    jcc(Cond::A, next);
    // A zero size names the site table; a function must actually
    // contain the return address, not merely start below it.
    const u32 has_size = label();
    push(MOp::Mov, 8, Operand{Reg::Rax},
         Operand{Mem{
             .base = Reg::R11, .index = std::nullopt, .scale = 1, .disp = 24}});
    push(MOp::Test, 8, Operand{Reg::Rax}, Operand{Reg::Rax});
    jcc(Cond::E, has_size);
    push(MOp::Add, 8, Operand{Reg::Rax}, Operand{Reg::Rdx});
    push(MOp::Cmp, 8, Operand{Reg::Rcx}, Operand{Reg::Rax});
    jcc(Cond::Ae, next);
    at_label(has_size);
    push(MOp::Cmp, 8, Operand{Reg::Rdx}, Operand{Reg::R8});
    jcc(Cond::Be, next);
    push(MOp::Mov, 8, Operand{Reg::R8}, Operand{Reg::Rdx});
    push(MOp::Mov, 8, Operand{Reg::R9},
         Operand{Mem{
             .base = Reg::R11, .index = std::nullopt, .scale = 1, .disp = 8}});
    push(MOp::Mov, 8, Operand{Reg::R10},
         Operand{Mem{
             .base = Reg::R11, .index = std::nullopt, .scale = 1, .disp = 16}});
    at_label(next);
    push(MOp::Add, 8, Operand{Reg::R11}, Operand{Imm{32}});
    push(MOp::Jmp, 8, Operand{Label{scan}});
    at_label(done);
  };

  // r15 walks the frame chain; rbp stays the panic's own frame.
  push(MOp::Mov, 8, Operand{Reg::R15}, Operand{Reg::Rbp});
  const u32 frame = label();
  const u32 next_frame = label();
  const u32 finish = label();
  at_label(frame);
  push(MOp::Mov, 8, Operand{Reg::Rcx},
       Operand{Mem{
           .base = Reg::R15, .index = std::nullopt, .scale = 1, .disp = 8}});
  push(MOp::Test, 8, Operand{Reg::Rcx}, Operand{Reg::Rcx});
  jcc(Cond::E, finish);

  lookup(funcs);
  push(MOp::Test, 8, Operand{Reg::R9}, Operand{Reg::R9});
  jcc(Cond::E, next_frame);
  emit_write(body, 2, Operand{at_text}, Operand{Imm{5}});
  emit_write(body, 2, Operand{Reg::R9}, Operand{Reg::R10});
  // A syscall clobbers rcx, which held the return address; the frame
  // still holds it, so load it again before the second lookup.
  push(MOp::Mov, 8, Operand{Reg::Rcx},
       Operand{Mem{
           .base = Reg::R15, .index = std::nullopt, .scale = 1, .disp = 8}});

  lookup(sites);
  const u32 print_newline = label();
  push(MOp::Test, 8, Operand{Reg::R9}, Operand{Reg::R9});
  jcc(Cond::E, print_newline);
  emit_write(body, 2, Operand{open_text}, Operand{Imm{2}});
  emit_write(body, 2, Operand{Reg::R9}, Operand{Reg::R10});
  emit_write(body, 2, Operand{close_text}, Operand{Imm{1}});

  at_label(print_newline);
  emit_write(body, 2, Operand{newline}, Operand{Imm{1}});

  at_label(next_frame);
  push(MOp::Mov, 8, Operand{Reg::Rdx},
       Operand{Mem{
           .base = Reg::R15, .index = std::nullopt, .scale = 1, .disp = 0}});
  push(MOp::Cmp, 8, Operand{Reg::Rdx}, Operand{Reg::R15});
  jcc(Cond::Be, finish);
  push(MOp::Mov, 8, Operand{Reg::R15}, Operand{Reg::Rdx});
  push(MOp::Jmp, 8, Operand{Label{frame}});

  at_label(finish);
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
  const SymbolRef bare = text(body, rodata, "panic: ");
  const SymbolRef colon = text(body, rodata, ": ");
  const SymbolRef newline = text(body, rodata, "\n");

  // With a site the trace reads `panic at <site>: <msg>`; without one
  // it reads `panic: <msg>`.
  const u32 with_site = body.label_count++;
  const u32 after_head = body.label_count++;
  body.insts.push_back(
      make_inst(MOp::Test, 8, Operand{Reg::R14}, Operand{Reg::R14}));
  body.insts.push_back(make_jcc(Cond::E, Operand{Label{with_site}}));
  emit_write(body, 2, Operand{prefix}, Operand{Imm{9}});
  emit_write(body, 2, Operand{Reg::R13}, Operand{Reg::R14});
  emit_write(body, 2, Operand{colon}, Operand{Imm{2}});
  body.insts.push_back(make_inst(MOp::Jmp, 8, Operand{Label{after_head}}));
  MInst at_with = make_inst(MOp::Nop, 8);
  at_with.label = with_site;
  body.insts.push_back(at_with);
  emit_write(body, 2, Operand{bare}, Operand{Imm{7}});
  MInst at_after = make_inst(MOp::Nop, 8);
  at_after.label = after_head;
  body.insts.push_back(at_after);
  emit_write(body, 2, Operand{Reg::Rbx}, Operand{Reg::R12});
  emit_write(body, 2, Operand{newline}, Operand{Imm{1}});
  emit_backtrace_walk(body, rodata);

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
  constexpr i64 kErrEintr = 4;
  constexpr i64 kErrEagain = 11;
  constexpr i64 kErrEnoent = 2;
  constexpr i64 kErrEacces = 13;
  constexpr i64 kErrEpipe = 32;
  constexpr i64 kErrEinval = 22;
  constexpr i64 INTERRUPTED = 1;
  constexpr i64 WOULD_BLOCK = 2;
  constexpr i64 NOT_FOUND = 3;
  constexpr i64 PERMISSION_DENIED = 4;
  constexpr i64 BROKEN_PIPE = 5;
  constexpr i64 INVALID_INPUT = 6;
  constexpr Mapping mappings[] = {
      {kErrEintr, INTERRUPTED}, {kErrEagain, WOULD_BLOCK},
      {kErrEnoent, NOT_FOUND},  {kErrEacces, PERMISSION_DENIED},
      {kErrEpipe, BROKEN_PIPE}, {kErrEinval, INVALID_INPUT}};
  for (const Mapping& mapping : mappings) {
    body.insts.push_back(make_inst(MOp::Cmp, 4, Operand{Reg::Rdi},
                                   Operand{Imm{mapping.number}}));
    const u32 next = body.label_count++;
    body.insts.push_back(make_jcc(Cond::Ne, Operand{Label{next}}));
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
Body build_alloc_fs(RoData& rodata) {
  Body body;
  const auto mov = [&](MOp op, Operand dst, Operand src) {
    body.insts.push_back(make_inst(op, 8, dst, src));
  };
  // A refused mapping reports the runtime's message and ends the
  // process; nothing past this point runs.
  const auto fail = [&]() {
    constexpr std::string_view kMessage = "panic: allocation failed\n";
    const SymbolRef ref = text(body, rodata, kMessage);
    emit_write(body, 2, Operand{ref},
               Operand{Imm{static_cast<i64>(kMessage.size())}});
    mov(MOp::Mov, Operand{Reg::Rdi}, Operand{Imm{1}});
    mov(MOp::Mov, Operand{Reg::Rax}, Operand{Imm{60}});
    body.insts.push_back(make_inst(MOp::Syscall, 8));
  };
  // A zero-byte request needs no mapping. Its answer is the alignment
  // itself: non-null, aligned, and never dereferenced.
  const u32 maps = body.label_count++;
  mov(MOp::Cmp, Operand{Reg::Rdi}, Operand{Imm{0}});
  body.insts.push_back(make_jcc(Cond::Ne, Operand{Label{maps}}));
  mov(MOp::Mov, Operand{Reg::Rax}, Operand{Reg::Rsi});
  body.insts.push_back(make_inst(MOp::Ret, 8));
  MInst has_bytes = make_inst(MOp::Nop, 8);
  has_bytes.label = maps;
  body.insts.push_back(has_bytes);
  // length = (size + 16 + 4095) & ~4095, refusing a size whose header
  // and alignment would wrap the word.
  const u32 no_wrap = body.label_count++;
  mov(MOp::Mov, Operand{Reg::Rcx}, Operand{Reg::Rdi});
  // The threshold is 2^64-1-4111, spelled as a negative i64 immediate.
  mov(MOp::Cmp, Operand{Reg::Rcx}, Operand{Imm{static_cast<i64>(-4112)}});
  body.insts.push_back(make_jcc(Cond::Be, Operand{Label{no_wrap}}));
  fail();
  MInst wrapped = make_inst(MOp::Nop, 8);
  wrapped.label = no_wrap;
  body.insts.push_back(wrapped);
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
  fail();
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
  // A zero-byte request answered with a dangling address owns no
  // mapping; its `dealloc` is a no-op.
  const u32 frees = body.label_count++;
  body.insts.push_back(
      make_inst(MOp::Cmp, 8, Operand{Reg::Rsi}, Operand{Imm{0}}));
  body.insts.push_back(make_jcc(Cond::Ne, Operand{Label{frees}}));
  body.insts.push_back(make_inst(MOp::Ret, 8));
  MInst has_bytes = make_inst(MOp::Nop, 8);
  has_bytes.label = frees;
  body.insts.push_back(has_bytes);
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
  // rdi = path bytes, rsi = byte count. Clamp a longer path to the
  // 4127 bytes the reserved 4160-byte frame can hold without reaching
  // the caller's saved arguments at [rbp-8] .. [rbp-32].
  const u32 fits = body.label_count++;
  body.insts.push_back(
      make_inst(MOp::Cmp, 8, Operand{Reg::Rsi}, Operand{Imm{4127}}));
  body.insts.push_back(make_jcc(Cond::Be, Operand{Label{fits}}));
  mov(MOp::Mov, Operand{Reg::Rsi}, Operand{Imm{4127}});
  MInst at_fits = make_inst(MOp::Nop, 8);
  at_fits.label = fits;
  body.insts.push_back(at_fits);
  // 4096 bytes of path plus 64 bytes so the copy never reaches the
  // caller's saved arguments at [rbp-8] .. [rbp-32].
  body.insts.push_back(
      make_inst(MOp::Sub, 8, Operand{Reg::Rsp}, Operand{Imm{4160}}));
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
  // A file created by O_CREAT needs readable/writable bits; a zero mode
  // would make a written file impossible to open again.
  mov(MOp::Mov, Operand{Reg::R10}, Operand{Imm{0644}});
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
  // r12 and r13 survive the syscalls; keep the caller's values.
  body.insts.push_back(make_inst(MOp::Push, 8, Operand{Reg::R12}));
  body.insts.push_back(make_inst(MOp::Push, 8, Operand{Reg::R13}));
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
  // The saves above live at [rbp-16] (r13) and [rbp-8] (r12). The stack
  // pointer has moved twice since then, so a pop would read the stat
  // buffer instead; load the saved words by their fixed addresses.
  mov(MOp::Mov, Operand{Reg::R13},
      Operand{Mem{
          .base = Reg::Rbp, .index = std::nullopt, .scale = 1, .disp = -16}});
  mov(MOp::Mov, Operand{Reg::R12},
      Operand{Mem{
          .base = Reg::Rbp, .index = std::nullopt, .scale = 1, .disp = -8}});
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

// alcy_env_find(name: str) -> str: scan the process environment for
// NAME=VALUE and answer VALUE as a borrowed view. A null pointer means
// not found. The table pointer was saved by the entry wrapper; hosted
// builds leave it null. On a freestanding ELF entry the kernel hands us
// envp as an array of NUL-terminated pointers ending in null.
Body build_env_find() {
  Body body;
  const auto mov = [&](MOp op, u8 width, Operand dst, Operand src) {
    body.insts.push_back(make_inst(op, width, dst, src));
  };
  const auto label = [&]() { return body.label_count++; };
  const auto jcc = [&](Cond cond, u32 target) {
    body.insts.push_back(make_jcc(cond, Operand{Label{target}}));
  };
  const auto at_label = [&](u32 id) {
    MInst nop = make_inst(MOp::Nop, 8);
    nop.label = id;
    body.insts.push_back(nop);
  };

  const SymbolRef environ_symbol = callee(body, "alcy_environ");
  // rdi = name pointer, rsi = name length. Keep the name in r10/r11 so
  // the byte comparison can use rax/rcx/rdx freely.
  mov(MOp::Mov, 8, Operand{Reg::R10}, Operand{Reg::Rdi});
  mov(MOp::Mov, 8, Operand{Reg::R11}, Operand{Reg::Rsi});
  mov(MOp::Mov, 8, Operand{Reg::Rax}, Operand{environ_symbol});
  mov(MOp::Mov, 8, Operand{Reg::Rax},
      Operand{
          Mem{.base = Reg::Rax, .index = std::nullopt, .scale = 1, .disp = 0}});
  const u32 walk = label();
  const u32 compare = label();
  const u32 next_entry = label();
  const u32 at_equals = label();
  const u32 value_scan = label();
  const u32 entry_mismatch = label();
  const u32 missing = label();
  const u32 done = label();

  at_label(walk);
  // rcx = *envp; null terminates the table.
  mov(MOp::Mov, 8, Operand{Reg::Rcx},
      Operand{
          Mem{.base = Reg::Rax, .index = std::nullopt, .scale = 1, .disp = 0}});
  mov(MOp::Test, 8, Operand{Reg::Rcx}, Operand{Reg::Rcx});
  jcc(Cond::E, missing);
  // Compare name bytes. rdx indexes within this entry.
  mov(MOp::Mov, 8, Operand{Reg::Rdx}, Operand{Imm{0}});
  at_label(compare);
  mov(MOp::Mov, 8, Operand{Reg::R8}, Operand{Reg::Rdx});
  mov(MOp::Cmp, 8, Operand{Reg::R8}, Operand{Reg::R11});
  jcc(Cond::Ae, at_equals);
  mov(MOp::Movzx, 1, Operand{Reg::R8},
      Operand{Mem{.base = Reg::Rcx, .index = Reg::R8, .scale = 1, .disp = 0}});
  mov(MOp::Movzx, 1, Operand{Reg::R9},
      Operand{Mem{.base = Reg::R10, .index = Reg::Rdx, .scale = 1, .disp = 0}});
  mov(MOp::Cmp, 1, Operand{Reg::R8}, Operand{Reg::R9});
  jcc(Cond::Ne, entry_mismatch);
  mov(MOp::Add, 8, Operand{Reg::Rdx}, Operand{Imm{1}});
  body.insts.push_back(make_inst(MOp::Jmp, 8, Operand{Label{compare}}));

  at_label(at_equals);
  // entry[name_len] must be '='.
  mov(MOp::Movzx, 1, Operand{Reg::R8},
      Operand{Mem{.base = Reg::Rcx, .index = Reg::R11, .scale = 1, .disp = 0}});
  mov(MOp::Cmp, 1, Operand{Reg::R8}, Operand{Imm{'='}});
  jcc(Cond::Ne, entry_mismatch);
  // VALUE starts at name_len + 1; find its NUL for the length.
  mov(MOp::Lea, 8, Operand{Reg::R9},
      Operand{Mem{.base = Reg::Rcx, .index = Reg::R11, .scale = 1, .disp = 1}});
  mov(MOp::Mov, 8, Operand{Reg::Rdx}, Operand{Imm{0}});
  const u32 found_value = label();
  at_label(value_scan);
  mov(MOp::Movzx, 1, Operand{Reg::R8},
      Operand{Mem{.base = Reg::R9, .index = Reg::Rdx, .scale = 1, .disp = 0}});
  body.insts.push_back(
      make_inst(MOp::Test, 1, Operand{Reg::R8}, Operand{Reg::R8}));
  jcc(Cond::E, found_value);
  mov(MOp::Add, 8, Operand{Reg::Rdx}, Operand{Imm{1}});
  body.insts.push_back(make_inst(MOp::Jmp, 8, Operand{Label{value_scan}}));

  at_label(entry_mismatch);
  at_label(next_entry);
  mov(MOp::Add, 8, Operand{Reg::Rax}, Operand{Imm{8}});
  body.insts.push_back(make_inst(MOp::Jmp, 8, Operand{Label{walk}}));

  at_label(found_value);
  mov(MOp::Mov, 8, Operand{Reg::Rax}, Operand{Reg::R9});
  const u32 after_found = label();
  body.insts.push_back(make_inst(MOp::Jmp, 8, Operand{Label{after_found}}));
  at_label(missing);
  mov(MOp::Xor, 8, Operand{Reg::Rax}, Operand{Reg::Rax});
  mov(MOp::Xor, 8, Operand{Reg::Rdx}, Operand{Reg::Rdx});
  at_label(after_found);
  at_label(done);
  // Setcc-style return: the view is {rax, rdx}.
  body.insts.push_back(make_inst(MOp::Ret, 8));
  return body;
}

// alcy_env_exists(name) -> i32: 1 when the find answers a non-null
// pointer. alcy_env_get(name) -> str returns the found view or an empty
// one; both share the find body's convention through an internal call.
Body build_env_exists() {
  Body body;
  // Inline the find by calling it as a local subroutine would need an
  // extra symbol; instead duplicate its result test by checking the
  // environment directly through the shared find body name.
  const SymbolRef find = callee(body, "alcy_env_find");
  body.insts.push_back(make_inst(MOp::Call, 8, Operand{find}));
  body.insts.push_back(
      make_inst(MOp::Test, 8, Operand{Reg::Rax}, Operand{Reg::Rax}));
  const u32 present = body.label_count++;
  const u32 join = body.label_count++;
  body.insts.push_back(make_jcc(Cond::Ne, Operand{Label{present}}));
  body.insts.push_back(
      make_inst(MOp::Xor, 4, Operand{Reg::Rax}, Operand{Reg::Rax}));
  body.insts.push_back(make_inst(MOp::Jmp, 8, Operand{Label{join}}));
  MInst at_present = make_inst(MOp::Nop, 8);
  at_present.label = present;
  body.insts.push_back(at_present);
  body.insts.push_back(
      make_inst(MOp::Mov, 4, Operand{Reg::Rax}, Operand{Imm{1}}));
  MInst at_join = make_inst(MOp::Nop, 8);
  at_join.label = join;
  body.insts.push_back(at_join);
  body.insts.push_back(make_inst(MOp::Ret, 8));
  return body;
}

Body build_env_get() {
  Body body;
  const SymbolRef find = callee(body, "alcy_env_find");
  body.insts.push_back(make_inst(MOp::Call, 8, Operand{find}));
  body.insts.push_back(make_inst(MOp::Ret, 8));
  return body;
}

}  // namespace

namespace {

// One table holds both the names this runtime answers and the bodies
// behind them. A new entry is one line, so `defines` and `build` cannot
// disagree about what the backend carries.
using Builder = Body (*)(RoData&);
struct RuntimeEntry {
  std::string_view name;
  Builder build;
};

constexpr RuntimeEntry RUNTIME[] = {
    {"alcy_print",
     [](RoData& rodata) {
       return build_writer("alcy_print", rodata, 1, /*newline=*/false);
     }},
    {"alcy_println",
     [](RoData& rodata) {
       return build_writer("alcy_println", rodata, 1, /*newline=*/true);
     }},
    {"alcy_eprint",
     [](RoData& rodata) {
       return build_writer("alcy_eprint", rodata, 2, /*newline=*/false);
     }},
    {"alcy_eprintln",
     [](RoData& rodata) {
       return build_writer("alcy_eprintln", rodata, 2, /*newline=*/true);
     }},
    {"alcy_panic", [](RoData& rodata) { return build_panic(rodata); }},
    {"alcy_sys_write", [](RoData&) { return build_syscall_body(1); }},
    {"alcy_sys_read", [](RoData&) { return build_syscall_body(0); }},
    {"alcy_error_kind", [](RoData&) { return build_error_kind(); }},
    {"alcy_arg_count", [](RoData&) { return build_arg_count(); }},
    {"alcy_arg", [](RoData&) { return build_arg(); }},
    {"alcy_file_size", [](RoData&) { return build_file_size(); }},
    {"alcy_read_file_into", [](RoData&) { return build_file_io(false); }},
    {"alcy_write_file", [](RoData&) { return build_file_io(true); }},
    {"alcy_alloc", [](RoData& rodata) { return build_alloc_fs(rodata); }},
    {"alcy_dealloc", [](RoData&) { return build_dealloc_fs(); }},
    {"alcy_env_find", [](RoData&) { return build_env_find(); }},
    {"alcy_env_exists", [](RoData&) { return build_env_exists(); }},
    {"alcy_env_get", [](RoData&) { return build_env_get(); }},
};

const RuntimeEntry* find(const std::string_view name) {
  for (const RuntimeEntry& entry : RUNTIME) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace

bool defines(const std::string_view name) {
  return find(name) != nullptr;
}

Body build(const std::string_view name, RoData& rodata) {
  const RuntimeEntry* entry = find(name);
  DCHECK_MSG(entry != nullptr,
             "the runtime was asked for a name it does not define");
  if (entry == nullptr) {
    return Body{};
  }
  return entry->build(rodata);
}

}  // namespace codegen::x86_64::runtime
