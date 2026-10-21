// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/deserialize.h"

#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/hash.h"
#include "debug/check.h"
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
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/register.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/summary.h"
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "ir/verifier.h"

namespace ir {

using binary::Section;

LoadedIr::LoadedIr(VerifiedStorage storage) : storage(std::move(storage)) {}

namespace {

constexpr u32 NONE = U32_MAX;
// Kinds 1..Summary must fit this; a larger kind is unknown and skipped.
constexpr usize MAX_SECTIONS = 32;

// A bounds-checked little-endian reader over the file bytes.
class Cursor {
 public:
  explicit Cursor(std::span<const u8> bytes, usize at = 0)
      : bytes_(bytes), at_(at) {}

  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] usize at() const { return at_; }

  bool seek(usize at) {
    if (at > bytes_.size()) {
      ok_ = false;
      return false;
    }
    at_ = at;
    return true;
  }

  bool read_u8(u8& out) {
    if (!need(1)) {
      return false;
    }
    out = bytes_[at_++];
    return true;
  }

  bool read_u16(u16& out) {
    if (!need(2)) {
      return false;
    }
    out = static_cast<u16>(bytes_[at_]) | static_cast<u16>(bytes_[at_ + 1])
                                              << 8;
    at_ += 2;
    return true;
  }

  bool read_u32(u32& out) {
    if (!need(4)) {
      return false;
    }
    out = 0;
    for (u32 i = 0; i < 4; ++i) {
      out |= static_cast<u32>(bytes_[at_ + i]) << (8 * i);
    }
    at_ += 4;
    return true;
  }

  bool read_u64(u64& out) {
    if (!need(8)) {
      return false;
    }
    out = 0;
    for (u32 i = 0; i < 8; ++i) {
      out |= static_cast<u64>(bytes_[at_ + i]) << (8 * i);
    }
    at_ += 8;
    return true;
  }

  bool read_digest(base::Digest128& out) {
    if (!need(16)) {
      return false;
    }
    out.low = 0;
    out.high = 0;
    for (u32 i = 0; i < 8; ++i) {
      out.low |= static_cast<u64>(bytes_[at_ + i]) << (8 * i);
      out.high |= static_cast<u64>(bytes_[at_ + 8 + i]) << (8 * i);
    }
    at_ += 16;
    return true;
  }

  bool bytes(usize size, std::span<const u8>& out) {
    if (!need(size)) {
      return false;
    }
    out = bytes_.subspan(at_, size);
    at_ += size;
    return true;
  }

 private:
  bool need(usize count) {
    if (!ok_ || count > bytes_.size() - at_) {
      ok_ = false;
      return false;
    }
    return true;
  }

  std::span<const u8> bytes_;
  usize at_ = 0;
  bool ok_ = true;
};

// What one read produces, before the storage is verified and the result
// is assembled.
struct Parsed {
  StorageState state;
  std::unique_ptr<ir::SymbolTable> strings =
      std::make_unique<ir::SymbolTable>();
  std::vector<str::StringPoolId> string_ids;
  std::vector<std::string> file_names;
  std::vector<base::Digest128> file_hashes;
  std::vector<LoadedIr::AddrNameEntry> addr_names;
  std::vector<SummaryEntry> summary;
  std::vector<SummaryField> summary_fields;
  std::vector<diag::Span> spans;
  usize prelude_functions = 0;
  PointerWidth width = PointerWidth::W64;
  std::string compiler_version;
  base::Digest128 identity;
};

using ParsedResult = base::Result<void, IrLoadError>;

[[nodiscard]] u32 record_count(usize payload_size, usize record_size) {
  if (record_size == 0 || payload_size < 4) {
    return NONE;
  }
  const usize rest = payload_size - 4;
  if (rest % record_size != 0) {
    return NONE;
  }
  const usize count = rest / record_size;
  if (count > NONE) {
    return NONE;
  }
  return static_cast<u32>(count);
}

[[nodiscard]] str::StringPoolId string_id(const Parsed& parsed, u32 index) {
  if (index == NONE) {
    return str::INVALID_STRING_POOL_ID;
  }
  if (index >= parsed.string_ids.size()) {
    return str::INVALID_STRING_POOL_ID;
  }
  return parsed.string_ids[index];
}

bool read_strings(Parsed& parsed, std::span<const u8> payload) {
  Cursor cursor(payload);
  u32 count = 0;
  if (!cursor.read_u32(count)) {
    return false;
  }
  // Every entry needs at least its four-byte length, so a count the
  // payload cannot hold is a lie; reserving on it would let a small
  // file ask for a huge allocation.
  if (count > (payload.size() - 4) / 4) {
    return false;
  }
  parsed.string_ids.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    u32 length = 0;
    std::span<const u8> bytes;
    if (!cursor.read_u32(length) || !cursor.bytes(length, bytes)) {
      return false;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()),
                                bytes.size());
    // The bytes belong to the buffer this reader was handed, so the table
    // keeps a copy of each name rather than a view of it.
    const std::optional<str::StringPoolId> id =
        parsed.strings->intern_copied(text);
    CHECK_MSG(id.has_value(),
              "the name table is full; a read interns the names a package has");
    parsed.string_ids.push_back(id.value_or(str::INVALID_STRING_POOL_ID));
  }
  return cursor.at() == payload.size();
}

