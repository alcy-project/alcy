// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/text_dump.h"

#include <algorithm>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "debug/dcheck.h"
#include "debug/fatal.h"
#include "diag/span.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"
#include "ir/block.h"
#include "ir/block_param.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/instruction_flags.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/register.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "ir/type_util.h"
#include "ir/write_input.h"

namespace ir {
namespace {

constexpr u32 NO_ENTRY = U32_MAX;

std::string_view kind_word(SymbolKind kind) {
  switch (kind) {
    case SymbolKind::Foreign: return "foreign";
    case SymbolKind::Free: return "free";
    case SymbolKind::Assoc: return "assoc";
    case SymbolKind::Method: return "method";
  }
  return "free";
}

// The text writer. One instance dumps one input; the output is the same
// for the same input, because every table is walked by index.
class TextWriter {
 public:
  explicit TextWriter(const WriteInput& input)
      : input_(input),
        state_(input.storage->state()),
        strings_(*input.strings),
        spans_(input.instr_spans),
        files_(input.files),
        addr_names_(input.addr_names),
        prelude_(input.prelude_functions) {
    count_function_names();
    index_addr_names();
  }

  std::string run() {
    write_header();
    out_ += '\n';
    write_preamble();
    for (u32 i = 0; i < state_.functions.size(); ++i) {
      const FunctionIdx function(i);
      if (i > 0) {
        out_ += '\n';
      }
      write_function(function);
    }
    return out_;
  }

 private:
  const WriteInput& input_;
  const StorageState& state_;
  const ir::SymbolTable& strings_;
  std::span<const diag::Span> spans_;
  FileTable files_;
  std::span<const AddrName> addr_names_;
  usize prelude_;

  std::string out_;
  // How many dumped functions share a name, so a reference can add the
  // index when the bare name would be ambiguous. Lookups only, never
  // iterated, so the output does not depend on the map.
  std::unordered_map<u32, u32> function_name_counts_;
  // Register -> index in `addr_names_`, or NO_ENTRY.
  std::vector<u32> addr_name_of_reg_;

  void count_function_names() {
    for (u32 i = 0; i < state_.functions.size(); ++i) {
      const FunctionMeta& meta = state_.functions[FunctionIdx(i)].meta;
      if (meta.name == str::INVALID_STRING_POOL_ID ||
          meta.name == str::EMPTY_STRING_ID) {
        continue;
      }
      ++function_name_counts_[meta.name.offset];
    }
  }

  void index_addr_names() {
    addr_name_of_reg_.assign(state_.registers.size(), NO_ENTRY);
    u32 entry = 0;
    for (const AddrName& name : addr_names_) {
      if (name.reg.idx < addr_name_of_reg_.size()) {
        addr_name_of_reg_[name.reg.idx] = entry;
      }
      ++entry;
    }
  }

  [[nodiscard]] std::string_view string_of(str::StringPoolId id) const {
    if (id == str::INVALID_STRING_POOL_ID) {
      return "<invalid>";
    }
    return strings_.get(id);
  }

  // The name a function prints under: its own when that is unambiguous
  // among the dumped functions, its index when two share it, and its
  // index alone when it has no name.
  [[nodiscard]] std::string function_name(FunctionIdx function) const {
    const FunctionMeta& meta = state_.functions[function].meta;
    const std::string_view name = string_of(meta.name);
    if (meta.name == str::INVALID_STRING_POOL_ID ||
        meta.name == str::EMPTY_STRING_ID) {
      return fmt::format("#{}", function.idx);
    }
    const auto found = function_name_counts_.find(meta.name.offset);
    if (found != function_name_counts_.end() && found->second > 1) {
      return fmt::format("{}#{}", name, function.idx);
    }
    return std::string(name);
  }

  [[nodiscard]] std::string external_name(ExternalFunctionIdx external) const {
    const FunctionMeta& meta = state_.external_functions[external].meta;
    if (meta.name == str::INVALID_STRING_POOL_ID ||
        meta.name == str::EMPTY_STRING_ID) {
      return fmt::format("#{}", external.idx);
    }
    return std::string(string_of(meta.name));
  }

