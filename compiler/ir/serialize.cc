// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/serialize.h"

#include <algorithm>
#include <bit>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "debug/dcheck.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"
#include "ir/binary_format.h"
#include "ir/block.h"
#include "ir/block_param.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/instruction_flags.h"
#include "ir/operand.h"
#include "ir/register.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/write_input.h"

namespace ir {
namespace {

using binary::Section;

// A byte sink for fixed little-endian records. Nothing here is LEB: the
// binary form is fixed width so a reader can index a table directly.
class ByteWriter {
 public:
  void put_u8(u8 value) { bytes_.push_back(value); }
  void put_u16(u16 value) {
    for (u32 i = 0; i < 2; ++i) {
      bytes_.push_back(static_cast<u8>(value >> (8 * i)));
    }
  }
  void put_u32(u32 value) {
    for (u32 i = 0; i < 4; ++i) {
      bytes_.push_back(static_cast<u8>(value >> (8 * i)));
    }
  }
  void put_u64(u64 value) {
    for (u32 i = 0; i < 8; ++i) {
      bytes_.push_back(static_cast<u8>(value >> (8 * i)));
    }
  }
  void bytes(std::span<const u8> data) {
    bytes_.insert(bytes_.end(), data.begin(), data.end());
  }
  void text(std::string_view text) {
    bytes(std::span<const u8>(reinterpret_cast<const u8*>(text.data()),
                              text.size()));
  }
  void zeros(usize count) { bytes_.insert(bytes_.end(), count, 0); }
  [[nodiscard]] usize size() const { return bytes_.size(); }
  [[nodiscard]] std::span<const u8> data() const { return bytes_; }

 private:
  std::vector<u8> bytes_;
};

struct SectionPayload {
  Section kind;
  ByteWriter payload;
};

u64 fnv1a64(std::span<const u8> data) {
  u64 hash = 0xCBF29CE484222325ULL;
  for (const u8 byte : data) {
    hash ^= byte;
    hash *= 0x100000001B3ULL;
  }
  return hash;
}

// The id a `U32_MAX` placeholder stands for: an invalid index or an
// absent entry.
constexpr u32 NONE = U32_MAX;

class Serializer {
 public:
  explicit Serializer(const WriteInput& input)
      : input_(input),
        state_(input.storage->state()),
        strings_(*input.strings),
        spans_(input.instr_spans),
        files_(input.files),
        addr_names_(input.addr_names) {
    DCHECK(input.storage != nullptr);
    DCHECK(input.strings != nullptr);
    DCHECK(spans_.empty() ||
           spans_.size() == static_cast<usize>(state_.instrs.size()));
  }

  std::vector<u8> run() {
    collect_strings();
    std::vector<SectionPayload> sections;
    sections.push_back({Section::Strings, strings_section()});
    sections.push_back({Section::Types, types_section()});
    sections.push_back({Section::Structs, structs_section()});
    sections.push_back({Section::Arrays, arrays_section()});
    sections.push_back({Section::Slices, slices_section()});
    sections.push_back({Section::Enums, enums_section()});
    sections.push_back({Section::EnumVariants, enum_variants_section()});
    sections.push_back({Section::Refs, refs_section()});
    sections.push_back({Section::Tuples, tuples_section()});
    sections.push_back({Section::Funcs, funcs_section()});
    sections.push_back({Section::Functions, functions_section()});
    sections.push_back({Section::Blocks, blocks_section()});
    sections.push_back({Section::BlockParams, block_params_section()});
    sections.push_back({Section::Registers, registers_section()});
    sections.push_back({Section::Instructions, instructions_section()});
    sections.push_back({Section::Operands, operands_section()});
    sections.push_back({Section::Immutables, immutables_section()});
    sections.push_back({Section::Externals, externals_section()});
    sections.push_back({Section::Files, files_section()});
    if (!spans_.empty()) {
      sections.push_back({Section::Spans, spans_section()});
    }
    if (!addr_names_.empty()) {
      sections.push_back({Section::AddrNames, addr_names_section()});
    }

    // The header and the table come first, so an offset depends on the
    // table's own size; compute them before writing anything.
    const std::string_view version = input_.compiler_version;
    const usize header_size = 4 + 2 + 2 + 4 + 4 + 4 + version.size() + 8;
    const usize table_size = sections.size() * 24;
    usize cursor = align_up(header_size + table_size, binary::SECTION_ALIGN);
    std::vector<usize> offsets(sections.size());
    for (usize i = 0; i < sections.size(); ++i) {
      offsets[i] = cursor;
      cursor =
          align_up(cursor + sections[i].payload.size(), binary::SECTION_ALIGN);
    }
    const usize footer_at = cursor;

    ByteWriter out;
    out.bytes(binary::MAGIC);
    out.put_u16(binary::MAJOR);
    out.put_u16(binary::MINOR);
    out.put_u32(binary::FLAG_LITTLE_ENDIAN);
    out.put_u32(input_.width == PointerWidth::W64 ? 64 : 32);
    out.put_u32(static_cast<u32>(version.size()));
    out.text(version);
    out.put_u64(static_cast<u64>(sections.size()));
    for (usize i = 0; i < sections.size(); ++i) {
      out.put_u32(static_cast<u32>(sections[i].kind));
      out.put_u32(0);
      out.put_u64(static_cast<u64>(offsets[i]));
      out.put_u64(static_cast<u64>(sections[i].payload.size()));
    }
    for (usize i = 0; i < sections.size(); ++i) {
      if (out.size() < offsets[i]) {
        out.zeros(offsets[i] - out.size());
      }
      out.bytes(sections[i].payload.data());
    }
    if (out.size() < footer_at) {
      out.zeros(footer_at - out.size());
    }
    out.put_u64(fnv1a64(out.data()));
    return std::vector<u8>(out.data().begin(), out.data().end());
  }

