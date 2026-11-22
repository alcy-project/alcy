// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/backend.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
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
#include "symbol/mangle.h"

namespace codegen::x86_64 {

namespace {

// The name a dependent links against for an exported function: the
// same mangling every other backend gives the IR signature.
std::string linkable_name(const ir::Function& function,
                          const ir::Storage& storage,
                          const ir::SymbolTable& strings) {
  symbol::Signature signature;
  signature.path = std::string(strings.get(function.meta.path));
  signature.name = std::string(strings.get(function.meta.name));
  signature.owner = function.meta.owner;
  switch (function.meta.kind) {
    case ir::SymbolKind::Free:
      signature.kind = symbol::Signature::Kind::Free;
      break;
    case ir::SymbolKind::Assoc:
      signature.kind = symbol::Signature::Kind::Assoc;
      break;
    case ir::SymbolKind::Method:
      signature.kind = symbol::Signature::Kind::Method;
      break;
    default: break;
  }
  const ir::TypeIdxRange generics = function.meta.generics;
  signature.generics.reserve(generics.size());
  for (u32 i = 0; i < generics.size(); ++i) {
    signature.generics.emplace_back(generics.head().idx + i);
  }
  return symbol::mangle(signature, storage, strings);
}

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
  return tag == ir::TypeTag::Void || tag == ir::TypeTag::I32 ||
         tag == ir::TypeTag::Enum;
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
  if (request.emit_entry) {
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
  }

  // A library has no entry to prune from: every free function is a
  // root, narrowed to the surface when visibility reaches the IR.
  std::vector<ir::FunctionIdx> roots;
  if (entry.has_value()) {
    roots.push_back(*entry);
  } else {
    std::vector<bool> parametric(storage.functions().size(), false);
    for (const ir::FunctionIdx index : storage.parametric_functions()) {
      parametric[index.idx] = true;
    }
    for (const ir::FunctionIdx index : storage.functions().idx_range()) {
      if (storage.functions()[index].meta.kind == ir::SymbolKind::Free &&
          !parametric[index.idx]) {
        roots.push_back(index);
      }
    }
  }
  const std::vector<bool> reachable = codegen::reachable_functions(
      storage, std::span(roots.data(), roots.size()));

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
  struct EmittedBody {
    u64 offset;
    u64 size;
  };
  const auto emit_body = [&](Body& body, const std::string& symbol,
                             const object::SymbolBind bind,
                             Encoded* captured = nullptr) -> EmittedBody {
    const Frame frame = layout_frame(body, storage, request.target.width);
    allocate(body, frame);
    Encoded encoded = encode(body);
    if (captured != nullptr) {
      *captured = encoded;
    }
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
    return EmittedBody{at, encoded.bytes.size()};
  };

