// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/riscv64/runtime.h"

#include <string>
#include <string_view>

#include "codegen/riscv64/frame.h"
#include "codegen/riscv64/isa.h"
#include "codegen/riscv64/minst.h"
#include "codegen/riscv64/rodata.h"
#include "debug/dcheck.h"

namespace codegen::riscv64::runtime {
namespace {

constexpr i64 SYS_OPENAT = 56;
constexpr i64 SYS_CLOSE = 57;
constexpr i64 SYS_WRITE = 64;
constexpr i64 SYS_READ = 63;
constexpr i64 SYS_FSTAT = 80;
constexpr i64 SYS_EXIT = 93;
constexpr i64 SYS_MUNMAP = 215;
constexpr i64 SYS_MMAP = 222;

enum class FileOp : u8 { Size, Read, Write };

constexpr i64 MAP_PRIVATE_ANON = 0x22;
constexpr i64 FILE_FRAME_BYTES = 768;
constexpr i64 FILE_RA_SLOT = 760;
constexpr i64 FILE_ARG0_SLOT = 752;
constexpr i64 FILE_ARG1_SLOT = 744;
constexpr i64 FILE_PATH_MAX = 511;
constexpr i64 STAT_SIZE_OFFSET = 560;
constexpr i64 FILE_MODE = 0644;
constexpr i64 O_WRONLY_CREAT_TRUNC = 0x241;
constexpr i64 PROT_READ_WRITE = 3;

void ecall(Body& body, const i64 number) {
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{A7}, Operand{ZERO},
                                 Operand{Imm{number}}));
  body.insts.push_back(make_inst(MOp::Ecall, 8));
}

// Write a (ptr, len) string to `descriptor`; a0=ptr, a1=len on entry.
Body build_write(const i64 descriptor) {
  Body body;
  // The syscall may consume fewer bytes than asked for. Keep the cursor
  // and the remaining count outside the argument registers it overwrites.
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{T0}, Operand{A0}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{T1}, Operand{A1}, Operand{Imm{0}}));
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{T2}, Operand{ZERO},
                                 Operand{Imm{descriptor}}));
  const u32 loop = body.label_count++;
  const u32 done = body.label_count++;
  MInst at_loop = make_inst(MOp::Nop, 8);
  at_loop.label = loop;
  body.insts.push_back(at_loop);
  MInst empty =
      make_inst(MOp::Beq, 8, Operand{T1}, Operand{ZERO}, Operand{Label{done}});
  empty.cond = Cond::Eq;
  body.insts.push_back(empty);
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{T2}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A1}, Operand{T0}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A2}, Operand{T1}, Operand{Imm{0}}));
  ecall(body, SYS_WRITE);
  MInst failed =
      make_inst(MOp::Blt, 8, Operand{A0}, Operand{ZERO}, Operand{Label{done}});
  failed.cond = Cond::Lt;
  body.insts.push_back(failed);
  body.insts.push_back(
      make_inst(MOp::Add, 8, Operand{T0}, Operand{T0}, Operand{A0}));
  body.insts.push_back(
      make_inst(MOp::Sub, 8, Operand{T1}, Operand{T1}, Operand{A0}));
  body.insts.push_back(
      make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{loop}}));
  MInst at_done = make_inst(MOp::Nop, 8);
  at_done.label = done;
  body.insts.push_back(at_done);
  return body;
}