 private:
  const WriteInput& input_;
  const StorageState& state_;
  const str::StringInterner& strings_;
  std::span<const diag::Span> spans_;
  FileTable files_;
  std::span<const AddrName> addr_names_;

  // String bytes in table order, and the content -> index map that
  // deduplicates. Lookups only, never iterated. A deque, not a vector:
  // the map's keys are views into these strings, and a vector would move
  // a short string's own buffer out from under them.
  std::deque<std::string> names_;
  std::unordered_map<std::string_view, u32> name_index_;
  // Pool offset -> string index, filled while the pool strings are added
  // in ascending offset order.
  std::unordered_map<u32, u32> pool_index_;

  u32 add_name(std::string_view bytes) {
    const auto found = name_index_.find(bytes);
    if (found != name_index_.end()) {
      return found->second;
    }
    const u32 index = static_cast<u32>(names_.size());
    names_.emplace_back(bytes);
    name_index_.emplace(std::string_view(names_.back()), index);
    return index;
  }

  [[nodiscard]] u32 find_name(std::string_view bytes) const {
    const auto found = name_index_.find(bytes);
    DCHECK(found != name_index_.end());
    return found == name_index_.end() ? NONE : found->second;
  }

  u32 string_index(str::StringPoolId id) const {
    if (id == str::INVALID_STRING_POOL_ID) {
      return NONE;
    }
    const auto found = pool_index_.find(id.offset);
    DCHECK(found != pool_index_.end());
    return found == pool_index_.end() ? NONE : found->second;
  }

  void collect_pool_id(str::StringPoolId id, std::vector<u32>& offsets) const {
    if (id != str::INVALID_STRING_POOL_ID) {
      offsets.push_back(id.offset);
    }
  }

  // The referenced pool strings, in ascending pool offset order, then
  // the address names in order; identical bytes share one entry.
  void collect_strings() {
    std::vector<u32> offsets;
    for (const Function& function : state_.functions) {
      collect_pool_id(function.meta.name, offsets);
      collect_pool_id(function.meta.path, offsets);
    }
    for (const ExternalFunction& external : state_.external_functions) {
      collect_pool_id(external.meta.name, offsets);
      collect_pool_id(external.meta.path, offsets);
    }
    for (const StructType& strukt : state_.struct_types) {
      collect_pool_id(strukt.name, offsets);
    }
    for (const EnumType& enum_type : state_.enum_types) {
      collect_pool_id(enum_type.name, offsets);
    }
    for (const EnumVariantType& variant : state_.enum_variant_types) {
      collect_pool_id(variant.name, offsets);
    }
    for (const Immutable& immutable : state_.immutables) {
      if (state_.types[immutable.type].tag == TypeTag::Str) {
        collect_pool_id(immutable.data.str_id_value, offsets);
      }
    }
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    for (const u32 offset : offsets) {
      pool_index_.emplace(offset,
                          add_name(strings_.get(str::StringPoolId{offset})));
    }
    for (const AddrName& name : addr_names_) {
      (void)add_name(name.name);
    }
    for (const std::string_view file : files_.names) {
      (void)add_name(file);
    }
  }