  [[nodiscard]] std::string composite_name(TypeIdx type) const {
    const TypeNode& node = state_.types[type];
    const str::StringPoolId id =
        node.tag == TypeTag::Struct ? state_.struct_types[node.as_struct()].name
                                    : state_.enum_types[node.as_enum()].name;
    const std::string_view name = string_of(id);
    if (id == str::INVALID_STRING_POOL_ID || id == str::EMPTY_STRING_ID) {
      return fmt::format("#{}", type.idx);
    }
    return std::string(name);
  }

  void write_type(TypeIdx type) {
    const TypeNode& node = state_.types[type];
    switch (node.tag) {
      case TypeTag::Void: out_ += "()"; break;
      case TypeTag::I1: out_ += "bool"; break;
      case TypeTag::I8: out_ += "i8"; break;
      case TypeTag::I16: out_ += "i16"; break;
      case TypeTag::I32: out_ += "i32"; break;
      case TypeTag::I64: out_ += "i64"; break;
      case TypeTag::U8: out_ += "u8"; break;
      case TypeTag::U16: out_ += "u16"; break;
      case TypeTag::U32: out_ += "u32"; break;
      case TypeTag::U64: out_ += "u64"; break;
      case TypeTag::F32: out_ += "f32"; break;
      case TypeTag::F64: out_ += "f64"; break;
      case TypeTag::Str: out_ += "str"; break;
      case TypeTag::Ptr: out_ += "ptr"; break;
      case TypeTag::Never: out_ += '!'; break;
      case TypeTag::Error: out_ += "<error>"; break;
      case TypeTag::Ref:
        out_ += '&';
        write_type(state_.ref_types[node.as_ref()].pointee);
        break;
      case TypeTag::MutRef:
        out_ += "&mut ";
        write_type(state_.ref_types[node.as_ref()].pointee);
        break;
      case TypeTag::RawPtr:
        out_ += '*';
        write_type(state_.ref_types[node.as_ref()].pointee);
        break;
      case TypeTag::RawMutPtr:
        out_ += "*mut ";
        write_type(state_.ref_types[node.as_ref()].pointee);
        break;
      case TypeTag::Slice:
        out_ += '[';
        write_type(state_.slice_types[node.as_slice()].element);
        out_ += ']';
        break;
      case TypeTag::Array: {
        const ArrayType& array = state_.array_types[node.as_array()];
        out_ += '[';
        write_type(array.element);
        fmt::format_to(std::back_inserter(out_), "; {}]", array.count);
        break;
      }
      case TypeTag::Tuple: {
        const TypeIdxRange elements =
            state_.tuple_types[node.as_tuple()].elements;
        out_ += '(';
        for (u32 i = 0; i < elements.size(); ++i) {
          if (i > 0) {
            out_ += ", ";
          }
          write_type(elements[i]);
        }
        if (elements.size() == 1) {
          out_ += ',';
        }
        out_ += ')';
        break;
      }
      case TypeTag::Struct:
      case TypeTag::Enum: {
        out_ += composite_name(type);
        const TypeIdxRange params =
            node.tag == TypeTag::Struct
                ? state_.struct_types[node.as_struct()].params
                : state_.enum_types[node.as_enum()].params;
        write_generics(params);
        break;
      }
      case TypeTag::Function:
      case TypeTag::Func: {
        const FuncType& signature = state_.func_types[node.as_func()];
        const bool closure = node.tag == TypeTag::Func;
        if (!closure) {
          out_ += "fn";
        }
        write_signature(signature);
        break;
      }
    }
  }

  void write_generics(TypeIdxRange params) {
    if (params.empty()) {
      return;
    }
    out_ += '<';
    for (u32 i = 0; i < params.size(); ++i) {
      if (i > 0) {
        out_ += ", ";
      }
      write_type(params[i]);
    }
    out_ += '>';
  }

  void write_signature(const FuncType& signature) {
    out_ += '(';
    for (u32 i = 0; i < signature.params.size(); ++i) {
      if (i > 0) {
        out_ += ", ";
      }
      write_type(signature.params[i]);
    }
    out_ += ')';
    if (state_.types[signature.ret].tag != TypeTag::Void) {
      out_ += " -> ";
      write_type(signature.ret);
    }
  }