Body build_alloc(RoData& rodata) {
  Body body;
  // A zero-byte request needs no mapping. Answer with the alignment
  // itself: non-null, aligned, and never dereferenced.
  const u32 zero_case = body.label_count++;
  const u32 failed = body.label_count++;
  const u32 done = body.label_count++;
  body.insts.push_back(make_inst(MOp::Beq, 8, Operand{A0}, Operand{ZERO},
                                 Operand{Label{zero_case}}));
  // mmap(0, size, PROT_READ|WRITE, MAP_PRIVATE|ANON, -1, 0)
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{T0}, Operand{A0},
                                 Operand{Imm{0}}));  // size
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A1}, Operand{T0}, Operand{Imm{0}}));
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{A2}, Operand{ZERO},
                                 Operand{Imm{PROT_READ_WRITE}}));
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{A3}, Operand{ZERO},
                                 Operand{Imm{MAP_PRIVATE_ANON}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A4}, Operand{ZERO}, Operand{Imm{-1}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A5}, Operand{ZERO}, Operand{Imm{0}}));
  ecall(body, SYS_MMAP);
  // A failed Linux mmap returns a negative errno (e.g. -12 for ENOMEM),
  // not libc's MAP_FAILED. Report the runtime's message and end the
  // process; do not hand back a block.
  MInst refused = make_inst(MOp::Blt, 8, Operand{A0}, Operand{ZERO},
                            Operand{Label{failed}});
  refused.cond = Cond::Lt;
  body.insts.push_back(refused);
  body.insts.push_back(
      make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{done}}));
  MInst at_failed = make_inst(MOp::Auipc, 8, Operand{T0}, Operand{Imm{0}});
  at_failed.label = failed;
  body.insts.push_back(at_failed);
  constexpr std::string_view kMessage = "panic: allocation failed\n";
  const u32 message_id = static_cast<u32>(body.symbols.size());
  body.symbols.push_back(rodata.name(rodata.intern(kMessage)));
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{T0}, Operand{T0},
                                 Operand{SymbolRef{message_id}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A1}, Operand{T0}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A2}, Operand{ZERO},
                Operand{Imm{static_cast<i64>(kMessage.size())}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO}, Operand{Imm{2}}));
  ecall(body, SYS_WRITE);
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO}, Operand{Imm{1}}));
  ecall(body, SYS_EXIT);
  MInst at_zero = make_inst(MOp::Nop, 8);
  at_zero.label = zero_case;
  body.insts.push_back(at_zero);
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{A1}, Operand{Imm{0}}));
  MInst at_done = make_inst(MOp::Nop, 8);
  at_done.label = done;
  body.insts.push_back(at_done);
  return body;
}

Body build_dealloc() {
  Body body;
  // A zero-byte request answered with a dangling address owns no
  // mapping; its dealloc is a no-op.
  const u32 free_done = body.label_count++;
  body.insts.push_back(make_inst(MOp::Beq, 8, Operand{A1}, Operand{ZERO},
                                 Operand{Label{free_done}}));
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{A1}, Operand{A1},
                                 Operand{Imm{0}}));  // len already in a1
  ecall(body, SYS_MUNMAP);
  MInst at_done = make_inst(MOp::Nop, 8);
  at_done.label = free_done;
  body.insts.push_back(at_done);
  return body;
}

// Intern a byte string under a name the runtime body can reference.
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

// The backend expands LoadAddr only for user bodies, so a runtime body
// materializes its own rodata address with an auipc/addi pair.
void write_text(Body& body, const SymbolRef symbol, const i64 length) {
  // A3 was copied into T3 already and is free across the write syscall.
  MInst hi = make_inst(MOp::Auipc, 8, Operand{A3}, Operand{Imm{0}});
  body.insts.push_back(hi);
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A3}, Operand{A3}, Operand{symbol}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO}, Operand{Imm{2}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A1}, Operand{A3}, Operand{Imm{0}}));
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{A2}, Operand{ZERO},
                                 Operand{Imm{length}}));
  ecall(body, SYS_WRITE);
}

// Write `length` bytes starting at `ptr` to stderr. A7 is set here so
// the write is self-contained; caller state lives in S registers.
void write_register(Body& body, Reg ptr, Reg length) {
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A1}, Operand{ptr}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A2}, Operand{length}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO}, Operand{Imm{2}}));
  ecall(body, SYS_WRITE);
}