  [[nodiscard]] u64 immutable_value(const Immutable& immutable) const {
    const TypeTag tag = state_.types[immutable.type].tag;
    switch (tag) {
      case TypeTag::I1: return immutable.data.i1_value ? 1 : 0;
      case TypeTag::I8: return static_cast<u8>(immutable.data.i8_value);
      case TypeTag::I16: return static_cast<u16>(immutable.data.i16_value);
      case TypeTag::I32: return static_cast<u32>(immutable.data.i32_value);
      case TypeTag::I64: return static_cast<u64>(immutable.data.i64_value);
      case TypeTag::U8: return immutable.data.u8_value;
      case TypeTag::U16: return immutable.data.u16_value;
      case TypeTag::U32: return immutable.data.u32_value;
      case TypeTag::U64: return immutable.data.u64_value;
      case TypeTag::F32: return std::bit_cast<u32>(immutable.data.f32_value);
      case TypeTag::F64: return std::bit_cast<u64>(immutable.data.f64_value);
      default: return immutable.data.ptr;
    }
  }

  [[nodiscard]] u32 type_payload(TypeIdx type) const {
    const TypeNode& node = state_.types[type];
    switch (node.tag) {
      case TypeTag::Struct: return node.as_struct().idx;
      case TypeTag::Array: return node.as_array().idx;
      case TypeTag::Slice: return node.as_slice().idx;
      case TypeTag::Enum: return node.as_enum().idx;
      case TypeTag::Ref:
      case TypeTag::MutRef:
      case TypeTag::RawPtr:
      case TypeTag::RawMutPtr: return node.as_ref().idx;
      case TypeTag::Tuple: return node.as_tuple().idx;
      case TypeTag::Func: return node.as_func().idx;
      default: return 0;
    }
  }

  template <typename Range>
  static void write_range(ByteWriter& out, const Range& range) {
    out.put_u32(range.head().idx);
    out.put_u32(range.size());
  }

  ByteWriter strings_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(names_.size()));
    for (const std::string& name : names_) {
      out.put_u32(static_cast<u32>(name.size()));
      out.text(name);
    }
    return out;
  }