bool read_types(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 8);
  if (count == NONE) {
    return false;
  }
  // The pre-interned tags are positions, not a table: the emitters
  // resolve them by index, so a file that shuffles them is refused.
  if (count < PRIMITIVE_TYPE_COUNT + 1) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u8 tag = 0;
    u8 pad = 0;
    u32 value = 0;
    if (!cursor.read_u8(tag) || !cursor.read_u8(pad) || !cursor.read_u8(pad) ||
        !cursor.read_u8(pad) || !cursor.read_u32(value)) {
      return false;
    }
    // The primitive tags are positions, and Function is pre-interned
    // right after them; the emitters resolve those indices directly, so
    // a file that shuffles them is refused.
    if (i < PRIMITIVE_TYPE_COUNT) {
      if (tag != static_cast<u8>(i)) {
        return false;
      }
    } else if (i == PRIMITIVE_TYPE_COUNT) {
      if (tag != static_cast<u8>(TypeTag::Function)) {
        return false;
      }
    }
    TypeNode node{};
    node.tag = static_cast<TypeTag>(tag);
    switch (node.tag) {
      case TypeTag::Struct: node.data.set(StructTypeIdx(value)); break;
      case TypeTag::Array: node.data.set(ArrayTypeIdx(value)); break;
      case TypeTag::Slice: node.data.set(SliceTypeIdx(value)); break;
      case TypeTag::Enum: node.data.set(EnumTypeIdx(value)); break;
      case TypeTag::Ref:
      case TypeTag::MutRef:
      case TypeTag::RawPtr:
      case TypeTag::RawMutPtr: node.data.set(RefTypeIdx(value)); break;
      case TypeTag::Tuple: node.data.set(TupleTypeIdx(value)); break;
      case TypeTag::Func: node.data.set(FuncTypeIdx(value)); break;
      case TypeTag::Const: node.data.set(ConstTypeIdx(value)); break;
      // A type parameter's payload is its index, not a table row.
      case TypeTag::Param: node.data.set(ParamIndex{value}); break;
      default: break;
    }
    parsed.state.types.emplace_back(node);
  }
  return true;
}

// Reads a range field pair, leaving the values as stored: whether they
// address their table is the verifier's to decide.
bool read_range(Cursor& cursor, u32& head, u32& size) {
  return cursor.read_u32(head) && cursor.read_u32(size);
}

bool read_structs(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 20);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 name = 0;
    u32 fields_head = 0;
    u32 fields_size = 0;
    u32 params_head = 0;
    u32 params_size = 0;
    if (!cursor.read_u32(name) ||
        !read_range(cursor, fields_head, fields_size) ||
        !read_range(cursor, params_head, params_size)) {
      return false;
    }
    parsed.state.struct_types.emplace_back(StructType{
        .name = string_id(parsed, name),
        .fields = TypeIdxRange{TypeIdx(fields_head), fields_size},
        .params = TypeIdxRange{TypeIdx(params_head), params_size},
    });
  }
  return true;
}

bool read_arrays(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 16);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 element = 0;
    u32 pad = 0;
    u64 length = 0;
    if (!cursor.read_u32(element) || !cursor.read_u32(pad) ||
        !cursor.read_u64(length)) {
      return false;
    }
    parsed.state.array_types.emplace_back(
        ArrayType{.element = TypeIdx(element), .count = length});
  }
  return true;
}

bool read_slices(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 4);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 element = 0;
    if (!cursor.read_u32(element)) {
      return false;
    }
    parsed.state.slice_types.emplace_back(
        SliceType{.element = TypeIdx(element)});
  }
  return true;
}

