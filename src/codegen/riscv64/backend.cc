// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/riscv64/backend.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "codegen/backend.h"
#include "codegen/diag_code.h"
#include "codegen/function_symbols.h"
#include "codegen/object/elf64.h"
#include "codegen/reachability.h"
#include "codegen/riscv64/allocator.h"
#include "codegen/riscv64/encoder.h"
#include "codegen/riscv64/entry.h"
#include "codegen/riscv64/frame.h"
#include "codegen/riscv64/isa.h"
#include "codegen/riscv64/minst.h"
#include "codegen/riscv64/printer.h"
#include "codegen/riscv64/rodata.h"
#include "codegen/riscv64/runtime.h"
#include "codegen/riscv64/selector.h"
#include "codegen/riscv64/tables.h"
#include "codegen/target.h"
#include "diag/span.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/storage.h"

namespace codegen::riscv64 {

bool Riscv64::supports(const codegen::OutputKind kind,
                       const codegen::Target& target) {
  return (kind == codegen::OutputKind::Object ||
          kind == codegen::OutputKind::Assembly) &&
         target.triple.rfind("riscv64", 0) == 0 &&
         target.triple.find("linux") != std::string::npos;
}

// Widen out-of-range branches and jumps to an auipc/jalr pair. Inserting
// words moves every later label, so widen to a fixed point and patch the
// indirect jumps only after the final layout is known. A marker label on
// each widened jalr pairs it with the label it jumps to.
bool widen_far_jumps(Body& body) {
  struct Widened {
    u32 marker = 0;
    u32 target = 0;
  };
  std::vector<Widened> widened;
  const u32 max_passes = static_cast<u32>(body.insts.size()) + 8;
  for (u32 pass = 0; pass < max_passes; ++pass) {
    u32 max_label = body.label_count;
    for (const MInst& inst : body.insts) {
      if (inst.label != MInst::NO_LABEL) {
        max_label = std::max(max_label, inst.label + 1);
      }
    }
    std::vector<u64> label_at(max_label, 0);
    std::vector<bool> has_label(max_label, false);
    u64 byte_at = 0;
    for (const MInst& inst : body.insts) {
      if (inst.label != MInst::NO_LABEL) {
        label_at[inst.label] = byte_at;
        has_label[inst.label] = true;
      }
      byte_at += 4;
    }
    bool changed = false;
    for (u32 i = 0; i < body.insts.size(); ++i) {
      MInst& inst = body.insts[i];
      const bool branch = inst.op == MOp::Beq || inst.op == MOp::Bne ||
                          inst.op == MOp::Blt || inst.op == MOp::Bge ||
                          inst.op == MOp::Bltu || inst.op == MOp::Bgeu;
      const u32 label_op = branch ? 2 : 1;
      const Label* label =
          (inst.op == MOp::Jal || branch) && inst.count > label_op
              ? std::get_if<Label>(&inst.at(label_op))
              : nullptr;
      if (label == nullptr || label->id >= label_at.size() ||
          !has_label[label->id]) {
        continue;
      }
      const u32 target_label = label->id;
      const u64 target = label_at[target_label];
      const u64 auipc_at = static_cast<u64>(branch ? i + 1 : i) * 4;
      const u64 inserted = branch ? 12 : 4;
      const i64 shift = target >= auipc_at ? static_cast<i64>(inserted) : 0;
      const i64 delta =
          static_cast<i64>(target + shift) - static_cast<i64>(auipc_at);
      const i64 limit = branch ? 4095 : 1048575;
      if (delta >= -limit && delta <= limit) {
        continue;
      }
      const i64 hi = upper20_shifted(delta);
      const i64 lo = delta - hi;
      const u32 marker = body.label_count++;
      if (branch) {
        const u32 skip = body.label_count++;
        MInst near = inst;
        near.cond = negate(inst.cond);
        near.ops[label_op] = Operand{Label{skip}};
        body.insts[i] = near;
        MInst auipc = make_inst(MOp::Auipc, 8, Operand{T0}, Operand{Imm{hi}});
        MInst jalr = make_inst(MOp::Jalr, 8, Operand{ZERO}, Operand{T0},
                               Operand{Imm{lo}});
        jalr.label = marker;
        MInst at_skip = make_inst(MOp::Nop, 8);
        at_skip.label = skip;
        body.insts.insert(body.insts.begin() + i + 1, {auipc, jalr, at_skip});
        i += 3;
      } else {
        MInst auipc = make_inst(MOp::Auipc, 8, Operand{T0}, Operand{Imm{hi}});
        auipc.label = inst.label;
        MInst jalr =
            make_inst(MOp::Jalr, 8, inst.at(0), Operand{T0}, Operand{Imm{lo}});
        jalr.label = marker;
        body.insts[i] = auipc;
        body.insts.insert(body.insts.begin() + i + 1, jalr);
        ++i;
      }
      widened.push_back(Widened{marker, target_label});
      changed = true;
    }
    if (!changed) {
      for (const Widened& item : widened) {
        u32 at = 0;
        while (at < body.insts.size() && body.insts[at].label != item.marker) {
          ++at;
        }
        if (at == 0 || at >= body.insts.size() || !has_label[item.target]) {
          return false;
        }
        const i64 delta = static_cast<i64>(label_at[item.target]) -
                          (static_cast<i64>(label_at[item.marker]) - 4);
        const i64 hi = upper20_shifted(delta);
        body.insts[at - 1].ops[1] = Operand{Imm{hi}};
        body.insts[at].ops[2] = Operand{Imm{delta - hi}};
      }
      return true;
    }
  }
  return false;
}

base::Result<std::vector<u8>, codegen::EmitError> Riscv64::emit(
    codegen::EmitRequest request) {
  ir::Storage storage = std::move(request.storage).unwrap();
  const bool want_assembly = request.kind == codegen::OutputKind::Assembly;
  std::vector<bool> parametric(storage.functions().size(), false);
  for (const ir::FunctionIdx index : storage.parametric_functions()) {
    parametric[index.idx] = true;
  }

  // A binary roots reachability at its entry; a library roots it at the
  // functions its surface names, or at every free function when no
  // surface was requested.
  std::optional<ir::FunctionIdx> entry_index;
  std::vector<ir::FunctionIdx> roots;
  if (request.emit_entry) {
    for (const ir::FunctionIdx index : storage.functions().idx_range()) {
      if (codegen::is_entry_candidate(storage, *request.strings,
                                      storage.functions()[index])) {
        entry_index = index;
        break;
      }
    }
    if (!entry_index.has_value()) {
      (void)request.bag->emit<i18n::Key::CodegenRiscv64Unsupported>(
          codegen::DiagCode::Unsupported, diag::Span{}, "main");
      return base::make_err(codegen::EmitError::Unsupported);
    }
    roots.push_back(*entry_index);
  } else {
    for (const ir::FunctionIdx index : storage.functions().idx_range()) {
      if (storage.functions()[index].meta.kind == ir::SymbolKind::Free &&
          !parametric[index.idx]) {
        roots.push_back(index);
      }
    }
    if (request.exported_functions.has_value()) {
      const auto& surface = *request.exported_functions;
      roots.erase(std::remove_if(roots.begin(), roots.end(),
                                 [&](const ir::FunctionIdx index) {
                                   return std::find(surface.begin(),
                                                    surface.end(),
                                                    index) == surface.end();
                                 }),
                  roots.end());
    }
  }
  const std::vector<bool> reachable = codegen::reachable_functions(
      storage, std::span(roots.data(), roots.size()));

  RoData rodata;

  struct Selected {
    u32 index = 0;
    bool is_runtime = false;
    Body body;
  };
  std::vector<Selected> functions;
  BacktraceTables tables;
  tables.enabled = request.options.backtrace;
  for (const ir::FunctionIdx index : storage.functions().idx_range()) {
    if (!reachable[index.idx] || parametric[index.idx]) {
      continue;
    }
    base::Result<Body, codegen::EmitError> selected = select_function(
        SelectContext{.storage = storage,
                      .strings = *request.strings,
                      .spans = request.instr_spans,
                      .function = storage.functions()[index],
                      .bag = *request.bag,
                      .width = request.target.width,
                      .rodata = rodata,
                      .overlay = request.overlay,
                      .instr_site_names = request.instr_site_names,
                      .backtrace = request.options.backtrace});
    if (selected.is_err()) {
      return base::make_err(std::move(selected).unwrap_err());
    }
    Body body = std::move(selected).unwrap();
    allocate(body, layout_frame(body));
    // Expand LoadAddr into an auipc/addi pair so the address can be
    // resolved PC-relative once all offsets are known.
    std::vector<MInst> expanded;
    for (MInst& inst : body.insts) {
      if (inst.op == MOp::LoadAddr) {
        MInst hi = make_inst(MOp::Auipc, 8, inst.ops[0], inst.ops[1]);
        hi.label = inst.label;
        expanded.push_back(hi);
        expanded.push_back(
            make_inst(MOp::Addi, 8, inst.ops[0], inst.ops[0], inst.ops[1]));
      } else {
        expanded.push_back(inst);
      }
    }
    body.insts = std::move(expanded);
    if (!widen_far_jumps(body)) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
    if (tables.enabled) {
      tables.functions.push_back(BacktraceEntry{
          0, 0,
          std::string(
              request.strings->get(storage.functions()[index].meta.name))});
      for (const Body::CallSite& call : body.call_sites) {
        tables.sites.push_back(BacktraceEntry{0, 0, call.site});
      }
    }
    functions.push_back(Selected{index.idx, false, std::move(body)});
  }

  // The runtime bodies a reachable call names. They follow user code in
  // the same text, so their symbols resolve like any internal call.
  std::unordered_set<std::string> runtime_names;
  for (const Selected& selected : functions) {
    for (const std::string& symbol : selected.body.symbols) {
      if (runtime::defines(symbol)) {
        runtime_names.insert(symbol);
      }
    }
  }
  for (const std::string& name : runtime_names) {
    Body body = runtime::build(name, rodata, tables.enabled);
    allocate(body, layout_frame(body));
    functions.push_back(Selected{0, true, std::move(body)});
    functions.back().body.symbols.push_back(name);
  }

  // Lay the bodies out in module order and collect each name's address.
  std::unordered_map<std::string, i64> offsets;
  std::vector<i64> base_of(functions.size(), 0);
  i64 at = 0;
  for (u32 f = 0; f < functions.size(); ++f) {
    base_of[f] = at;
    at += static_cast<i64>(functions[f].body.insts.size()) * 4;
    if (!functions[f].is_runtime) {
      offsets["alcy.fn." + std::to_string(functions[f].index)] = base_of[f];
    }
    for (const std::string& symbol : functions[f].body.symbols) {
      if (runtime::defines(symbol)) {
        offsets[symbol] = base_of[f];
      }
    }
    if (!functions[f].is_runtime && entry_index.has_value() &&
        functions[f].index == entry_index->idx) {
      offsets["main"] = base_of[f];
    }
  }

  const i64 start_at = at;
  EntryBody entry;
  if (request.emit_entry) {
    entry = build_entry(storage.functions()[*entry_index], storage,
                        request.target.width, rodata);
  }

  for (u32 f = 0, b = 0; f < functions.size(); ++f) {
    if (functions[f].is_runtime) {
      continue;
    }
    if (b < tables.functions.size()) {
      tables.functions[b].offset = base_of[f];
      tables.functions[b].size =
          static_cast<u64>(functions[f].body.insts.size()) * 4;
      ++b;
    }
  }

  // Now that function offsets are known, resolve the entry's call.
  if (request.emit_entry) {
    const i64 target = offsets.at("main");
    const i64 here = start_at + static_cast<i64>(entry.call_at) * 4;
    const i64 jump = target - here;
    if (fits_s21(jump)) {
      entry.insts[entry.call_at].ops[1] = Operand{Imm{jump}};
    } else {
      const i64 hi = upper20(jump);
      const i64 lo = lower12(jump);
      entry.insts[entry.call_at] =
          make_inst(MOp::Auipc, 8, Operand{T0}, Operand{Imm{hi << 12}});
      MInst jump_reg =
          make_inst(MOp::Jalr, 8, Operand{RA}, Operand{T0}, Operand{Imm{lo}});
      entry.insts.insert(entry.insts.begin() + entry.call_at + 1, jump_reg);
      if (entry.message_at.has_value()) {
        ++*entry.message_at;
      }
    }
  }
  const u64 entry_bytes = static_cast<u64>(entry.insts.size()) * 4;
  const u64 table_start = (static_cast<u64>(at) + entry_bytes + 7) & ~7ull;
  u64 after_table = table_start;
  u64 sites_start = table_start;
  if (tables.enabled) {
    offsets["alcy_bt_text_base"] = 0;
    offsets["alcy_bt_funcs_array"] = static_cast<i64>(table_start);
    after_table += tables.functions_size();
    sites_start = after_table;
    offsets["alcy_bt_sites_array"] = static_cast<i64>(sites_start);
    after_table += tables.sites_size();
  }

  // String literals live at the end of .text so their byte offsets are
  // in the same address space as the code that names them.
  if (!rodata.names().empty()) {
    u64 rodata_at = tables.enabled
                        ? after_table
                        : (static_cast<u64>(at) + entry_bytes + 7) & ~7ull;
    for (u32 i = 0; i < rodata.names().size(); ++i) {
      offsets[rodata.names()[i]] = static_cast<i64>(rodata_at);
      rodata_at += (rodata.bytes()[i].size() + 7) & ~7ull;
    }
  }

  if (entry.message_at.has_value() && entry.err_index.has_value()) {
    const i64 here = start_at + static_cast<i64>(*entry.message_at) * 4;
    const i64 delta = offsets[rodata.names()[*entry.err_index]] - here;
    const i64 hi = upper20_shifted(delta);
    entry.insts[*entry.message_at].ops[1] = Operand{Imm{hi}};
    entry.insts[*entry.message_at + 1].ops[2] = Operand{Imm{delta - hi}};
  }

  // A call's SymbolRef names a function in its own body's table. Rewrite
  // it to the relative jal offset from the instruction's byte address.
  for (u32 f = 0; f < functions.size(); ++f) {
    Body& body = functions[f].body;
    for (u32 i = 0; i < body.insts.size(); ++i) {
      MInst& inst = body.insts[i];
      for (Operand& operand : inst.ops) {
        const SymbolRef* symbol = std::get_if<SymbolRef>(&operand);
        if (symbol == nullptr || symbol->id >= body.symbols.size()) {
          continue;
        }
        auto found = offsets.find(body.symbols[symbol->id]);
        if (found == offsets.end()) {
          // An external name survives as a relocation; the encoder emits
          // a placeholder the linker fills in.
          if (runtime::defines(body.symbols[symbol->id])) {
            return base::make_err(codegen::EmitError::Unsupported);
          }
          continue;
        }
        const i64 here = base_of[f] + static_cast<i64>(i) * 4;
        if (inst.op == MOp::Addi && inst.count > 2 &&
            std::get_if<SymbolRef>(&inst.ops[2]) == symbol && i > 0 &&
            functions[f].body.insts[i - 1].op == MOp::Auipc) {
          const i64 auipc_at = here - 4;
          const i64 delta = found->second - auipc_at;
          const i64 hi = upper20_shifted(delta);
          const i64 lo = delta - hi;
          MInst& hi_inst = functions[f].body.insts[i - 1];
          for (Operand& hi_op : hi_inst.ops) {
            if (std::holds_alternative<Imm>(hi_op)) {
              hi_op = Operand{Imm{hi}};
              break;
            }
          }
          operand = Operand{Imm{lo}};
        } else {
          operand = Operand{Imm{found->second - here}};
        }
      }
    }
  }

  std::vector<u32> function_label_base(functions.size(), 0);
  std::vector<PrintedBody> printed;
  Body whole;
  u32 label_base = 0;
  for (u32 fi = 0; fi < functions.size(); ++fi) {
    Selected& selected = functions[fi];
    function_label_base[fi] = label_base;
    for (MInst& inst : selected.body.insts) {
      if (inst.label != MInst::NO_LABEL) {
        inst.label += label_base;
      }
      for (Operand& operand : inst.ops) {
        if (Label* label = std::get_if<Label>(&operand); label != nullptr) {
          label->id += label_base;
        }
      }
    }
    for (const MInst& inst : selected.body.insts) {
      if (inst.label != MInst::NO_LABEL && inst.label >= label_base) {
        label_base = inst.label + 1;
      }
    }
    for (const MInst& inst : selected.body.insts) {
      for (const Operand& operand : inst.ops) {
        if (const Label* label = std::get_if<Label>(&operand);
            label != nullptr && label->id >= label_base) {
          label_base = label->id + 1;
        }
      }
    }
    if (want_assembly) {
      const std::string symbol =
          functions[fi].is_runtime
              ? functions[fi].body.symbols.back()
              : codegen::linkable_name(storage.functions()[functions[fi].index],
                                       storage, *request.strings);
      printed.push_back(
          PrintedBody{symbol, functions[fi].body,
                      functions[fi].is_runtime ? object::SymbolBind::Local
                                               : object::SymbolBind::Global,
                      static_cast<u64>(base_of[fi]),
                      static_cast<u64>(functions[fi].body.insts.size() * 4)});
    }
    // The encoder resolves symbol operands through Body::symbols; carry
    // each function's table into the concatenated body so external names
    // can survive as relocations.
    for (const std::string& symbol : selected.body.symbols) {
      if (std::find(whole.symbols.begin(), whole.symbols.end(), symbol) ==
          whole.symbols.end()) {
        whole.symbols.push_back(symbol);
      }
    }
    whole.insts.insert(whole.insts.end(), selected.body.insts.begin(),
                       selected.body.insts.end());
  }
  whole.label_count = label_base;
  for (MInst& inst : entry.insts) {
    if (inst.label != MInst::NO_LABEL) {
      inst.label += label_base;
    }
    for (Operand& operand : inst.ops) {
      if (Label* label = std::get_if<Label>(&operand); label != nullptr) {
        label->id += label_base;
      }
    }
  }
  whole.label_count = label_base + entry.labels;
  if (want_assembly && request.emit_entry) {
    Body entry_body;
    entry_body.insts = entry.insts;
    printed.push_back(PrintedBody{
        "_start", entry_body, object::SymbolBind::Global,
        static_cast<u64>(start_at), static_cast<u64>(entry.insts.size() * 4)});
  }
  whole.insts.insert(whole.insts.end(), entry.insts.begin(), entry.insts.end());
  Encoded code = encode(whole);
  for (u32 fi = 0, si = 0; fi < functions.size(); ++fi) {
    for (const Body::CallSite& call : functions[fi].body.call_sites) {
      if (si >= tables.sites.size()) {
        break;
      }
      const u32 label = call.label + function_label_base[fi];
      if (label < code.label_offsets.size()) {
        tables.sites[si].offset =
            static_cast<i64>(code.label_offsets[label]) + 4;
      }
      ++si;
    }
  }

  object::Object object;
  object::Section text{.name = ".text",
                       .bytes = std::move(code.bytes),
                       .align = 16,
                       .exec = true,
                       .alloc = true,
                       .bits = true};
  emit_tables(tables, table_start, sites_start, text.bytes, object);
  for (const std::string& bytes : rodata.bytes()) {
    while (text.bytes.size() % 8 != 0) {
      text.bytes.push_back(0);
    }
    text.bytes.insert(text.bytes.end(), bytes.begin(), bytes.end());
  }
  if (request.emit_entry) {
    const u64 main_at = static_cast<u64>(offsets.at("main"));
    object.symbols.push_back(object::Symbol{
        .name = "main",
        .section = 0,
        .offset = main_at,
        .size = static_cast<u64>(start_at) - main_at,
        .bind = object::SymbolBind::Global,
        .type = object::SymbolType::Func,
    });
    object.symbols.push_back(object::Symbol{
        .name = "_start",
        .section = 0,
        .offset = static_cast<u64>(start_at),
        .size = static_cast<u64>(entry.insts.size() * 4),
        .bind = object::SymbolBind::Global,
        .type = object::SymbolType::Func,
    });
  } else if (request.exported_functions.has_value()) {
    const auto& surface = *request.exported_functions;
    for (u32 f = 0; f < functions.size(); ++f) {
      if (functions[f].is_runtime ||
          std::find(surface.begin(), surface.end(),
                    ir::FunctionIdx{functions[f].index}) == surface.end()) {
        continue;
      }
      const u64 size = f + 1 < functions.size()
                           ? static_cast<u64>(base_of[f + 1] - base_of[f])
                           : static_cast<u64>(at - base_of[f]);
      object.symbols.push_back(object::Symbol{
          .name =
              codegen::linkable_name(storage.functions()[functions[f].index],
                                     storage, *request.strings),
          .section = 0,
          .offset = static_cast<u64>(base_of[f]),
          .size = size,
          .bind = object::SymbolBind::Global,
          .type = object::SymbolType::Func,
      });
    }
  }
  // External names survive as symbol patches. Turn them into ELF
  // relocations and undefined symbols so the linker supplies the address.
  std::unordered_set<std::string> carried;
  for (const object::Symbol& symbol : object.symbols) {
    carried.insert(symbol.name);
  }
  for (const Encoded::Patch& patch : code.patches) {
    if (patch.symbol.empty() || !carried.insert(patch.symbol).second) {
      continue;
    }
    object.relocations.push_back(object::Relocation{
        .section = 0,
        .offset = patch.offset,
        .kind = object::RelocKind::Plt32,
        .symbol = patch.symbol,
        .addend = 0,
    });
    object.symbols.push_back(object::Symbol{
        .name = patch.symbol,
        .section = object::Symbol::UNDEFINED,
        .bind = object::SymbolBind::Global,
        .type = object::SymbolType::Func,
    });
  }
  object.sections.push_back(std::move(text));
  if (want_assembly) {
    const std::string asm_text = print_assembly(object, printed);
    return base::make_ok(std::vector<u8>(asm_text.begin(), asm_text.end()));
  }
  return base::make_ok(object::write_elf64(object, object::Machine::Riscv64));
}

}  // namespace codegen::riscv64