  ByteWriter types_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.types.size()));
    for (u32 i = 0; i < state_.types.size(); ++i) {
      const TypeIdx type(i);
      out.put_u8(static_cast<u8>(state_.types[type].tag));
      out.zeros(3);
      out.put_u32(type_payload(type));
    }
    return out;
  }

  ByteWriter structs_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.struct_types.size()));
    for (const StructType& strukt : state_.struct_types) {
      out.put_u32(string_index(strukt.name));
      write_range(out, strukt.fields);
      write_range(out, strukt.params);
    }
    return out;
  }

  ByteWriter arrays_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.array_types.size()));
    for (const ArrayType& array : state_.array_types) {
      out.put_u32(array.element.idx);
      out.put_u32(0);
      out.put_u64(array.count);
    }
    return out;
  }

  ByteWriter slices_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.slice_types.size()));
    for (const SliceType& slice : state_.slice_types) {
      out.put_u32(slice.element.idx);
    }
    return out;
  }

  ByteWriter enums_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.enum_types.size()));
    for (const EnumType& enum_type : state_.enum_types) {
      out.put_u32(string_index(enum_type.name));
      write_range(out, enum_type.variants);
      write_range(out, enum_type.params);
    }
    return out;
  }

  ByteWriter enum_variants_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.enum_variant_types.size()));
    for (const EnumVariantType& variant : state_.enum_variant_types) {
      out.put_u32(string_index(variant.name));
      write_range(out, variant.fields);
    }
    return out;
  }

  ByteWriter refs_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.ref_types.size()));
    for (const RefType& ref : state_.ref_types) {
      out.put_u32(ref.pointee.idx);
    }
    return out;
  }

  ByteWriter tuples_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.tuple_types.size()));
    for (const TupleType& tuple : state_.tuple_types) {
      write_range(out, tuple.elements);
    }
    return out;
  }

  ByteWriter funcs_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.func_types.size()));
    for (const FuncType& func : state_.func_types) {
      out.put_u32(func.ret.idx);
      write_range(out, func.params);
    }
    return out;
  }

  ByteWriter functions_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.functions.size()));
    for (const Function& function : state_.functions) {
      const FunctionMeta& meta = function.meta;
      out.put_u32(meta.return_type.idx);
      write_range(out, meta.param_types);
      out.put_u32(string_index(meta.name));
      out.put_u32(string_index(meta.path));
      out.put_u8(static_cast<u8>(meta.kind));
      out.zeros(3);
      write_range(out, meta.generics);
      write_range(out, function.blocks);
    }
    return out;
  }

  ByteWriter blocks_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.blocks.size()));
    for (const Block& block : state_.blocks) {
      write_range(out, block.instrs);
      write_range(out, block.block_params);
    }
    return out;
  }

  ByteWriter block_params_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.block_params.size()));
    for (const BlockParam& param : state_.block_params) {
      out.put_u32(param.type.idx);
      out.put_u32(param.reg.idx);
    }
    return out;
  }

  ByteWriter registers_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.registers.size()));
    for (const Register& reg : state_.registers) {
      out.put_u32(reg.type.idx);
      out.put_u32(reg.def_idx.is_valid() ? reg.def_idx.idx : NONE);
    }
    return out;
  }

  ByteWriter instructions_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.instrs.size()));
    for (const Instruction& instr : state_.instrs) {
      out.put_u8(static_cast<u8>(instr.op));
      const u8 flags = static_cast<u8>(static_cast<u8>(instr.flags.rmw_op)) |
                       static_cast<u8>(instr.flags.some_flag ? 1u : 0u) << 3;
      out.put_u8(flags);
      out.zeros(2);
      out.put_u32(instr.dst.is_valid() ? instr.dst.idx : NONE);
      out.put_u32(instr.measure.is_valid() ? instr.measure.idx : NONE);
      write_range(out, instr.operands);
    }
    return out;
  }

  ByteWriter operands_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.operands.size()));
    for (const Operand& operand : state_.operands) {
      out.put_u8(static_cast<u8>(operand.tag()));
      out.zeros(3);
      out.put_u32(operand_payload(operand));
      out.put_u32(operand.type.idx);
    }
    return out;
  }

  [[nodiscard]] static u32 operand_payload(const Operand& operand) {
    using Payload = Operand::Payload;
    switch (operand.tag()) {
      case Payload::TagOf<RegisterIdx>: return operand.as_register().idx;
      case Payload::TagOf<FunctionIdx>: return operand.as_function().idx;
      case Payload::TagOf<BlockIdx>: return operand.as_block().idx;
      case Payload::TagOf<ImmutableIdx>: return operand.as_immutable().idx;
      case Payload::TagOf<ExternalFunctionIdx>:
        return operand.as_external_function().idx;
      default: return 0;
    }
  }

  ByteWriter immutables_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.immutables.size()));
    for (const Immutable& immutable : state_.immutables) {
      const bool is_str = state_.types[immutable.type].tag == TypeTag::Str;
      out.put_u32(immutable.type.idx);
      out.put_u32(is_str ? string_index(immutable.data.str_id_value) : 0);
      out.put_u64(is_str ? 0 : immutable_value(immutable));
    }
    return out;
  }

  ByteWriter externals_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(state_.external_functions.size()));
    for (const ExternalFunction& external : state_.external_functions) {
      const FunctionMeta& meta = external.meta;
      out.put_u32(meta.return_type.idx);
      write_range(out, meta.param_types);
      out.put_u32(string_index(meta.name));
      out.put_u32(string_index(meta.path));
      out.put_u8(static_cast<u8>(meta.kind));
      out.put_u8(static_cast<u8>(external.calling_conv));
      out.zeros(2);
      write_range(out, meta.generics);
    }
    return out;
  }

  ByteWriter files_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(files_.names.size()));
    for (u32 i = 0; i < files_.names.size(); ++i) {
      out.put_u32(find_name(files_.names[i]));
      out.put_u32(0);
      out.put_u64(files_.hash_of(i));
    }
    return out;
  }

  ByteWriter spans_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(spans_.size()));
    for (const diag::Span& span : spans_) {
      out.put_u32(span.file);
      out.put_u32(span.offset);
      out.put_u32(span.length);
    }
    return out;
  }

  ByteWriter addr_names_section() const {
    ByteWriter out;
    out.put_u32(static_cast<u32>(addr_names_.size()));
    for (const AddrName& name : addr_names_) {
      out.put_u32(name.reg.idx);
      out.put_u32(find_name(name.name));
      const u8 flags = static_cast<u8>(name.is_param ? 1u : 0u) |
                       static_cast<u8>(name.is_capture ? 2u : 0u);
      out.put_u8(flags);
      out.zeros(3);
    }
    return out;
  }
};

}  // namespace

std::vector<u8> serialize(const WriteInput& input) {
  DCHECK(input.storage != nullptr);
  DCHECK(input.strings != nullptr);
  Serializer serializer(input);
  return serializer.run();
}

}  // namespace ir
