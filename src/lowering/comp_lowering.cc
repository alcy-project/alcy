// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/types.h"
#include "ast/ast.h"
#include "comp/comp_value.h"
#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_interner.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/opcode.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "lowering/lowerer.h"

namespace lowering {

Val Lowerer::Comp::materialize(const CompVal& value, diag::Span span) {
  const ir::TypeTag tag = owner_.tag_of(value.type);
  switch (value.value.tag) {
    case CompValue::Tag::Int:
    case CompValue::Tag::Bool: {
      const u64 bits = value.value.tag == CompValue::Tag::Bool
                           ? (value.value.bool_value ? 1 : 0)
                           : value.value.int_value;
      return Val{owner_.imm_from_u64(tag, value.type, bits), value.type, false,
                 false};
    }
    case CompValue::Tag::Str: {
      const str::StringPoolId id = owner_.intern_copied(value.value.str_value);
      if (id == str::INVALID_STRING_POOL_ID) {
        return Val{owner_.size_one, owner_.error_type(), false, false};
      }
      const ir::ImmutableIdx imm = owner_.builder.immutable(
          {.type = value.type, .data = {.str_id_value = id}});
      return Val{owner_.to_operand(imm, value.type), value.type, false, false};
    }
    case CompValue::Tag::Void:
      return Val{owner_.size_one, value.type, false, false};
    case CompValue::Tag::Tuple: {
      const ir::TupleType& shape =
          owner_.builder.state()
              .tuple_types[owner_.builder.state().types[value.type].as_tuple()];
      if (shape.elements.size() != value.value.fields.size()) {
        owner_.internal(span, "comp tuple arity");
        return Val{owner_.size_one, owner_.error_type(), false, false};
      }
      const ir::RegisterIdx addr =
          owner_.emit(ir::Opcode::Alloca, value.type, {owner_.size_one});
      for (u32 i = 0; i < static_cast<u32>(shape.elements.size()); ++i) {
        const CompVal field{value.value.fields[i], shape.elements[i]};
        Val lowered = materialize(field, span);
        if (owner_.failed) {
          return Val{owner_.size_one, owner_.error_type(), false, false};
        }
        const ir::RegisterIdx gep =
            owner_.emit(ir::Opcode::GetElementPtr, shape.elements[i],
                        {owner_.to_operand(addr, value.type), owner_.zero_i32,
                         owner_.index_operand(i)});
        owner_.emit_void(ir::Opcode::Store,
                         {owner_.use_value(lowered),
                          owner_.to_operand(gep, shape.elements[i])});
      }
      return Val{owner_.to_operand(addr, value.type), value.type, true, false};
    }
    case CompValue::Tag::Array: {
      const ir::ArrayType& shape =
          owner_.builder.state()
              .array_types[owner_.builder.state().types[value.type].as_array()];
      if (shape.count != value.value.fields.size()) {
        owner_.internal(span, "comp array arity");
        return Val{owner_.size_one, owner_.error_type(), false, false};
      }
      const ir::RegisterIdx addr =
          owner_.emit(ir::Opcode::Alloca, value.type, {owner_.size_one});
      for (u32 i = 0; i < static_cast<u32>(shape.count); ++i) {
        const CompVal field{value.value.fields[i], shape.element};
        Val lowered = materialize(field, span);
        if (owner_.failed) {
          return Val{owner_.size_one, owner_.error_type(), false, false};
        }
        const ir::RegisterIdx gep =
            owner_.emit(ir::Opcode::GetElementPtr, shape.element,
                        {owner_.to_operand(addr, value.type), owner_.zero_i32,
                         owner_.index_operand(i)});
        owner_.emit_void(
            ir::Opcode::Store,
            {owner_.use_value(lowered), owner_.to_operand(gep, shape.element)});
      }
      return Val{owner_.to_operand(addr, value.type), value.type, true, false};
    }
    case CompValue::Tag::Struct: {
      const auto* info = owner_.struct_info(value.type);
      if (info == nullptr || info->fields.size() != value.value.fields.size()) {
        owner_.internal(span, "comp struct arity");
        return Val{owner_.size_one, owner_.error_type(), false, false};
      }
      const ir::RegisterIdx addr =
          owner_.emit(ir::Opcode::Alloca, value.type, {owner_.size_one});
      for (u32 i = 0; i < static_cast<u32>(info->fields.size()); ++i) {
        const ir::TypeIdx field_type =
            owner_.field_type_of(value.type, i, span);
        if (owner_.failed) {
          return Val{owner_.size_one, owner_.error_type(), false, false};
        }
        const CompVal field{value.value.fields[i], field_type};
        Val lowered = materialize(field, span);
        if (owner_.failed) {
          return Val{owner_.size_one, owner_.error_type(), false, false};
        }
        const ir::RegisterIdx gep =
            owner_.emit(ir::Opcode::GetElementPtr, field_type,
                        {owner_.to_operand(addr, value.type), owner_.zero_i32,
                         owner_.index_operand(i)});
        owner_.emit_void(
            ir::Opcode::Store,
            {owner_.use_value(lowered), owner_.to_operand(gep, field_type)});
      }
      return Val{owner_.to_operand(addr, value.type), value.type, true, false};
    }
    case CompValue::Tag::Enum: {
      const std::vector<ir::TypeIdx> payloads =
          owner_.variant_payload(value.type, value.value.variant);
      if (payloads.size() != value.value.fields.size()) {
        owner_.internal(span, "comp variant arity");
        return Val{owner_.size_one, owner_.error_type(), false, false};
      }
      const ir::TypeIdx slot = owner_.enum_slot_type(value.type);
      const ir::RegisterIdx addr =
          owner_.emit(ir::Opcode::Alloca, slot, {owner_.size_one});
      const ir::EnumNiche niche =
          ir::enum_niche(owner_.builder.state(), value.type);
      if (niche.has) {
        // The fieldless variant is the payload's spare pattern; the
        // carrying one is the payload the loop below stores (ADR-0087).
        if (value.value.variant == niche.fieldless) {
          owner_.emit_void(ir::Opcode::Store, {owner_.niche_sentinel(niche),
                                               owner_.to_operand(addr, slot)});
        }
      } else {
        const ir::RegisterIdx tag_reg =
            owner_.emit(ir::Opcode::GetElementPtr,
                        owner_.builder.primitive(ir::TypeTag::I32),
                        {owner_.to_operand(addr, slot), owner_.zero_i32,
                         owner_.index_operand(0)});
        owner_.emit_void(ir::Opcode::Store,
                         {owner_.disc_operand(value.value.variant),
                          owner_.to_operand(tag_reg, slot)});
      }
      if (!payloads.empty() && !owner_.is_unit_payload(payloads)) {
        // The payload lives in the slot, the way a constructed variant
        // stores it; a boxed tuple would make every reader of the slot
        // load the payload pointer as the field itself.
        const ir::TypeIdxRange fields =
            owner_.variant_fields(value.type, value.value.variant);
        const Val slot_addr{owner_.to_operand(addr, slot), slot, true, false};
        for (usize i = 0; i < payloads.size(); ++i) {
          const CompVal field{value.value.fields[i], payloads[i]};
          Val lowered = materialize(field, span);
          if (owner_.failed) {
            return Val{owner_.size_one, owner_.error_type(), false, false};
          }
          const ir::RegisterIdx field_addr =
              niche.has
                  ? addr
                  : owner_.payload_field_addr(slot_addr, value.type, fields,
                                              static_cast<u32>(i));
          const ir::OperandIdx stored =
              niche.has && niche.slot != niche.payload
                  ? owner_.to_operand(
                        owner_.emit(ir::Opcode::TypeCast, niche.slot,
                                    {owner_.use_value(lowered)}),
                        niche.slot)
                  : owner_.use_value(lowered);
          owner_.emit_void(
              ir::Opcode::Store,
              {stored, owner_.to_operand(field_addr, payloads[i])});
        }
      }
      return Val{owner_.to_operand(addr, slot), value.type, true, false};
    }
  }
}

// Top-level comp evaluation: the evaluator gets the current
// instantiation as its seed and the persistent comp bindings as the
// outer scope; a failure is reported where the lowering reports.
bool Lowerer::Comp::evaluate(u32 mod, ast::ExprIdx expr, CompVal& out) {
  owner_.comp_.clear_failure();
  if (!owner_.comp_.evaluate(mod, expr, owner_.cur_inst_, &owner_.comp_scope_,
                             out)) {
    report_failure();
    return false;
  }
  return true;
}

bool Lowerer::Comp::evaluate_item(u32 mod, ast::ExprIdx init, CompVal& out) {
  owner_.comp_.clear_failure();
  if (!owner_.comp_.evaluate_item(mod, init, owner_.cur_inst_, out)) {
    report_failure();
    return false;
  }
  return true;
}

bool Lowerer::Comp::bind_pattern(u32 mod,
                                 ast::PatternIdx pattern,
                                 const CompVal& value,
                                 CompScope& scope,
                                 diag::Span span) {
  owner_.comp_.clear_failure();
  if (!owner_.comp_.bind_pattern(mod, pattern, value, scope, span)) {
    report_failure();
    return false;
  }
  return true;
}

void Lowerer::Comp::report_failure() {
  switch (owner_.comp_.failure_kind()) {
    case analyzer::CompEvaluator::Failure::Internal:
      owner_.internal(owner_.comp_.failure_span(), owner_.comp_.failure());
      return;
    case analyzer::CompEvaluator::Failure::Overflow:
      owner_.comp_overflow(owner_.comp_.failure_span());
      return;
    case analyzer::CompEvaluator::Failure::DivZero:
      owner_.comp_div_zero(owner_.comp_.failure_span());
      return;
    case analyzer::CompEvaluator::Failure::OutOfBounds:
      owner_.comp_oob(owner_.comp_.failure_span());
      return;
    case analyzer::CompEvaluator::Failure::Budget:
      owner_.comp_budget(owner_.comp_.failure_span());
      return;
    case analyzer::CompEvaluator::Failure::Program:
    case analyzer::CompEvaluator::Failure::None: break;
  }
  owner_.unsupported(owner_.comp_.failure_span(), owner_.comp_.failure());
}

}  // namespace lowering
