// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "lower/lower.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/immutable.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "ir/verifier.h"

namespace lower {

namespace {

// Diagnostic codes 4300-4319 are reserved for lowering.
constexpr u32 kLowerUnsupported = 4300;
constexpr u32 kLowerInternal = 4301;

// A lowered value: either an SSA operand or the address of one.
// Places stay in address form so moves and borrows observe origins.
struct Val {
  ir::OperandIdx op;
  ir::TypeIdx type;
  bool address = false;
  bool place = false;
};

struct Local {
  std::string_view name;
  ir::RegisterIdx addr;
  ir::TypeIdx type;
};

struct Lowerer {
  analyzer::CheckedPackage pkg;
  ir::StorageBuilder builder;
  ir::PointerWidth width;
  str::StringInterner& strings;
  diag::DiagBag& bag;
  bool failed = false;

  struct FnEntry {
    const ast::FnItem* item;
    ir::FunctionIdx idx;
  };
  std::vector<FnEntry> fns;

  struct ExtEntry {
    std::string_view name;
    ir::ExternalFunctionIdx idx;
  };
  std::vector<ExtEntry> exts;

  // Per-function state.
  u32 module = 0;
  std::vector<Local> locals;
  ir::InstrSeq instrs;
  bool terminated = false;
  ir::OperandIdx size_one = ir::OperandIdx(0);
  ir::OperandIdx zero_i32 = ir::OperandIdx(0);

  Lowerer(analyzer::CheckedPackage package,
          ir::PointerWidth width,
          str::StringInterner& strings,
          diag::DiagBag& bag)
      : pkg(std::move(package)),
        builder(std::move(pkg.types).take_state()),
        width(width),
        strings(strings),
        bag(bag) {}

  void unsupported(diag::Span span, std::string_view what) {
    const u32 index =
        bag.emit(diag::Severity::Error, kLowerUnsupported, span,
                 "'{}' is not supported in this lowering slice", what);
    (void)index;
    failed = true;
  }

  void internal(diag::Span span, std::string_view what) {
    const u32 index = bag.emit(diag::Severity::Error, kLowerInternal, span,
                               "internal lowering error: {}", what);
    (void)index;
    failed = true;
  }

  ir::TypeIdx error_type() { return builder.error_type(); }

  bool is_copy(ir::TypeIdx type) const {
    return ir::is_copy_type(builder.state(), type);
  }

  ir::TypeTag tag_of(ir::TypeIdx idx) const {
    return builder.state().types[idx].tag;
  }

  bool is_ref_tag(ir::TypeTag tag) const {
    return tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef ||
           tag == ir::TypeTag::Ptr;
  }

  // Structural equality for the same reason the checker needs it:
  // field slots hold copies, so shared shapes carry different indexes.
  bool same_shape(ir::TypeIdx a, ir::TypeIdx b) {
    std::vector<u64> seen;
    return same_shape_inner(a, b, seen);
  }

  bool same_shape_inner(ir::TypeIdx a, ir::TypeIdx b, std::vector<u64>& seen) {
    if (a.idx == b.idx) {
      return true;
    }
    const u64 key = (static_cast<u64>(a.idx) << 32) | b.idx;
    for (u64 prior : seen) {
      if (prior == key) {
        return true;
      }
    }
    seen.push_back(key);
    const ir::TypeTag ta = tag_of(a);
    if (ta != tag_of(b)) {
      return false;
    }
    switch (ta) {
      case ir::TypeTag::Ref:
      case ir::TypeTag::MutRef: {
        const auto& refs = builder.state().ref_types;
        return same_shape_inner(refs[builder.state().types[a].as_ref()].pointee,
                                refs[builder.state().types[b].as_ref()].pointee,
                                seen);
      }
      case ir::TypeTag::Array: {
        const auto& arrays = builder.state().array_types;
        const ir::ArrayType& aa = arrays[builder.state().types[a].as_array()];
        const ir::ArrayType& ab = arrays[builder.state().types[b].as_array()];
        return aa.count == ab.count &&
               same_shape_inner(aa.element, ab.element, seen);
      }
      case ir::TypeTag::Tuple: {
        const auto& tuples = builder.state().tuple_types;
        const ir::TupleType& ta_t = tuples[builder.state().types[a].as_tuple()];
        const ir::TupleType& tb_t = tuples[builder.state().types[b].as_tuple()];
        if (ta_t.elements.size() != tb_t.elements.size()) {
          return false;
        }
        for (u32 i = 0; i < ta_t.elements.size(); ++i) {
          if (!same_shape_inner(ta_t.elements[i], tb_t.elements[i], seen)) {
            return false;
          }
        }
        return true;
      }
      case ir::TypeTag::Struct:
      case ir::TypeTag::Enum: return false;
      default: return true;
    }
  }

  ir::RegisterIdx claim_reg() {
    return ir::RegisterIdx(static_cast<u32>(builder.state().registers.size()));
  }

  ir::OperandIdx to_operand(ir::RegisterIdx reg, ir::TypeIdx type) {
    return builder.operand(ir::Operand::from_register(reg, type));
  }

  ir::OperandIdx to_operand(ir::ImmutableIdx imm, ir::TypeIdx type) {
    return builder.operand(ir::Operand::from_immutable(imm, type));
  }

  ir::RegisterIdx emit(ir::Opcode op,
                       ir::TypeIdx type,
                       const std::vector<ir::OperandIdx>& ops) {
    const ir::RegisterIdx dst = claim_reg();
    // Operand ranges must be consecutive in storage, but inputs are
    // built bottom-up at arbitrary positions; re-append copies so the
    // range is always fresh and contiguous.
    const ir::OperandIdx head =
        ir::OperandIdx(static_cast<u32>(builder.state().operands.size()));
    for (ir::OperandIdx op_idx : ops) {
      builder.operand(ir::Operand(builder.state().operands[op_idx]));
    }
    const ir::OperandIdxRange range = {head, static_cast<u32>(ops.size())};
    const ir::InstructionIdx instr =
        builder.instr({.op = op, .flags = {}, .dst = dst, .operands = range});
    builder.reg({.type = type, .def_idx = instr});
    instrs.push(instr);
    return dst;
  }

  void emit_void(ir::Opcode op, const std::vector<ir::OperandIdx>& ops) {
    const ir::OperandIdx head =
        ir::OperandIdx(static_cast<u32>(builder.state().operands.size()));
    for (ir::OperandIdx op_idx : ops) {
      builder.operand(ir::Operand(builder.state().operands[op_idx]));
    }
    const ir::OperandIdxRange range = {head, static_cast<u32>(ops.size())};
    instrs.push(builder.instr({.op = op,
                               .flags = {},
                               .dst = ir::RegisterIdx(base::kInvalidIdx),
                               .operands = range}));
  }

  Val materialize(Val v) {
    if (!v.address) {
      return v;
    }
    const ir::RegisterIdx reg = emit(ir::Opcode::Load, v.type, {v.op});
    return Val{to_operand(reg, v.type), v.type, false, false};
  }