// Walk the frame chain s1 and print "  at <name>" for each frame whose
// return address falls in a compiled function. Each table entry is
// {start, size, name_length, pad, name_bytes}, 8-byte aligned; the table
// ends with an all-ones start word. Every live value is in a distinct
// register so writes and frame advancement cannot clobber one another.
void emit_backtrace_walk(Body& body, RoData& rodata) {
  constexpr Reg base = S0;
  constexpr Reg cursor = Reg::X18;
  constexpr Reg table = Reg::X19;
  constexpr Reg pc = Reg::X20;
  constexpr Reg start = Reg::X21;
  constexpr Reg span = Reg::X22;
  constexpr Reg name = Reg::X23;
  constexpr Reg length = Reg::X24;
  constexpr Reg previous = Reg::X25;
  constexpr Reg sentinel = Reg::X27;

  const u32 table_id = static_cast<u32>(body.symbols.size());
  body.symbols.push_back("alcy_bt_funcs_array");
  const u32 base_id = static_cast<u32>(body.symbols.size());
  body.symbols.push_back("alcy_bt_text_base");
  body.insts.push_back(
      make_inst(MOp::Auipc, 8, Operand{base}, Operand{Imm{0}}));
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{base}, Operand{base},
                                 Operand{SymbolRef{base_id}}));
  const u32 sites_id = static_cast<u32>(body.symbols.size());
  body.symbols.push_back("alcy_bt_sites_array");
  const SymbolRef open_text = text(body, rodata, " (");
  const SymbolRef close_text = text(body, rodata, ")");
  const SymbolRef at_text = text(body, rodata, "  at ");
  const SymbolRef newline = text(body, rodata, "\n");
  const u32 frame_loop = body.label_count++;
  const u32 scan = body.label_count++;
  const u32 advance = body.label_count++;
  const u32 next_frame = body.label_count++;
  const u32 site_scan = body.label_count++;
  const u32 site_advance = body.label_count++;
  const u32 print_newline = body.label_count++;
  const u32 skip_print = body.label_count++;
  const u32 finish = body.label_count++;
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{cursor}, Operand{S1}, Operand{Imm{0}}));
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{sentinel}, Operand{ZERO},
                                 Operand{Imm{-1}}));
  MInst at_frame = make_inst(MOp::Nop, 8);
  at_frame.label = frame_loop;
  body.insts.push_back(at_frame);
  body.insts.push_back(
      make_inst(MOp::Auipc, 8, Operand{table}, Operand{Imm{0}}));
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{table}, Operand{table},
                                 Operand{SymbolRef{table_id}}));
  body.insts.push_back(make_inst(
      MOp::Ld, 8, Operand{pc},
      Operand{Mem{.base = RegRef{cursor}, .disp = RETURN_ADDR_DISP}}));
  body.insts.push_back(
      make_inst(MOp::Sub, 8, Operand{pc}, Operand{pc}, Operand{base}));
  MInst no_pc = make_inst(MOp::Beq, 8, Operand{pc}, Operand{ZERO},
                          Operand{Label{finish}});
  no_pc.cond = Cond::Eq;
  body.insts.push_back(no_pc);
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{name}, Operand{ZERO}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{length}, Operand{ZERO}, Operand{Imm{0}}));
  MInst at_scan = make_inst(MOp::Nop, 8);
  at_scan.label = scan;
  body.insts.push_back(at_scan);
  body.insts.push_back(
      make_inst(MOp::Ld, 8, Operand{start},
                Operand{Mem{.base = RegRef{table}, .disp = 0}}));
  body.insts.push_back(
      make_inst(MOp::Ld, 8, Operand{length},
                Operand{Mem{.base = RegRef{table}, .disp = 16}}));
  MInst at_end = make_inst(MOp::Beq, 8, Operand{start}, Operand{sentinel},
                           Operand{Label{skip_print}});
  at_end.cond = Cond::Eq;
  body.insts.push_back(at_end);
  MInst too_high = make_inst(MOp::Bltu, 8, Operand{pc}, Operand{start},
                             Operand{Label{skip_print}});
  too_high.cond = Cond::Ltu;
  body.insts.push_back(too_high);
  body.insts.push_back(
      make_inst(MOp::Ld, 8, Operand{span},
                Operand{Mem{.base = RegRef{table}, .disp = 8}}));
  body.insts.push_back(
      make_inst(MOp::Add, 8, Operand{span}, Operand{start}, Operand{span}));
  MInst too_low = make_inst(MOp::Bgeu, 8, Operand{pc}, Operand{span},
                            Operand{Label{advance}});
  too_low.cond = Cond::Geu;
  body.insts.push_back(too_low);
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{name}, Operand{table}, Operand{Imm{32}}));
  body.insts.push_back(
      make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{next_frame}}));
  MInst at_advance = make_inst(MOp::Nop, 8);
  at_advance.label = advance;
  body.insts.push_back(at_advance);
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{table}, Operand{table},
                                 Operand{Imm{32}}));
  body.insts.push_back(
      make_inst(MOp::Add, 8, Operand{table}, Operand{table}, Operand{length}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{table}, Operand{table}, Operand{Imm{7}}));
  body.insts.push_back(make_inst(MOp::Andi, 8, Operand{table}, Operand{table},
                                 Operand{Imm{-8}}));
  body.insts.push_back(
      make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{scan}}));
  MInst at_next = make_inst(MOp::Nop, 8);
  at_next.label = next_frame;
  body.insts.push_back(at_next);
  write_text(body, at_text, 5);
  write_register(body, name, length);
  // A second lookup with the same return address finds the call site.
  body.insts.push_back(
      make_inst(MOp::Auipc, 8, Operand{table}, Operand{Imm{0}}));
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{table}, Operand{table},
                                 Operand{SymbolRef{sites_id}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{name}, Operand{ZERO}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{length}, Operand{ZERO}, Operand{Imm{0}}));
  MInst at_site_scan = make_inst(MOp::Nop, 8);
  at_site_scan.label = site_scan;
  body.insts.push_back(at_site_scan);
  body.insts.push_back(
      make_inst(MOp::Ld, 8, Operand{start},
                Operand{Mem{.base = RegRef{table}, .disp = 0}}));
  body.insts.push_back(
      make_inst(MOp::Ld, 8, Operand{span},
                Operand{Mem{.base = RegRef{table}, .disp = 16}}));
  MInst site_end = make_inst(MOp::Beq, 8, Operand{start}, Operand{sentinel},
                             Operand{Label{print_newline}});
  site_end.cond = Cond::Eq;
  body.insts.push_back(site_end);
  MInst site_high = make_inst(MOp::Bltu, 8, Operand{pc}, Operand{start},
                              Operand{Label{site_advance}});
  site_high.cond = Cond::Ltu;
  body.insts.push_back(site_high);
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{length}, Operand{span}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{name}, Operand{table}, Operand{Imm{32}}));
  MInst at_site_advance = make_inst(MOp::Nop, 8);
  at_site_advance.label = site_advance;
  body.insts.push_back(at_site_advance);
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{table}, Operand{table},
                                 Operand{Imm{32}}));
  body.insts.push_back(
      make_inst(MOp::Add, 8, Operand{table}, Operand{table}, Operand{span}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{table}, Operand{table}, Operand{Imm{7}}));
  body.insts.push_back(make_inst(MOp::Andi, 8, Operand{table}, Operand{table},
                                 Operand{Imm{-8}}));
  body.insts.push_back(
      make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{site_scan}}));
  MInst at_newline = make_inst(MOp::Nop, 8);
  at_newline.label = print_newline;
  body.insts.push_back(at_newline);
  MInst site_none = make_inst(MOp::Beq, 8, Operand{length}, Operand{ZERO},
                              Operand{Label{skip_print}});
  site_none.cond = Cond::Eq;
  body.insts.push_back(site_none);
  write_text(body, open_text, 2);
  write_register(body, name, length);
  write_text(body, close_text, 1);
  MInst at_frame_next = make_inst(MOp::Nop, 8);
  at_frame_next.label = skip_print;
  body.insts.push_back(at_frame_next);
  write_text(body, newline, 1);
  body.insts.push_back(
      make_inst(MOp::Ld, 8, Operand{previous},
                Operand{Mem{.base = RegRef{cursor}, .disp = PREV_FP_DISP}}));
  MInst bad_chain = make_inst(MOp::Bgeu, 8, Operand{cursor}, Operand{previous},
                              Operand{Label{finish}});
  bad_chain.cond = Cond::Geu;
  body.insts.push_back(bad_chain);
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{cursor},
                                 Operand{previous}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{frame_loop}}));
  MInst at_finish = make_inst(MOp::Nop, 8);
  at_finish.label = finish;
  body.insts.push_back(at_finish);
}

