// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/runtime.h"

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

}  // namespace

bool defines(const std::string_view name) {
  return name == "alcy_print" || name == "alcy_println" ||
         name == "alcy_eprint" || name == "alcy_eprintln" ||
         name == "alcy_panic" || name == "alcy_sys_write" ||
         name == "alcy_sys_read";
}

Body build(const std::string_view name, RoData& rodata) {
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
  DCHECK_MSG(false, "the runtime was asked for a name it does not define");
  return Body{};
}

}  // namespace codegen::x86_64::runtime