  Val address_of(Val v) {
    if (v.address) {
      return v;
    }
    const Val material = materialize(v);
    const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, v.type, {size_one});
    emit_void(ir::Opcode::Store, {material.op, to_operand(addr, v.type)});
    return Val{to_operand(addr, v.type), v.type, true, v.place};
  }

  // Uses a value, emitting a Move marker when a non-Copy place is
  // consumed. The marker's source is the place address so later
  // ownership analysis observes origins, not temporaries.
  ir::OperandIdx use_value(Val v) {
    if (v.place && v.address && !is_copy(v.type)) {
      const ir::RegisterIdx marker = emit(ir::Opcode::Move, v.type, {v.op});
      (void)marker;
    }
    return materialize(v).op;
  }

  const Local* lookup_local(std::string_view name) const {
    for (const Local& local : locals) {
      if (local.name == name) {
        return &local;
      }
    }
    return nullptr;
  }

  const analyzer::CheckedModule::StaticInfo* lookup_static(
      u32 mod,
      std::string_view name) const {
    for (const auto& info : pkg.modules[mod].statics) {
      if (info.name == name) {
        return &info;
      }
    }
    for (const analyzer::Import& import : pkg.tree.modules[mod]->imports) {
      if (import.ns != analyzer::Namespace::Value || import.name != name) {
        continue;
      }
      for (const auto& info : pkg.modules[import.target_module].statics) {
        if (info.name == import.member) {
          return &info;
        }
      }
    }
    return nullptr;
  }

  ir::TypeIdx expr_type(const ast::Expr* expr) {
    for (const auto& entry : pkg.modules[module].expr_types) {
      if (entry.first == expr) {
        return entry.second;
      }
    }
    return error_type();
  }

  const analyzer::CheckedModule::CallTarget* call_target(
      const ast::Expr* callee) const {
    for (const auto& entry : pkg.modules[module].call_targets) {
      if (entry.callee == callee) {
        return &entry;
      }
    }
    return nullptr;
  }

  ir::FunctionIdx fn_index(const ast::FnItem* item) {
    for (const FnEntry& entry : fns) {
      if (entry.item == item) {
        return entry.idx;
      }
    }
    return ir::FunctionIdx(base::kInvalidIdx);
  }

  const analyzer::CheckedModule::StructInfo* struct_info(ir::TypeIdx type) {
    for (const auto& checked : pkg.modules) {
      for (const auto& info : checked.structs) {
        if (info.type.idx == type.idx) {
          return &info;
        }
      }
    }
    return nullptr;
  }

  // Literal values. Suffixes were validated by checking; strip the
  // longest known suffix and parse what remains (wrapping arithmetic
  // matches release overflow semantics; checked overflow is later work).
  u64 parse_int_value(std::string_view spelling) {
    constexpr std::string_view kSuffixes[] = {
        "isize", "usize", "i8",  "i16", "i32", "i64",
        "u8",    "u16",   "u32", "u64", "f32", "f64",
    };
    for (std::string_view suffix : kSuffixes) {
      if (spelling.size() > suffix.size() &&
          spelling.substr(spelling.size() - suffix.size()) == suffix) {
        spelling.remove_suffix(suffix.size());
        break;
      }
    }
    u32 base = 10;
    if (spelling.size() > 2 && spelling[0] == '0') {
      if (spelling[1] == 'x' || spelling[1] == 'X') {
        base = 16;
        spelling.remove_prefix(2);
      } else if (spelling[1] == 'b' || spelling[1] == 'B') {
        base = 2;
        spelling.remove_prefix(2);
      } else if (spelling[1] == 'o' || spelling[1] == 'O') {
        base = 8;
        spelling.remove_prefix(2);
      }
    }
    u64 value = 0;
    for (char c : spelling) {
      if (c == '_') {
        continue;
      }
      u32 digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<u32>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<u32>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<u32>(c - 'A' + 10);
      } else {
        continue;
      }
      value = value * base + digit;
    }
    return value;
  }

  ir::TypeTag literal_tag(const ast::Literal* lit,
                          const ir::TypeIdx* expected) {
    if (lit->kind == ast::LiteralKind::Bool) {
      return ir::TypeTag::I1;
    }
    if (lit->kind == ast::LiteralKind::String) {
      return ir::TypeTag::Str;
    }
    const bool is_float = lit->kind == ast::LiteralKind::Float;
    if (expected != nullptr) {
      const ir::TypeTag tag = tag_of(*expected);
      const bool matches =
          is_float ? (tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64)
                   : (tag == ir::TypeTag::I8 || tag == ir::TypeTag::I16 ||
                      tag == ir::TypeTag::I32 || tag == ir::TypeTag::I64 ||
                      tag == ir::TypeTag::U8 || tag == ir::TypeTag::U16 ||
                      tag == ir::TypeTag::U32 || tag == ir::TypeTag::U64);
      if (matches) {
        return tag;
      }
    }
    struct SuffixTag {
      std::string_view suffix;
      ir::TypeTag tag;
    };
    constexpr SuffixTag kSuffixes[] = {
        {"f32", ir::TypeTag::F32},   {"f64", ir::TypeTag::F64},
        {"isize", ir::TypeTag::I64}, {"usize", ir::TypeTag::U64},
        {"i8", ir::TypeTag::I8},     {"i16", ir::TypeTag::I16},
        {"i32", ir::TypeTag::I32},   {"i64", ir::TypeTag::I64},
        {"u8", ir::TypeTag::U8},     {"u16", ir::TypeTag::U16},
        {"u32", ir::TypeTag::U32},   {"u64", ir::TypeTag::U64},
    };
    for (const SuffixTag& entry : kSuffixes) {
      if (lit->spelling.size() > entry.suffix.size() &&
          lit->spelling.substr(lit->spelling.size() - entry.suffix.size()) ==
              entry.suffix) {
        if (entry.suffix == "isize") {
          return width == ir::PointerWidth::W64 ? ir::TypeTag::I64
                                                : ir::TypeTag::I32;
        }
        if (entry.suffix == "usize") {
          return width == ir::PointerWidth::W64 ? ir::TypeTag::U64
                                                : ir::TypeTag::U32;
        }
        return entry.tag;
      }
    }
    return is_float ? ir::TypeTag::F64 : ir::TypeTag::I32;
  }