Body build_panic(RoData& rodata, bool backtrace) {
  Body body;
  // The four arguments are the message pointer/length and the source
  // site pointer/length. Keep them in registers the write syscall does
  // not touch, then print `panic at <site>: <msg>` or `panic: <msg>`.
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{T0}, Operand{A0}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{T1}, Operand{A1}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{T2}, Operand{A2}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{T3}, Operand{A3}, Operand{Imm{0}}));
  const SymbolRef at_text = text(body, rodata, "panic at ");
  const SymbolRef bare = text(body, rodata, "panic: ");
  const SymbolRef colon = text(body, rodata, ": ");
  const SymbolRef newline = text(body, rodata, "\n");
  const u32 with_site = body.label_count++;
  const u32 after_head = body.label_count++;
  MInst branch = make_inst(MOp::Beq, 8, Operand{T3}, Operand{ZERO},
                           Operand{Label{with_site}});
  branch.cond = Cond::Eq;
  body.insts.push_back(branch);
  write_text(body, at_text, 9);
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO}, Operand{Imm{2}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A1}, Operand{T2}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A2}, Operand{T3}, Operand{Imm{0}}));
  ecall(body, SYS_WRITE);
  write_text(body, colon, 2);
  body.insts.push_back(
      make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{after_head}}));
  MInst at_with = make_inst(MOp::Nop, 8);
  at_with.label = with_site;
  body.insts.push_back(at_with);
  write_text(body, bare, 7);
  MInst at_after = make_inst(MOp::Nop, 8);
  at_after.label = after_head;
  body.insts.push_back(at_after);
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO}, Operand{Imm{2}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A1}, Operand{T0}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A2}, Operand{T1}, Operand{Imm{0}}));
  ecall(body, SYS_WRITE);
  write_text(body, newline, 1);
  if (backtrace) {
    emit_backtrace_walk(body, rodata);
  }
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO}, Operand{Imm{101}}));
  ecall(body, SYS_EXIT);
  return body;
}