  // The names a backtrace shows, gathered while bodies are laid out.
  struct BacktraceFunction {
    std::string symbol;
    std::string name;
    u64 size;
  };
  struct BacktraceSite {
    std::string symbol;
    i64 addend;
    std::string site;
  };
  std::vector<BacktraceFunction> backtrace_functions;
  std::vector<BacktraceSite> backtrace_sites;

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
                        request.target.width, rodata, request.overlay,
                        request.instr_site_names, request.options.backtrace);
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
    const bool exported = request.exported_functions.has_value() &&
                          std::find(request.exported_functions->begin(),
                                    request.exported_functions->end(),
                                    index) != request.exported_functions->end();
    // A library's surface is named as the IR spells it: a dependent's
    // object refers to the mangled name, not this backend's private
    // numbering.
    const std::string symbol = index == entry ? entry_symbol
                               : exported
                                   ? linkable_name(function, storage, strings)
                                   : internal_symbol(index);
    Encoded encoded;
    const EmittedBody emitted = emit_body(
        body, symbol,
        exported ? object::SymbolBind::Global : object::SymbolBind::Local,
        request.options.backtrace ? &encoded : nullptr);
    if (index == entry || exported) {
      // A call or a function value names a function by its IR index,
      // while an entry or an exported surface carries another spelling.
      // Give both names to the same bytes so calls inside the object
      // resolve without depending on the link.
      object.symbols.push_back(object::Symbol{
          .name = internal_symbol(index),
          .section = 0,
          .offset = emitted.offset,
          .size = emitted.size,
          .bind = object::SymbolBind::Local,
          .type = object::SymbolType::Func,
      });
    }
    if (request.options.backtrace) {
      std::string shown = std::string(strings.get(function.meta.path));
      if (!shown.empty()) {
        shown += "::";
      }
      shown += strings.get(function.meta.name);
      backtrace_functions.push_back(
          BacktraceFunction{symbol, shown, encoded.bytes.size()});
      for (const Body::CallSite& call : body.call_sites) {
        if (call.label < encoded.label_offsets.size()) {
          backtrace_sites.push_back(BacktraceSite{
              symbol,
              static_cast<i64>(encoded.label_offsets[call.label]) +
                  call.call_len,
              call.site});
        }
      }
    }
  }

  // The env wrappers call alcy_env_find; make sure it is emitted too.
  const bool uses_env = std::find(runtime_names.begin(), runtime_names.end(),
                                  "alcy_env_exists") != runtime_names.end() ||
                        std::find(runtime_names.begin(), runtime_names.end(),
                                  "alcy_env_get") != runtime_names.end();
  if (uses_env && std::find(runtime_names.begin(), runtime_names.end(),
                            "alcy_env_find") == runtime_names.end()) {
    runtime_names.push_back("alcy_env_find");
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
  if (request.emit_entry) {
    Body wrapper;
    wrapper.symbols.push_back(entry_symbol);
    // The C runtime enters `main` with rsp%16==8. A pushed frame pointer
    // restores the SysV alignment the call to alcy_main below needs;
    // the freestanding _start already enters aligned and never returns.
    if (!request.options.freestanding) {
      wrapper.insts.push_back(make_inst(MOp::Push, 8, Operand{Reg::Rbp}));
    }
    const ir::TypeIdx return_type =
        storage.functions()[*entry].meta.return_type;
    const ir::TypeTag return_tag = storage.types()[return_type.idx].tag;
    const bool returns_enum = return_tag == ir::TypeTag::Enum;
    const bool returns_void = return_tag == ir::TypeTag::Void;

    // The argument runtime reads these globals. A hosted main receives
    // the pair in registers; at the ELF entry the initial stack holds
    // argc and then the argv pointers, so the freestanding wrapper
    // gathers them from there before it touches the stack itself.
    const u32 argc = wrapper.symbols.size();
    wrapper.symbols.push_back("alcy_argc");
    const u32 argv = wrapper.symbols.size();
    wrapper.symbols.push_back("alcy_argv");
    const u32 environ = wrapper.symbols.size();
    wrapper.symbols.push_back("alcy_environ");
    if (request.options.freestanding) {
      wrapper.insts.push_back(make_inst(MOp::Mov, 8, Operand{Reg::Rax},
                                        Operand{Mem{.base = Reg::Rsp,
                                                    .index = std::nullopt,
                                                    .scale = 1,
                                                    .disp = 0}}));
      wrapper.insts.push_back(
          make_inst(MOp::Mov, 8, Operand{Reg::Rcx}, Operand{SymbolRef{argc}}));
      wrapper.insts.push_back(make_inst(
          MOp::Mov, 8,
          Operand{Mem{
              .base = Reg::Rcx, .index = std::nullopt, .scale = 1, .disp = 0}},
          Operand{Reg::Rax}));
      wrapper.insts.push_back(make_inst(MOp::Lea, 8, Operand{Reg::Rax},
                                        Operand{Mem{.base = Reg::Rsp,
                                                    .index = std::nullopt,
                                                    .scale = 1,
                                                    .disp = 8}}));
      wrapper.insts.push_back(
          make_inst(MOp::Mov, 8, Operand{Reg::Rcx}, Operand{SymbolRef{argv}}));
      wrapper.insts.push_back(make_inst(
          MOp::Mov, 8,
          Operand{Mem{
              .base = Reg::Rcx, .index = std::nullopt, .scale = 1, .disp = 0}},
          Operand{Reg::Rax}));
      // envp follows argv and its terminating null: rsp + 8 + (argc+1)*8.
      // argc was loaded into rax at the top of this branch and rsp still
      // points at the initial stack.
      wrapper.insts.push_back(make_inst(MOp::Mov, 8, Operand{Reg::Rax},
                                        Operand{Mem{.base = Reg::Rsp,
                                                    .index = std::nullopt,
                                                    .scale = 1,
                                                    .disp = 0}}));
      wrapper.insts.push_back(make_inst(MOp::Lea, 8, Operand{Reg::Rax},
                                        Operand{Mem{.base = Reg::Rsp,
                                                    .index = RegRef{Reg::Rax},
                                                    .scale = 8,
                                                    .disp = 16}}));
      wrapper.insts.push_back(make_inst(MOp::Mov, 8, Operand{Reg::Rcx},
                                        Operand{SymbolRef{environ}}));
      wrapper.insts.push_back(make_inst(
          MOp::Mov, 8,
          Operand{Mem{
              .base = Reg::Rcx, .index = std::nullopt, .scale = 1, .disp = 0}},
          Operand{Reg::Rax}));
    } else {
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
      // A hosted main receives envp as its third argument.
      wrapper.insts.push_back(make_inst(MOp::Mov, 8, Operand{Reg::Rax},
                                        Operand{SymbolRef{environ}}));
      wrapper.insts.push_back(make_inst(
          MOp::Mov, 8,
          Operand{Mem{
              .base = Reg::Rax, .index = std::nullopt, .scale = 1, .disp = 0}},
          Operand{Reg::Rdx}));
    }

    // An enum answer arrives in a slot the caller owns. The wrapper
    // reserves it and reads the discriminant back to pick the status.
    i64 sret_bytes = 0;
    if (returns_enum) {
      const ir::TypeLayout layout =
          storage.layout_of(return_type, request.target.width);
      sret_bytes = static_cast<i64>(ir::align_up(layout.size, 16));
      wrapper.insts.push_back(
          make_inst(MOp::Sub, 8, Operand{Reg::Rsp}, Operand{Imm{sret_bytes}}));
    }
    if (returns_enum) {
      wrapper.insts.push_back(
          make_inst(MOp::Mov, 8, Operand{Reg::Rdi}, Operand{Reg::Rsp}));
    }
    wrapper.insts.push_back(make_inst(MOp::Call, 8, Operand{SymbolRef{0}}));

    if (returns_void) {
      wrapper.insts.push_back(
          make_inst(MOp::Xor, 4, Operand{Reg::Rax}, Operand{Reg::Rax}));
    } else if (returns_enum) {
      // A variant past the first is an error: the status is one and
      // the reason goes to stderr, the same as the other backends.
      constexpr std::string_view kError = "main returned Err\n";
      const std::string& message = rodata.intern(kError);
      const u32 message_id = static_cast<u32>(wrapper.symbols.size());
      wrapper.symbols.push_back(message);
      wrapper.insts.push_back(make_inst(MOp::Mov, 4, Operand{Reg::Rax},
                                        Operand{Mem{.base = Reg::Rsp,
                                                    .index = std::nullopt,
                                                    .scale = 1,
                                                    .disp = 0}}));
      wrapper.insts.push_back(
          make_inst(MOp::Cmp, 4, Operand{Reg::Rax}, Operand{Imm{0}}));
      const u32 is_ok = wrapper.label_count++;
      wrapper.insts.push_back(make_jcc(Cond::E, Operand{Label{is_ok}}));
      wrapper.insts.push_back(
          make_inst(MOp::Mov, 8, Operand{Reg::Rdx},
                    Operand{Imm{static_cast<i64>(kError.size())}}));
      wrapper.insts.push_back(make_inst(MOp::Mov, 8, Operand{Reg::Rsi},
                                        Operand{SymbolRef{message_id}}));
      wrapper.insts.push_back(
          make_inst(MOp::Mov, 4, Operand{Reg::Rdi}, Operand{Imm{2}}));
      wrapper.insts.push_back(
          make_inst(MOp::Mov, 4, Operand{Reg::Rax}, Operand{Imm{1}}));
      wrapper.insts.push_back(make_inst(MOp::Syscall, 8));
      wrapper.insts.push_back(
          make_inst(MOp::Mov, 4, Operand{Reg::Rax}, Operand{Imm{1}}));
      const u32 join = wrapper.label_count++;
      wrapper.insts.push_back(make_inst(MOp::Jmp, 8, Operand{Label{join}}));
      MInst at_ok = make_inst(MOp::Nop, 8);
      at_ok.label = is_ok;
      wrapper.insts.push_back(at_ok);
      wrapper.insts.push_back(
          make_inst(MOp::Xor, 4, Operand{Reg::Rax}, Operand{Reg::Rax}));
      MInst at_join = make_inst(MOp::Nop, 8);
      at_join.label = join;
      wrapper.insts.push_back(at_join);
      wrapper.insts.push_back(
          make_inst(MOp::Add, 8, Operand{Reg::Rsp}, Operand{Imm{sret_bytes}}));
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
      wrapper.insts.push_back(make_inst(MOp::Pop, 8, Operand{Reg::Rbp}));
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

  // The tables a panic walk reads. Each is an array of
  // {address, text pointer, text length} with a zero address ending it.
  // They ride at the end of .text so the object's single relocation
  // section names the right segment; the strings themselves stay in
  // rodata.
  const u32 section = 0;
  while (text.bytes.size() % 8 != 0) {
    text.bytes.push_back(0);
  }
  const auto put_u64 = [&](u64 value) {
    for (u32 at = 0; at < 8; ++at) {
      text.bytes.push_back(static_cast<u8>(value >> (at * 8)));
    }
  };
  const auto put_address = [&](const std::string& symbol, i64 addend) {
    object.relocations.push_back(object::Relocation{
        .section = section,
        .offset = text.bytes.size(),
        .kind = object::RelocKind::Abs64,
        .symbol = symbol,
        .addend = addend,
    });
    put_u64(0);
  };
  const auto put_text = [&](std::string_view text_bytes) {
    const std::string& name = rodata.intern(text_bytes);
    object.relocations.push_back(object::Relocation{
        .section = section,
        .offset = text.bytes.size(),
        .kind = object::RelocKind::Abs64,
        .symbol = name,
        .addend = 0,
    });
    put_u64(0);
    put_u64(text_bytes.size());
  };

  const u64 functions_at = text.bytes.size();
  for (const BacktraceFunction& function : backtrace_functions) {
    put_address(function.symbol, 0);
    put_text(function.name);
    put_u64(function.size);
  }
  put_u64(0);
  put_u64(0);
  put_u64(0);
  put_u64(0);
  object.symbols.push_back(object::Symbol{
      .name = "alcy_bt_funcs_array",
      .section = section,
      .offset = functions_at,
      .size = text.bytes.size() - functions_at,
      .bind = object::SymbolBind::Local,
      .type = object::SymbolType::Object,
  });

  const u64 sites_at = text.bytes.size();
  for (const BacktraceSite& site : backtrace_sites) {
    put_address(site.symbol, site.addend);
    put_text(site.site);
    put_u64(0);  // no size: the site table is not range-checked
  }
  put_u64(0);
  put_u64(0);
  put_u64(0);
  put_u64(0);
  object.symbols.push_back(object::Symbol{
      .name = "alcy_bt_sites_array",
      .section = section,
      .offset = sites_at,
      .size = text.bytes.size() - sites_at,
      .bind = object::SymbolBind::Local,
      .type = object::SymbolType::Object,
  });

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
  // Only an object that actually uses the argument runtime carries them,
  // so a library that never asks for args leaves these to the binary.
  const bool uses_args =
      std::find(runtime_names.begin(), runtime_names.end(), "alcy_arg") !=
          runtime_names.end() ||
      std::find(runtime_names.begin(), runtime_names.end(), "alcy_arg_count") !=
          runtime_names.end() ||
      std::find(runtime_names.begin(), runtime_names.end(), "alcy_env_find") !=
          runtime_names.end() ||
      std::find(runtime_names.begin(), runtime_names.end(),
                "alcy_env_exists") != runtime_names.end() ||
      std::find(runtime_names.begin(), runtime_names.end(), "alcy_env_get") !=
          runtime_names.end();
  if (request.emit_entry || uses_args) {
    object::Section globals{.name = ".data",
                            .bytes = std::vector<u8>(24, 0),
                            .align = 8,
                            .write = true,
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
    object.symbols.push_back(
        object::Symbol{.name = "alcy_environ",
                       .section = section,
                       .offset = 16,
                       .size = 8,
                       .bind = object::SymbolBind::Global,
                       .type = object::SymbolType::Object});
    object.sections.push_back(std::move(globals));
  }

  // The names the bodies reach but this object does not carry: the
  // platform's own allocator, and whatever else a runtime body asked
  // for. They ride the symbol table as undefined, and the link resolves
  // them.
  // Own the names: inserting into object.symbols below can reallocate it,
  // invalidating any string_view that points into an existing symbol.
  std::unordered_set<std::string> carried_names;
  carried_names.reserve(object.symbols.size() + object.relocations.size());
  for (const object::Symbol& symbol : object.symbols) {
    carried_names.insert(symbol.name);
  }
  for (const object::Relocation& reloc : object.relocations) {
    if (carried_names.insert(reloc.symbol).second) {
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