  void write_header() {
    const u32 width = input_.width == PointerWidth::W64 ? 64 : 32;
    fmt::format_to(std::back_inserter(out_),
                   "// alcy ir, format 1, pointer width {}\n", width);
    if (prelude_ != 0) {
      fmt::format_to(std::back_inserter(out_), "// prelude: {} functions\n",
                     prelude_);
    }
  }

  void write_preamble() {
    bool wrote = false;
    for (u32 i = 0; i < state_.types.size(); ++i) {
      const TypeIdx type(i);
      const TypeTag tag = state_.types[type].tag;
      if (tag != TypeTag::Struct && tag != TypeTag::Enum) {
        continue;
      }
      if (tag == TypeTag::Struct) {
        const StructType& strukt =
            state_.struct_types[state_.types[type].as_struct()];
        out_ += "struct ";
        out_ += composite_name(type);
        write_generics(strukt.params);
        out_ += '(';
        for (u32 f = 0; f < strukt.fields.size(); ++f) {
          if (f > 0) {
            out_ += ", ";
          }
          write_type(strukt.fields[f]);
        }
        out_ += ')';
      } else {
        const EnumType& enum_type =
            state_.enum_types[state_.types[type].as_enum()];
        out_ += "enum ";
        out_ += composite_name(type);
        write_generics(enum_type.params);
        out_ += " { ";
        for (u32 v = 0; v < enum_type.variants.size(); ++v) {
          if (v > 0) {
            out_ += ", ";
          }
          const EnumVariantType& variant =
              state_.enum_variant_types[enum_type.variants[v]];
          out_ += string_of(variant.name);
          if (!variant.fields.empty()) {
            out_ += '(';
            for (u32 f = 0; f < variant.fields.size(); ++f) {
              if (f > 0) {
                out_ += ", ";
              }
              write_type(variant.fields[f]);
            }
            out_ += ')';
          }
        }
        out_ += " }";
      }
      fmt::format_to(std::back_inserter(out_), "  // #{}", type.idx);
      out_ += '\n';
      wrote = true;
    }
    for (u32 i = 0; i < state_.external_functions.size(); ++i) {
      const ExternalFunction& external =
          state_.external_functions[ExternalFunctionIdx(i)];
      out_ += "extern fn ";
      out_ += external_name(ExternalFunctionIdx(i));
      out_ += '(';
      for (u32 p = 0; p < external.meta.param_types.size(); ++p) {
        if (p > 0) {
          out_ += ", ";
        }
        write_type(external.meta.param_types[p]);
      }
      out_ += ')';
      if (state_.types[external.meta.return_type].tag != TypeTag::Void) {
        out_ += " -> ";
        write_type(external.meta.return_type);
      }
      fmt::format_to(std::back_inserter(out_), "  // #{}", i);
      out_ += '\n';
      wrote = true;
    }
    if (wrote) {
      out_ += '\n';
    }
  }

  void write_function(FunctionIdx function) {
    const Function& body = state_.functions[function];
    const FunctionMeta& meta = body.meta;
    out_ += "fn ";
    out_ += function_name(function);
    out_ += '(';
    const bool has_entry = !body.blocks.empty();
    if (has_entry) {
      const Block& entry = state_.blocks[body.blocks.head()];
      for (u32 i = 0; i < entry.block_params.size(); ++i) {
        if (i > 0) {
          out_ += ", ";
        }
        const BlockParam& param = state_.block_params[entry.block_params[i]];
        fmt::format_to(std::back_inserter(out_), "v{}: ", param.reg.idx);
        write_type(param.type);
      }
    }
    out_ += ')';
    if (state_.types[meta.return_type].tag != TypeTag::Void) {
      out_ += " -> ";
      write_type(meta.return_type);
    }
    fmt::format_to(std::back_inserter(out_), " {{  // #{}", function.idx);
    if (meta.kind != SymbolKind::Free) {
      out_ += ' ';
      out_ += kind_word(meta.kind);
    }
    out_ += '\n';

    for (u32 b = 0; b < body.blocks.size(); ++b) {
      const BlockIdx block_id = body.blocks[b];
      const Block& block = state_.blocks[block_id];
      fmt::format_to(std::back_inserter(out_), "b{}", block_id.idx);
      // The entry block's parameters are the function header's, so
      // repeating them here would only add noise.
      const bool is_entry = block_id.idx == body.blocks.head().idx;
      if (!is_entry && !block.block_params.empty()) {
        out_ += " (";
        for (u32 i = 0; i < block.block_params.size(); ++i) {
          if (i > 0) {
            out_ += ", ";
          }
          const BlockParam& param = state_.block_params[block.block_params[i]];
          fmt::format_to(std::back_inserter(out_), "v{}: ", param.reg.idx);
          write_type(param.type);
        }
        out_ += ')';
      }
      out_ += ":\n";
      for (const InstructionIdx instr : block.instrs) {
        write_instruction(state_.instrs[instr], instr);
      }
    }
    out_ += "}\n";
  }