bool read_enums(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 20);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 name = 0;
    u32 variants_head = 0;
    u32 variants_size = 0;
    u32 params_head = 0;
    u32 params_size = 0;
    if (!cursor.read_u32(name) ||
        !read_range(cursor, variants_head, variants_size) ||
        !read_range(cursor, params_head, params_size)) {
      return false;
    }
    parsed.state.enum_types.emplace_back(EnumType{
        .name = string_id(parsed, name),
        .variants = EnumVariantTypeIdxRange{EnumVariantTypeIdx(variants_head),
                                            variants_size},
        .params = TypeIdxRange{TypeIdx(params_head), params_size},
    });
  }
  return true;
}

bool read_enum_variants(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 12);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 name = 0;
    u32 fields_head = 0;
    u32 fields_size = 0;
    if (!cursor.read_u32(name) ||
        !read_range(cursor, fields_head, fields_size)) {
      return false;
    }
    parsed.state.enum_variant_types.emplace_back(EnumVariantType{
        .name = string_id(parsed, name),
        .fields = TypeIdxRange{TypeIdx(fields_head), fields_size},
    });
  }
  return true;
}

bool read_refs(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 4);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 pointee = 0;
    if (!cursor.read_u32(pointee)) {
      return false;
    }
    parsed.state.ref_types.emplace_back(RefType{.pointee = TypeIdx(pointee)});
  }
  return true;
}

bool read_tuples(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 8);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 head = 0;
    u32 size = 0;
    if (!read_range(cursor, head, size)) {
      return false;
    }
    parsed.state.tuple_types.emplace_back(
        TupleType{.elements = TypeIdxRange{TypeIdx(head), size}});
  }
  return true;
}

bool read_funcs(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 12);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 ret = 0;
    u32 head = 0;
    u32 size = 0;
    if (!cursor.read_u32(ret) || !read_range(cursor, head, size)) {
      return false;
    }
    parsed.state.func_types.emplace_back(FuncType{
        .params = TypeIdxRange{TypeIdx(head), size}, .ret = TypeIdx(ret)});
  }
  return true;
}

bool read_functions(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 44);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 return_type = 0;
    u32 param_head = 0;
    u32 param_size = 0;
    u32 name = 0;
    u32 path = 0;
    u32 owner = 0;
    u8 kind = 0;
    u8 pad = 0;
    u32 generics_head = 0;
    u32 generics_size = 0;
    u32 blocks_head = 0;
    u32 blocks_size = 0;
    if (!cursor.read_u32(return_type) ||
        !read_range(cursor, param_head, param_size) || !cursor.read_u32(name) ||
        !cursor.read_u32(path) || !cursor.read_u32(owner) ||
        !cursor.read_u8(kind) || !cursor.read_u8(pad) || !cursor.read_u8(pad) ||
        !cursor.read_u8(pad) ||
        !read_range(cursor, generics_head, generics_size) ||
        !read_range(cursor, blocks_head, blocks_size)) {
      return false;
    }
    parsed.state.functions.emplace_back(Function{
        .meta = {.return_type = TypeIdx(return_type),
                 .param_types = TypeIdxRange{TypeIdx(param_head), param_size},
                 .name = string_id(parsed, name),
                 .path = string_id(parsed, path),
                 .kind = static_cast<SymbolKind>(kind),
                 .generics =
                     TypeIdxRange{TypeIdx(generics_head), generics_size},
                 .owner = TypeIdx(owner)},
        .blocks = BlockIdxRange{BlockIdx(blocks_head), blocks_size},
    });
  }
  return true;
}

bool read_blocks(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 16);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 instrs_head = 0;
    u32 instrs_size = 0;
    u32 params_head = 0;
    u32 params_size = 0;
    if (!read_range(cursor, instrs_head, instrs_size) ||
        !read_range(cursor, params_head, params_size)) {
      return false;
    }
    parsed.state.blocks.emplace_back(Block{
        .instrs = InstructionIdxRange{InstructionIdx(instrs_head), instrs_size},
        .block_params =
            BlockParamIdxRange{BlockParamIdx(params_head), params_size},
    });
  }
  return true;
}

bool read_block_params(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 8);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 type = 0;
    u32 reg = 0;
    if (!cursor.read_u32(type) || !cursor.read_u32(reg)) {
      return false;
    }
    parsed.state.block_params.emplace_back(
        BlockParam{.type = TypeIdx(type), .reg = RegisterIdx(reg)});
  }
  return true;
}