Body build_syscall(const i64 number) {
  Body body;
  ecall(body, number);
  return body;
}

// op: 0=file size, 1=read into, 2=write. The path arrives as (ptr, len)
// in a0/a1 and the buffer as (ptr, len) in a2/a3. A small frame holds a
// NUL-terminated path copy, a stat buffer and the saved buffer arguments;
// every displacement stays inside a signed 12-bit immediate.
Body build_file(const FileOp op) {
  Body body;
  constexpr Reg cursor = Reg::X22;
  constexpr Reg bound = Reg::X23;
  constexpr Reg fd = Reg::X25;
  constexpr Reg result = Reg::X26;
  constexpr Reg temp = Reg::X27;
  const auto addi = [&](Reg dst, Reg src, i64 value) {
    body.insts.push_back(make_inst(MOp::Addi, 8, Operand{dst}, Operand{src},
                                   Operand{Imm{value}}));
  };
  const auto add = [&](Reg dst, Reg a, Reg b) {
    body.insts.push_back(
        make_inst(MOp::Add, 8, Operand{dst}, Operand{a}, Operand{b}));
  };
  const auto ld = [&](Reg dst, i32 disp) {
    body.insts.push_back(
        make_inst(MOp::Ld, 8, Operand{dst},
                  Operand{Mem{.base = RegRef{SP}, .disp = disp}}));
  };
  const auto sd = [&](Reg src, i32 disp) {
    body.insts.push_back(
        make_inst(MOp::Sd, 8, Operand{src},
                  Operand{Mem{.base = RegRef{SP}, .disp = disp}}));
  };
  const auto at = [&](const u32 label) {
    MInst inst = make_inst(MOp::Nop, 8);
    inst.label = label;
    body.insts.push_back(inst);
  };

  addi(SP, SP, -FILE_FRAME_BYTES);
  sd(RA, FILE_RA_SLOT);
  sd(A2, FILE_ARG0_SLOT);
  sd(A3, FILE_ARG1_SLOT);
  // Clamp the path to the FILE_PATH_MAX bytes the frame leaves for it.
  addi(bound, ZERO, FILE_PATH_MAX);
  const u32 fits = body.label_count++;
  MInst too_long = make_inst(MOp::Bltu, 8, Operand{A1}, Operand{bound},
                             Operand{Label{fits}});
  too_long.cond = Cond::Ltu;
  body.insts.push_back(too_long);
  addi(A1, ZERO, FILE_PATH_MAX);
  at(fits);

  const u32 copy = body.label_count++;
  const u32 copied = body.label_count++;
  addi(cursor, ZERO, 0);
  at(copy);
  MInst end_copy = make_inst(MOp::Beq, 8, Operand{cursor}, Operand{A1},
                             Operand{Label{copied}});
  end_copy.cond = Cond::Eq;
  body.insts.push_back(end_copy);
  add(temp, A0, cursor);
  body.insts.push_back(
      make_inst(MOp::Lbu, 1, Operand{result},
                Operand{Mem{.base = RegRef{temp}, .disp = 0}}));
  add(temp, SP, cursor);
  body.insts.push_back(
      make_inst(MOp::Sb, 1, Operand{result},
                Operand{Mem{.base = RegRef{temp}, .disp = 0}}));
  addi(cursor, cursor, 1);
  body.insts.push_back(
      make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{copy}}));
  at(copied);
  add(temp, SP, cursor);
  body.insts.push_back(
      make_inst(MOp::Sb, 1, Operand{ZERO},
                Operand{Mem{.base = RegRef{temp}, .disp = 0}}));

  addi(A7, ZERO, SYS_OPENAT);
  addi(A0, ZERO, -100);  // AT_FDCWD
  addi(A1, SP, 0);       // the path copy
  addi(A2, ZERO, op == FileOp::Write ? O_WRONLY_CREAT_TRUNC : 0);
  addi(A3, ZERO, FILE_MODE);
  body.insts.push_back(make_inst(MOp::Ecall, 8));
  addi(fd, A0, 0);
  const u32 failed = body.label_count++;
  addi(result, ZERO, -4095);
  MInst is_error = make_inst(MOp::Bgeu, 8, Operand{fd}, Operand{result},
                             Operand{Label{failed}});
  is_error.cond = Cond::Geu;
  body.insts.push_back(is_error);

  if (op == FileOp::Size) {
    addi(A7, ZERO, SYS_FSTAT);
    addi(A0, fd, 0);
    addi(A1, SP, 512);
    body.insts.push_back(make_inst(MOp::Ecall, 8));
    ld(result, STAT_SIZE_OFFSET);
  } else {
    addi(A7, ZERO, op == FileOp::Read ? SYS_READ : SYS_WRITE);
    addi(A0, fd, 0);
    ld(A1, FILE_ARG0_SLOT);
    ld(A2, FILE_ARG1_SLOT);
    body.insts.push_back(make_inst(MOp::Ecall, 8));
    addi(result, A0, 0);
  }
  addi(A7, ZERO, SYS_CLOSE);
  addi(A0, fd, 0);
  body.insts.push_back(make_inst(MOp::Ecall, 8));
  addi(A0, result, 0);
  const u32 finish = body.label_count++;
  body.insts.push_back(
      make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{finish}}));
  at(failed);
  addi(A0, ZERO, -1);
  at(finish);
  // allocate() owns the entry prologue/epilogue; only undo this frame.
  addi(SP, SP, FILE_FRAME_BYTES);
  return body;
}

