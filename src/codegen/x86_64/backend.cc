// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/backend.h"

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
#include "codegen/x86_64/select.h"
#include "debug/dcheck.h"
#include "i18n/messages.h"

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

bool X86_64::supports(const codegen::OutputKind kind,
                      const codegen::Target& target) {
  return kind == codegen::OutputKind::Object &&
         target.triple.rfind("x86_64", 0) == 0 &&
         target.triple.find("linux") != std::string::npos;
}

base::Result<std::vector<u8>, codegen::EmitError> X86_64::emit(
    codegen::EmitRequest request) {
  if (request.options.freestanding) {
    // The freestanding entry and the runtime it calls are the next thing
    // this backend grows, not something it can half-answer.
    return base::make_err(codegen::EmitError::Unsupported);
  }
  ir::Storage storage = std::move(request.storage).unwrap();
  const ir::SymbolTable& strings = *request.strings;
  diag::DiagBag& bag = *request.bag;

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

  for (const ir::FunctionIdx index : storage.functions().idx_range()) {
    if (!reachable[index.idx]) {
      continue;
    }
    const ir::Function& function = storage.functions()[index];
    base::Result<Body, codegen::EmitError> selected =
        select_function(storage, strings, request.instr_spans, function, bag,
                        request.target.width);
    if (selected.is_err()) {
      return base::make_err(std::move(selected).unwrap_err());
    }
    Body body = std::move(selected).unwrap();
    const Frame frame = layout_frame(body, storage, request.target.width);
    allocate(body, frame);
    Encoded encoded = encode(body);

    const u64 at = text.bytes.size();
    text.bytes.insert(text.bytes.end(), encoded.bytes.begin(),
                      encoded.bytes.end());
    object.symbols.push_back(object::Symbol{
        .name = index == *entry ? entry_symbol
                                : "alcy.fn." + std::to_string(index.idx),
        .section = 0,
        .offset = at,
        .size = encoded.bytes.size(),
        .bind = object::SymbolBind::Local,
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

  // The wrapper the startup calls. It hands the program's own `main`
  // the call and answers 0 when `main` returned nothing.
  {
    Body wrapper;
    wrapper.symbols.push_back(entry_symbol);
    wrapper.insts.push_back(make_inst(MOp::Call, 8, Operand{SymbolRef{0}}));
    if (storage.types()[storage.functions()[*entry].meta.return_type.idx].tag ==
        ir::TypeTag::Void) {
      wrapper.insts.push_back(
          make_inst(MOp::Xor, 4, Operand{Reg::Rax}, Operand{Reg::Rax}));
    }
    wrapper.insts.push_back(make_inst(MOp::Ret, 8));
    Encoded encoded = encode(wrapper);
    const u64 at = text.bytes.size();
    text.bytes.insert(text.bytes.end(), encoded.bytes.begin(),
                      encoded.bytes.end());
    object.symbols.push_back(object::Symbol{
        .name = "main",
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
  return base::make_ok(object::write_elf64_x86_64(object));
}

}  // namespace codegen::x86_64