  void write_instruction(const Instruction& instr, InstructionIdx index) {
    out_ += "  ";
    using O = Opcode;
    switch (instr.op) {
      case O::Noop: out_ += "nop"; break;
      case O::Alloca: {
        write_dst(instr);
        out_ += "alloca ";
        if (instr.dst.is_valid()) {
          write_type(state_.registers[instr.dst].type);
        } else {
          out_ += '?';
        }
        const Operand& size = state_.operands[instr.operands.head()];
        if (!is_immediate_one(size)) {
          out_ += ", ";
          write_operand(size);
        }
        break;
      }
      case O::Load: {
        write_dst(instr);
        out_ += '*';
        write_operand(state_.operands[instr.operands.head()]);
        break;
      }
      case O::Store: {
        out_ += '*';
        write_operand(state_.operands[instr.operands.head() + 1]);
        out_ += " = ";
        write_operand(state_.operands[instr.operands.head()]);
        break;
      }
      case O::GetElementPtr: {
        write_dst(instr);
        out_ += "addr(";
        write_operand(state_.operands[instr.operands.head()]);
        for (u32 i = 1; i < instr.operands.size(); ++i) {
          out_ += ", ";
          write_operand(state_.operands[instr.operands.head() + i]);
        }
        out_ += ')';
        break;
      }
      case O::ElemOffset: {
        write_dst(instr);
        out_ += '&';
        write_operand(state_.operands[instr.operands.head()]);
        out_ += '[';
        write_operand(state_.operands[instr.operands.head() + 1]);
        out_ += ']';
        break;
      }
      case O::ExtractValue: {
        write_dst(instr);
        write_operand(state_.operands[instr.operands.head()]);
        for (u32 i = 1; i < instr.operands.size(); ++i) {
          fmt::format_to(
              std::back_inserter(out_), ".{}",
              operand_u32(state_.operands[instr.operands.head() + i]));
        }
        break;
      }
      case O::InsertValue: {
        write_dst(instr);
        out_ += "insert(";
        write_operand(state_.operands[instr.operands.head()]);
        out_ += ", ";
        write_operand(state_.operands[instr.operands.head() + 1]);
        for (u32 i = 2; i < instr.operands.size(); ++i) {
          fmt::format_to(
              std::back_inserter(out_), ", {}",
              operand_u32(state_.operands[instr.operands.head() + i]));
        }
        out_ += ')';
        break;
      }
      case O::Memcopy: {
        out_ += "memcopy(";
        for (u32 i = 0; i < 3; ++i) {
          if (i > 0) {
            out_ += ", ";
          }
          write_operand(state_.operands[instr.operands.head() + i]);
        }
        out_ += ')';
        break;
      }
      case O::IntAdd:
      case O::FAdd: write_binary(instr, "+"); break;
      case O::IntSub:
      case O::FSub: write_binary(instr, "-"); break;
      case O::IntMul:
      case O::FMul: write_binary(instr, "*"); break;
      case O::IntDiv:
      case O::UintDiv:
      case O::FDiv: write_binary(instr, "/"); break;
      case O::IntRem:
      case O::UintRem: write_binary(instr, "%"); break;
      case O::And: write_binary(instr, "&"); break;
      case O::Or: write_binary(instr, "|"); break;
      case O::Xor: write_binary(instr, "^"); break;
      case O::ShiftLeft: write_binary(instr, "<<"); break;
      case O::ArithmeticShiftRight:
      case O::LogicalShiftRight: write_binary(instr, ">>"); break;
      case O::Not: {
        write_dst(instr);
        const Operand& value = state_.operands[instr.operands.head()];
        const TypeTag tag = state_.types[value.type].tag;
        out_ += tag == TypeTag::I1 ? "!" : "~";
        write_operand(value);
        break;
      }
      case O::BitReverse: {
        write_dst(instr);
        out_ += "bit_reverse(";
        write_operand(state_.operands[instr.operands.head()]);
        out_ += ')';
        break;
      }
      case O::Eq: write_binary(instr, "=="); break;
      case O::Ne: write_binary(instr, "!="); break;
      case O::Le: write_binary(instr, "<="); break;
      case O::Lt: write_binary(instr, "<"); break;
      case O::Ge: write_binary(instr, ">="); break;
      case O::Gt: write_binary(instr, ">"); break;
      case O::TypeCast: {
        write_dst(instr);
        write_operand(state_.operands[instr.operands.head()]);
        out_ += " as ";
        if (instr.dst.is_valid()) {
          write_type(state_.registers[instr.dst].type);
        } else {
          out_ += '?';
        }
        break;
      }
      case O::TypeSizeOf:
      case O::TypeAlignOf: {
        write_dst(instr);
        out_ += instr.op == O::TypeSizeOf ? "size_of(" : "align_of(";
        write_type(instr.measure);
        out_ += ')';
        break;
      }
      case O::Select: {
        write_dst(instr);
        out_ += "select(";
        for (u32 i = 0; i < 3; ++i) {
          if (i > 0) {
            out_ += ", ";
          }
          write_operand(state_.operands[instr.operands.head() + i]);
        }
        out_ += ')';
        break;
      }
      case O::Br: {
        out_ += "br ";
        write_operand(state_.operands[instr.operands.head()]);
        write_branch_args(instr);
        break;
      }
      case O::CondBr: {
        out_ += "condbr ";
        write_operand(state_.operands[instr.operands.head()]);
        out_ += ", ";
        write_operand(state_.operands[instr.operands.head() + 1]);
        out_ += ", ";
        write_operand(state_.operands[instr.operands.head() + 2]);
        break;
      }
      case O::Switch: {
        out_ += "switch ";
        write_operand(state_.operands[instr.operands.head()]);
        out_ += ", [";
        for (u32 i = 2; i + 1 < instr.operands.size(); i += 2) {
          if (i > 2) {
            out_ += ", ";
          }
          write_operand(state_.operands[instr.operands.head() + i]);
          out_ += " -> ";
          write_operand(state_.operands[instr.operands.head() + i + 1]);
        }
        out_ += "], default ";
        write_operand(state_.operands[instr.operands.head() + 1]);
        break;
      }
      case O::Call: {
        write_dst(instr);
        write_operand(state_.operands[instr.operands.head()]);
        out_ += '(';
        for (u32 i = 1; i < instr.operands.size(); ++i) {
          if (i > 1) {
            out_ += ", ";
          }
          write_operand(state_.operands[instr.operands.head() + i]);
        }
        out_ += ')';
        break;
      }
      case O::Ret: {
        out_ += "ret";
        if (!instr.operands.empty()) {
          out_ += ' ';
          write_operand(state_.operands[instr.operands.head()]);
        }
        break;
      }
      case O::Unreachable: out_ += "unreachable"; break;
      case O::AtomicLoad: {
        write_dst(instr);
        out_ += "atomic.load(";
        write_operand(state_.operands[instr.operands.head()]);
        out_ += ')';
        break;
      }
      case O::AtomicStore: {
        out_ += "atomic.store(";
        write_operand(state_.operands[instr.operands.head()]);
        out_ += ", ";
        write_operand(state_.operands[instr.operands.head() + 1]);
        out_ += ')';
        break;
      }
      case O::AtomicRmw: {
        write_dst(instr);
        out_ += "atomic.rmw.";
        out_ += rmw_word(instr.flags.rmw_op);
        out_ += '(';
        write_operand(state_.operands[instr.operands.head()]);
        out_ += ", ";
        write_operand(state_.operands[instr.operands.head() + 1]);
        out_ += ')';
        break;
      }
      case O::AtomicCompareExchange: {
        write_dst(instr);
        out_ += "atomic.cas(";
        for (u32 i = 0; i < 3; ++i) {
          if (i > 0) {
            out_ += ", ";
          }
          write_operand(state_.operands[instr.operands.head() + i]);
        }
        out_ += ')';
        break;
      }
      case O::Fence: out_ += "fence"; break;
      case O::Move: {
        write_dst(instr);
        out_ += "move ";
        write_operand(state_.operands[instr.operands.head()]);
        break;
      }
      case O::Drop: {
        out_ += "drop ";
        write_operand(state_.operands[instr.operands.head()]);
        break;
      }
      case O::Borrow: {
        write_dst(instr);
        const bool mutable_borrow =
            instr.dst.is_valid() &&
            state_.types[state_.registers[instr.dst].type].tag ==
                TypeTag::MutRef;
        out_ += mutable_borrow ? "&mut " : "&";
        write_operand(state_.operands[instr.operands.head()]);
        break;
      }
    }
    write_comment(instr, index);
    out_ += '\n';
  }