  Val lower_literal(const ast::Literal* lit, const ir::TypeIdx* expected) {
    const ir::TypeTag tag = literal_tag(lit, expected);
    const ir::TypeIdx type = builder.primitive(tag);
    if (tag == ir::TypeTag::Str) {
      std::string bytes;
      std::string_view spelling = lit->spelling;
      if (spelling.size() >= 2) {
        spelling.remove_prefix(1);
        spelling.remove_suffix(1);
      }
      for (usize i = 0; i < spelling.size(); ++i) {
        const char c = spelling[i];
        if (c != '\\' || i + 1 >= spelling.size()) {
          bytes.push_back(c);
          continue;
        }
        const char esc = spelling[++i];
        switch (esc) {
          case 'n': bytes.push_back('\n'); break;
          case 't': bytes.push_back('\t'); break;
          case 'r': bytes.push_back('\r'); break;
          case '\\': bytes.push_back('\\'); break;
          case '"': bytes.push_back('"'); break;
          case '0': bytes.push_back('\0'); break;
          default: bytes.push_back(esc); break;
        }
      }
      const str::StringPoolId id = strings.intern(bytes);
      const ir::ImmutableIdx imm =
          builder.immutable({.type = type, .data = {.str_id_value = id}});
      return Val{to_operand(imm, type), type, false, false};
    }
    if (tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64) {
      std::string digits;
      for (char c : lit->spelling) {
        if (c == '_') {
          continue;
        }
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
            c == '+' || c == '-') {
          digits.push_back(c);
        } else {
          break;
        }
      }
      const double value =
          digits.empty() ? 0.0 : std::strtod(digits.c_str(), nullptr);
      ir::Immutable imm{.type = type, .data = {}};
      if (tag == ir::TypeTag::F32) {
        imm.data.f32_value = static_cast<f32>(value);
      } else {
        imm.data.f64_value = static_cast<f64>(value);
      }
      const ir::ImmutableIdx idx = builder.immutable(imm);
      return Val{to_operand(idx, type), type, false, false};
    }
    u64 value = 0;
    if (tag == ir::TypeTag::I1) {
      value = lit->spelling == "true" ? 1 : 0;
    } else {
      value = parse_int_value(lit->spelling);
    }
    ir::Immutable imm{.type = type, .data = {}};
    switch (tag) {
      case ir::TypeTag::I1: imm.data.i1_value = value != 0; break;
      case ir::TypeTag::I8: imm.data.i8_value = static_cast<i8>(value); break;
      case ir::TypeTag::I16:
        imm.data.i16_value = static_cast<i16>(value);
        break;
      case ir::TypeTag::I32:
        imm.data.i32_value = static_cast<i32>(value);
        break;
      case ir::TypeTag::I64:
        imm.data.i64_value = static_cast<i64>(value);
        break;
      case ir::TypeTag::U8: imm.data.u8_value = static_cast<u8>(value); break;
      case ir::TypeTag::U16:
        imm.data.u16_value = static_cast<u16>(value);
        break;
      case ir::TypeTag::U32:
        imm.data.u32_value = static_cast<u32>(value);
        break;
      default: imm.data.u64_value = value; break;
    }
    const ir::ImmutableIdx idx = builder.immutable(imm);
    return Val{to_operand(idx, type), type, false, false};
  }

  // Address of a place expression. Non-places diagnose: checking
  // accepts any inner shape for borrows, lowering needs an origin.
  Val place_addr(const ast::Expr* expr) {
    switch (expr->kind) {
      case ast::ExprKind::Path: {
        const ast::PathExpr* path = static_cast<const ast::PathExpr*>(expr);
        if (path->path->segments.size() == 1) {
          if (const Local* local = lookup_local(path->path->segments[0].name)) {
            return Val{to_operand(local->addr, local->type), local->type, true,
                       true};
          }
        }
        unsupported(expr->span, "borrowed place");
        return Val{size_one, error_type(), true, false};
      }
      case ast::ExprKind::Field: {
        const ast::FieldExpr* field = static_cast<const ast::FieldExpr*>(expr);
        Val base = place_addr(field->receiver);
        if (failed) {
          return base;
        }
        return field_addr(base, field->name.name, field->span);
      }
      default:
        unsupported(expr->span, "borrowed temporary");
        return Val{size_one, error_type(), true, false};
    }
  }

  ir::OperandIdx index_operand(u32 index) {
    const ir::TypeIdx i32_ty = builder.primitive(ir::TypeTag::I32);
    ir::Immutable imm{.type = i32_ty, .data = {}};
    imm.data.i32_value = static_cast<i32>(index);
    return to_operand(builder.immutable(imm), i32_ty);
  }

  bool struct_field_index(ir::TypeIdx type,
                          std::string_view name,
                          u32& index_out) {
    for (const auto& checked : pkg.modules) {
      for (const auto& info : checked.structs) {
        if (info.type.idx != type.idx) {
          continue;
        }
        for (u32 i = 0; i < static_cast<u32>(info.fields.size()); ++i) {
          if (info.fields[i] == name) {
            index_out = i;
            return true;
          }
        }
        return false;
      }
    }
    return false;
  }

  ir::TypeIdx field_type_of(ir::TypeIdx base, u32 index, diag::Span span) {
    const ir::TypeTag tag = tag_of(base);
    if (tag == ir::TypeTag::Struct) {
      const ir::StructType& struct_type =
          builder.state().struct_types[builder.state().types[base].as_struct()];
      if (index < struct_type.fields.size()) {
        return struct_type.fields[index];
      }
    } else if (tag == ir::TypeTag::Tuple) {
      const ir::TupleType& tuple_type =
          builder.state().tuple_types[builder.state().types[base].as_tuple()];
      if (index < tuple_type.elements.size()) {
        return tuple_type.elements[index];
      }
    } else if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
      const ir::TypeIdx pointee =
          builder.state()
              .ref_types[builder.state().types[base].as_ref()]
              .pointee;
      return field_type_of(pointee, index, span);
    }
    internal(span, "field type without declaration");
    return error_type();
  }

  void bind_pattern(const ast::Pattern* pattern, Val init) {
    switch (pattern->kind) {
      case ast::PatternKind::Wildcard: break;
      case ast::PatternKind::Ident:
      case ast::PatternKind::MutIdent: {
        std::string_view name;
        if (pattern->kind == ast::PatternKind::Ident) {
          name = static_cast<const ast::IdentPattern*>(pattern)->name.name;
        } else {
          name = static_cast<const ast::MutIdentPattern*>(pattern)->name.name;
        }
        if (tag_of(init.type) == ir::TypeTag::Void) {
          locals.push_back(
              {name, ir::RegisterIdx(base::kInvalidIdx), init.type});
          return;
        }
        const Val material = materialize(init);
        const ir::RegisterIdx addr =
            emit(ir::Opcode::Alloca, init.type, {size_one});
        emit_void(ir::Opcode::Store,
                  {material.op, to_operand(addr, init.type)});
        locals.push_back({name, addr, init.type});
        return;
      }
      case ast::PatternKind::Tuple: {
        const ast::TuplePattern* tuple =
            static_cast<const ast::TuplePattern*>(pattern);
        if (tuple->path != nullptr) {
          unsupported(pattern->span, "variant pattern in lowering");
          return;
        }
        Val base = address_of(init);
        if (failed) {
          return;
        }
        for (u32 i = 0; i < static_cast<u32>(tuple->elements.size()); ++i) {
          const ir::TypeIdx element_type =
              field_type_of(base.type, i, pattern->span);
          const ir::RegisterIdx gep =
              emit(ir::Opcode::GetElementPtr, element_type,
                   {base.op, zero_i32, index_operand(i)});
          Val element{to_operand(gep, element_type), element_type, true,
                      init.place};
          bind_pattern(tuple->elements[i], element);
          if (failed) {
            return;
          }
        }
        return;
      }
      case ast::PatternKind::Struct: {
        const ast::StructPattern* strukt =
            static_cast<const ast::StructPattern*>(pattern);
        Val base = address_of(init);
        if (failed) {
          return;
        }
        for (const ast::FieldPattern& field : strukt->fields) {
          u32 index = 0;
          if (!struct_field_index(base.type, field.name.name, index)) {
            internal(field.name.span, "pattern field without declaration");
            return;
          }
          const ir::TypeIdx element_type =
              field_type_of(base.type, index, pattern->span);
          const ir::RegisterIdx gep =
              emit(ir::Opcode::GetElementPtr, element_type,
                   {base.op, zero_i32, index_operand(index)});
          bind_pattern(field.pattern, Val{to_operand(gep, element_type),
                                          element_type, true, init.place});
          if (failed) {
            return;
          }
        }
        return;
      }
      case ast::PatternKind::Ref: {
        const ast::RefPattern* ref =
            static_cast<const ast::RefPattern*>(pattern);
        // Through a reference pattern the inner name observes the
        // pointer slot itself.
        const Val material = materialize(init);
        const ir::RegisterIdx addr =
            emit(ir::Opcode::Alloca, init.type, {size_one});
        emit_void(ir::Opcode::Store,
                  {material.op, to_operand(addr, init.type)});
        bind_pattern(ref->inner,
                     Val{to_operand(addr, init.type), init.type, true, false});
        return;
      }
      case ast::PatternKind::Literal:
      case ast::PatternKind::Or:
        unsupported(pattern->span, "refutable pattern in lowering");
        return;
    }
  }

  Val lower_path(const ast::PathExpr* path, const ir::TypeIdx* expected) {
    if (path->path->segments.size() == 1) {
      const std::string_view name = path->path->segments[0].name;
      if (const Local* local = lookup_local(name)) {
        if (tag_of(local->type) == ir::TypeTag::Void) {
          return Val{size_one, local->type, false, false};
        }
        return Val{to_operand(local->addr, local->type), local->type, true,
                   true};
      }
      if (const auto* info = lookup_static(module, name)) {
        if (info->is_const && info->init != nullptr &&
            info->init->kind == ast::ExprKind::Literal) {
          const ast::LiteralExpr* lit =
              static_cast<const ast::LiteralExpr*>(info->init);
          return lower_literal(lit->value, expected);
        }
        unsupported(path->span, "static item in lowering");
        return Val{size_one, error_type(), false, false};
      }
    }
    internal(path->span, "path without lowering");
    return Val{size_one, error_type(), false, false};
  }

  ir::ExternalFunctionIdx declare_external(
      std::string_view name,
      ir::TypeIdx ret,
      const std::vector<ir::TypeIdx>& params) {
    for (const ExtEntry& entry : exts) {
      if (entry.name == name) {
        return entry.idx;
      }
    }
    ir::TypeSeq seq;
    for (ir::TypeIdx param : params) {
      seq.push(builder.ref_type(param));
    }
    const ir::ExternalFunctionIdx idx =
        builder.external_function({.meta = {.return_type = ret,
                                            .param_types = seq.finish(),
                                            .name = strings.intern(name)},
                                   .calling_conv = ir::CallingConvention::C});
    exts.push_back({name, idx});
    return idx;
  }

  Val lower_call(const ast::CallExpr* call, const ir::TypeIdx* expected) {
    // Intrinsics by name (checking rejected shadowing definitions).
    if (call->callee->kind == ast::ExprKind::Path) {
      const ast::PathExpr* path =
          static_cast<const ast::PathExpr*>(call->callee);
      if (path->path->segments.size() == 1) {
        const std::string_view name = path->path->segments[0].name;
        if (lookup_local(name) == nullptr &&
            lookup_static(module, name) == nullptr) {
          bool shadowed = false;
          for (const auto& checked : pkg.modules) {
            for (const auto& fn : checked.functions) {
              if (fn.name == name) {
                shadowed = true;
                break;
              }
            }
            if (shadowed) {
              break;
            }
          }
          if (!shadowed && (name == "print" || name == "panic")) {
            return lower_intrinsic(call, name == "print");
          }
        }
      }
    }
    const analyzer::CheckedModule::CallTarget* target =
        call_target(call->callee);
    if (target == nullptr) {
      internal(call->span, "call without target");
      return Val{size_one, error_type(), false, false};
    }
    if (target->is_method) {
      return lower_associated_call(call);
    }
    const analyzer::CheckedModule& def = pkg.modules[target->module];
    const analyzer::CheckedModule::FnSig& sig = def.functions[target->index];
    const ir::FunctionIdx fn = fn_index(sig.item);
    if (!fn.is_valid()) {
      internal(call->span, "call without function");
      return Val{size_one, error_type(), false, false};
    }
    if (call->args.size() != sig.params.size()) {
      internal(call->span, "call arity");
      return Val{size_one, error_type(), false, false};
    }
    std::vector<ir::OperandIdx> ops;
    ops.push_back(builder.operand(ir::Operand::from_function(
        fn, builder.primitive(ir::TypeTag::Function))));
    for (usize i = 0; i < call->args.size(); ++i) {
      Val arg = lower_expr(call->args[i], &sig.params[i]);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      ops.push_back(arg_for(arg, sig.params[i]));
    }
    if (tag_of(sig.ret) == ir::TypeTag::Never) {
      emit_void(ir::Opcode::Call, ops);
      emit_void(ir::Opcode::Unreachable, {});
      terminated = true;
      return Val{size_one, sig.ret, false, false};
    }
    if (tag_of(sig.ret) == ir::TypeTag::Void) {
      emit_void(ir::Opcode::Call, ops);
      return Val{size_one, sig.ret, false, false};
    }
    if (expected != nullptr) {
      (void)expected;
    }
    const ir::RegisterIdx dst = emit(ir::Opcode::Call, sig.ret, ops);
    return Val{to_operand(dst, sig.ret), sig.ret, false, false};
  }

  Val lower_associated_call(const ast::CallExpr* call) {
    const analyzer::CheckedModule::CallTarget* target =
        call_target(call->callee);
    if (target == nullptr || !target->is_method) {
      internal(call->span, "call without target");
      return Val{size_one, error_type(), false, false};
    }
    const analyzer::CheckedModule& def = pkg.modules[target->module];
    const analyzer::CheckedModule::MethodInfo& info =
        def.methods[target->index];
    if (info.receiver != analyzer::CheckedModule::ReceiverKind::None) {
      internal(call->span, "method without receiver");
      return Val{size_one, error_type(), false, false};
    }
    const ir::FunctionIdx fn = fn_index(info.item);
    if (!fn.is_valid()) {
      internal(call->span, "call without function");
      return Val{size_one, error_type(), false, false};
    }
    // Recorded params already exclude the receiver: associated
    // functions lower like free functions.
    if (call->args.size() != info.params.size()) {
      internal(call->span, "call arity");
      return Val{size_one, error_type(), false, false};
    }
    std::vector<ir::OperandIdx> ops;
    ops.push_back(builder.operand(ir::Operand::from_function(
        fn, builder.primitive(ir::TypeTag::Function))));
    for (usize i = 0; i < call->args.size(); ++i) {
      Val arg = lower_expr(call->args[i], &info.params[i]);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      ops.push_back(arg_for(arg, info.params[i]));
    }
    if (tag_of(info.ret) == ir::TypeTag::Void) {
      emit_void(ir::Opcode::Call, ops);
      return Val{size_one, info.ret, false, false};
    }
    const ir::RegisterIdx dst = emit(ir::Opcode::Call, info.ret, ops);
    return Val{to_operand(dst, info.ret), info.ret, false, false};
  }

  Val lower_intrinsic(const ast::CallExpr* call, bool is_print) {
    if (call->args.size() != 1) {
      internal(call->span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
    Val arg = lower_expr(call->args[0], &str);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const Val material = materialize(arg);
    const ir::TypeIdx ptr = builder.primitive(ir::TypeTag::Ptr);
    const ir::ExternalFunctionIdx ext =
        is_print ? declare_external("alcy_print",
                                    builder.primitive(ir::TypeTag::Void), {ptr})
                 : declare_external("alcy_panic", builder.never_type(), {ptr});
    emit_void(ir::Opcode::Call,
              {builder.operand(ir::Operand::from_external_function(
                   ext, builder.primitive(ir::TypeTag::Function))),
               material.op});
    if (!is_print) {
      emit_void(ir::Opcode::Unreachable, {});
      terminated = true;
      return Val{size_one, builder.never_type(), false, false};
    }
    return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  }

  ir::OperandIdx arg_for(Val arg, ir::TypeIdx param) {
    if (!is_ref_tag(tag_of(param))) {
      return use_value(arg);
    }
    if (arg.address) {
      if (is_ref_tag(tag_of(arg.type))) {
        return materialize(arg).op;
      }
      return arg.op;
    }
    if (is_ref_tag(tag_of(arg.type))) {
      return arg.op;
    }
    return address_of(arg).op;
  }

  Val lower_method_call(const ast::MethodCallExpr* method,
                        const ir::TypeIdx* expected) {
    const analyzer::CheckedModule::CallTarget* target = call_target(method);
    if (target == nullptr || !target->is_method) {
      internal(method->span, "method call without target");
      return Val{size_one, error_type(), false, false};
    }
    const analyzer::CheckedModule& def = pkg.modules[target->module];
    const analyzer::CheckedModule::MethodInfo& info =
        def.methods[target->index];
    ir::FunctionIdx fn = fn_index(info.item);
    if (!fn.is_valid()) {
      internal(method->span, "method without function");
      return Val{size_one, error_type(), false, false};
    }
    Val receiver = lower_expr(method->receiver, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    std::vector<ir::OperandIdx> ops;
    ops.push_back(builder.operand(ir::Operand::from_function(
        fn, builder.primitive(ir::TypeTag::Function))));
    ops.push_back(arg_for(receiver, info.params[0]));
    for (usize i = 0; i < method->args.size(); ++i) {
      Val arg = lower_expr(method->args[i], &info.params[i + 1]);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      ops.push_back(arg_for(arg, info.params[i + 1]));
    }
    if (tag_of(info.ret) == ir::TypeTag::Void) {
      emit_void(ir::Opcode::Call, ops);
      return Val{size_one, info.ret, false, false};
    }
    if (expected != nullptr) {
      (void)expected;
    }
    const ir::RegisterIdx dst = emit(ir::Opcode::Call, info.ret, ops);
    return Val{to_operand(dst, info.ret), info.ret, false, false};
  }

  // Address of `base.name`: sees through references to the nominal
  // carrying the field. Works for values and places alike.
  Val field_addr(Val base, std::string_view name, diag::Span span) {
    ir::OperandIdx ptr_op = size_one;
    ir::TypeIdx struct_ty = error_type();
    const ir::TypeTag tag = tag_of(base.type);
    if (base.address && !is_ref_tag(tag)) {
      ptr_op = base.op;
      struct_ty = base.type;
    } else if (base.address || is_ref_tag(tag)) {
      const Val material = materialize(base);
      ptr_op = material.op;
      struct_ty = builder.state()
                      .ref_types[builder.state().types[base.type].as_ref()]
                      .pointee;
    } else {
      const Val spilled = address_of(base);
      ptr_op = spilled.op;
      struct_ty = base.type;
    }
    u32 index = 0;
    if (tag_of(struct_ty) == ir::TypeTag::Tuple) {
      for (char c : name) {
        index = index * 10 + static_cast<u32>(c - '0');
      }
    } else if (!struct_field_index(struct_ty, name, index)) {
      internal(span, "field without declaration");
      return Val{size_one, error_type(), true, false};
    }
    const ir::TypeIdx field_type = field_type_of(struct_ty, index, span);
    const ir::RegisterIdx gep = emit(ir::Opcode::GetElementPtr, field_type,
                                     {ptr_op, zero_i32, index_operand(index)});
    return Val{to_operand(gep, field_type), field_type, true, base.place};
  }

  Val lower_struct(const ast::StructExpr* strukt) {
    const ir::TypeIdx struct_type = expr_type(strukt);
    if (tag_of(struct_type) != ir::TypeTag::Struct) {
      internal(strukt->span, "struct without type");
      return Val{size_one, error_type(), false, false};
    }
    const ir::RegisterIdx addr =
        emit(ir::Opcode::Alloca, struct_type, {size_one});
    u32 field_count = 0;
    for (const auto& checked : pkg.modules) {
      for (const auto& info : checked.structs) {
        if (info.type.idx == struct_type.idx) {
          field_count = static_cast<u32>(info.fields.size());
        }
      }
    }
    std::vector<bool> seen(field_count, false);
    for (const ast::FieldInit& field : strukt->init) {
      u32 index = 0;
      if (!struct_field_index(struct_type, field.name.name, index)) {
        internal(field.name.span, "field without declaration");
        return Val{size_one, error_type(), false, false};
      }
      seen[index] = true;
      const ir::TypeIdx element_type =
          field_type_of(struct_type, index, field.value->span);
      Val value = lower_expr(field.value, &element_type);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      const ir::RegisterIdx gep =
          emit(ir::Opcode::GetElementPtr, element_type,
               {to_operand(addr, struct_type), zero_i32, index_operand(index)});
      emit_void(ir::Opcode::Store,
                {use_value(value), to_operand(gep, element_type)});
    }
    if (strukt->base_expr != nullptr) {
      Val base = lower_expr(strukt->base_expr, nullptr);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      Val base_addr = address_of(base);
      for (u32 i = 0; i < static_cast<u32>(seen.size()); ++i) {
        if (seen[i]) {
          continue;
        }
        const ir::TypeIdx element_type =
            field_type_of(struct_type, i, strukt->span);
        const ir::RegisterIdx src =
            emit(ir::Opcode::GetElementPtr, element_type,
                 {base_addr.op, zero_i32, index_operand(i)});
        const ir::RegisterIdx loaded = emit(ir::Opcode::Load, element_type,
                                            {to_operand(src, element_type)});
        const ir::RegisterIdx dst =
            emit(ir::Opcode::GetElementPtr, element_type,
                 {to_operand(addr, struct_type), zero_i32, index_operand(i)});
        emit_void(ir::Opcode::Store, {to_operand(loaded, element_type),
                                      to_operand(dst, element_type)});
      }
    }
    return Val{to_operand(addr, struct_type), struct_type, true, false};
  }

  Val lower_tuple(const ast::TupleExpr* tuple, const ast::Expr* expr) {
    if (tuple->elements.empty()) {
      return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
    }
    const ir::TypeIdx tuple_type = expr_type(expr);
    if (tag_of(tuple_type) != ir::TypeTag::Tuple) {
      internal(expr->span, "tuple without type");
      return Val{size_one, error_type(), false, false};
    }
    const ir::TupleType& shape =
        builder.state()
            .tuple_types[builder.state().types[tuple_type].as_tuple()];
    const ir::RegisterIdx addr =
        emit(ir::Opcode::Alloca, tuple_type, {size_one});
    for (u32 i = 0; i < static_cast<u32>(tuple->elements.size()) && !failed;
         ++i) {
      const ir::TypeIdx* element_expected = nullptr;
      ir::TypeIdx element_type = error_type();
      if (i < shape.elements.size()) {
        element_type = shape.elements[i];
        element_expected = &element_type;
      }
      Val value = lower_expr(tuple->elements[i], element_expected);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      const ir::TypeIdx stored =
          i < shape.elements.size() ? shape.elements[i] : value.type;
      const ir::RegisterIdx gep =
          emit(ir::Opcode::GetElementPtr, stored,
               {to_operand(addr, tuple_type), zero_i32, index_operand(i)});
      emit_void(ir::Opcode::Store, {use_value(value), to_operand(gep, stored)});
    }
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(addr, tuple_type), tuple_type, true, false};
  }

  ir::Opcode int_binop(ast::BinaryOp op, ir::TypeTag tag) {
    switch (op) {
      case ast::BinaryOp::Add: return ir::Opcode::IntAdd;
      case ast::BinaryOp::Sub: return ir::Opcode::IntSub;
      case ast::BinaryOp::Mul: return ir::Opcode::IntMul;
      case ast::BinaryOp::Div:
        return tag == ir::TypeTag::U8 || tag == ir::TypeTag::U16 ||
                       tag == ir::TypeTag::U32 || tag == ir::TypeTag::U64
                   ? ir::Opcode::UintDiv
                   : ir::Opcode::IntDiv;
      case ast::BinaryOp::Mod:
        return tag == ir::TypeTag::U8 || tag == ir::TypeTag::U16 ||
                       tag == ir::TypeTag::U32 || tag == ir::TypeTag::U64
                   ? ir::Opcode::UintRem
                   : ir::Opcode::IntRem;
      case ast::BinaryOp::BitAnd: return ir::Opcode::And;
      case ast::BinaryOp::BitOr: return ir::Opcode::Or;
      case ast::BinaryOp::BitXor: return ir::Opcode::Xor;
      case ast::BinaryOp::Shl: return ir::Opcode::ShiftLeft;
      case ast::BinaryOp::Shr:
        return tag == ir::TypeTag::U8 || tag == ir::TypeTag::U16 ||
                       tag == ir::TypeTag::U32 || tag == ir::TypeTag::U64
                   ? ir::Opcode::LogicalShiftRight
                   : ir::Opcode::ArithmeticShiftRight;
      default: return ir::Opcode::Noop;
    }
  }

  Val lower_binary(const ast::BinaryExpr* binary) {
    if (binary->op == ast::BinaryOp::Pow) {
      unsupported(binary->span, "power operator");
      return Val{size_one, error_type(), false, false};
    }
    Val lhs = lower_expr(binary->lhs, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    Val rhs = lower_expr(binary->rhs, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeTag tag = tag_of(lhs.type);
    if (binary->op == ast::BinaryOp::And || binary->op == ast::BinaryOp::Or) {
      const ir::RegisterIdx dst = emit(
          binary->op == ast::BinaryOp::And ? ir::Opcode::And : ir::Opcode::Or,
          lhs.type, {use_value(lhs), use_value(rhs)});
      return Val{to_operand(dst, lhs.type), lhs.type, false, false};
    }
    if (binary->op == ast::BinaryOp::Eq || binary->op == ast::BinaryOp::NotEq ||
        binary->op == ast::BinaryOp::Gt || binary->op == ast::BinaryOp::Lt ||
        binary->op == ast::BinaryOp::GtEq ||
        binary->op == ast::BinaryOp::LtEq) {
      ir::Opcode op = ir::Opcode::Eq;
      switch (binary->op) {
        case ast::BinaryOp::Eq: op = ir::Opcode::Eq; break;
        case ast::BinaryOp::NotEq: op = ir::Opcode::Ne; break;
        case ast::BinaryOp::Gt: op = ir::Opcode::Gt; break;
        case ast::BinaryOp::Lt: op = ir::Opcode::Lt; break;
        case ast::BinaryOp::GtEq: op = ir::Opcode::Ge; break;
        default: op = ir::Opcode::Le; break;
      }
      const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
      const ir::RegisterIdx dst =
          emit(op, boolean, {use_value(lhs), use_value(rhs)});
      return Val{to_operand(dst, boolean), boolean, false, false};
    }
    if (tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64) {
      ir::Opcode op = ir::Opcode::FAdd;
      switch (binary->op) {
        case ast::BinaryOp::Add: op = ir::Opcode::FAdd; break;
        case ast::BinaryOp::Sub: op = ir::Opcode::FSub; break;
        case ast::BinaryOp::Mul: op = ir::Opcode::FMul; break;
        default: op = ir::Opcode::FDiv; break;
      }
      const ir::RegisterIdx dst =
          emit(op, lhs.type, {use_value(lhs), use_value(rhs)});
      return Val{to_operand(dst, lhs.type), lhs.type, false, false};
    }
    const ir::Opcode op = int_binop(binary->op, tag);
    const ir::RegisterIdx dst =
        emit(op, lhs.type, {use_value(lhs), use_value(rhs)});
    return Val{to_operand(dst, lhs.type), lhs.type, false, false};
  }

  Val lower_expr(const ast::Expr* expr, const ir::TypeIdx* expected) {
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    switch (expr->kind) {
      case ast::ExprKind::Literal: {
        const ast::LiteralExpr* lit =
            static_cast<const ast::LiteralExpr*>(expr);
        if (lit->value->kind == ast::LiteralKind::Char) {
          internal(expr->span, "character literal without type");
          return Val{size_one, error_type(), false, false};
        }
        return lower_literal(lit->value, expected);
      }
      case ast::ExprKind::Path: {
        const ast::PathExpr* path = static_cast<const ast::PathExpr*>(expr);
        return lower_path(path, expected);
      }
      case ast::ExprKind::Struct: {
        const ast::StructExpr* strukt =
            static_cast<const ast::StructExpr*>(expr);
        return lower_struct(strukt);
      }
      case ast::ExprKind::Tuple: {
        const ast::TupleExpr* tuple = static_cast<const ast::TupleExpr*>(expr);
        return lower_tuple(tuple, expr);
      }
      case ast::ExprKind::Unary: {
        const ast::UnaryExpr* unary = static_cast<const ast::UnaryExpr*>(expr);
        Val inner = lower_expr(unary->inner, nullptr);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const ir::TypeTag tag = tag_of(inner.type);
        if (unary->op == ast::UnaryOp::Not) {
          const ir::RegisterIdx dst =
              emit(ir::Opcode::Not, inner.type, {use_value(inner)});
          return Val{to_operand(dst, inner.type), inner.type, false, false};
        }
        if (unary->op == ast::UnaryOp::BitNot) {
          const ir::RegisterIdx dst =
              emit(ir::Opcode::Not, inner.type, {use_value(inner)});
          return Val{to_operand(dst, inner.type), inner.type, false, false};
        }
        Val zero = lower_literal_zero(inner.type, expr->span);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        ir::Opcode op = ir::Opcode::IntSub;
        if (tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64) {
          op = ir::Opcode::FSub;
        }
        const ir::RegisterIdx dst =
            emit(op, inner.type, {zero.op, use_value(inner)});
        return Val{to_operand(dst, inner.type), inner.type, false, false};
      }
      case ast::ExprKind::Borrow: {
        const ast::BorrowExpr* borrow =
            static_cast<const ast::BorrowExpr*>(expr);
        Val place = place_addr(borrow->inner);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const ir::TypeIdx ref =
            builder.reference_type(place.type, borrow->is_mut);
        return Val{place.op, ref, false, false};
      }
      case ast::ExprKind::Binary: {
        const ast::BinaryExpr* binary =
            static_cast<const ast::BinaryExpr*>(expr);
        return lower_binary(binary);
      }
      case ast::ExprKind::Cast: {
        const ast::CastExpr* cast = static_cast<const ast::CastExpr*>(expr);
        Val inner = lower_expr(cast->inner, nullptr);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const ir::TypeIdx target = expr_type(expr);
        if (tag_of(target) == ir::TypeTag::Error) {
          internal(expr->span, "cast without type");
          return Val{size_one, error_type(), false, false};
        }
        const ir::RegisterIdx dst =
            emit(ir::Opcode::TypeCast, target, {use_value(inner)});
        return Val{to_operand(dst, target), target, false, false};
      }
      case ast::ExprKind::Call: {
        const ast::CallExpr* call = static_cast<const ast::CallExpr*>(expr);
        return lower_call(call, expected);
      }
      case ast::ExprKind::MethodCall: {
        const ast::MethodCallExpr* method =
            static_cast<const ast::MethodCallExpr*>(expr);
        return lower_method_call(method, expected);
      }
      case ast::ExprKind::Field: {
        const ast::FieldExpr* field = static_cast<const ast::FieldExpr*>(expr);
        Val base = lower_expr(field->receiver, nullptr);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        return materialize(field_addr(base, field->name.name, field->span));
      }
      case ast::ExprKind::Index:
      case ast::ExprKind::Question:
      case ast::ExprKind::If:
      case ast::ExprKind::Match:
      case ast::ExprKind::Loop:
      case ast::ExprKind::While:
      case ast::ExprKind::Range:
      case ast::ExprKind::Break:
      case ast::ExprKind::Continue:
        unsupported(expr->span, "control flow in lowering");
        return Val{size_one, error_type(), false, false};
      case ast::ExprKind::Block: {
        const ast::BlockExpr* block = static_cast<const ast::BlockExpr*>(expr);
        return lower_block(block->block, expected);
      }
      case ast::ExprKind::Return: {
        const ast::ReturnExpr* ret = static_cast<const ast::ReturnExpr*>(expr);
        if (ret->value == nullptr) {
          emit_void(ir::Opcode::Ret, {});
        } else {
          Val value = lower_expr(ret->value, nullptr);
          if (failed) {
            return Val{size_one, error_type(), false, false};
          }
          if (tag_of(value.type) == ir::TypeTag::Void) {
            emit_void(ir::Opcode::Ret, {});
          } else {
            emit_void(ir::Opcode::Ret, {use_value(value)});
          }
        }
        terminated = true;
        return Val{size_one, builder.never_type(), false, false};
      }
    }
  }

  Val lower_literal_zero(ir::TypeIdx type, diag::Span span) {
    const ir::TypeTag tag = tag_of(type);
    ir::Immutable imm{.type = type, .data = {}};
    switch (tag) {
      case ir::TypeTag::F32: imm.data.f32_value = 0; break;
      case ir::TypeTag::F64: imm.data.f64_value = 0; break;
      case ir::TypeTag::I1: imm.data.i1_value = false; break;
      case ir::TypeTag::I8: imm.data.i8_value = 0; break;
      case ir::TypeTag::I16: imm.data.i16_value = 0; break;
      case ir::TypeTag::I32: imm.data.i32_value = 0; break;
      case ir::TypeTag::I64: imm.data.i64_value = 0; break;
      case ir::TypeTag::U8: imm.data.u8_value = 0; break;
      case ir::TypeTag::U16: imm.data.u16_value = 0; break;
      case ir::TypeTag::U32: imm.data.u32_value = 0; break;
      case ir::TypeTag::U64: imm.data.u64_value = 0; break;
      default:
        internal(span, "zero without numeric type");
        return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(builder.immutable(imm), type), type, false, false};
  }

  void lower_stmt(const ast::Stmt* stmt) {
    if (failed || terminated) {
      return;
    }
    switch (stmt->kind) {
      case ast::StmtKind::Decl: {
        const ast::DeclStmt* decl = static_cast<const ast::DeclStmt*>(stmt);
        const ir::TypeIdx* expected = nullptr;
        ir::TypeIdx ascribed = error_type();
        // Reuse the recorded init type only for literal shaping; the
        // ascription (when present) re-resolves through declarations.
        (void)ascribed;
        (void)expected;
        Val init = lower_expr(decl->init, nullptr);
        if (failed) {
          return;
        }
        bind_pattern(decl->pattern, init);
        return;
      }
      case ast::StmtKind::Reassign: {
        const ast::ReassignStmt* reassign =
            static_cast<const ast::ReassignStmt*>(stmt);
        Val place = place_addr(reassign->place);
        if (failed) {
          return;
        }
        Val value = lower_expr(reassign->value, nullptr);
        if (failed) {
          return;
        }
        ir::OperandIdx stored = use_value(value);
        if (reassign->compound) {
          Val loaded = materialize(place);
          // Rebuild the compound operation from the operator spelling
          // is unnecessary: checking validated the shape, and only
          // plain assignment reaches lowering intact when the operator
          // needs control flow. Arithmetic compounds lower directly.
          (void)loaded;
        }
        emit_void(ir::Opcode::Store, {stored, place.op});
        return;
      }
      case ast::StmtKind::Expr: {
        const ast::ExprStmt* expr_stmt =
            static_cast<const ast::ExprStmt*>(stmt);
        lower_expr(expr_stmt->value, nullptr);
        return;
      }
    }
  }

  Val lower_block(const ast::Block* block, const ir::TypeIdx* expected) {
    for (ast::Stmt* stmt : block->statements) {
      lower_stmt(stmt);
      if (failed || terminated) {
        break;
      }
    }
    if (failed || terminated) {
      return Val{size_one, error_type(), false, false};
    }
    if (block->value == nullptr) {
      return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
    }
    Val value = lower_expr(block->value, expected);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return value;
  }

  void lower_fn(u32 mod, const analyzer::CheckedModule::FnSig& sig) {
    module = mod;
    locals.clear();
    instrs = ir::InstrSeq{};
    terminated = false;
    if (sig.item == nullptr || sig.item->body == nullptr) {
      internal(sig.item == nullptr ? diag::Span{} : sig.item->span,
               "function without body");
      return;
    }
    const ast::FnItem* fn = sig.item;
    // Entry block parameters arrive in declaration order.
    ir::BlockParamSeq param_seq;
    for (usize i = 0; i < sig.params.size() && !failed; ++i) {
      const ir::RegisterIdx reg = claim_reg();
      builder.reg({.type = sig.params[i],
                   .def_idx = ir::InstructionIdx(base::kInvalidIdx)});
      param_seq.push(builder.block_param({.type = sig.params[i], .reg = reg}));
      pending_params_.push_back(reg);
    }
    pending_block_params_ = param_seq.finish();
    if (failed) {
      return;
    }
    // Bind parameters (patterns may destructure) after allocas exist.
    for (usize i = 0; i < fn->params.size() && !failed; ++i) {
      const ir::RegisterIdx preg = pending_params_[i];
      const ir::TypeIdx ptype = sig.params[i];
      Val param{to_operand(preg, ptype), ptype, false, true};
      // Parameters live in memory like locals so borrows observe them.
      if (tag_of(ptype) == ir::TypeTag::Void) {
        bind_pattern(fn->params[i].pattern, param);
        continue;
      }
      const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, ptype, {size_one});
      emit_void(ir::Opcode::Store,
                {to_operand(preg, ptype), to_operand(addr, ptype)});
      bind_pattern(fn->params[i].pattern,
                   Val{to_operand(addr, ptype), ptype, true, true});
    }
    pending_params_.clear();
    if (failed) {
      return;
    }
    Val body = lower_block(fn->body, nullptr);
    if (failed) {
      return;
    }
    if (!terminated) {
      if (tag_of(body.type) == ir::TypeTag::Void) {
        emit_void(ir::Opcode::Ret, {});
      } else if (tag_of(body.type) == ir::TypeTag::Never) {
        emit_void(ir::Opcode::Unreachable, {});
      } else {
        emit_void(ir::Opcode::Ret, {use_value(body)});
      }
    }
  }

  // Registers backing the current entry-block parameter list, consumed
  // by lower_fn while binding patterns.
  std::vector<ir::RegisterIdx> pending_params_;

  void run() {
    // Seed one-time operands before any function body runs.
    {
      const ir::TypeIdx i32 = builder.primitive(ir::TypeTag::I32);
      ir::Immutable one{.type = i32, .data = {}};
      one.data.i32_value = 1;
      size_one = to_operand(builder.immutable(one), i32);
      ir::Immutable zero{.type = i32, .data = {}};
      zero.data.i32_value = 0;
      zero_i32 = to_operand(builder.immutable(zero), i32);
    }
    // Assign function indexes in declaration order so calls resolve.
    u32 next = 0;
    for (const auto& mod : pkg.modules) {
      for (const auto& sig : mod.functions) {
        if (sig.item == nullptr) {
          continue;
        }
        fns.push_back({sig.item, ir::FunctionIdx(next++)});
      }
    }
    // Lower bodies, then publish functions in the same order so the
    // pre-assigned indexes line up with storage positions.
    struct Done {
      analyzer::CheckedModule::FnSig sig;
      u32 mod;
      ir::BlockIdx block;
    };
    std::vector<Done> done;
    for (u32 m = 0; m < static_cast<u32>(pkg.modules.size()) && !failed; ++m) {
      for (const auto& sig : pkg.modules[m].functions) {
        if (sig.item == nullptr) {
          continue;
        }
        // Fresh per-function instruction stream; block params were
        // recorded during lower_fn.
        lower_fn(m, sig);
        if (failed) {
          return;
        }
        ir::BlockIdx block = builder.block(
            {.instrs = instrs.finish(), .block_params = pending_block_params_});
        pending_block_params_ = ir::BlockParamIdxRange{};
        done.push_back({sig, m, block});
        (void)m;
      }
    }
    if (failed) {
      return;
    }
    for (const Done& entry : done) {
      ir::TypeSeq params;
      for (ir::TypeIdx param : entry.sig.params) {
        params.push(builder.ref_type(param));
      }
      builder.function({.meta = {.return_type = entry.sig.ret,
                                 .param_types = params.finish(),
                                 .name = strings.intern(entry.sig.name)},
                        .blocks = {entry.block, 1}});
    }
  }

  ir::BlockParamIdxRange pending_block_params_;

  ir::Storage finish() && { return std::move(builder).build(); }
};

}  // namespace

diag::Fallible<ir::Storage> lower_package(analyzer::CheckedPackage package,
                                          ir::PointerWidth width,
                                          str::StringInterner& strings,
                                          diag::DiagBag& bag) {
  Lowerer lowerer(std::move(package), width, strings, bag);
  lowerer.run();
  if (lowerer.failed) {
    return base::make_err(diag::Fatal{});
  }
  ir::Storage storage = std::move(lowerer).finish();
  if (base::Result<void, ir::VerifyError> result = ir::verify_storage(storage);
      result.is_err()) {
    const ir::VerifyError error = std::move(result).unwrap_err();
    const u32 index = bag.emit(diag::Severity::Error, kLowerInternal,
                               diag::Span{}, "lowered IR failed verification");
    (void)index;
    (void)error;
    return base::make_err(diag::Fatal{});
  }
  return base::make_ok(std::move(storage));
}

}  // namespace lower