bool read_registers(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 8);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 type = 0;
    u32 def = 0;
    if (!cursor.read_u32(type) || !cursor.read_u32(def)) {
      return false;
    }
    parsed.state.registers.emplace_back(Register{
        .type = TypeIdx(type),
        .def_idx =
            def == NONE ? InstructionIdx::invalid() : InstructionIdx(def),
    });
  }
  return true;
}

bool read_instructions(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 20);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u8 op = 0;
    u8 flags = 0;
    u8 pad = 0;
    u32 dst = 0;
    u32 measure = 0;
    u32 operands_head = 0;
    u32 operands_size = 0;
    if (!cursor.read_u8(op) || !cursor.read_u8(flags) || !cursor.read_u8(pad) ||
        !cursor.read_u8(pad) || !cursor.read_u32(dst) ||
        !cursor.read_u32(measure) ||
        !read_range(cursor, operands_head, operands_size)) {
      return false;
    }
    InstructionFlags instruction_flags{};
    instruction_flags.rmw_op = static_cast<AtomicRmwOp>(flags & 0x07u);
    instruction_flags.inbounds = (flags & 0x08u) != 0;
    instruction_flags.mem_align_log2 = static_cast<u8>(flags >> 4) & 0x0fu;
    parsed.state.instrs.emplace_back(Instruction{
        .op = static_cast<Opcode>(op),
        .flags = instruction_flags,
        .dst = dst == NONE ? RegisterIdx::invalid() : RegisterIdx(dst),
        .measure = measure == NONE ? TypeIdx::invalid() : TypeIdx(measure),
        .operands = OperandIdxRange{OperandIdx(operands_head), operands_size},
    });
  }
  return true;
}

bool read_operands(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 12);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u8 tag = 0;
    u8 pad = 0;
    u32 value = 0;
    u32 type = 0;
    if (!cursor.read_u8(tag) || !cursor.read_u8(pad) || !cursor.read_u8(pad) ||
        !cursor.read_u8(pad) || !cursor.read_u32(value) ||
        !cursor.read_u32(type)) {
      return false;
    }
    Operand operand = Operand::invalid();
    if (tag == static_cast<u8>(Operand::TAG_OF<RegisterIdx>)) {
      operand = Operand::from_register(RegisterIdx(value), TypeIdx(type));
    } else if (tag == static_cast<u8>(Operand::TAG_OF<FunctionIdx>)) {
      operand = Operand::from_function(FunctionIdx(value), TypeIdx(type));
    } else if (tag == static_cast<u8>(Operand::TAG_OF<BlockIdx>)) {
      operand = Operand::from_block(BlockIdx(value), TypeIdx(type));
    } else if (tag == static_cast<u8>(Operand::TAG_OF<ImmutableIdx>)) {
      operand = Operand::from_immutable(ImmutableIdx(value), TypeIdx(type));
    } else if (tag == static_cast<u8>(Operand::TAG_OF<ExternalFunctionIdx>)) {
      operand = Operand::from_external_function(ExternalFunctionIdx(value),
                                                TypeIdx(type));
    } else if (tag != static_cast<u8>(Operand::TAG_OF<void>)) {
      return false;
    } else {
      operand.type = TypeIdx(type);
    }
    parsed.state.operands.emplace_back(operand);
  }
  return true;
}

bool read_immutables(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 16);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  // The type table is read before this section, so the tag is known.
  for (u32 i = 0; i < count; ++i) {
    u32 type = 0;
    u32 aux = 0;
    u64 value = 0;
    if (!cursor.read_u32(type) || !cursor.read_u32(aux) ||
        !cursor.read_u64(value)) {
      return false;
    }
    if (type >= parsed.state.types.size()) {
      return false;
    }
    Immutable immutable{.type = TypeIdx(type), .data = {}};
    switch (parsed.state.types[immutable.type].tag) {
      case TypeTag::Str:
        immutable.data.str_id_value = string_id(parsed, aux);
        break;
      case TypeTag::I1: immutable.data.i1_value = value != 0; break;
      case TypeTag::I8: immutable.data.i8_value = static_cast<i8>(value); break;
      case TypeTag::I16:
        immutable.data.i16_value = static_cast<i16>(value);
        break;
      case TypeTag::I32:
        immutable.data.i32_value = static_cast<i32>(value);
        break;
      case TypeTag::I64:
        immutable.data.i64_value = static_cast<i64>(value);
        break;
      case TypeTag::U8: immutable.data.u8_value = static_cast<u8>(value); break;
      case TypeTag::U16:
        immutable.data.u16_value = static_cast<u16>(value);
        break;
      case TypeTag::U32:
        immutable.data.u32_value = static_cast<u32>(value);
        break;
      case TypeTag::U64: immutable.data.u64_value = value; break;
      case TypeTag::F32:
        immutable.data.f32_value = std::bit_cast<f32>(static_cast<u32>(value));
        break;
      case TypeTag::F64:
        immutable.data.f64_value = std::bit_cast<f64>(value);
        break;
      default: immutable.data.ptr = value; break;
    }
    parsed.state.immutables.emplace_back(immutable);
  }
  return true;
}