  void write_dst(const Instruction& instr) {
    if (instr.dst.is_valid()) {
      fmt::format_to(std::back_inserter(out_), "v{} = ", instr.dst.idx);
    }
  }

  void write_binary(const Instruction& instr, std::string_view token) {
    write_dst(instr);
    write_operand(state_.operands[instr.operands.head()]);
    out_ += ' ';
    out_ += token;
    out_ += ' ';
    write_operand(state_.operands[instr.operands.head() + 1]);
  }

  void write_branch_args(const Instruction& instr) {
    if (instr.operands.size() <= 1) {
      return;
    }
    out_ += '(';
    for (u32 i = 1; i < instr.operands.size(); ++i) {
      if (i > 1) {
        out_ += ", ";
      }
      write_operand(state_.operands[instr.operands.head() + i]);
    }
    out_ += ')';
  }

  void write_operand(const Operand& operand) {
    using Payload = Operand::Payload;
    switch (operand.tag()) {
      case Payload::TagOf<RegisterIdx>:
        fmt::format_to(std::back_inserter(out_), "v{}",
                       operand.as_register().idx);
        return;
      case Payload::TagOf<BlockIdx>:
        fmt::format_to(std::back_inserter(out_), "b{}", operand.as_block().idx);
        return;
      case Payload::TagOf<ImmutableIdx>:
        write_immutable(state_.immutables[operand.as_immutable()]);
        return;
      case Payload::TagOf<FunctionIdx>:
        out_ += function_name(operand.as_function());
        return;
      case Payload::TagOf<ExternalFunctionIdx>:
        out_ += external_name(operand.as_external_function());
        return;
      default: out_ += '?'; return;
    }
  }