Body build_memcpy() {
  Body body;
  constexpr Reg result = Reg::X25;
  const u32 loop = body.label_count++;
  const u32 done = body.label_count++;
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{result}, Operand{A0}, Operand{Imm{0}}));
  MInst at_loop = make_inst(MOp::Nop, 8);
  at_loop.label = loop;
  body.insts.push_back(at_loop);
  MInst end =
      make_inst(MOp::Beq, 8, Operand{A2}, Operand{ZERO}, Operand{Label{done}});
  end.cond = Cond::Eq;
  body.insts.push_back(end);
  body.insts.push_back(make_inst(MOp::Lbu, 1, Operand{T0},
                                 Operand{Mem{.base = RegRef{A1}, .disp = 0}}));
  body.insts.push_back(make_inst(MOp::Sb, 1, Operand{T0},
                                 Operand{Mem{.base = RegRef{A0}, .disp = 0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{A0}, Operand{Imm{1}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A1}, Operand{A1}, Operand{Imm{1}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A2}, Operand{A2}, Operand{Imm{-1}}));
  body.insts.push_back(
      make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{loop}}));
  MInst at_done = make_inst(MOp::Nop, 8);
  at_done.label = done;
  body.insts.push_back(at_done);
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{result}, Operand{Imm{0}}));
  return body;
}