bool read_externals(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 36);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 return_type = 0;
    u32 param_head = 0;
    u32 param_size = 0;
    u32 name = 0;
    u32 path = 0;
    u32 owner = 0;
    u8 kind = 0;
    u8 cc = 0;
    u8 pad = 0;
    u32 generics_head = 0;
    u32 generics_size = 0;
    if (!cursor.read_u32(return_type) ||
        !read_range(cursor, param_head, param_size) || !cursor.read_u32(name) ||
        !cursor.read_u32(path) || !cursor.read_u32(owner) ||
        !cursor.read_u8(kind) || !cursor.read_u8(cc) || !cursor.read_u8(pad) ||
        !cursor.read_u8(pad) ||
        !read_range(cursor, generics_head, generics_size)) {
      return false;
    }
    parsed.state.external_functions.emplace_back(ExternalFunction{
        .meta = {.return_type = TypeIdx(return_type),
                 .param_types = TypeIdxRange{TypeIdx(param_head), param_size},
                 .name = string_id(parsed, name),
                 .path = string_id(parsed, path),
                 .kind = static_cast<SymbolKind>(kind),
                 .generics =
                     TypeIdxRange{TypeIdx(generics_head), generics_size},
                 .owner = TypeIdx(owner)},
        .calling_conv = static_cast<CallingConvention>(cc),
    });
  }
  return true;
}

bool read_files(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 24);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 name = 0;
    u32 pad = 0;
    base::Digest128 hash;
    if (!cursor.read_u32(name) || !cursor.read_u32(pad) ||
        !cursor.read_digest(hash)) {
      return false;
    }
    if (name != NONE && name >= parsed.string_ids.size()) {
      return false;
    }
    parsed.file_names.emplace_back(
        name == NONE
            ? std::string{}
            : std::string(parsed.strings->get(parsed.string_ids[name])));
    parsed.file_hashes.push_back(hash);
  }
  return true;
}

bool read_consts(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 8);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u64 value = 0;
    if (!cursor.read_u64(value)) {
      return false;
    }
    parsed.state.const_types.emplace_back(ConstType{.value = value});
  }
  return true;
}

bool read_spans(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 12);
  if (count == NONE) {
    return false;
  }
  if (count != 0 && count != parsed.state.instrs.size()) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 file = 0;
    u32 offset = 0;
    u32 length = 0;
    if (!cursor.read_u32(file) || !cursor.read_u32(offset) ||
        !cursor.read_u32(length)) {
      return false;
    }
    if (file != NONE && file >= parsed.file_names.size()) {
      return false;
    }
    parsed.spans.push_back(
        diag::Span{.file = file, .offset = offset, .length = length});
  }
  return true;
}

bool read_addr_names(Parsed& parsed, std::span<const u8> payload) {
  const u32 count = record_count(payload.size(), 12);
  if (count == NONE) {
    return false;
  }
  Cursor cursor(payload);
  u32 read = 0;
  if (!cursor.read_u32(read) || read != count) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 reg = 0;
    u32 name = 0;
    u8 flags = 0;
    u8 pad = 0;
    if (!cursor.read_u32(reg) || !cursor.read_u32(name) ||
        !cursor.read_u8(flags) || !cursor.read_u8(pad) ||
        !cursor.read_u8(pad) || !cursor.read_u8(pad)) {
      return false;
    }
    if (reg >= parsed.state.registers.size()) {
      return false;
    }
    if (name != NONE && name >= parsed.string_ids.size()) {
      return false;
    }
    parsed.addr_names.push_back(LoadedIr::AddrNameEntry{
        .reg = RegisterIdx(reg),
        .name = name == NONE
                    ? std::string{}
                    : std::string(parsed.strings->get(parsed.string_ids[name])),
        .is_param = (flags & 0x01u) != 0,
        .is_capture = (flags & 0x02u) != 0,
    });
  }
  return true;
}