  void write_immutable(const Immutable& immutable) {
    const TypeTag tag = state_.types[immutable.type].tag;
    if (tag == TypeTag::Str) {
      write_quoted(string_of(immutable.data.str_id_value));
      return;
    }
    if (is_float_type(tag)) {
      if (tag == TypeTag::F32) {
        fmt::format_to(std::back_inserter(out_), "{}",
                       immutable.data.f32_value);
      } else {
        fmt::format_to(std::back_inserter(out_), "{}",
                       immutable.data.f64_value);
      }
      return;
    }
    if (is_integer_type(tag)) {
      write_integer(tag, immutable);
      return;
    }
    const u64 value = immutable.data.ptr;
    if (value == 0) {
      out_ += "null";
    } else {
      fmt::format_to(std::back_inserter(out_), "0x{:x}", value);
    }
  }

  void write_integer(TypeTag tag, const Immutable& immutable) {
    switch (tag) {
      case TypeTag::I1:
        out_ += immutable.data.i1_value ? "true" : "false";
        break;
      case TypeTag::I8:
        fmt::format_to(std::back_inserter(out_), "{}", immutable.data.i8_value);
        break;
      case TypeTag::I16:
        fmt::format_to(std::back_inserter(out_), "{}",
                       immutable.data.i16_value);
        break;
      case TypeTag::I32:
        fmt::format_to(std::back_inserter(out_), "{}",
                       immutable.data.i32_value);
        break;
      case TypeTag::I64:
        fmt::format_to(std::back_inserter(out_), "{}",
                       immutable.data.i64_value);
        break;
      case TypeTag::U8:
        fmt::format_to(std::back_inserter(out_), "{}", immutable.data.u8_value);
        break;
      case TypeTag::U16:
        fmt::format_to(std::back_inserter(out_), "{}",
                       immutable.data.u16_value);
        break;
      case TypeTag::U32:
        fmt::format_to(std::back_inserter(out_), "{}",
                       immutable.data.u32_value);
        break;
      case TypeTag::U64:
        fmt::format_to(std::back_inserter(out_), "{}",
                       immutable.data.u64_value);
        break;
      default: UNREACHABLE();
    }
  }