Body build_error_kind() {
  Body body;
  // Map Linux errno numbers to the portable kinds the std asks for.
  // Linux errno numbers mapped to the portable codes lib/std/sys names.
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
  constexpr struct {
    i64 number;
    i64 kind;
  } mappings[] = {{kErrEintr, INTERRUPTED}, {kErrEagain, WOULD_BLOCK},
                  {kErrEnoent, NOT_FOUND},  {kErrEacces, PERMISSION_DENIED},
                  {kErrEpipe, BROKEN_PIPE}, {kErrEinval, INVALID_INPUT}};
  const u32 done = body.label_count++;
  for (const auto& mapping : mappings) {
    const u32 next = body.label_count++;
    MInst branch =
        make_inst(MOp::Bne, 8, Operand{A0}, Operand{Imm{mapping.number}},
                  Operand{Label{next}});
    branch.cond = Cond::Ne;
    body.insts.push_back(branch);
    body.insts.push_back(make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO},
                                   Operand{Imm{mapping.kind}}));
    body.insts.push_back(
        make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{done}}));
    MInst at_next = make_inst(MOp::Nop, 8);
    at_next.label = next;
    body.insts.push_back(at_next);
  }
  MInst at_done = make_inst(MOp::Nop, 8);
  at_done.label = done;
  body.insts.push_back(at_done);
  return body;
}