bool read_summary(Parsed& parsed, std::span<const u8> payload) {
  if (payload.size() < 4) {
    return false;
  }
  Cursor cursor(payload);
  u32 count = 0;
  if (!cursor.read_u32(count)) {
    return false;
  }
  // The rows are twenty-eight bytes each, followed by the field runs.
  const usize rows_end = 4 + static_cast<usize>(count) * 28;
  if (payload.size() < rows_end) {
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    u32 function = 0;
    u32 type = 0;
    u32 name = 0;
    u32 path = 0;
    u32 symbol = 0;
    u32 owner = 0;
    u8 kind = 0;
    u8 flags = 0;
    u8 reserved = 0;
    if (!cursor.read_u32(function) || !cursor.read_u32(type) ||
        !cursor.read_u32(name) || !cursor.read_u32(path) ||
        !cursor.read_u32(symbol) || !cursor.read_u32(owner) ||
        !cursor.read_u8(kind) || !cursor.read_u8(flags) ||
        !cursor.read_u8(reserved) || !cursor.read_u8(reserved)) {
      return false;
    }
    if (name != NONE && name >= parsed.string_ids.size()) {
      return false;
    }
    if (path != NONE && path >= parsed.string_ids.size()) {
      return false;
    }
    if (symbol != NONE && symbol >= parsed.string_ids.size()) {
      return false;
    }
    if (owner != NONE && owner >= parsed.state.types.size()) {
      return false;
    }
    const SummaryKind summary_kind = static_cast<SummaryKind>(kind);
    if (kind > static_cast<u8>(SummaryKind::Enum)) {
      return false;
    }
    if (function != NONE && function >= parsed.state.functions.size()) {
      return false;
    }
    if (type != NONE && type >= parsed.state.types.size()) {
      return false;
    }
    // A function row names lowered code; a nominal row names a type, or
    // nothing when the declaration is generic. The other field is never
    // set, so a row cannot name both.
    const bool is_nominal = summary_kind == SummaryKind::Struct ||
                            summary_kind == SummaryKind::Enum;
    if (is_nominal ? function != NONE : type != NONE) {
      return false;
    }
    parsed.summary.push_back(SummaryEntry{
        .function = function == NONE ? FunctionIdx(base::INVALID_IDX)
                                     : FunctionIdx(function),
        .type = type == NONE ? TypeIdx(base::INVALID_IDX) : TypeIdx(type),
        .name = string_id(parsed, name),
        .path = string_id(parsed, path),
        .symbol = string_id(parsed, symbol),
        .owner = owner == NONE ? TypeIdx(base::INVALID_IDX) : TypeIdx(owner),
        .kind = summary_kind,
        .needs_body = (flags & 0x01u) != 0,
    });
  }
  u32 groups = 0;
  if (!cursor.read_u32(groups)) {
    return false;
  }
  struct Run {
    u32 row;
    u32 first;
    u32 count;
  };
  std::vector<Run> runs;
  runs.reserve(groups);
  for (u32 i = 0; i < groups; ++i) {
    u32 row = 0;
    u32 first = 0;
    u32 length = 0;
    if (!cursor.read_u32(row) || !cursor.read_u32(first) ||
        !cursor.read_u32(length)) {
      return false;
    }
    if (row >= parsed.summary.size()) {
      return false;
    }
    runs.push_back(Run{row, first, length});
  }
  u32 name_count = 0;
  if (!cursor.read_u32(name_count)) {
    return false;
  }
  parsed.summary_fields.reserve(name_count);
  for (u32 i = 0; i < name_count; ++i) {
    u32 index = 0;
    u8 flags = 0;
    u8 reserved = 0;
    if (!cursor.read_u32(index) || !cursor.read_u8(flags) ||
        !cursor.read_u8(reserved) || !cursor.read_u8(reserved) ||
        !cursor.read_u8(reserved)) {
      return false;
    }
    if (index != NONE && index >= parsed.string_ids.size()) {
      return false;
    }
    parsed.summary_fields.push_back(SummaryField{
        .name = string_id(parsed, index), .is_pub = (flags & 0x01u) != 0});
  }
  if (cursor.at() != payload.size()) {
    return false;
  }
  for (const Run& run : runs) {
    if (static_cast<usize>(run.first) + run.count >
        parsed.summary_fields.size()) {
      return false;
    }
    parsed.summary[run.row].fields = std::span<const SummaryField>(
        parsed.summary_fields.data() + run.first, run.count);
  }
  return true;
}

