// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/verifier.h"

#include <array>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "ir/block.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/parametric.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/type_util.h"

namespace ir {

namespace {

// Checks that [head, head + size) lies within a storage of vec_size.
template <typename Idx>
constexpr bool range_in_bounds(const Idx head,
                               const u32 size,
                               const usize vec_size) {
  return static_cast<u64>(head.idx) + size <= vec_size;
}

VerificationResult err(const VerificationErrorKind kind, const u32 index) {
  return base::make_err(VerificationError{.kind = kind, .index = index});
}

// The operand count a well-formed instruction of an opcode takes. `max`
// is ANY_OPERANDS where the shape depends on a target block's parameters
// or on the callee's signature; those shapes have their own checks.
// `known` is false for a value no enumerator defines, which only a
// reader can produce: it casts a byte out of a file.
struct OperandArity {
  u32 min = 0;
  u32 max = 0;
  bool known = false;
};

constexpr u32 ANY_OPERANDS = U32_MAX;

constexpr OperandArity operand_arity(const Opcode op) {
  using O = Opcode;
  switch (op) {
    case O::Noop:
    case O::TypeSizeOf:
    case O::TypeAlignOf:
    case O::Unreachable:
    case O::Fence: return {0, 0, true};
    case O::Alloca:
    case O::Load:
    case O::EnumTagLoad:
    case O::Not:
    case O::BitReverse:
    case O::FNeg:
    case O::TypeCast:
    case O::AtomicLoad:
    case O::Move:
    case O::Drop:
    case O::Borrow:
    case O::ParametricDrop: return {1, 1, true};
    case O::Store:
    case O::EnumTagSet:
    case O::ElemOffset:
    case O::IntAdd:
    case O::IntSub:
    case O::IntMul:
    case O::IntAddWrap:
    case O::IntSubWrap:
    case O::IntMulWrap:
    case O::IntDiv:
    case O::UintDiv:
    case O::IntRem:
    case O::UintRem:
    case O::FAdd:
    case O::FSub:
    case O::FMul:
    case O::FDiv:
    case O::And:
    case O::Or:
    case O::Xor:
    case O::ShiftLeft:
    case O::ArithmeticShiftRight:
    case O::LogicalShiftRight:
    case O::Eq:
    case O::Ne:
    case O::Le:
    case O::Lt:
    case O::Ge:
    case O::Gt:
    case O::AtomicStore:
    case O::AtomicRmw: return {2, 2, true};
    case O::Memcopy:
    case O::Memset:
    case O::Select:
    case O::CondBr:
    case O::AtomicCompareExchange: return {3, 3, true};
    case O::Ret: return {0, 1, true};
    case O::GetElementPtr:
    case O::ExtractValue:
    case O::Switch: return {2, ANY_OPERANDS, true};
    case O::EnumFieldAddr: return {3, 3, true};
    case O::InsertValue: return {3, ANY_OPERANDS, true};
    case O::Br:
    case O::Call: return {1, ANY_OPERANDS, true};
    case O::CompValue: return {1, 1, true};
    case O::CompStrByte: return {2, 2, true};
    case O::CompStrSlice: return {3, 3, true};
    case O::CompVariantTag: return {1, 1, true};
    case O::CompField: return {2, 2, true};
    case O::CompArrayIndex: return {2, 2, true};
    case O::CompMakeVariant: return {1, ANY_OPERANDS, true};
    case O::CompStrLen: return {1, 1, true};
  }
  return {};
}

// The opcodes whose instruction shape the loop below checks beyond the
// generic arity and operand rules. One comparison picks the check, so a
// function's instruction stream costs a jump table lookup rather than a
// chain of opcode tests per instruction.
enum class InstrShape : u8 {
  Other,
  Call,
  Br,
  CondBr,
  Switch,
  GetElementPtr,
  ElemOffset,
  TypeQuery,
  Borrow,
  Memcopy,
  Memset,
  ExtractValue,
  InsertValue,
};

constexpr InstrShape shape_of(const Opcode op) {
  using O = Opcode;
  switch (op) {
    case O::Call: return InstrShape::Call;
    case O::Br: return InstrShape::Br;
    case O::CondBr: return InstrShape::CondBr;
    case O::Switch: return InstrShape::Switch;
    case O::GetElementPtr: return InstrShape::GetElementPtr;
    case O::ElemOffset: return InstrShape::ElemOffset;
    case O::TypeSizeOf:
    case O::TypeAlignOf: return InstrShape::TypeQuery;
    case O::Borrow: return InstrShape::Borrow;
    case O::Memcopy: return InstrShape::Memcopy;
    case O::Memset: return InstrShape::Memset;
    case O::ExtractValue: return InstrShape::ExtractValue;
    case O::InsertValue: return InstrShape::InsertValue;
    default: return InstrShape::Other;
  }
}

// Whether an opcode ends its block, as a byte per opcode: the five
// comparisons is_terminator makes answer from one load instead.
constexpr usize OPCODE_BYTES = 256;

constexpr std::array<u8, OPCODE_BYTES> make_terminators() {
  std::array<u8, OPCODE_BYTES> ends{};
  for (usize value = 0; value < OPCODE_BYTES; ++value) {
    ends[value] = static_cast<u8>(is_terminator(static_cast<Opcode>(value)));
  }
  return ends;
}

constexpr std::array<u8, OPCODE_BYTES> TERMINATORS = make_terminators();

constexpr bool ends_block(const Opcode op) {
  return TERMINATORS[static_cast<u8>(op)] != 0;
}

constexpr bool is_valid_symbol_kind(const SymbolKind kind) {
  using K = SymbolKind;
  return kind == K::Foreign || kind == K::Free || kind == K::Assoc ||
         kind == K::Method;
}

constexpr bool is_valid_calling_convention(const CallingConvention convention) {
  return convention == CallingConvention::C;
}

// The checks every function-like meta shares: its return type, its
// parameter and instantiation ranges, and its symbol kind. `index` is
// the row the error belongs to.
VerificationResult check_function_meta(const Storage& storage,
                                       const FunctionMeta& meta,
                                       const u32 index) {
  if (meta.return_type.idx >= storage.types().size()) {
    return err(VerificationErrorKind::TypeIdxOutOfRange, index);
  }
  if (!range_in_bounds(meta.param_types.head(), meta.param_types.size(),
                       storage.types().size())) {
    return err(VerificationErrorKind::FunctionParamTypesOutOfRange, index);
  }
  if (!range_in_bounds(meta.generics.head(), meta.generics.size(),
                       storage.types().size())) {
    return err(VerificationErrorKind::FunctionGenericsOutOfRange, index);
  }
  if (!is_valid_symbol_kind(meta.kind)) {
    return err(VerificationErrorKind::InvalidSymbolKind, index);
  }
  return base::make_ok();
}

// How many words a value of this type rides as, read from the tag alone. A
// `str` and a slice are a data pointer and a length; the primitive block
// below them is one word per tag. A pointer-shaped tag names the address of
// a place rather than the value stored there, so a row and an operand of
// pointer shape say nothing about the width of what the other one holds, and
// an aggregate's width is in its layout table. Neither answers a width here.
enum class WordCount : u8 { One, Two, Unknown };

WordCount word_count(TypeTag tag) {
  if (tag == TypeTag::Str || tag == TypeTag::Slice) {
    return WordCount::Two;
  }
  if (tag != TypeTag::Void &&
      static_cast<u8>(tag) < static_cast<u8>(TypeTag::Str)) {
    return WordCount::One;
  }
  return WordCount::Unknown;
}

// A resolved body may name no semantic construct whose meaning a later
// pass was to supply. Parametric artifacts are the first shape; this
// helper is where further semantic markers join the check.
// `allow_parametric` is membership in the list of bodies the
// specialization pass still clones; semantic markers are refused in
// both cases.
VerificationResult check_resolved_body(const Storage& storage,
                                       FunctionIdx fidx,
                                       bool allow_parametric) {
  const Function& func = storage.functions()[fidx];
  // Semantic markers never belong to a published body, including a
  // parametric one; the specialization pass does not resolve them.
  for (const BlockIdx block_idx : func.blocks) {
    const Block& block = storage.blocks()[block_idx];
    if (!range_in_bounds(block.instrs.head(), block.instrs.size(),
                         storage.instrs().size())) {
      continue;
    }
    for (const InstructionIdx iidx : block.instrs) {
      const Opcode op = storage.instrs()[iidx].op;
      if (op == Opcode::CompValue || op == Opcode::CompStrByte ||
          op == Opcode::CompStrSlice || op == Opcode::CompVariantTag ||
          op == Opcode::CompField || op == Opcode::CompArrayIndex ||
          op == Opcode::CompMakeVariant || op == Opcode::CompStrLen) {
        return err(VerificationErrorKind::MisplacedSemanticMarker, iidx.idx);
      }
    }
  }
  if (allow_parametric) {
    return base::make_ok();
  }
  if (type_names_param(storage.state(), func.meta.return_type) ||
      type_names_param(storage.state(), func.meta.owner)) {
    return err(VerificationErrorKind::MisplacedParametricType, fidx.idx);
  }
  for (const TypeIdx param : func.meta.param_types) {
    if (type_names_param(storage.state(), param)) {
      return err(VerificationErrorKind::MisplacedParametricType, fidx.idx);
    }
  }
  for (const TypeIdx generic : func.meta.generics) {
    if (type_names_param(storage.state(), generic)) {
      return err(VerificationErrorKind::MisplacedParametricType, fidx.idx);
    }
  }
  for (const BlockIdx block_idx : func.blocks) {
    const Block& block = storage.blocks()[block_idx];
    if (!range_in_bounds(block.instrs.head(), block.instrs.size(),
                         storage.instrs().size())) {
      break;
    }
    if (range_in_bounds(block.block_params.head(), block.block_params.size(),
                        storage.block_params().size())) {
      for (const BlockParamIdx pidx : block.block_params) {
        if (type_names_param(storage.state(),
                             storage.block_params()[pidx].type)) {
          return err(VerificationErrorKind::MisplacedParametricType, pidx.idx);
        }
      }
    }
    for (const InstructionIdx iidx : block.instrs) {
      const Instruction& instr = storage.instrs()[iidx];
      if (instr.op == Opcode::ParametricDrop) {
        return err(VerificationErrorKind::MisplacedParametricDrop, iidx.idx);
      }
      if (type_names_param(storage.state(), instr.measure)) {
        return err(VerificationErrorKind::MisplacedParametricType, iidx.idx);
      }
      for (const OperandIdx operand : instr.operands) {
        if (operand.idx >= storage.operands().size()) {
          continue;
        }
        const Operand& entry = storage.operands()[operand];
        if (entry.is<ParametricCalleeIdx>()) {
          return err(VerificationErrorKind::MisplacedParametricCall, iidx.idx);
        }
        if (type_names_param(storage.state(), entry.type)) {
          return err(VerificationErrorKind::MisplacedParametricType,
                     operand.idx);
        }
      }
    }
  }
  return base::make_ok();
}

}  // namespace

VerificationResult verify_storage(const Storage& storage) {
  PROFILE_SCOPE_WITH_CATEGORY("verify-ir", "frontend");
  // The global profiler is deliberate: this takes no context, so an
  // explicit instance would thread a pointer through StorageBuilder and
  // every caller. Thread it down once this slice earns its own timeline.
  for (TypeIdx tidx(0); tidx.idx < storage.types().size(); ++tidx) {
    const TypeNode& node = storage.types()[tidx];
    if (node.tag == TypeTag::Struct) {
      const StructTypeIdx sidx = node.as_struct();
      if (sidx.idx >= storage.struct_types().size()) {
        return err(VerificationErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
      const StructType& struct_type = storage.struct_types()[sidx];
      if (!range_in_bounds(struct_type.fields.head(), struct_type.fields.size(),
                           storage.types().size())) {
        return err(VerificationErrorKind::StructFieldsOutOfRange, sidx.idx);
      }
      if (!range_in_bounds(struct_type.params.head(), struct_type.params.size(),
                           storage.types().size())) {
        return err(VerificationErrorKind::StructParamsOutOfRange, sidx.idx);
      }
      if (!range_in_bounds(struct_type.field_hints.head(),
                           struct_type.field_hints.size(),
                           storage.layout_hints().size())) {
        return err(VerificationErrorKind::StructLayoutHintsOutOfRange,
                   sidx.idx);
      }
      if (struct_type.field_hints.size() > struct_type.fields.size()) {
        return err(VerificationErrorKind::StructLayoutHintsOutOfRange,
                   sidx.idx);
      }
    } else if (node.tag == TypeTag::Slice) {
      const SliceTypeIdx sidx = node.as_slice();
      if (sidx.idx >= storage.slice_types().size()) {
        return err(VerificationErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
      if (storage.slice_types()[sidx].element.idx >= storage.types().size()) {
        return err(VerificationErrorKind::TypeIdxOutOfRange,
                   storage.slice_types()[sidx].element.idx);
      }
    } else if (tidx.idx >= PRIMITIVE_TYPE_COUNT &&
               has_pointer_payload(node.tag)) {
      // The pre-interned slots of the pointer tags carry no payload and no
      // value names them; every reference the builder created sits past
      // them and points at a real pointee.
      const RefTypeIdx ridx = node.as_ref();
      if (ridx.idx >= storage.ref_types().size()) {
        return err(VerificationErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
      if (storage.ref_types()[ridx].pointee.idx >= storage.types().size()) {
        return err(VerificationErrorKind::TypeIdxOutOfRange,
                   storage.ref_types()[ridx].pointee.idx);
      }
    } else if (node.tag == TypeTag::Array) {
      const ArrayTypeIdx aidx = node.as_array();
      if (aidx.idx >= storage.array_types().size()) {
        return err(VerificationErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
      if (storage.array_types()[aidx].element.idx >= storage.types().size()) {
        return err(VerificationErrorKind::TypeIdxOutOfRange,
                   storage.array_types()[aidx].element.idx);
      }
    } else if (node.tag == TypeTag::Enum) {
      const EnumTypeIdx eidx = node.as_enum();
      if (eidx.idx >= storage.enum_types().size()) {
        return err(VerificationErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
      const EnumType& enum_type = storage.enum_types()[eidx];
      if (!range_in_bounds(enum_type.variants.head(), enum_type.variants.size(),
                           storage.enum_variant_types().size())) {
        return err(VerificationErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
      for (EnumVariantTypeIdx vidx = enum_type.variants.head();
           vidx.idx < enum_type.variants.head().idx + enum_type.variants.size();
           vidx = EnumVariantTypeIdx(vidx.idx + 1)) {
        const EnumVariantType& variant = storage.enum_variant_types()[vidx];
        if (!range_in_bounds(variant.fields.head(), variant.fields.size(),
                             storage.types().size())) {
          return err(VerificationErrorKind::EnumFieldsOutOfRange, vidx.idx);
        }
      }
      if (!range_in_bounds(enum_type.params.head(), enum_type.params.size(),
                           storage.types().size())) {
        return err(VerificationErrorKind::EnumParamsOutOfRange, eidx.idx);
      }
    } else if (node.tag == TypeTag::Tuple) {
      const TupleTypeIdx tuple_idx = node.as_tuple();
      if (tuple_idx.idx >= storage.tuple_types().size()) {
        return err(VerificationErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
      const TupleType& tuple = storage.tuple_types()[tuple_idx];
      if (!range_in_bounds(tuple.elements.head(), tuple.elements.size(),
                           storage.types().size())) {
        return err(VerificationErrorKind::TupleFieldsOutOfRange, tuple_idx.idx);
      }
    } else if (node.tag == TypeTag::Func) {
      const FuncTypeIdx func_idx = node.as_func();
      if (func_idx.idx >= storage.func_types().size()) {
        return err(VerificationErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
      const FuncType& func = storage.func_types()[func_idx];
      if (!range_in_bounds(func.params.head(), func.params.size(),
                           storage.types().size())) {
        return err(VerificationErrorKind::FuncFieldsOutOfRange, func_idx.idx);
      }
      if (func.ret.idx >= storage.types().size()) {
        return err(VerificationErrorKind::TypeIdxOutOfRange, tidx.idx);
      }
    } else if (node.tag == TypeTag::Const) {
      if (node.as_const().idx >= storage.const_types().size()) {
        return err(VerificationErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
    }
  }

  for (RegisterIdx ridx(0); ridx.idx < storage.registers().size(); ++ridx) {
    if (storage.registers()[ridx].type.idx >= storage.types().size()) {
      return err(VerificationErrorKind::TypeIdxOutOfRange, ridx.idx);
    }
  }

  for (ImmutableIdx iidx(0); iidx.idx < storage.immutables().size(); ++iidx) {
    if (storage.immutables()[iidx].type.idx >= storage.types().size()) {
      return err(VerificationErrorKind::TypeIdxOutOfRange, iidx.idx);
    }
  }

  // Bodies whose parametric types the specialization pass still resolves.
  std::vector<bool> unresolved(storage.functions().size(), false);
  for (const FunctionIdx fidx : storage.parametric_functions()) {
    if (fidx.idx >= storage.functions().size()) {
      return err(VerificationErrorKind::ParametricFunctionOutOfRange, fidx.idx);
    }
    unresolved[fidx.idx] = true;
  }

  for (FunctionIdx fidx(0); fidx.idx < storage.functions().size(); ++fidx) {
    const Function& func = storage.functions()[fidx];
    if (!range_in_bounds(func.blocks.head(), func.blocks.size(),
                         storage.blocks().size())) {
      return err(VerificationErrorKind::FunctionBlocksOutOfRange, fidx.idx);
    }
    VerificationResult meta = check_function_meta(storage, func.meta, fidx.idx);
    if (meta.is_err()) {
      return meta;
    }
    // A body outside the list has resolved everything the parametric
    // form waits on: an end or a callee record has no pass left that
    // would rewrite it, and a type parameter has no layout any reader
    // could answer.
    VerificationResult resolved =
        check_resolved_body(storage, fidx, unresolved[fidx.idx]);
    if (resolved.is_err()) {
      return resolved;
    }
  }

  // An external function has no body, so its meta is everything a
  // consumer reads; nothing may index the tables it names without these.
  for (ExternalFunctionIdx eidx(0);
       eidx.idx < storage.external_functions().size(); ++eidx) {
    const ExternalFunction& external = storage.external_functions()[eidx];
    VerificationResult meta =
        check_function_meta(storage, external.meta, eidx.idx);
    if (meta.is_err()) {
      return meta;
    }
    if (!is_valid_calling_convention(external.calling_conv)) {
      return err(VerificationErrorKind::InvalidCallingConvention, eidx.idx);
    }
  }

  // The parametric rows name the tables the specialization pass reads;
  // a row whose indexes leave them could not be resolved, so the
  // verifier refuses it here rather than in the pass.
  for (ParametricCalleeIdx cidx(0);
       cidx.idx < storage.parametric_callees().size(); ++cidx) {
    const ParametricCallee& callee = storage.parametric_callees()[cidx];
    if (callee.kind == ParametricCalleeKind::Generic) {
      if (callee.function.idx >= storage.functions().size()) {
        return err(VerificationErrorKind::ParametricCalleeOutOfRange, cidx.idx);
      }
    } else if (callee.spec == str::INVALID_STRING_POOL_ID ||
               callee.method == str::INVALID_STRING_POOL_ID) {
      return err(VerificationErrorKind::ParametricCalleeOutOfRange, cidx.idx);
    }
    if (!range_in_bounds(callee.args.head(), callee.args.size(),
                         storage.types().size())) {
      return err(VerificationErrorKind::ParametricCalleeArgsOutOfRange,
                 cidx.idx);
    }
  }
  // Register definitions: bound by block params or defined by instructions.
  // One byte per register, because every definition and every operand that
  // names a register reads or writes this set through a bit proxy otherwise.
  std::vector<u8> defined(storage.registers().size(), u8{0});
  for (BlockIdx bidx(0); bidx.idx < storage.blocks().size(); ++bidx) {
    const Block& block = storage.blocks()[bidx];
    const InstructionIdx instr_end(block.instrs.head().idx +
                                   block.instrs.size());
    if (!range_in_bounds(block.instrs.head(), block.instrs.size(),
                         storage.instrs().size())) {
      return err(VerificationErrorKind::BlockInstrsOutOfRange, bidx.idx);
    }
    if (!range_in_bounds(block.block_params.head(), block.block_params.size(),
                         storage.block_params().size())) {
      return err(VerificationErrorKind::BlockParamsOutOfRange, bidx.idx);
    }
    for (const BlockParamIdx pidx : block.block_params) {
      const BlockParam& param = storage.block_params()[pidx];
      if (param.type.idx >= storage.types().size()) {
        return err(VerificationErrorKind::TypeIdxOutOfRange, pidx.idx);
      }
      const RegisterIdx reg = param.reg;
      if (reg.idx >= storage.registers().size()) {
        return err(VerificationErrorKind::BlockParamRegOutOfRange, reg.idx);
      }
      if (defined[reg.idx]) {
        return err(VerificationErrorKind::RedefinedRegister, reg.idx);
      }
      defined[reg.idx] = true;
    }
    for (const InstructionIdx iidx : block.instrs) {
      const RegisterIdx dst = storage.instrs()[iidx].dst;
      if (dst.is_valid()) {
        if (dst.idx >= storage.registers().size()) {
          return err(VerificationErrorKind::InstrDstOutOfRange, iidx.idx);
        }
        if (defined[dst.idx]) {
          return err(VerificationErrorKind::RedefinedRegister, dst.idx);
        }
        defined[dst.idx] = true;
      }
    }
  }

  for (BlockIdx bidx(0); bidx.idx < storage.blocks().size(); ++bidx) {
    const Block& block = storage.blocks()[bidx];
    const InstructionIdx instr_end(block.instrs.head().idx +
                                   block.instrs.size());
    if (block.instrs.empty()) {
      return err(VerificationErrorKind::UnterminatedBlock, bidx.idx);
    }
    for (const InstructionIdx iidx : block.instrs) {
      const Instruction& instr = storage.instrs()[iidx];
      const bool last = (iidx.idx + 1 == instr_end.idx);
      if (ends_block(instr.op) != last) {
        if (last) {
          return err(VerificationErrorKind::UnterminatedBlock, bidx.idx);
        }
        return err(VerificationErrorKind::MisplacedTerminator, iidx.idx);
      }
      if (!range_in_bounds(instr.operands.head(), instr.operands.size(),
                           storage.operands().size())) {
        return err(VerificationErrorKind::InstrOperandsOutOfRange, iidx.idx);
      }
      const u32 first_operand = instr.operands.head().idx;
      const u32 operand_count = instr.operands.size();
      for (u32 offset = 0; offset < operand_count; ++offset) {
        const Operand& operand = storage.operands()[first_operand + offset];
        if (operand.type.idx >= storage.types().size()) {
          return err(VerificationErrorKind::TypeIdxOutOfRange,
                     first_operand + offset);
        }
        switch (operand.tag()) {
          case Operand::TAG_OF<RegisterIdx>:
            if (operand.as_register().idx >= storage.registers().size()) {
              return err(VerificationErrorKind::OperandIdxOutOfRange,
                         first_operand + offset);
            }
            if (!defined[operand.as_register().idx]) {
              return err(VerificationErrorKind::UndefinedRegister,
                         operand.as_register().idx);
            }
            {
              // A register's row and the operand that names it are two
              // places a value's type is written. They need not name the
              // same index, since a type index can be resolved in place,
              // but a reader takes the tag as the width of the word.
              const TypeIdx row_type =
                  storage.registers()[operand.as_register()].type;
              const TypeTag row_tag = storage.types()[row_type.idx].tag;
              const TypeTag op_tag = storage.types()[operand.type.idx].tag;
              const bool row_scalar =
                  static_cast<u8>(row_tag) < static_cast<u8>(TypeTag::Str);
              const bool op_scalar =
                  static_cast<u8>(op_tag) < static_cast<u8>(TypeTag::Str);
              if (row_tag != op_tag && row_scalar && op_scalar) {
                return err(VerificationErrorKind::OperandTypeMismatch,
                           first_operand + offset);
              }
              // A pair read as a word, or a word as a pair, reads past
              // the end of the value whatever the tags are called.
              const WordCount row_words = word_count(row_tag);
              const WordCount op_words = word_count(op_tag);
              if (row_words != WordCount::Unknown &&
                  op_words != WordCount::Unknown && row_words != op_words) {
                return err(VerificationErrorKind::OperandTypeMismatch,
                           first_operand + offset);
              }
            }
            break;
          case Operand::TAG_OF<FunctionIdx>:
            if (operand.as_function().idx >= storage.functions().size()) {
              return err(VerificationErrorKind::OperandIdxOutOfRange,
                         first_operand + offset);
            }
            break;
          case Operand::TAG_OF<BlockIdx>:
            if (operand.as_block().idx >= storage.blocks().size()) {
              return err(VerificationErrorKind::OperandIdxOutOfRange,
                         first_operand + offset);
            }
            break;
          case Operand::TAG_OF<ImmutableIdx>:
            if (operand.as_immutable().idx >= storage.immutables().size()) {
              return err(VerificationErrorKind::OperandIdxOutOfRange,
                         first_operand + offset);
            }
            break;
          case Operand::TAG_OF<ExternalFunctionIdx>:
            if (operand.as_external_function().idx >=
                storage.external_functions().size()) {
              return err(VerificationErrorKind::OperandIdxOutOfRange,
                         first_operand + offset);
            }
            break;
          case Operand::TAG_OF<ParametricCalleeIdx>:
            if (operand.as_parametric_callee().idx >=
                storage.parametric_callees().size()) {
              return err(VerificationErrorKind::OperandIdxOutOfRange,
                         first_operand + offset);
            }
            break;
          case Operand::TAG_OF<void>:
            return err(VerificationErrorKind::UnknownOperandTag,
                       first_operand + offset);
        }
      }
      switch (shape_of(instr.op)) {
        case InstrShape::Call: {
          if (instr.operands.empty()) {
            return err(VerificationErrorKind::InvalidCallee, iidx.idx);
          }
          const Operand& head = storage.operands()[instr.operands.head()];
          if (head.is<ParametricCalleeIdx>()) {
            // The call's signature is the callee operand's own type, the
            // function type the definition-site check filled.
            const TypeNode& callee_type = storage.types()[head.type.idx];
            if (callee_type.tag != TypeTag::Func) {
              return err(VerificationErrorKind::InvalidCallee, iidx.idx);
            }
            const FuncType& sig = storage.func_types()[callee_type.as_func()];
            if (instr.operands.size() - 1 != sig.params.size()) {
              return err(VerificationErrorKind::InvalidCallee, iidx.idx);
            }
            break;
          }
          if (!head.is<FunctionIdx>() && !head.is<ExternalFunctionIdx>()) {
            // A call through a function value carries the value: the
            // callee is a register of function type, and the operands
            // after it are the arguments.
            if (!head.is<RegisterIdx>()) {
              return err(VerificationErrorKind::InvalidCallee, iidx.idx);
            }
            const TypeNode& callee_type =
                storage
                    .types()[storage.registers()[head.as_register()].type.idx];
            if (callee_type.tag != TypeTag::Func) {
              return err(VerificationErrorKind::InvalidCallee, iidx.idx);
            }
            const FuncType& sig = storage.func_types()[callee_type.as_func()];
            if (instr.operands.size() - 1 != sig.params.size()) {
              return err(VerificationErrorKind::InvalidCallee, iidx.idx);
            }
            break;
          }
          const FunctionMeta& meta =
              head.is<FunctionIdx>()
                  ? storage.functions()[head.as_function()].meta
                  : storage.external_functions()[head.as_external_function()]
                        .meta;
          if (instr.operands.size() - 1 != meta.param_types.size()) {
            return err(VerificationErrorKind::InvalidCallee, iidx.idx);
          }
          break;
        }
        case InstrShape::Br: {
          if (instr.operands.empty()) {
            return err(VerificationErrorKind::InvalidBranchTarget, iidx.idx);
          }
          const Operand& target = storage.operands()[instr.operands.head()];
          if (!target.is<BlockIdx>()) {
            return err(VerificationErrorKind::InvalidBranchTarget, iidx.idx);
          }
          // operands[1..] are the target's parameters, positionally.
          if (instr.operands.size() !=
              1 + storage.blocks()[target.as_block()].block_params.size()) {
            return err(VerificationErrorKind::InvalidBranchTarget, iidx.idx);
          }
          break;
        }
        case InstrShape::CondBr: {
          // operands = [cond(i1), true_block, false_block]; targets take
          // no block parameters.
          if (instr.operands.size() != 3) {
            return err(VerificationErrorKind::InvalidCondBr, iidx.idx);
          }
          const Operand& cond = storage.operands()[instr.operands.head()];
          if (storage.types()[cond.type.idx].tag != TypeTag::I1) {
            return err(VerificationErrorKind::InvalidCondBr, iidx.idx);
          }
          for (u32 offset = 1; offset <= 2; ++offset) {
            const Operand& target =
                storage.operands()[instr.operands.head() + offset];
            if (!target.is<BlockIdx>()) {
              return err(VerificationErrorKind::InvalidCondBr, iidx.idx);
            }
            if (!storage.blocks()[target.as_block()].block_params.empty()) {
              return err(VerificationErrorKind::InvalidCondBr, iidx.idx);
            }
          }
          break;
        }
        case InstrShape::Switch: {
          // operands = [value, default_block, (case_imm, case_block)...];
          // Targets take no block parameters.
          if (instr.operands.size() < 2 || (instr.operands.size() % 2) != 0) {
            return err(VerificationErrorKind::InvalidSwitch, iidx.idx);
          }
          const Operand& value = storage.operands()[instr.operands.head()];
          if (!is_integer_type(storage.types()[value.type.idx].tag)) {
            return err(VerificationErrorKind::InvalidSwitch, iidx.idx);
          }
          const Operand& default_target =
              storage.operands()[instr.operands.head() + 1];
          if (!default_target.is<BlockIdx>()) {
            return err(VerificationErrorKind::InvalidSwitch, iidx.idx);
          }
          if (!storage.blocks()[default_target.as_block()]
                   .block_params.empty()) {
            return err(VerificationErrorKind::InvalidSwitch, iidx.idx);
          }
          for (u32 offset = 2; offset < instr.operands.size(); offset += 2) {
            const Operand& case_value =
                storage.operands()[instr.operands.head() + offset];
            const Operand& case_target =
                storage.operands()[instr.operands.head() + offset + 1];
            if (!case_value.is<ImmutableIdx>() || !case_target.is<BlockIdx>()) {
              return err(VerificationErrorKind::InvalidSwitch, iidx.idx);
            }
            if (!storage.blocks()[case_target.as_block()]
                     .block_params.empty()) {
              return err(VerificationErrorKind::InvalidSwitch, iidx.idx);
            }
          }
          break;
        }
        case InstrShape::GetElementPtr: {
          // operands = [base_ptr(register), index(integer)...].
          if (instr.operands.size() < 2) {
            return err(VerificationErrorKind::InvalidGetElementPtr, iidx.idx);
          }
          if (!storage.operands()[instr.operands.head()].is<RegisterIdx>()) {
            return err(VerificationErrorKind::InvalidGetElementPtr, iidx.idx);
          }
          for (u32 offset = 1; offset < instr.operands.size(); ++offset) {
            const Operand& index =
                storage.operands()[instr.operands.head() + offset];
            if (!is_integer_type(storage.types()[index.type.idx].tag)) {
              return err(VerificationErrorKind::InvalidGetElementPtr, iidx.idx);
            }
          }
          break;
        }
        case InstrShape::ElemOffset: {
          const ir::OperandIdx base = instr.operands.head();
          if (instr.operands.size() != 2 ||
              !storage.operands()[base].is<RegisterIdx>() ||
              !is_integer_type(
                  storage
                      .types()[storage.operands()[ir::OperandIdx(base.idx + 1)]
                                   .type.idx]
                      .tag) ||
              !instr.dst.is_valid()) {
            return err(VerificationErrorKind::InvalidGetElementPtr, iidx.idx);
          }
          break;
        }
        case InstrShape::TypeQuery: {
          if (!instr.operands.empty() || !instr.measure.is_valid() ||
              !instr.dst.is_valid() ||
              instr.measure.idx >= storage.types().size()) {
            return err(VerificationErrorKind::InvalidTypeQuery, iidx.idx);
          }
          break;
        }
        case InstrShape::Borrow: {
          if (instr.operands.size() != 1) {
            return err(VerificationErrorKind::InvalidBorrow, iidx.idx);
          }
          if (!storage.operands()[instr.operands.head()].is<RegisterIdx>()) {
            return err(VerificationErrorKind::InvalidBorrow, iidx.idx);
          }
          if (!instr.dst.is_valid()) {
            return err(VerificationErrorKind::InvalidBorrow, iidx.idx);
          }
          break;
        }
        case InstrShape::Memcopy: {
          if (instr.operands.size() != 3 || instr.dst.is_valid()) {
            return err(VerificationErrorKind::InvalidMemcopy, iidx.idx);
          }
          for (u32 offset = 0; offset < 2; ++offset) {
            const Operand& ptr =
                storage.operands()[instr.operands.head() + offset];
            const TypeTag tag = storage.types()[ptr.type.idx].tag;
            if (tag != TypeTag::Ref && tag != TypeTag::MutRef &&
                tag != TypeTag::Ptr) {
              return err(VerificationErrorKind::InvalidMemcopy, iidx.idx);
            }
          }
          const Operand& len = storage.operands()[instr.operands.head() + 2];
          if (!is_integer_type(storage.types()[len.type.idx].tag)) {
            return err(VerificationErrorKind::InvalidMemcopy, iidx.idx);
          }
          break;
        }
        case InstrShape::Memset: {
          if (instr.operands.size() != 3 || instr.dst.is_valid()) {
            return err(VerificationErrorKind::InvalidMemset, iidx.idx);
          }
          // A fill names its destination as a store does: a register
          // whose type carries the pointee, so the address needs no
          // separate pointee row.
          if (!storage.operands()[instr.operands.head()].is<RegisterIdx>()) {
            return err(VerificationErrorKind::InvalidMemset, iidx.idx);
          }
          const Operand& byte = storage.operands()[instr.operands.head() + 1];
          if (storage.types()[byte.type.idx].tag != TypeTag::U8) {
            return err(VerificationErrorKind::InvalidMemset, iidx.idx);
          }
          const Operand& len = storage.operands()[instr.operands.head() + 2];
          if (!is_integer_type(storage.types()[len.type.idx].tag)) {
            return err(VerificationErrorKind::InvalidMemset, iidx.idx);
          }
          break;
        }
        case InstrShape::ExtractValue:
        case InstrShape::InsertValue: {
          // Extract: [aggregate, index(imm)...]; Insert: [aggregate, value,
          // index(imm)...]. Indexes must be integer immediates.
          const u32 first_index = (instr.op == Opcode::ExtractValue) ? 1 : 2;
          if (instr.operands.size() < first_index + 1) {
            return err(VerificationErrorKind::InvalidExtractInsert, iidx.idx);
          }
          for (u32 offset = first_index; offset < instr.operands.size();
               ++offset) {
            const Operand& index =
                storage.operands()[instr.operands.head() + offset];
            if (!index.is<ImmutableIdx>() ||
                !is_integer_type(storage.types()[index.type.idx].tag)) {
              return err(VerificationErrorKind::InvalidExtractInsert, iidx.idx);
            }
          }
          break;
        }
        case InstrShape::Other: break;
      }

      // The generic arity check runs last so an opcode's own shape check
      // above reports the narrower error where there is one.

      const OperandArity arity = operand_arity(instr.op);
      if (!arity.known) {
        return err(VerificationErrorKind::UnknownOpcode, iidx.idx);
      }
      if (instr.operands.size() < arity.min ||
          instr.operands.size() > arity.max) {
        return err(VerificationErrorKind::InstrOperandArity, iidx.idx);
      }
    }
  }

  for (FunctionIdx fidx(0); fidx.idx < storage.functions().size(); ++fidx) {
    if (storage.functions()[fidx].blocks.empty()) {
      return err(VerificationErrorKind::UnterminatedBlock, fidx.idx);
    }
  }

  return base::make_ok();
}

}  // namespace ir