Body build_arg() {
  Body body;
  // _start left argc in x21 and the argv base in x4. An index past the
  // end answers null, like the other implementations.
  const u32 past_end = body.label_count++;
  const u32 done = body.label_count++;
  MInst branch = make_inst(MOp::Bgeu, 8, Operand{A0}, Operand{Reg::X21},
                           Operand{Label{past_end}});
  branch.cond = Cond::Geu;
  body.insts.push_back(branch);
  body.insts.push_back(
      make_inst(MOp::Slli, 8, Operand{T0}, Operand{A0}, Operand{Imm{3}}));
  body.insts.push_back(
      make_inst(MOp::Add, 8, Operand{T0}, Operand{Reg::X4}, Operand{T0}));
  body.insts.push_back(make_inst(MOp::Ld, 8, Operand{A0},
                                 Operand{Mem{.base = RegRef{T0}, .disp = 0}}));
  // The result is a byte slice: a0 is the pointer, a1 its length.
  constexpr Reg cursor = Reg::X22;
  constexpr Reg length = Reg::X23;
  constexpr Reg byte = Reg::X25;
  const u32 scan = body.label_count++;
  const u32 have_length = body.label_count++;
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{cursor}, Operand{A0}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{length}, Operand{ZERO}, Operand{Imm{0}}));
  MInst at_scan = make_inst(MOp::Nop, 8);
  at_scan.label = scan;
  body.insts.push_back(at_scan);
  body.insts.push_back(
      make_inst(MOp::Lbu, 1, Operand{byte},
                Operand{Mem{.base = RegRef{cursor}, .disp = 0}}));
  MInst end_scan = make_inst(MOp::Beq, 8, Operand{byte}, Operand{ZERO},
                             Operand{Label{have_length}});
  end_scan.cond = Cond::Eq;
  body.insts.push_back(end_scan);
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{cursor}, Operand{cursor},
                                 Operand{Imm{1}}));
  body.insts.push_back(make_inst(MOp::Addi, 8, Operand{length}, Operand{length},
                                 Operand{Imm{1}}));
  body.insts.push_back(
      make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{scan}}));
  MInst at_length = make_inst(MOp::Nop, 8);
  at_length.label = have_length;
  body.insts.push_back(at_length);
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A1}, Operand{length}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{done}}));
  MInst at_end = make_inst(MOp::Nop, 8);
  at_end.label = past_end;
  body.insts.push_back(at_end);
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO}, Operand{Imm{0}}));
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A1}, Operand{ZERO}, Operand{Imm{0}}));
  MInst at_done = make_inst(MOp::Nop, 8);
  at_done.label = done;
  body.insts.push_back(at_done);
  return body;
}

Body build_arg_count() {
  Body body;
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{Reg::X21}, Operand{Imm{0}}));
  return body;
}

using Builder = Body (*)(RoData&, bool);

struct RuntimeEntry {
  std::string_view name;
  Builder build;
};

// One table holds the names this runtime answers and the bodies behind
// them, so `defines` and `build` cannot disagree about what it carries.
constexpr RuntimeEntry RUNTIME[] = {
    {"alcy_print", [](RoData&, bool) { return build_write(1); }},
    {"alcy_println", [](RoData&, bool) { return build_write(1); }},
    {"alcy_eprint", [](RoData&, bool) { return build_write(2); }},
    {"alcy_eprintln", [](RoData&, bool) { return build_write(2); }},
    {"alcy_alloc", [](RoData& rodata, bool) { return build_alloc(rodata); }},
    {"alcy_dealloc", [](RoData&, bool) { return build_dealloc(); }},
    {"alcy_panic",
     [](RoData& rodata, bool backtrace) {
       return build_panic(rodata, backtrace);
     }},
    {"alcy_arg_count", [](RoData&, bool) { return build_arg_count(); }},
    {"alcy_arg", [](RoData&, bool) { return build_arg(); }},
    {"alcy_sys_write", [](RoData&, bool) { return build_syscall(SYS_WRITE); }},
    {"alcy_sys_read", [](RoData&, bool) { return build_syscall(SYS_READ); }},
    {"alcy_error_kind", [](RoData&, bool) { return build_error_kind(); }},
    {"alcy_file_size", [](RoData&, bool) { return build_file(FileOp::Size); }},
    {"alcy_read_file_into",
     [](RoData&, bool) { return build_file(FileOp::Read); }},
    {"alcy_write_file",
     [](RoData&, bool) { return build_file(FileOp::Write); }},
    {"memcpy", [](RoData&, bool) { return build_memcpy(); }},
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

Body build(const std::string_view name, RoData& rodata, const bool backtrace) {
  const RuntimeEntry* entry = find(name);
  DCHECK_MSG(entry != nullptr,
             "the runtime was asked for a name it does not define");
  if (entry == nullptr) {
    return Body{};
  }
  return entry->build(rodata, backtrace);
}

}  // namespace codegen::riscv64::runtime