bool read_prelude(Parsed& parsed, std::span<const u8> payload) {
  if (payload.size() != 4) {
    return false;
  }
  Cursor cursor(payload);
  u32 count = 0;
  if (!cursor.read_u32(count)) {
    return false;
  }
  parsed.prelude_functions = count;
  return true;
}

bool read_identity(Parsed& parsed, std::span<const u8> payload) {
  if (payload.size() != 16) {
    return false;
  }
  Cursor cursor(payload);
  base::Digest128 digest;
  if (!cursor.read_digest(digest)) {
    return false;
  }
  parsed.identity = digest;
  return true;
}

// One dispatched reader per known kind, in the order the format writes
// them.
bool read_section(Parsed& parsed, Section kind, std::span<const u8> payload) {
  switch (kind) {
    case Section::Strings: return read_strings(parsed, payload);
    case Section::Types: return read_types(parsed, payload);
    case Section::Structs: return read_structs(parsed, payload);
    case Section::Arrays: return read_arrays(parsed, payload);
    case Section::Slices: return read_slices(parsed, payload);
    case Section::Enums: return read_enums(parsed, payload);
    case Section::EnumVariants: return read_enum_variants(parsed, payload);
    case Section::Refs: return read_refs(parsed, payload);
    case Section::Tuples: return read_tuples(parsed, payload);
    case Section::Funcs: return read_funcs(parsed, payload);
    case Section::Functions: return read_functions(parsed, payload);
    case Section::Blocks: return read_blocks(parsed, payload);
    case Section::BlockParams: return read_block_params(parsed, payload);
    case Section::Registers: return read_registers(parsed, payload);
    case Section::Instructions: return read_instructions(parsed, payload);
    case Section::Operands: return read_operands(parsed, payload);
    case Section::Immutables: return read_immutables(parsed, payload);
    case Section::Externals: return read_externals(parsed, payload);
    case Section::Files: return read_files(parsed, payload);
    case Section::Spans: return read_spans(parsed, payload);
    case Section::AddrNames: return read_addr_names(parsed, payload);
    case Section::Prelude: return read_prelude(parsed, payload);
    case Section::Consts: return read_consts(parsed, payload);
    case Section::Identity: return read_identity(parsed, payload);
    case Section::Summary: return read_summary(parsed, payload);
  }
  return true;
}

bool known_kind(u32 kind) {
  return kind >= static_cast<u32>(Section::Strings) &&
         kind <= static_cast<u32>(Section::Summary);
}