  static void write_quoted(std::string& out, std::string_view bytes) {
    out += '"';
    for (const char raw : bytes) {
      const u8 byte = static_cast<u8>(raw);
      switch (byte) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case 0: out += "\\0"; break;
        default:
          if (byte >= 0x20 && byte <= 0x7E) {
            out += static_cast<char>(byte);
          } else {
            fmt::format_to(std::back_inserter(out), "\\x{:02x}", byte);
          }
          break;
      }
    }
    out += '"';
  }

  void write_quoted(std::string_view bytes) { write_quoted(out_, bytes); }

  [[nodiscard]] bool is_immediate_one(const Operand& operand) const {
    if (!operand.is<ImmutableIdx>()) {
      return false;
    }
    const Immutable& immutable = state_.immutables[operand.as_immutable()];
    const TypeTag tag = state_.types[immutable.type].tag;
    if (!is_integer_type(tag)) {
      return false;
    }
    return immutable.as_u64_integer(tag) == 1;
  }

  [[nodiscard]] u32 operand_u32(const Operand& operand) const {
    DCHECK(operand.is<ImmutableIdx>());
    const Immutable& immutable = state_.immutables[operand.as_immutable()];
    const TypeTag tag = state_.types[immutable.type].tag;
    return static_cast<u32>(immutable.as_u64_integer(tag));
  }

  static std::string_view rmw_word(AtomicRmwOp op) {
    switch (op) {
      case AtomicRmwOp::Add: return "add";
      case AtomicRmwOp::Sub: return "sub";
      case AtomicRmwOp::And: return "and";
      case AtomicRmwOp::Or: return "or";
      case AtomicRmwOp::Xor: return "xor";
      case AtomicRmwOp::Exchange: return "xchg";
    }
    return "add";
  }

  void write_comment(const Instruction& instr, InstructionIdx index) {
    std::string comment;
    if (instr.op == Opcode::Alloca && instr.dst.is_valid() &&
        instr.dst.idx < addr_name_of_reg_.size()) {
      const u32 entry = addr_name_of_reg_[instr.dst.idx];
      if (entry != NO_ENTRY) {
        const AddrName& name = addr_names_[entry];
        comment += name.name;
        if (name.is_param || name.is_capture) {
          comment += name.is_param && name.is_capture
                         ? " (param, capture)"
                         : (name.is_param ? " (param)" : " (capture)");
        }
      }
    }
    if (index.idx < spans_.size()) {
      if (!comment.empty()) {
        comment += "  ";
      }
      const diag::Span& span = spans_[index.idx];
      const std::string_view file = files_.name_of(span.file);
      if (file.empty()) {
        comment += "?:";
      } else {
        comment += file;
        comment += ':';
      }
      fmt::format_to(std::back_inserter(comment), "{}+{}", span.offset,
                     span.length);
    }
    if (!comment.empty()) {
      out_ += "  // ";
      out_ += comment;
    }
  }
};

}  // namespace

std::string write_text(const WriteInput& input) {
  DCHECK(input.storage != nullptr);
  DCHECK(input.strings != nullptr);
  TextWriter writer(input);
  return writer.run();
}

}  // namespace ir
