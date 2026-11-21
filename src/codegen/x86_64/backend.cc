// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/backend.h"

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "codegen/backend.h"
#include "codegen/diag_code.h"
#include "codegen/object/elf64.h"
#include "codegen/reach.h"
#include "codegen/target.h"
#include "codegen/x86_64/allocate.h"
#include "codegen/x86_64/encode.h"
#include "codegen/x86_64/frame.h"
#include "codegen/x86_64/isa.h"
#include "codegen/x86_64/minst.h"
#include "codegen/x86_64/rodata.h"
#include "codegen/x86_64/runtime.h"
#include "codegen/x86_64/select.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"

namespace codegen::x86_64 {

namespace {

// Whether this function is the program's entry: a free, argumentless
// `main`, the same test the analyzer applies.
bool is_entry_candidate(const ir::Storage& storage,
                        const ir::SymbolTable& strings,
                        const ir::Function& function) {
  if (function.meta.kind != ir::SymbolKind::Free) {
    return false;
  }
  if (strings.get(function.meta.name) != "main") {
    return false;
  }
  if (!function.meta.param_types.empty()) {
    return false;
  }
  const ir::TypeTag tag = storage.types()[function.meta.return_type.idx].tag;
  return tag == ir::TypeTag::Void || tag == ir::TypeTag::I32;
}

}  // namespace

bool X8664::supports(const codegen::OutputKind kind,
                     const codegen::Target& target) {
  return kind == codegen::OutputKind::Object &&
         target.triple.rfind("x86_64", 0) == 0 &&
         target.triple.find("linux") != std::string::npos;
}

base::Result<std::vector<u8>, codegen::EmitError> X8664::emit(
    codegen::EmitRequest request) {
  ir::Storage storage = std::move(request.storage).unwrap();
  const ir::SymbolTable& strings = *request.strings;
  diag::DiagBag& bag = *request.bag;
  RoData rodata;

  // The entry the program starts at, which is also what reachability
  // grows from: a program's object carries what it reaches and nothing
  // else.
  std::optional<ir::FunctionIdx> entry;
  for (const ir::FunctionIdx index : storage.functions().idx_range()) {
    if (is_entry_candidate(storage, strings, storage.functions()[index])) {
      entry = index;
      break;
    }
  }
  if (!entry.has_value()) {
    (void)bag.emit<i18n::Key::CodegenX86Unsupported>(
        codegen::DiagCode::Unsupported, diag::Span{}, "main");
    return base::make_err(codegen::EmitError::Unsupported);
  }

  const std::vector<bool> reachable =
      codegen::reachable_functions(storage, std::span(&*entry, 1));

  // One section for the code, every function at its own offset, and the
  // patches each body's references asked for, moved by that offset.
  object::Section text{.name = ".text",
                       .bytes = {},
                       .align = 16,
                       .exec = true,
                       .alloc = true,
                       .bits = true};
  object::Object object;

  // The name the platform's startup reads is a wrapper's, not the
  // program's own: a `main` that returns nothing still has to answer 0,
  // and a program that calls `main` itself calls a name of its own.
  const std::string entry_symbol = "alcy_main";

  // One body's bytes, its symbol, and the patches its references asked
  // for, moved by the offset it landed at.
  const auto emit_body = [&](Body& body, const std::string& symbol,
                             const object::SymbolBind bind) {
    const Frame frame = layout_frame(body, storage, request.target.width);
    allocate(body, frame);
    Encoded encoded = encode(body);
    const u64 at = text.bytes.size();
    text.bytes.insert(text.bytes.end(), encoded.bytes.begin(),
                      encoded.bytes.end());
    object.symbols.push_back(object::Symbol{
        .name = symbol,
        .section = 0,
        .offset = at,
        .size = encoded.bytes.size(),
        .bind = bind,
        .type = object::SymbolType::Func,
    });
    for (Encoded::Patch& patch : encoded.patches) {
      object.relocations.push_back(object::Relocation{
          .section = 0,
          .offset = at + patch.offset,
          .kind = patch.kind,
          .symbol = std::move(patch.symbol),
          .addend = patch.addend,
      });
    }
  };

  // The runtime names the selected bodies reach, which is what this
  // object has to define.
  std::vector<std::string> runtime_names;

  for (const ir::FunctionIdx index : storage.functions().idx_range()) {
    if (!reachable[index.idx]) {
      continue;
    }
    const ir::Function& function = storage.functions()[index];
    base::Result<Body, codegen::EmitError> selected =
        select_function(storage, strings, request.instr_spans, function, bag,
                        request.target.width, rodata, request.overlay);
    if (selected.is_err()) {
      return base::make_err(std::move(selected).unwrap_err());
    }
    Body body = std::move(selected).unwrap();
    for (const std::string& name : body.symbols) {
      if (!runtime::defines(name)) {
        continue;
      }
      // A runtime piece this backend has not written yet is a refusal
      // at the construct that reached it, not an object the link
      // refuses over a name nobody carries.
      bool already = false;
      for (const std::string& known : runtime_names) {
        if (known == name) {
          already = true;
          break;
        }
      }
      if (!already) {
        runtime_names.push_back(name);
      }
    }
    emit_body(body, index == *entry ? entry_symbol : internal_symbol(index),
              object::SymbolBind::Local);
  }

  // The runtime, for the names the program reaches. A body is built
  // before the data section is written, because the text it prints is
  // data too.
  for (const std::string& name : runtime_names) {
    Body body = runtime::build(name, rodata, request.options.freestanding);
    emit_body(body, name, object::SymbolBind::Local);
  }

  // The wrapper the startup calls. It hands the program's own `main`
  // the call and answers 0 when `main` returned nothing.
  {
    Body wrapper;
    wrapper.symbols.push_back(entry_symbol);
    if (!request.options.freestanding) {
      // `main(argc, argv)` hands the process arguments to the runtime
      // globals before the program body runs.
      const u32 argc = wrapper.symbols.size();
      wrapper.symbols.push_back("alcy_argc");
      const u32 argv = wrapper.symbols.size();
      wrapper.symbols.push_back("alcy_argv");
      wrapper.insts.push_back(
          make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{SymbolRef{argc}}));
      wrapper.insts.push_back(make_inst(
          MOp::Mov, 4,
          Operand{Mem{
              .base = Reg::Rax, .index = std::nullopt, .scale = 1, .disp = 0}},
          Operand{Reg::Rdi}));
      wrapper.insts.push_back(
          make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{SymbolRef{argv}}));
      wrapper.insts.push_back(make_inst(
          MOp::Mov, 8,
          Operand{Mem{
              .base = Reg::Rax, .index = std::nullopt, .scale = 1, .disp = 0}},
          Operand{Reg::Rsi}));
    }
    wrapper.insts.push_back(make_inst(MOp::Call, 8, Operand{SymbolRef{0}}));
    if (storage.types()[storage.functions()[*entry].meta.return_type.idx].tag ==
        ir::TypeTag::Void) {
      wrapper.insts.push_back(
          make_inst(MOp::Xor, 4, Operand{Reg::Rax}, Operand{Reg::Rax}));
    }
    if (request.options.freestanding) {
      // `_start` owns the process: the answer from `main` becomes the
      // status of the exit syscall, with no startup object to return to.
      wrapper.insts.push_back(
          make_inst(MOp::Mov, 8, Operand{Reg::Rdi}, Operand{Reg::Rax}));
      wrapper.insts.push_back(
          make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{Imm{60}}));
      wrapper.insts.push_back(make_inst(MOp::Syscall, 8));
    } else {
      wrapper.insts.push_back(make_inst(MOp::Ret, 8));
    }
    Encoded encoded = encode(wrapper);
    const u64 at = text.bytes.size();
    text.bytes.insert(text.bytes.end(), encoded.bytes.begin(),
                      encoded.bytes.end());
    object.symbols.push_back(object::Symbol{
        .name = request.options.freestanding ? "_start" : "main",
        .section = 0,
        .offset = at,
        .size = encoded.bytes.size(),
        .bind = object::SymbolBind::Global,
        .type = object::SymbolType::Func,
    });
    for (Encoded::Patch& patch : encoded.patches) {
      object.relocations.push_back(object::Relocation{
          .section = 0,
          .offset = at + patch.offset,
          .kind = patch.kind,
          .symbol = std::move(patch.symbol),
          .addend = patch.addend,
      });
    }
  }

  object.sections.push_back(std::move(text));

  // The read-only data, after every body and runtime piece has said
  // what it names. It is the second section, which is the index its
  // symbols spell.
  if (!rodata.names().empty()) {
    object::Section data{.name = ".rodata",
                         .bytes = {},
                         .align = 1,
                         .alloc = true,
                         .bits = true};
    for (usize at = 0; at < rodata.names().size(); ++at) {
      const u64 offset = data.bytes.size();
      const std::string& bytes = rodata.bytes()[at];
      data.bytes.insert(data.bytes.end(), bytes.begin(), bytes.end());
      object.symbols.push_back(object::Symbol{
          .name = rodata.names()[at],
          .section = 1,
          .offset = offset,
          .size = bytes.size(),
          .bind = object::SymbolBind::Local,
          .type = object::SymbolType::Object,
      });
    }
    object.sections.push_back(std::move(data));
  }

  // The process arguments the entry wrapper saved, in the third section.
  // Only hosted builds have a `main` that received them.
  if (!request.options.freestanding) {
    object::Section globals{.name = ".data",
                            .bytes = std::vector<u8>(16, 0),
                            .align = 8,
                            .alloc = true,
                            .bits = true};
    const u32 section = static_cast<u32>(object.sections.size());
    object.symbols.push_back(
        object::Symbol{.name = "alcy_argc",
                       .section = section,
                       .offset = 0,
                       .size = 8,
                       .bind = object::SymbolBind::Global,
                       .type = object::SymbolType::Object});
    object.symbols.push_back(
        object::Symbol{.name = "alcy_argv",
                       .section = section,
                       .offset = 8,
                       .size = 8,
                       .bind = object::SymbolBind::Global,
                       .type = object::SymbolType::Object});
    object.sections.push_back(std::move(globals));
  }

  // The names the bodies reach but this object does not carry: the
  // platform's own allocator, and whatever else a runtime body asked
  // for. They ride the symbol table as undefined, and the link resolves
  // them.
  for (const object::Relocation& reloc : object.relocations) {
    bool carried = false;
    for (const object::Symbol& symbol : object.symbols) {
      if (symbol.name == reloc.symbol) {
        carried = true;
        break;
      }
    }
    if (!carried) {
      object.symbols.push_back(object::Symbol{
          .name = reloc.symbol,
          .section = object::Symbol::UNDEFINED,
          .bind = object::SymbolBind::Global,
      });
    }
  }

  return base::make_ok(object::write_elf64_x86_64(object));
}

}  // namespace codegen::x86_64