base::Result<LoadedIr, IrLoadError> load(std::span<const u8> bytes) {
  if (bytes.size() < 28) {
    return base::make_err(IrLoadError::Truncated);
  }
  Cursor cursor(bytes);
  std::span<const u8> magic;
  if (!cursor.bytes(4, magic)) {
    return base::make_err(IrLoadError::Truncated);
  }
  if (magic[0] != binary::MAGIC[0] || magic[1] != binary::MAGIC[1] ||
      magic[2] != binary::MAGIC[2] || magic[3] != binary::MAGIC[3]) {
    return base::make_err(IrLoadError::BadMagic);
  }
  // The trailing footer hashes every byte before it, so a package whose
  // bytes changed under a cache or in transit is refused before any of
  // it is read (compiler/docs/ir-format.md).
  Cursor footer(bytes, bytes.size() - 16);
  base::Digest128 expected_hash;
  if (!footer.read_digest(expected_hash) ||
      base::hash128(bytes.first(bytes.size() - 16)) != expected_hash) {
    return base::make_err(IrLoadError::BadChecksum);
  }
  u16 major = 0;
  u16 minor = 0;
  u32 flags = 0;
  u32 width = 0;
  u32 version_len = 0;
  if (!cursor.read_u16(major) || !cursor.read_u16(minor) ||
      !cursor.read_u32(flags) || !cursor.read_u32(width) ||
      !cursor.read_u32(version_len)) {
    return base::make_err(IrLoadError::Truncated);
  }
  if (major != binary::MAJOR) {
    return base::make_err(IrLoadError::UnsupportedVersion);
  }
  (void)minor;
  if ((flags & binary::FLAG_LITTLE_ENDIAN) == 0) {
    return base::make_err(IrLoadError::BadFlags);
  }
  if (width != 32 && width != 64) {
    return base::make_err(IrLoadError::BadShape);
  }
  Parsed parsed;
  parsed.width = width == 64 ? PointerWidth::W64 : PointerWidth::W32;
  std::span<const u8> version;
  if (!cursor.bytes(version_len, version)) {
    return base::make_err(IrLoadError::Truncated);
  }
  parsed.compiler_version.assign(reinterpret_cast<const char*>(version.data()),
                                 version.size());
  u64 section_count = 0;
  if (!cursor.read_u64(section_count)) {
    return base::make_err(IrLoadError::Truncated);
  }
  if (section_count > MAX_SECTIONS) {
    return base::make_err(IrLoadError::BadShape);
  }

  struct Range {
    usize offset = 0;
    usize size = 0;
    bool present = false;
  };
  std::array<Range, MAX_SECTIONS> sections{};
  const usize body_start = cursor.at();
  for (u64 i = 0; i < section_count; ++i) {
    u32 kind = 0;
    u32 entry_flags = 0;
    u64 offset = 0;
    u64 size = 0;
    if (!cursor.read_u32(kind) || !cursor.read_u32(entry_flags) ||
        !cursor.read_u64(offset) || !cursor.read_u64(size)) {
      return base::make_err(IrLoadError::Truncated);
    }
    (void)entry_flags;
    if (!known_kind(kind)) {
      // A newer minor version may add a section this reader skips.
      continue;
    }
    Range& range = sections[kind];
    if (range.present) {
      return base::make_err(IrLoadError::BadSection);
    }
    if (offset % binary::SECTION_ALIGN != 0 || offset < body_start ||
        offset > bytes.size() || size > bytes.size() ||
        offset + size > bytes.size() - 8) {
      return base::make_err(IrLoadError::BadSection);
    }
    range = Range{static_cast<usize>(offset), static_cast<usize>(size), true};
  }
  // No two sections may overlap.
  for (usize a = 0; a < MAX_SECTIONS; ++a) {
    for (usize b = a + 1; b < MAX_SECTIONS; ++b) {
      if (!sections[a].present || !sections[b].present) {
        continue;
      }
      const usize a_end = sections[a].offset + sections[a].size;
      const usize b_end = sections[b].offset + sections[b].size;
      if (sections[a].offset < b_end && sections[b].offset < a_end) {
        return base::make_err(IrLoadError::BadSection);
      }
    }
  }
  // The core sections are always written; a file without them is not a
  // package this reader knows.
  for (u32 kind = static_cast<u32>(Section::Strings);
       kind <= static_cast<u32>(Section::Files); ++kind) {
    if (!sections[kind].present) {
      return base::make_err(IrLoadError::BadSection);
    }
  }

  for (u32 kind = static_cast<u32>(Section::Strings);
       kind <= static_cast<u32>(Section::Summary); ++kind) {
    if (!sections[kind].present) {
      continue;
    }
    const Range& range = sections[kind];
    if (!read_section(parsed, static_cast<Section>(kind),
                      bytes.subspan(range.offset, range.size))) {
      return base::make_err(IrLoadError::BadShape);
    }
  }

  // The verifier is the index checker: every range and composite
  // payload was bounds-checked by the time this returns, so a malicious
  // file cannot make anything below read out of bounds.
  base::Result<VerifiedStorage, VerificationError> verified =
      StorageBuilder(std::move(parsed.state)).build();
  if (verified.is_err()) {
    return base::make_err(IrLoadError::Verification);
  }

  // The result is assembled around the storage last, because the
  // storage has no default constructor.
  LoadedIr loaded(std::move(verified).unwrap());
  loaded.strings = std::move(parsed.strings);
  loaded.instr_spans = std::move(parsed.spans);
  loaded.file_names = std::move(parsed.file_names);
  loaded.file_hashes = std::move(parsed.file_hashes);
  loaded.addr_names = std::move(parsed.addr_names);
  loaded.prelude_functions = parsed.prelude_functions;
  loaded.identity = parsed.identity;
  loaded.summary = std::move(parsed.summary);
  loaded.summary_fields = std::move(parsed.summary_fields);
  loaded.width = parsed.width;
  loaded.compiler_version = std::move(parsed.compiler_version);
  return base::make_ok(std::move(loaded));
}

}  // namespace

base::Result<LoadedIr, IrLoadError> deserialize(std::span<const u8> bytes) {
  return load(bytes);
}

}  // namespace ir
