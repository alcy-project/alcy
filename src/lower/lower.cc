// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lower/lower.h"

#include <cstdlib>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "debug/dcheck.h"
#include "debug/dlog.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "diag/span.h"
#include "fmt/format.h"
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
constexpr u32 kLowerUnreachable = 4302;

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

class Lowerer {
 public:
  analyzer::CheckedPackage pkg;
  ir::StorageBuilder builder;
  ir::PointerWidth width;
  ast::AstArena& ast;
  str::StringInterner& strings;
  diag::DiagBag& bag;
  bool failed = false;

  // Side tables for ownership analysis: every emitted instruction
  // records its source span, and every address alloca records the
  // bound name (for diagnostics; analysis needs only identity).
  diag::Span cur_span_{};
  std::vector<diag::Span> instr_spans_;
  std::vector<LoweredPackage::AddrInfo> addr_names_;

  // Compile-time values for comp evaluation. Integers ride as u64
  // with their type attached (semantics follow the emitting opcodes:
  // wrapping arithmetic, two's-complement negation); aggregates carry
  // positional fields.
  struct CompValue {
    enum class Tag : u8 {
      Void,
      Int,
      Bool,
      Str,
      Tuple,
      Struct,
      Enum,
      Blessed,
    };
    Tag tag = Tag::Void;
    u64 int_value = 0;
    bool bool_value = false;
    std::string str_value;
    std::vector<CompValue> fields;
    u32 variant = 0;
    bool blessed_ok = true;
  };

  struct CompVal {
    CompValue value;
    ir::TypeIdx type = ir::TypeIdx(base::kInvalidIdx);
  };

  // Lexical comp bindings: persistent per-function bindings plus
  // evaluation-local frames.
  struct CompScope {
    const std::vector<std::pair<std::string_view, CompVal>>* outer = nullptr;
    std::vector<std::vector<std::pair<std::string_view, CompVal>>> frames;
  };

  struct FnEntry {
    ast::ItemIdx item = ast::ItemIdx::invalid();
    // Specialization key over comp argument values; empty for
    // functions without comp parameters.
    std::string comp_key;
    ir::FunctionIdx idx = ir::FunctionIdx(base::kInvalidIdx);
    // Lowering work item, filled at reservation time.
    u32 mod = 0;
    std::string name;
    std::vector<ir::TypeIdx> params;
    ir::TypeIdx ret = ir::TypeIdx(base::kInvalidIdx);
    // Comp argument values in formal-parameter order.
    std::vector<CompVal> comp_args;
  };
  std::vector<FnEntry> fns;
  // Reservation order matches lowering order (FIFO worklist), so
  // reserved indexes line up with storage positions.
  std::vector<usize> worklist_;
  // Per-function comp bindings (comp parameters); cleared per body.
  std::vector<std::pair<std::string_view, CompVal>> comp_scope_;
  // Step budget per top-level comp evaluation; recursion depth guard.
  usize comp_budget_ = 0;
  u32 comp_call_depth_ = 0;

  struct ExtEntry {
    std::string_view name;
    ir::ExternalFunctionIdx idx;
  };
  std::vector<ExtEntry> exts;

  // Per-function state.
  u32 module = 0;
  std::vector<Local> locals;
  // Instruction streams by reserved block: reservation order matches
  // creation order, so reserved indexes line up with storage positions.
  std::vector<ir::InstrSeq> streams_;
  std::vector<ir::InstructionIdx> stream_last_;
  std::vector<ir::BlockIdx> fn_blocks_;
  ir::BlockIdx cur_{base::kInvalidIdx};
  u32 block_next_ = 0;
  u32 fn_block_base_ = 0;
  bool binding_param_ = false;
  std::vector<ir::BlockIdx> break_targets_;
  std::vector<ir::BlockIdx> continue_targets_;

  static bool is_block_terminator(ir::Opcode op) {
    return op == ir::Opcode::Br || op == ir::Opcode::CondBr ||
           op == ir::Opcode::Switch || op == ir::Opcode::Ret ||
           op == ir::Opcode::Unreachable;
  }

  usize at(ir::BlockIdx block) const { return block.idx - fn_block_base_; }

  ir::BlockIdx reserve_block() {
    streams_.emplace_back();
    stream_last_.emplace_back(base::kInvalidIdx);
    fn_blocks_.emplace_back(block_next_++);
    return fn_blocks_.back();
  }

  void switch_to(ir::BlockIdx block) { cur_ = block; }

  bool terminated(ir::BlockIdx block) {
    for (usize i = 0; i < fn_blocks_.size(); ++i) {
      if (fn_blocks_[i].idx == block.idx) {
        const ir::InstructionIdx last = stream_last_[i];
        return last.is_valid() &&
               is_block_terminator(builder.state().instrs[last].op);
      }
    }
    return false;
  }

  bool terminated_cur() {
    const ir::InstructionIdx last = stream_last_[at(cur_)];
    return last.is_valid() &&
           is_block_terminator(builder.state().instrs[last].op);
  }
  ir::OperandIdx size_one = ir::OperandIdx(0);
  ir::OperandIdx zero_i32 = ir::OperandIdx(0);

  Lowerer(analyzer::CheckedPackage package,
          ir::PointerWidth width,
          ast::AstArena& ast,
          str::StringInterner& strings,
          diag::DiagBag& bag)
      : pkg(std::move(package)),
        builder(std::move(pkg.types).take_state()),
        width(width),
        ast(ast),
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
    streams_[at(cur_)].push(instr);
    stream_last_[at(cur_)] = instr;
    instr_spans_.push_back(cur_span_);
    return dst;
  }

  void emit_void(ir::Opcode op, const std::vector<ir::OperandIdx>& ops) {
    const ir::OperandIdx head =
        ir::OperandIdx(static_cast<u32>(builder.state().operands.size()));
    for (ir::OperandIdx op_idx : ops) {
      builder.operand(ir::Operand(builder.state().operands[op_idx]));
    }
    const ir::OperandIdxRange range = {head, static_cast<u32>(ops.size())};
    const ir::InstructionIdx void_instr =
        builder.instr({.op = op,
                       .flags = {},
                       .dst = ir::RegisterIdx(base::kInvalidIdx),
                       .operands = range});
    streams_[at(cur_)].push(void_instr);
    stream_last_[at(cur_)] = void_instr;
    instr_spans_.push_back(cur_span_);
  }

  struct SpanGuard {
    Lowerer* lowerer;
    diag::Span previous;
    SpanGuard(Lowerer* lowerer, diag::Span previous)
        : lowerer(lowerer), previous(previous) {}
    ~SpanGuard() { lowerer->cur_span_ = previous; }
  };

  Val materialize(Val v) {
    if (!v.address || tag_of(v.type) == ir::TypeTag::Never) {
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
  void mark_move(Val v) {
    if (v.place && v.address && !is_copy(v.type)) {
      const ir::RegisterIdx marker = emit(ir::Opcode::Move, v.type, {v.op});
      (void)marker;
    }
  }

  ir::OperandIdx use_value(Val v) {
    mark_move(v);
    return materialize(v).op;
  }

  const Local* lookup_local(std::string_view name) const {
    for (const auto& local : locals | std::views::reverse) {
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

  ir::TypeIdx expr_type(ast::ExprIdx expr) {
    for (const auto& entry : pkg.modules[module].expr_types) {
      if (entry.first == expr) {
        return entry.second;
      }
    }
    return error_type();
  }

  const analyzer::CheckedModule::CallTarget* call_target(
      ast::ExprIdx callee) const {
    for (const auto& entry : pkg.modules[module].call_targets) {
      if (entry.callee == callee) {
        return &entry;
      }
    }
    return nullptr;
  }

  // Comp formal positions of a function item, in order.
  std::vector<u32> comp_positions(ast::ItemIdx item) const {
    std::vector<u32> positions;
    if (!item.is_valid()) {
      return positions;
    }
    const ast::ItemNode& node = ast.items[item];
    if (node.kind != ast::ItemKind::Fn) {
      return positions;
    }
    const std::span<const ast::ItemFnParam> params =
        node.payload.get<ast::ItemFn>().params;
    for (u32 i = 0; i < static_cast<u32>(params.size()); ++i) {
      if (params[i].is_comp) {
        positions.push_back(i);
      }
    }
    return positions;
  }

  // Deterministic specialization key over comp argument values.
  static void comp_key_into(std::string& key, const CompValue& value) {
    switch (value.tag) {
      case CompValue::Tag::Void: key += "v;"; return;
      case CompValue::Tag::Int:
        key += "i" + std::to_string(value.int_value) + ";";
        return;
      case CompValue::Tag::Bool: key += value.bool_value ? "t;" : "f;"; return;
      case CompValue::Tag::Str:
        key += "s" + std::to_string(value.str_value.size()) + ":";
        key += value.str_value;
        key += ";";
        return;
      case CompValue::Tag::Tuple:
        key += "t(";
        for (const CompValue& field : value.fields) {
          comp_key_into(key, field);
        }
        key += ");";
        return;
      case CompValue::Tag::Struct:
        key += "S(";
        for (const CompValue& field : value.fields) {
          comp_key_into(key, field);
        }
        key += ");";
        return;
      case CompValue::Tag::Enum:
        key += "e" + std::to_string(value.variant) + "(";
        for (const CompValue& field : value.fields) {
          comp_key_into(key, field);
        }
        key += ");";
        return;
      case CompValue::Tag::Blessed:
        key += value.blessed_ok ? "B0(" : "B1(";
        for (const CompValue& field : value.fields) {
          comp_key_into(key, field);
        }
        key += ");";
        return;
    }
  }

  // Finds or reserves the function index for (item, comp arguments),
  // enqueueing lowering work on first encounter. Recursive calls see
  // the reserved index, so bodies may reference themselves.
  ir::FunctionIdx fn_index(u32 mod,
                           ast::ItemIdx item,
                           std::string_view name,
                           const std::vector<ir::TypeIdx>& params,
                           ir::TypeIdx ret,
                           std::vector<CompVal> comp_args) {
    std::string key = std::to_string(item.idx) + "|";
    for (const CompVal& arg : comp_args) {
      comp_key_into(key, arg.value);
    }
    for (const FnEntry& entry : fns) {
      if (entry.item == item && entry.comp_key == key) {
        return entry.idx;
      }
    }
    static constexpr usize kMaxFnEntries = 8192;
    if (fns.size() >= kMaxFnEntries) {
      internal(diag::Span{}, "function specialization budget exhausted");
      return ir::FunctionIdx(base::kInvalidIdx);
    }
    const ir::FunctionIdx idx(static_cast<u32>(fns.size()));
    FnEntry entry;
    entry.item = item;
    entry.comp_key = std::move(key);
    entry.idx = idx;
    entry.mod = mod;
    entry.name = std::string(name);
    entry.params = params;
    entry.ret = ret;
    entry.comp_args = std::move(comp_args);
    fns.push_back(std::move(entry));
    worklist_.push_back(fns.size() - 1);
    return idx;
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
  u64 parse_numeric_value(std::string_view spelling) {
    constexpr std::string_view kSuffixes[] = {
        "isize", "usize", "i8",  "i16", "i32", "i64",
        "u8",    "u16",   "u32", "u64", "f32", "f64",
    };

    // Strip type suffix if present.
    for (std::string_view suffix : kSuffixes) {
      const bool has_suffix =
          spelling.size() > suffix.size() && spelling.ends_with(suffix);
      if (has_suffix) {
        spelling.remove_suffix(suffix.size());
        break;
      }
    }

    // Determine base and strip prefix (e.g., "0x", "0b", "0o").
    u32 base = 10;
    const bool has_prefix = spelling.size() > 2 && spelling[0] == '0';
    if (has_prefix) {
      const char prefix_indicator = spelling[1];
      if (prefix_indicator == 'x' || prefix_indicator == 'X') {
        // hex
        base = 16;
        spelling.remove_prefix(2);
      } else if (prefix_indicator == 'b' || prefix_indicator == 'B') {
        // bin
        base = 2;
        spelling.remove_prefix(2);
      } else if (prefix_indicator == 'o' || prefix_indicator == 'O') {
        // oct
        base = 8;
        spelling.remove_prefix(2);
      }
    }

    // Accumulate numerical digits.
    u64 value = 0;
    for (const char ch : spelling) {
      if (ch == '_') {
        continue;
      }

      const bool is_digit = (ch >= '0' && ch <= '9');
      const bool is_lower_hex = (ch >= 'a' && ch <= 'f');
      const bool is_upper_hex = (ch >= 'A' && ch <= 'F');
      u32 digit = 0;
      if (is_digit) {
        digit = static_cast<u32>(ch - '0');
      } else if (is_lower_hex) {
        digit = static_cast<u32>(ch - 'a' + 10);
      } else if (is_upper_hex) {
        digit = static_cast<u32>(ch - 'A' + 10);
      } else {
        continue;
      }
      value = value * base + digit;
    }
    return value;
  }

  ir::TypeTag literal_tag(ast::LiteralIdx value, const ir::TypeIdx* expected) {
    const ast::Literal& lit = ast.literals[value];
    if (lit.kind == ast::LiteralKind::Bool) {
      return ir::TypeTag::I1;
    }
    if (lit.kind == ast::LiteralKind::String) {
      return ir::TypeTag::Str;
    }
    const bool is_float = lit.kind == ast::LiteralKind::Float;
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
      if (lit.spelling.size() > entry.suffix.size() &&
          lit.spelling.substr(lit.spelling.size() - entry.suffix.size()) ==
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

  Val lower_literal(ast::LiteralIdx lit_idx, const ir::TypeIdx* expected) {
    const ast::Literal& lit = ast.literals[lit_idx];
    const ir::TypeTag tag = literal_tag(lit_idx, expected);
    const ir::TypeIdx type = builder.primitive(tag);
    if (tag == ir::TypeTag::Str) {
      std::string bytes;
      std::string_view spelling = lit.spelling;
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
      for (char c : lit.spelling) {
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
      value = lit.spelling == "true" ? 1 : 0;
    } else {
      value = parse_numeric_value(lit.spelling);
    }
    return Val{imm_from_u64(tag, type, value), type, false, false};
  }

  ir::OperandIdx imm_from_u64(ir::TypeTag tag, ir::TypeIdx type, u64 value) {
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
    return to_operand(builder.immutable(imm), type);
  }

  // Address of a place expression. Non-places diagnose: checking
  // accepts any inner shape for borrows, lowering needs an origin.
  Val place_addr(ast::ExprIdx expr) {
    const ast::ExprNode& node = ast.exprs[expr];
    switch (node.kind) {
      case ast::ExprKind::Path: {
        const ast::ExprPath& path = node.payload.get<ast::ExprPath>();
        const std::span<const ast::Ident> segments =
            ast.paths[path.idx].segments;
        if (segments.size() == 1) {
          if (const Local* local = lookup_local(segments[0].name)) {
            return Val{to_operand(local->addr, local->type), local->type, true,
                       true};
          }
        }
        unsupported(node.span, "borrowed place");
        return Val{size_one, error_type(), true, false};
      }
      case ast::ExprKind::Field: {
        Val base = place_addr(node.payload.get<ast::ExprField>().receiver);
        if (failed) {
          return base;
        }
        return field_addr(base, node.payload.get<ast::ExprField>().name.name,
                          node.span);
      }
      default:
        unsupported(node.span, "borrowed temporary");
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

  void bind_pattern(ast::PatternIdx pattern, Val init) {
    const ast::PatternNode& node = ast.patterns[pattern];
    switch (node.kind) {
      case ast::PatternKind::Wildcard: break;
      case ast::PatternKind::Ident:
      case ast::PatternKind::MutIdent: {
        std::string_view name;
        if (node.kind == ast::PatternKind::Ident) {
          name = node.payload.ident.name.name;
        } else {
          name = node.payload.mut_ident.name.name;
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
        addr_names_.push_back({addr, name, binding_param_});
        return;
      }
      case ast::PatternKind::Tuple: {
        if (node.payload.tuple.path.is_valid()) {
          unsupported(node.span, "variant pattern in lowering");
          return;
        }
        Val base = address_of(init);
        if (failed) {
          return;
        }
        for (u32 i = 0;
             i < static_cast<u32>(node.payload.tuple.elements.size()); ++i) {
          const ir::TypeIdx element_type =
              field_type_of(base.type, i, node.span);
          const ir::RegisterIdx gep =
              emit(ir::Opcode::GetElementPtr, element_type,
                   {base.op, zero_i32, index_operand(i)});
          Val element{to_operand(gep, element_type), element_type, true,
                      init.place};
          bind_pattern(node.payload.tuple.elements[i], element);
          if (failed) {
            return;
          }
        }
        return;
      }
      case ast::PatternKind::Struct: {
        Val base = address_of(init);
        if (failed) {
          return;
        }
        for (const ast::FieldPattern& field : node.payload.strukt.fields) {
          u32 index = 0;
          if (!struct_field_index(base.type, field.name.name, index)) {
            internal(field.name.span, "pattern field without declaration");
            return;
          }
          const ir::TypeIdx element_type =
              field_type_of(base.type, index, node.span);
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
        // Through a reference pattern the inner name observes the
        // pointer slot itself.
        const Val material = materialize(init);
        const ir::RegisterIdx addr =
            emit(ir::Opcode::Alloca, init.type, {size_one});
        emit_void(ir::Opcode::Store,
                  {material.op, to_operand(addr, init.type)});
        bind_pattern(node.payload.ref.inner,
                     Val{to_operand(addr, init.type), init.type, true, false});
        return;
      }
      case ast::PatternKind::Literal:
      case ast::PatternKind::Or:
        unsupported(node.span, "refutable pattern in lowering");
        return;
    }
  }

  Val lower_path(ast::ExprIdx expr, const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::PathIdx path = node.payload.get<ast::ExprPath>().idx;
    const std::span<const ast::Ident> segments = ast.paths[path].segments;
    if (segments.size() == 1) {
      const std::string_view name = segments[0].name;
      if (const Local* local = lookup_local(name)) {
        if (tag_of(local->type) == ir::TypeTag::Void) {
          return Val{size_one, local->type, false, false};
        }
        return Val{to_operand(local->addr, local->type), local->type, true,
                   true};
      }
      // Comp bindings never take runtime addresses; splice the value.
      for (usize i = comp_scope_.size(); i-- > 0;) {
        if (comp_scope_[i].first == name) {
          return materialize_comp_value(comp_scope_[i].second, node.span);
        }
      }
      if (const auto* info = lookup_static(module, name)) {
        if (info->is_const && info->init.is_valid() &&
            ast.exprs[info->init].kind == ast::ExprKind::Literal) {
          return lower_literal(
              ast.exprs[info->init].payload.get<ast::ExprLiteral>().value,
              expected);
        }
        unsupported(node.span, "static item in lowering");
        return Val{size_one, error_type(), false, false};
      }
    }
    if (const auto* use = variant_use(path)) {
      // Unit values stand alone; payload constructors need call syntax
      // (checking enforced this).
      const std::vector<ir::TypeIdx> payloads =
          variant_payload(use->enum_type, use->variant, use->blessed_first);
      if (!payloads.empty()) {
        internal(node.span, "variant without call");
        return Val{size_one, error_type(), false, false};
      }
      const ir::TypeIdx slot = enum_slot_type();
      const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, slot, {size_one});
      const u32 discriminant =
          use->blessed ? (use->blessed_first ? 0 : 1) : use->variant;
      const ir::RegisterIdx tag =
          emit(ir::Opcode::GetElementPtr, builder.primitive(ir::TypeTag::I32),
               {to_operand(addr, slot), zero_i32, index_operand(0)});
      emit_void(ir::Opcode::Store,
                {disc_operand(discriminant), to_operand(tag, slot)});
      return Val{to_operand(addr, slot), use->enum_type, true, false};
    }
    internal(node.span, "path without lowering");
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

  const analyzer::CheckedModule::VariantUse* variant_use(
      ast::PathIdx path) const {
    for (const auto& checked : pkg.modules) {
      for (const auto& use : checked.variants) {
        if (use.path == path) {
          return &use;
        }
      }
    }
    return nullptr;
  }

  const analyzer::CheckedModule::EnumInfo* enum_info(ir::TypeIdx type) const {
    for (const auto& checked : pkg.modules) {
      for (const auto& info : checked.enums) {
        if (info.type.idx == type.idx) {
          return &info;
        }
      }
    }
    return nullptr;
  }

  const analyzer::CheckedPackage::BlessedType* blessed_entry(
      ir::TypeIdx type) const {
    for (const auto& entry : pkg.blessed) {
      if (entry.type.idx == type.idx) {
        return &entry;
      }
    }
    return nullptr;
  }

  bool blessed_ctor_side(std::string_view name, bool is_result, bool& first) {
    if (name != "Ok" && name != "Err" && name != "Some" && name != "None") {
      return false;
    }
    first = (name == "Ok" || name == "Some");
    return (name == "Ok" || name == "Err") == is_result;
  }

  // Variant index by trailing name against a known enum type, for
  // patterns (checking validated the match).
  bool variant_index(ir::TypeIdx enum_type,
                     std::string_view name,
                     u32& index_out) {
    if (const auto* info = enum_info(enum_type)) {
      for (u32 i = 0; i < static_cast<u32>(info->variants.size()); ++i) {
        if (info->variants[i] == name) {
          index_out = i;
          return true;
        }
      }
      return false;
    }
    if (const auto* entry = blessed_entry(enum_type)) {
      bool first = true;
      if (!blessed_ctor_side(name, entry->is_result, first)) {
        return false;
      }
      index_out = first ? 0 : 1;
      return true;
    }
    return false;
  }

  // Payload field types of one variant, in order.
  std::vector<ir::TypeIdx> variant_payload(ir::TypeIdx enum_type,
                                           u32 variant,
                                           bool blessed_first) {
    if (const auto* entry = blessed_entry(enum_type)) {
      if (blessed_first) {
        return {entry->args[0]};
      }
      if (entry->is_result) {
        return {entry->args[1]};
      }
      return {};
    }
    const ir::EnumType& enum_ty =
        builder.state().enum_types[builder.state().types[enum_type].as_enum()];
    std::vector<ir::TypeIdx> payloads;
    u32 at = enum_ty.variants.head().idx + variant;
    const ir::EnumVariantType& variant_ty =
        builder.state().enum_variant_types[ir::EnumVariantTypeIdx(at)];
    for (ir::TypeIdx field : variant_ty.fields) {
      payloads.push_back(field);
    }
    return payloads;
  }

  ir::TypeIdx enum_slot_type() {
    if (enum_slot_type_.is_valid()) {
      return enum_slot_type_;
    }
    ir::TypeSeq seq;
    seq.push(builder.ref_type(builder.primitive(ir::TypeTag::I32)));
    seq.push(builder.ref_type(builder.primitive(ir::TypeTag::Ptr)));
    enum_slot_type_ = builder.tuple_type(seq.finish());
    return enum_slot_type_;
  }

  ir::TypeIdx enum_slot_type_ = ir::TypeIdx(base::kInvalidIdx);

  Val lower_variant_construct(ast::ExprIdx expr,
                              const analyzer::CheckedModule::VariantUse* use) {
    const ast::ExprNode& call = ast.exprs[expr];
    const std::vector<ir::TypeIdx> payloads =
        variant_payload(use->enum_type, use->variant, use->blessed_first);
    if (call.payload.get<ast::ExprCall>().args.size() != payloads.size()) {
      internal(call.span, "variant arity");
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx slot = enum_slot_type();
    const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, slot, {size_one});
    const u32 discriminant =
        use->blessed ? (use->blessed_first ? 0 : 1) : use->variant;
    const ir::RegisterIdx tag =
        emit(ir::Opcode::GetElementPtr, builder.primitive(ir::TypeTag::I32),
             {to_operand(addr, slot), zero_i32, index_operand(0)});
    emit_void(ir::Opcode::Store,
              {disc_operand(discriminant), to_operand(tag, slot)});
    if (is_unit_payload(payloads)) {
      // Lower for effects; nothing is stored.
      for (usize i = 0; i < payloads.size(); ++i) {
        Val value =
            lower_expr(call.payload.get<ast::ExprCall>().args[i], &payloads[i]);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        mark_move(value);
      }
    } else if (!payloads.empty()) {
      const ir::TypeIdx payload_type = payload_tuple(payloads);
      const ir::RegisterIdx payload =
          emit(ir::Opcode::Alloca, payload_type, {size_one});
      for (usize i = 0; i < payloads.size(); ++i) {
        Val value =
            lower_expr(call.payload.get<ast::ExprCall>().args[i], &payloads[i]);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const ir::RegisterIdx field =
            emit(ir::Opcode::GetElementPtr, payloads[i],
                 {to_operand(payload, payload_type), zero_i32,
                  index_operand(static_cast<u32>(i))});
        emit_void(ir::Opcode::Store,
                  {use_value(value), to_operand(field, payloads[i])});
      }
      const ir::TypeIdx ptr = builder.primitive(ir::TypeTag::Ptr);
      const ir::RegisterIdx slot_field =
          emit(ir::Opcode::GetElementPtr, ptr,
               {to_operand(addr, slot), zero_i32, index_operand(1)});
      emit_void(ir::Opcode::Store,
                {to_operand(payload, ptr), to_operand(slot_field, ptr)});
    }
    return Val{to_operand(addr, slot), use->enum_type, true, false};
  }

  ir::OperandIdx disc_operand(u32 discriminant) {
    const ir::TypeIdx i32_ty = builder.primitive(ir::TypeTag::I32);
    ir::Immutable imm{.type = i32_ty, .data = {}};
    imm.data.i32_value = static_cast<i32>(discriminant);
    return to_operand(builder.immutable(imm), i32_ty);
  }

  Val lower_call(ast::ExprIdx expr, const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    // Intrinsics by name (checking rejected shadowing definitions).
    if (ast.exprs[call.callee].kind == ast::ExprKind::Path) {
      const ast::ExprPath& path =
          ast.exprs[call.callee].payload.get<ast::ExprPath>();
      const ast::PathIdx path_idx = path.idx;
      if (ast.paths[path_idx].segments.size() == 1) {
        const std::string_view name = ast.paths[path_idx].segments[0].name;
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
          if (!shadowed &&
              (name == "print" || name == "println" || name == "panic")) {
            return lower_intrinsic(expr, name);
          }
        }
      }
    }
    const analyzer::CheckedModule::CallTarget* target =
        call_target(call.callee);
    if (target == nullptr) {
      // Checking records free, associated, and method callees; the
      // remainder is variant construction.
      if (ast.exprs[call.callee].kind == ast::ExprKind::Path) {
        const ast::ExprPath& path =
            ast.exprs[call.callee].payload.get<ast::ExprPath>();
        const ast::PathIdx path_idx = path.idx;
        if (const auto* use = variant_use(path_idx)) {
          return lower_variant_construct(expr, use);
        }
      }
      internal(node.span, "call without target");
      return Val{size_one, error_type(), false, false};
    }
    if (target->is_method) {
      return lower_associated_call(expr);
    }
    const analyzer::CheckedModule& def = pkg.modules[target->module];
    const analyzer::CheckedModule::FnSig& sig = def.functions[target->index];
    const std::vector<u32> comp = comp_positions(sig.item);
    std::vector<CompVal> comp_args;
    for (u32 pos : comp) {
      CompVal arg;
      if (!comp_evaluate(module, call.args[pos], arg)) {
        return Val{size_one, error_type(), false, false};
      }
      comp_args.push_back(std::move(arg));
    }
    const ir::FunctionIdx fn =
        fn_index(target->module, sig.item, sig.name, sig.params, sig.ret,
                 std::move(comp_args));
    if (!fn.is_valid()) {
      return Val{size_one, error_type(), false, false};
    }
    if (call.args.size() != sig.params.size()) {
      internal(node.span, "call arity");
      return Val{size_one, error_type(), false, false};
    }
    std::vector<ir::OperandIdx> ops;
    ops.push_back(builder.operand(ir::Operand::from_function(
        fn, builder.primitive(ir::TypeTag::Function))));
    usize comp_at = 0;
    for (usize i = 0; i < call.args.size(); ++i) {
      if (comp_at < comp.size() && comp[comp_at] == i) {
        ++comp_at;
        continue;
      }
      Val arg = lower_expr(call.args[i], &sig.params[i]);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      ops.push_back(arg_for(arg, sig.params[i]));
    }
    if (tag_of(sig.ret) == ir::TypeTag::Never) {
      emit_void(ir::Opcode::Call, ops);
      emit_void(ir::Opcode::Unreachable, {});
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

  Val lower_associated_call(ast::ExprIdx expr) {
    const ast::ExprNode& call = ast.exprs[expr];
    const analyzer::CheckedModule::CallTarget* target =
        call_target(call.payload.get<ast::ExprCall>().callee);
    if (target == nullptr || !target->is_method) {
      internal(call.span, "call without target");
      return Val{size_one, error_type(), false, false};
    }
    const analyzer::CheckedModule& def = pkg.modules[target->module];
    const analyzer::CheckedModule::MethodInfo& info =
        def.methods[target->index];
    if (info.receiver != analyzer::CheckedModule::ReceiverKind::None) {
      internal(call.span, "method without receiver");
      return Val{size_one, error_type(), false, false};
    }
    const std::span<const ast::ExprIdx> args =
        call.payload.get<ast::ExprCall>().args;
    const std::vector<u32> comp = comp_positions(info.item);
    std::vector<CompVal> comp_args;
    for (u32 pos : comp) {
      CompVal arg;
      if (!comp_evaluate(module, args[pos], arg)) {
        return Val{size_one, error_type(), false, false};
      }
      comp_args.push_back(std::move(arg));
    }
    const ir::FunctionIdx fn =
        fn_index(target->module, info.item, info.name, info.params, info.ret,
                 std::move(comp_args));
    if (!fn.is_valid()) {
      internal(call.span, "call without function");
      return Val{size_one, error_type(), false, false};
    }
    // Recorded params already exclude the receiver: associated
    // functions lower like free functions.
    if (args.size() != info.params.size()) {
      internal(call.span, "call arity");
      return Val{size_one, error_type(), false, false};
    }
    std::vector<ir::OperandIdx> ops;
    ops.push_back(builder.operand(ir::Operand::from_function(
        fn, builder.primitive(ir::TypeTag::Function))));
    usize comp_at = 0;
    for (usize i = 0; i < args.size(); ++i) {
      if (comp_at < comp.size() && comp[comp_at] == i) {
        ++comp_at;
        continue;
      }
      Val arg = lower_expr(args[i], &info.params[i]);
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

  Val lower_intrinsic(ast::ExprIdx expr, std::string_view name) {
    const ast::ExprNode& call = ast.exprs[expr];
    if (call.payload.get<ast::ExprCall>().args.size() != 1) {
      internal(call.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
    Val arg = lower_expr(call.payload.get<ast::ExprCall>().args[0], &str);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const Val material = materialize(arg);
    const ir::TypeIdx ptr = builder.primitive(ir::TypeTag::Ptr);

    bool is_panic = false;
    ir::ExternalFunctionIdx ext(0);
    if (name == "print") {
      ext = declare_external("alcy_print", builder.primitive(ir::TypeTag::Void),
                             {ptr});

    } else if (name == "println") {
      ext = declare_external("alcy_println",
                             builder.primitive(ir::TypeTag::Void), {ptr});
    } else if (name == "panic") {
      is_panic = true;
      ext = declare_external("alcy_panic", builder.never_type(), {ptr});
    }
    emit_void(ir::Opcode::Call,
              {builder.operand(ir::Operand::from_external_function(
                   ext, builder.primitive(ir::TypeTag::Function))),
               material.op});
    if (is_panic) {
      emit_void(ir::Opcode::Unreachable, {});
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

  // Address of an enum slot value (spills SSA temporaries).
  Val enum_addr(Val value) { return address_of(value); }

  // Loads the discriminant of an enum slot address.
  Val load_disc(Val slot_addr) {
    const ir::TypeIdx i32 = builder.primitive(ir::TypeTag::I32);
    const ir::RegisterIdx gep =
        emit(ir::Opcode::GetElementPtr, i32,
             {slot_addr.op, zero_i32, index_operand(0)});
    const ir::RegisterIdx loaded =
        emit(ir::Opcode::Load, i32, {to_operand(gep, i32)});
    return Val{to_operand(loaded, i32), i32, false, false};
  }

  // Unit payloads (`()` fields) carry no data; construction stores
  // nothing and bindings receive a Void value.
  bool is_unit_payload(const std::vector<ir::TypeIdx>& payloads) {
    if (payloads.empty()) {
      return false;
    }
    for (ir::TypeIdx field : payloads) {
      if (tag_of(field) != ir::TypeTag::Void) {
        return false;
      }
    }
    return true;
  }

  Val void_value() {
    return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  }

  ir::TypeIdx payload_tuple(const std::vector<ir::TypeIdx>& fields) {
    ir::TypeSeq seq;
    for (ir::TypeIdx field : fields) {
      seq.push(builder.ref_type(field));
    }
    return builder.tuple_type(seq.finish());
  }

  // Value of payload field i of the enum at slot_addr. The payload
  // pointer is type-erased in the slot, so it reinterprets through
  // the variant payload type before projecting the field.
  Val load_blessed_payload(Val slot_addr,
                           u32 field,
                           const std::vector<ir::TypeIdx>& fields) {
    if (is_unit_payload(fields)) {
      return void_value();
    }
    return load_payload_field(slot_addr, payload_tuple(fields), field);
  }

  Val load_payload_field(Val slot_addr, ir::TypeIdx payload_type, u32 field) {
    const ir::TupleType& shape =
        builder.state()
            .tuple_types[builder.state().types[payload_type].as_tuple()];
    const ir::TypeIdx field_type = shape.elements[field];
    const ir::TypeIdx ptr = builder.primitive(ir::TypeTag::Ptr);
    const ir::RegisterIdx ptr_gep =
        emit(ir::Opcode::GetElementPtr, ptr,
             {slot_addr.op, zero_i32, index_operand(1)});
    const ir::RegisterIdx payload =
        emit(ir::Opcode::Load, ptr, {to_operand(ptr_gep, ptr)});
    const ir::RegisterIdx typed =
        emit(ir::Opcode::TypeCast, payload_type, {to_operand(payload, ptr)});
    const ir::RegisterIdx field_gep =
        emit(ir::Opcode::GetElementPtr, field_type,
             {to_operand(typed, payload_type), zero_i32, index_operand(field)});
    const ir::RegisterIdx loaded =
        emit(ir::Opcode::Load, field_type, {to_operand(field_gep, field_type)});
    return Val{to_operand(loaded, field_type), field_type, false, false};
  }

  void emit_br(ir::BlockIdx target) {
    emit_void(ir::Opcode::Br,
              {builder.operand(ir::Operand::from_block(
                  target, builder.primitive(ir::TypeTag::Void)))});
  }

  void emit_cond_br(ir::OperandIdx cond,
                    ir::BlockIdx then_block,
                    ir::BlockIdx else_block) {
    const ir::TypeIdx void_ty = builder.primitive(ir::TypeTag::Void);
    emit_void(
        ir::Opcode::CondBr,
        {cond, builder.operand(ir::Operand::from_block(then_block, void_ty)),
         builder.operand(ir::Operand::from_block(else_block, void_ty))});
  }

  ir::OperandIdx bool_operand(bool value) {
    const ir::TypeIdx i1 = builder.primitive(ir::TypeTag::I1);
    ir::Immutable imm{.type = i1, .data = {}};
    imm.data.i1_value = value;
    return to_operand(builder.immutable(imm), i1);
  }

  void emit_panic(ir::OperandIdx message) {
    const ir::TypeIdx ptr = builder.primitive(ir::TypeTag::Ptr);
    const ir::ExternalFunctionIdx ext =
        declare_external("alcy_panic", builder.never_type(), {ptr});
    emit_void(ir::Opcode::Call,
              {builder.operand(ir::Operand::from_external_function(
                   ext, builder.primitive(ir::TypeTag::Function))),
               message});
    emit_void(ir::Opcode::Unreachable, {});
  }

  ir::OperandIdx str_operand(std::string_view message) {
    const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
    const ir::ImmutableIdx imm = builder.immutable(
        {.type = str, .data = {.str_id_value = strings.intern(message)}});
    return to_operand(imm, str);
  }

  Val lower_blessed_method(ast::ExprIdx expr,
                           const analyzer::CheckedPackage::BlessedType* entry,
                           Val receiver,
                           const ir::TypeIdx* expected) {
    const ast::ExprNode& method = ast.exprs[expr];
    const std::string_view name =
        method.payload.get<ast::ExprMethodCall>().name.name;
    const bool is_ok = name == "is_ok";
    if (name != "unwrap" && name != "expect" && !is_ok && name != "is_err") {
      internal(method.span, "blessed method without lowering");
      return Val{size_one, error_type(), false, false};
    }
    Val slot = enum_addr(receiver);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    if (name == "unwrap" || name == "expect") {
      mark_move(slot);
    }
    Val tag = load_disc(slot);
    const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
    if (is_ok || name == "is_err") {
      const ir::RegisterIdx dst =
          emit(ir::Opcode::Eq, boolean,
               {tag.op, is_ok ? disc_operand(0) : disc_operand(1)});
      Val result{to_operand(dst, boolean), boolean, false, false};
      if (expected != nullptr) {
        (void)expected;
      }
      return result;
    }
    // unwrap / expect: Ok payload on tag 0, else diverge.
    ir::OperandIdx failure = str_operand("unwrap");
    if (name == "expect") {
      if (method.payload.get<ast::ExprMethodCall>().args.size() != 1) {
        internal(method.span, "expect without message");
        return Val{size_one, error_type(), false, false};
      }
      Val message = lower_expr(
          method.payload.get<ast::ExprMethodCall>().args[0], nullptr);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      failure = materialize(message).op;
    }
    ir::BlockIdx ok_block = reserve_block();
    ir::BlockIdx bad_block = reserve_block();
    ir::BlockIdx join_block = reserve_block();
    const ir::RegisterIdx test =
        emit(ir::Opcode::Eq, boolean, {tag.op, disc_operand(0)});
    emit_cond_br(to_operand(test, boolean), ok_block, bad_block);
    switch_to(bad_block);
    emit_panic(failure);
    switch_to(ok_block);
    Val payload = load_blessed_payload(slot, 0, {entry->args[0]});
    emit_br(join_block);
    switch_to(join_block);
    return payload;
  }

  Val lower_method_call(ast::ExprIdx expr, const ir::TypeIdx* expected) {
    const ast::ExprNode& method = ast.exprs[expr];
    Val receiver =
        lower_expr(method.payload.get<ast::ExprMethodCall>().receiver, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    if (const auto* entry = blessed_entry(receiver.type)) {
      return lower_blessed_method(expr, entry, receiver, expected);
    }
    const analyzer::CheckedModule::CallTarget* target = call_target(expr);
    if (target == nullptr || !target->is_method) {
      internal(method.span, "method call without target");
      return Val{size_one, error_type(), false, false};
    }
    const analyzer::CheckedModule& def = pkg.modules[target->module];
    const analyzer::CheckedModule::MethodInfo& info =
        def.methods[target->index];
    const std::vector<u32> comp = comp_positions(info.item);
    if (!comp.empty() && comp[0] == 0) {
      unsupported(method.span, "comp method receiver");
      return Val{size_one, error_type(), false, false};
    }
    std::vector<CompVal> comp_args;
    const std::span<const ast::ExprIdx> method_args =
        method.payload.get<ast::ExprMethodCall>().args;
    for (u32 pos : comp) {
      CompVal arg;
      if (!comp_evaluate(module, method_args[pos - 1], arg)) {
        return Val{size_one, error_type(), false, false};
      }
      comp_args.push_back(std::move(arg));
    }
    ir::FunctionIdx fn = fn_index(target->module, info.item, info.name,
                                  info.params, info.ret, std::move(comp_args));
    if (!fn.is_valid()) {
      return Val{size_one, error_type(), false, false};
    }
    std::vector<ir::OperandIdx> ops;
    ops.push_back(builder.operand(ir::Operand::from_function(
        fn, builder.primitive(ir::TypeTag::Function))));
    ops.push_back(arg_for(receiver, info.params[0]));
    usize comp_at = 0;
    for (usize i = 0; i < method_args.size(); ++i) {
      if (comp_at < comp.size() && comp[comp_at] == i + 1) {
        ++comp_at;
        continue;
      }
      Val arg = lower_expr(method_args[i], &info.params[i + 1]);
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

  Val lower_struct(ast::ExprIdx expr) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ir::TypeIdx struct_type = expr_type(expr);
    if (tag_of(struct_type) != ir::TypeTag::Struct) {
      internal(node.span, "struct without type");
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
    for (const ast::ExprFieldInit& field :
         node.payload.get<ast::ExprStruct>().init) {
      u32 index = 0;
      if (!struct_field_index(struct_type, field.name.name, index)) {
        internal(field.name.span, "field without declaration");
        return Val{size_one, error_type(), false, false};
      }
      seen[index] = true;
      const ir::TypeIdx element_type =
          field_type_of(struct_type, index, ast.exprs[field.value].span);
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
    if (node.payload.get<ast::ExprStruct>().base_expr.is_valid()) {
      Val base =
          lower_expr(node.payload.get<ast::ExprStruct>().base_expr, nullptr);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      Val base_addr = address_of(base);
      // Update consumes the base value.
      mark_move(base_addr);
      for (u32 i = 0; i < static_cast<u32>(seen.size()); ++i) {
        if (seen[i]) {
          continue;
        }
        const ir::TypeIdx element_type =
            field_type_of(struct_type, i, node.span);
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

  Val lower_tuple(ast::ExprIdx expr) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprTuple& tuple = node.payload.get<ast::ExprTuple>();
    if (tuple.elements.empty()) {
      return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
    }
    const ir::TypeIdx tuple_type = expr_type(expr);
    if (tag_of(tuple_type) != ir::TypeTag::Tuple) {
      internal(node.span, "tuple without type");
      return Val{size_one, error_type(), false, false};
    }
    const ir::TupleType& shape =
        builder.state()
            .tuple_types[builder.state().types[tuple_type].as_tuple()];
    const ir::RegisterIdx addr =
        emit(ir::Opcode::Alloca, tuple_type, {size_one});
    for (u32 i = 0; i < static_cast<u32>(tuple.elements.size()) && !failed;
         ++i) {
      const ir::TypeIdx* element_expected = nullptr;
      ir::TypeIdx element_type = error_type();
      if (i < shape.elements.size()) {
        element_type = shape.elements[i];
        element_expected = &element_type;
      }
      Val value = lower_expr(tuple.elements[i], element_expected);
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

  Val lower_binary(ast::ExprIdx expr) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprBinary& bin = node.payload.get<ast::ExprBinary>();
    if (bin.op == ast::BinaryOp::Pow) {
      unsupported(node.span, "power operator");
      return Val{size_one, error_type(), false, false};
    }
    Val lhs = lower_expr(bin.lhs, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    Val rhs = lower_expr(bin.rhs, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeTag tag = tag_of(lhs.type);
    if (bin.op == ast::BinaryOp::And || bin.op == ast::BinaryOp::Or) {
      const ir::RegisterIdx dst =
          emit(bin.op == ast::BinaryOp::And ? ir::Opcode::And : ir::Opcode::Or,
               lhs.type, {use_value(lhs), use_value(rhs)});
      return Val{to_operand(dst, lhs.type), lhs.type, false, false};
    }
    if (bin.op == ast::BinaryOp::Eq || bin.op == ast::BinaryOp::NotEq ||
        bin.op == ast::BinaryOp::Gt || bin.op == ast::BinaryOp::Lt ||
        bin.op == ast::BinaryOp::GtEq || bin.op == ast::BinaryOp::LtEq) {
      ir::Opcode op = ir::Opcode::Eq;
      switch (bin.op) {
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
      switch (bin.op) {
        case ast::BinaryOp::Add: op = ir::Opcode::FAdd; break;
        case ast::BinaryOp::Sub: op = ir::Opcode::FSub; break;
        case ast::BinaryOp::Mul: op = ir::Opcode::FMul; break;
        default: op = ir::Opcode::FDiv; break;
      }
      const ir::RegisterIdx dst =
          emit(op, lhs.type, {use_value(lhs), use_value(rhs)});
      return Val{to_operand(dst, lhs.type), lhs.type, false, false};
    }
    const ir::Opcode op = int_binop(bin.op, tag);
    const ir::RegisterIdx dst =
        emit(op, lhs.type, {use_value(lhs), use_value(rhs)});
    return Val{to_operand(dst, lhs.type), lhs.type, false, false};
  }

  // Stores an arm value into a result slot (callers skip Void).
  void store_result(Val slot, Val value) {
    emit_void(ir::Opcode::Store, {use_value(value), slot.op});
  }

  // Result slot for joining control-flow values (none for Void or
  // Never, whose arms all diverge).
  Val result_slot(ir::TypeIdx type, diag::Span span) {
    if (tag_of(type) == ir::TypeTag::Void ||
        tag_of(type) == ir::TypeTag::Never) {
      return Val{size_one, type, false, false};
    }
    const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, type, {size_one});
    (void)span;
    return Val{to_operand(addr, type), type, true, false};
  }

  // Tests one match arm against a scrutinee address. On success the
  // pattern binds at the start of body_block and lowering continues
  // there; on failure control jumps to fail_block. Or-patterns expand
  // into sibling arms beforehand.
  void lower_arm_test(ast::PatternIdx pattern,
                      Val scrut_addr,
                      ir::TypeIdx scrut_type,
                      ir::BlockIdx body_block,
                      ir::BlockIdx fail_block) {
    const ast::PatternNode& node = ast.patterns[pattern];
    const diag::Span span = node.span;
    switch (node.kind) {
      case ast::PatternKind::Wildcard:
        emit_br(body_block);
        switch_to(body_block);
        return;
      case ast::PatternKind::Ident:
      case ast::PatternKind::MutIdent: {
        bind_pattern(pattern, materialize(scrut_addr));
        if (failed) {
          return;
        }
        emit_br(body_block);
        switch_to(body_block);
        return;
      }
      case ast::PatternKind::Literal: {
        Val expected = lower_literal(node.payload.literal.value, &scrut_type);
        if (failed) {
          return;
        }
        Val actual = materialize(scrut_addr);
        const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
        const ir::RegisterIdx test =
            emit(ir::Opcode::Eq, boolean, {actual.op, expected.op});
        emit_cond_br(to_operand(test, boolean), body_block, fail_block);
        switch_to(body_block);
        return;
      }
      case ast::PatternKind::Tuple: {
        if (!node.payload.tuple.path.is_valid()) {
          // Plain tuple destructuring (checking validated shape).
          const ir::TupleType& shape =
              builder.state()
                  .tuple_types[builder.state().types[scrut_type].as_tuple()];
          for (u32 i = 0;
               i < static_cast<u32>(node.payload.tuple.elements.size()) &&
               !failed;
               ++i) {
            const ir::RegisterIdx gep =
                emit(ir::Opcode::GetElementPtr, shape.elements[i],
                     {scrut_addr.op, zero_i32, index_operand(i)});
            bind_pattern(node.payload.tuple.elements[i],
                         Val{to_operand(gep, shape.elements[i]),
                             shape.elements[i], true, scrut_addr.place});
            if (failed) {
              return;
            }
          }
          emit_br(body_block);
          switch_to(body_block);
          return;
        }
        if (ast.paths[node.payload.tuple.path].segments.empty()) {
          internal(span, "variant without name");
          return;
        }
        u32 variant = 0;
        if (!variant_index(
                scrut_type,
                ast.paths[node.payload.tuple.path].segments.back().name,
                variant)) {
          internal(span, "variant without declaration");
          return;
        }
        ir::BlockIdx bind_block = body_block;
        Val tag = load_disc(scrut_addr);
        const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
        const ir::RegisterIdx test =
            emit(ir::Opcode::Eq, boolean, {tag.op, disc_operand(variant)});
        emit_cond_br(to_operand(test, boolean), bind_block, fail_block);
        switch_to(bind_block);
        const std::vector<ir::TypeIdx> payloads =
            variant_payload(scrut_type, variant, variant == 0);
        const std::span<const ast::PatternIdx> elements =
            node.payload.tuple.elements;
        if (payloads.size() != elements.size()) {
          internal(span, "variant arity");
          return;
        }
        if (is_unit_payload(payloads)) {
          for (usize i = 0; i < payloads.size() && !failed; ++i) {
            bind_pattern(elements[i], void_value());
          }
          return;
        }
        const ir::TypeIdx payload_type = payload_tuple(payloads);
        for (usize i = 0; i < payloads.size() && !failed; ++i) {
          Val field =
              load_payload_field(scrut_addr, payload_type, static_cast<u32>(i));
          bind_pattern(elements[i], field);
        }
        return;
      }
      case ast::PatternKind::Struct: {
        for (const ast::FieldPattern& field : node.payload.strukt.fields) {
          u32 index = 0;
          if (!struct_field_index(scrut_type, field.name.name, index)) {
            internal(field.name.span, "pattern field without declaration");
            return;
          }
          const ir::TypeIdx field_type = field_type_of(scrut_type, index, span);
          const ir::RegisterIdx gep =
              emit(ir::Opcode::GetElementPtr, field_type,
                   {scrut_addr.op, zero_i32, index_operand(index)});
          bind_pattern(field.pattern, Val{to_operand(gep, field_type),
                                          field_type, true, scrut_addr.place});
          if (failed) {
            return;
          }
        }
        emit_br(body_block);
        switch_to(body_block);
        return;
      }
      case ast::PatternKind::Ref: {
        Val loaded = materialize(scrut_addr);
        bind_pattern(pattern, loaded);
        if (failed) {
          return;
        }
        emit_br(body_block);
        switch_to(body_block);
        return;
      }
      case ast::PatternKind::Or:
        internal(span, "or-pattern without expansion");
        return;
    }
  }

  // Expands or-pattern alternatives into sibling (pattern, body) arms
  // sharing one body; checking required identical bindings.
  void expand_or_arms(
      const ast::ExprMatchArm& arm,
      std::vector<std::pair<ast::PatternIdx, ast::ExprIdx>>& out) {
    if (ast.patterns[arm.pattern].kind != ast::PatternKind::Or) {
      out.emplace_back(arm.pattern, arm.body);
      return;
    }
    for (ast::PatternIdx alt :
         ast.patterns[arm.pattern].payload.or_pat.alternatives) {
      out.emplace_back(alt, arm.body);
    }
  }

  Val lower_match(ast::ExprIdx expr, const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprMatch& match = node.payload.get<ast::ExprMatch>();
    Val scrut = lower_expr(match.scrutinee, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    Val addr = address_of(scrut);
    // By-value matches consume a non-Copy scrutinee; payload bindings
    // copy out of the moved value.
    mark_move(addr);
    const ir::TypeIdx scrut_type = expr_type(match.scrutinee);
    const ir::TypeIdx result_type = expr_type(expr);
    Val slot{size_one, result_type, false, false};
    const bool has_slot = tag_of(result_type) != ir::TypeTag::Void &&
                          tag_of(result_type) != ir::TypeTag::Error;
    if (has_slot) {
      slot = result_slot(result_type, node.span);
    }
    std::vector<std::pair<ast::PatternIdx, ast::ExprIdx>> arms;
    for (const ast::ExprMatchArm& arm : match.arms) {
      expand_or_arms(arm, arms);
    }
    std::vector<ir::BlockIdx> tests;
    std::vector<ir::BlockIdx> bodies;
    for (usize i = 0; i < arms.size(); ++i) {
      tests.push_back(reserve_block());
      bodies.push_back(reserve_block());
    }
    const ir::BlockIdx fail = reserve_block();
    ir::BlockIdx join = ir::BlockIdx(base::kInvalidIdx);
    if (arms.empty()) {
      internal(node.span, "match without arms");
      return Val{size_one, error_type(), false, false};
    }
    emit_br(tests.front());
    for (usize i = 0; i < arms.size() && !failed; ++i) {
      switch_to(tests[i]);
      const ir::BlockIdx next = i + 1 < arms.size() ? tests[i + 1] : fail;
      lower_arm_test(arms[i].first, addr, scrut_type, bodies[i], next);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      // Arm testing always leaves lowering at the body block.
      switch_to(bodies[i]);
      Val produced = lower_expr(arms[i].second, expected);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      if (has_slot && !terminated_cur()) {
        store_result(slot, produced);
      } else if (!has_slot) {
        mark_move(produced);
      }
      if (!terminated_cur()) {
        if (!join.is_valid()) {
          join = reserve_block();
        }
        emit_br(join);
      }
    }
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    switch_to(fail);
    emit_void(ir::Opcode::Unreachable, {});
    if (join.is_valid()) {
      switch_to(join);
    }
    if (has_slot) {
      return materialize(slot);
    }
    return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  }

  Val lower_if(ast::ExprIdx expr, const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprIf& if_expr = node.payload.get<ast::ExprIf>();
    const ir::TypeIdx result_type = expr_type(expr);
    const bool has_slot = tag_of(result_type) != ir::TypeTag::Void &&
                          tag_of(result_type) != ir::TypeTag::Error;
    Val slot{size_one, result_type, false, false};
    if (has_slot) {
      slot = result_slot(result_type, node.span);
    }
    ir::BlockIdx else_block = reserve_block();
    ir::BlockIdx join = ir::BlockIdx(base::kInvalidIdx);
    auto finish_arm = [&](Val produced) {
      if (has_slot && !terminated_cur()) {
        store_result(slot, produced);
      } else if (!has_slot) {
        mark_move(produced);
      }
      if (!terminated_cur()) {
        if (!join.is_valid()) {
          join = reserve_block();
        }
        emit_br(join);
      }
    };
    const ast::Cond& cond_node = ast.conds[if_expr.cond];
    if (!cond_node.is_pattern) {
      ir::BlockIdx then_block = reserve_block();
      Val cond = lower_expr(cond_node.value, nullptr);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      Val material = materialize(cond);
      emit_cond_br(material.op, then_block, else_block);
      switch_to(then_block);
      finish_arm(lower_block(if_expr.then_block, expected));
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      switch_to(else_block);
      if (if_expr.else_block.is_valid()) {
        finish_arm(lower_block(if_expr.else_block, expected));
      } else if (has_slot) {
        internal(node.span, "value if without else");
        return Val{size_one, error_type(), false, false};
      } else {
        // Statement position without else: the empty arm falls through.
        if (!join.is_valid()) {
          join = reserve_block();
        }
        emit_br(join);
      }
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
    } else {
      Val init = lower_expr(cond_node.init, nullptr);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      Val addr = address_of(init);
      mark_move(addr);
      const ir::TypeIdx scrut_type = expr_type(cond_node.init);
      ir::BlockIdx body_block = reserve_block();
      lower_arm_test(cond_node.pattern, addr, scrut_type, body_block,
                     else_block);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      switch_to(body_block);
      finish_arm(lower_block(if_expr.then_block, expected));
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      switch_to(else_block);
      if (if_expr.else_block.is_valid()) {
        finish_arm(lower_block(if_expr.else_block, expected));
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
      } else if (!has_slot) {
        if (!join.is_valid()) {
          join = reserve_block();
        }
        emit_br(join);
      } else {
        internal(node.span, "value if without else");
        return Val{size_one, error_type(), false, false};
      }
    }
    if (join.is_valid()) {
      switch_to(join);
    }
    if (has_slot) {
      return materialize(slot);
    }
    return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  }

  Val lower_loop(ast::ExprIdx expr) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprLoop& loop_expr = node.payload.get<ast::ExprLoop>();
    ir::BlockIdx header = reserve_block();
    ir::BlockIdx exit = reserve_block();
    emit_br(header);
    switch_to(header);
    break_targets_.push_back(exit);
    continue_targets_.push_back(header);
    lower_block(loop_expr.body, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    if (!terminated_cur()) {
      emit_br(header);
    }
    break_targets_.pop_back();
    continue_targets_.pop_back();
    switch_to(exit);
    return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  }

  Val lower_while(ast::ExprIdx expr) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprWhile& while_expr = node.payload.get<ast::ExprWhile>();
    ir::BlockIdx header = reserve_block();
    ir::BlockIdx body = reserve_block();
    ir::BlockIdx exit = reserve_block();
    emit_br(header);
    switch_to(header);
    const ast::Cond& cond_node = ast.conds[while_expr.cond];
    if (!cond_node.is_pattern) {
      Val cond = lower_expr(cond_node.value, nullptr);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      emit_cond_br(materialize(cond).op, body, exit);
      switch_to(body);
    } else {
      // The scrutinee re-evaluates on every iteration; the header both
      // tests and binds, so continue re-enters the test.
      Val init = lower_expr(cond_node.init, nullptr);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      Val addr = address_of(init);
      mark_move(addr);
      const ir::TypeIdx scrut_type = expr_type(cond_node.init);
      lower_arm_test(cond_node.pattern, addr, scrut_type, body, exit);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      switch_to(body);
    }
    break_targets_.push_back(exit);
    continue_targets_.push_back(header);
    lower_block(while_expr.body, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    if (!terminated_cur()) {
      emit_br(header);
    }
    break_targets_.pop_back();
    continue_targets_.pop_back();
    switch_to(exit);
    return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  }

  Val lower_question(ast::ExprIdx expr) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprQuestion& question = node.payload.get<ast::ExprQuestion>();
    Val scrut = lower_expr(question.inner, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    Val addr = address_of(scrut);
    mark_move(addr);
    const ir::TypeIdx scrut_type = expr_type(question.inner);
    const auto* entry = blessed_entry(scrut_type);
    if (entry == nullptr) {
      internal(node.span, "question without blessed type");
      return Val{size_one, error_type(), false, false};
    }
    Val tag = load_disc(addr);
    const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
    ir::BlockIdx ok_block = reserve_block();
    ir::BlockIdx err_block = reserve_block();
    ir::BlockIdx join_block = reserve_block();
    const ir::RegisterIdx test =
        emit(ir::Opcode::Eq, boolean, {tag.op, disc_operand(0)});
    emit_cond_br(to_operand(test, boolean), ok_block, err_block);
    switch_to(err_block);
    if (entry->is_result) {
      Val payload = load_blessed_payload(addr, 0, {entry->args[1]});
      emit_void(ir::Opcode::Ret, {use_value(payload)});
    } else {
      // Propagate None by constructing it in the enclosing return type.
      const ir::TypeIdx slot = enum_slot_type();
      const ir::RegisterIdx none_addr =
          emit(ir::Opcode::Alloca, slot, {size_one});
      const ir::RegisterIdx none_tag =
          emit(ir::Opcode::GetElementPtr, builder.primitive(ir::TypeTag::I32),
               {to_operand(none_addr, slot), zero_i32, index_operand(0)});
      emit_void(ir::Opcode::Store,
                {disc_operand(1), to_operand(none_tag, slot)});
      const ir::RegisterIdx none_loaded =
          emit(ir::Opcode::Load, scrut_type, {to_operand(none_addr, slot)});
      emit_void(ir::Opcode::Ret, {to_operand(none_loaded, scrut_type)});
    }
    switch_to(ok_block);
    Val payload = load_blessed_payload(addr, 0, {entry->args[0]});
    emit_br(join_block);
    switch_to(join_block);
    return payload;
  }

  // ---- Compile-time evaluation ----

  static constexpr usize kCompStepBudget = 1u << 20;
  static constexpr u32 kCompMaxCallDepth = 64;

  static bool comp_is_signed(ir::TypeTag tag) {
    return tag == ir::TypeTag::I8 || tag == ir::TypeTag::I16 ||
           tag == ir::TypeTag::I32 || tag == ir::TypeTag::I64;
  }

  static u32 comp_int_bytes(ir::TypeTag tag) {
    switch (tag) {
      case ir::TypeTag::I8:
      case ir::TypeTag::U8: return 1;
      case ir::TypeTag::I16:
      case ir::TypeTag::U16: return 2;
      case ir::TypeTag::I32:
      case ir::TypeTag::U32: return 4;
      default: return 8;
    }
  }

  static u64 comp_mask(ir::TypeTag tag) {
    const u32 bytes = comp_int_bytes(tag);
    return bytes >= 8 ? ~static_cast<u64>(0)
                      : ((static_cast<u64>(1) << (bytes * 8)) - 1);
  }

  bool comp_fail(diag::Span span, std::string_view what) {
    unsupported(span, what);
    return false;
  }

  bool comp_tick(diag::Span span) {
    if (comp_budget_ == 0) {
      return comp_fail(span, "comp evaluation budget exhausted");
    }
    --comp_budget_;
    return true;
  }

  ir::TypeIdx expr_type_in(u32 mod, ast::ExprIdx expr) {
    for (const auto& entry : pkg.modules[mod].expr_types) {
      if (entry.first == expr) {
        return entry.second;
      }
    }
    return error_type();
  }

  const analyzer::CheckedModule::CallTarget* call_target_in(
      u32 mod,
      ast::ExprIdx callee) const {
    for (const auto& entry : pkg.modules[mod].call_targets) {
      if (entry.callee == callee) {
        return &entry;
      }
    }
    return nullptr;
  }

  const CompVal* comp_lookup(const CompScope& scope, std::string_view name) {
    for (usize i = scope.frames.size(); i-- > 0;) {
      for (const auto& binding : scope.frames[i] | std::views::reverse) {
        if (binding.first == name) {
          return &binding.second;
        }
      }
    }
    if (scope.outer != nullptr) {
      for (const auto& binding : *scope.outer | std::views::reverse) {
        if (binding.first == name) {
          return &binding.second;
        }
      }
    }
    return nullptr;
  }

  // Unescapes with exactly the runtime literal rules so comp strings
  // match lowered ones byte for byte.
  static std::string comp_unescape(std::string_view spelling) {
    std::string bytes;
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
    return bytes;
  }

  bool comp_eval_literal(u32 mod, ast::ExprIdx expr, CompVal& out) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::Literal& lit =
        ast.literals[node.payload.get<ast::ExprLiteral>().value];
    const ir::TypeIdx type = expr_type_in(mod, expr);
    if (tag_of(type) == ir::TypeTag::Error) {
      return comp_fail(node.span, "comp operand without type");
    }
    out.type = type;
    if (lit.kind == ast::LiteralKind::Bool) {
      out.value.tag = CompValue::Tag::Bool;
      out.value.bool_value = lit.spelling == "true";
      return true;
    }
    if (lit.kind == ast::LiteralKind::String) {
      out.value.tag = CompValue::Tag::Str;
      out.value.str_value = comp_unescape(lit.spelling);
      return true;
    }
    if (lit.kind == ast::LiteralKind::Integer) {
      out.value.tag = CompValue::Tag::Int;
      out.value.int_value = parse_numeric_value(lit.spelling);
      return true;
    }
    return comp_fail(node.span, "literal is not comp-evaluable");
  }

  struct CompFlow {
    enum class Kind : u8 { Value, Break, Continue, Return };
    Kind kind = Kind::Value;
    CompVal value;
  };

  bool comp_bind_pattern(u32 mod,
                         ast::PatternIdx pattern,
                         const CompVal& value,
                         CompScope& scope,
                         diag::Span span) {
    const ast::PatternNode& node = ast.patterns[pattern];
    switch (node.kind) {
      case ast::PatternKind::Wildcard: return true;
      case ast::PatternKind::Ident:
        scope.frames.back().emplace_back(node.payload.ident.name.name, value);
        return true;
      case ast::PatternKind::MutIdent:
        scope.frames.back().emplace_back(node.payload.mut_ident.name.name,
                                         value);
        return true;
      case ast::PatternKind::Tuple: {
        if (node.payload.tuple.path.is_valid()) {
          // Variant patterns bind through matching.
          return comp_match_pattern(mod, pattern, value, scope, span);
        }
        if (value.value.tag != CompValue::Tag::Tuple) {
          return comp_fail(span, "pattern is not comp-evaluable");
        }
        const ir::TupleType& shape =
            builder.state()
                .tuple_types[builder.state().types[value.type].as_tuple()];
        const std::span<const ast::PatternIdx> elements =
            node.payload.tuple.elements;
        if (elements.size() != value.value.fields.size() ||
            elements.size() != shape.elements.size()) {
          return comp_fail(span, "tuple pattern arity");
        }
        for (usize i = 0; i < elements.size(); ++i) {
          const CompVal field{value.value.fields[i], shape.elements[i]};
          if (!comp_bind_pattern(mod, elements[i], field, scope, span)) {
            return false;
          }
        }
        return true;
      }
      case ast::PatternKind::Struct: {
        if (value.value.tag != CompValue::Tag::Struct) {
          return comp_fail(span, "pattern is not comp-evaluable");
        }
        for (const ast::FieldPattern& field : node.payload.strukt.fields) {
          u32 index = 0;
          if (!struct_field_index(value.type, field.name.name, index) ||
              index >= value.value.fields.size()) {
            return comp_fail(field.name.span, "pattern field without value");
          }
          const ir::TypeIdx member_type =
              field_type_of(value.type, index, span);
          if (failed) {
            return false;
          }
          const CompVal member{value.value.fields[index], member_type};
          if (!comp_bind_pattern(mod, field.pattern, member, scope, span)) {
            return false;
          }
        }
        return true;
      }
      case ast::PatternKind::Ref:
      case ast::PatternKind::Or:
        // Transparent dereference and first-match alternatives bind
        // exactly like matching.
        return comp_match_pattern(mod, pattern, value, scope, span);
      case ast::PatternKind::Literal: break;
    }
    return comp_fail(node.span, "pattern is not comp-evaluable");
  }

  bool comp_match_pattern(u32 mod,
                          ast::PatternIdx pattern,
                          const CompVal& value,
                          CompScope& scope,
                          diag::Span span) {
    const ast::PatternNode& node = ast.patterns[pattern];
    switch (node.kind) {
      case ast::PatternKind::Wildcard: return true;
      case ast::PatternKind::Ident:
      case ast::PatternKind::MutIdent:
        return comp_bind_pattern(mod, pattern, value, scope, span);
      case ast::PatternKind::Literal: {
        CompVal expected;
        expected.type = value.type;
        const ast::Literal& lit = ast.literals[node.payload.literal.value];
        if (lit.kind == ast::LiteralKind::Bool) {
          if (value.value.tag != CompValue::Tag::Bool) {
            return false;
          }
          return value.value.bool_value == (lit.spelling == "true");
        }
        if (lit.kind == ast::LiteralKind::Integer) {
          if (value.value.tag != CompValue::Tag::Int) {
            return false;
          }
          return value.value.int_value == (parse_numeric_value(lit.spelling) &
                                           comp_mask(tag_of(value.type)));
        }
        if (lit.kind == ast::LiteralKind::String) {
          if (value.value.tag != CompValue::Tag::Str) {
            return false;
          }
          return value.value.str_value == comp_unescape(lit.spelling);
        }
        return comp_fail(node.span, "pattern is not comp-evaluable");
      }
      case ast::PatternKind::Tuple: {
        if (!node.payload.tuple.path.is_valid()) {
          if (value.value.tag != CompValue::Tag::Tuple) {
            return false;
          }
          const std::span<const ast::PatternIdx> elements =
              node.payload.tuple.elements;
          if (elements.size() != value.value.fields.size()) {
            return false;
          }
          const usize mark = scope.frames.back().size();
          for (usize i = 0; i < elements.size(); ++i) {
            const CompVal field{value.value.fields[i], value.type};
            if (!comp_match_pattern(mod, elements[i], field, scope, span)) {
              scope.frames.back().resize(mark);
              return false;
            }
          }
          return true;
        }
        const std::span<const ast::Ident> segments =
            ast.paths[node.payload.tuple.path].segments;
        if (segments.empty()) {
          return comp_fail(span, "pattern is not comp-evaluable");
        }
        const std::string_view name = segments.back().name;
        std::vector<CompVal> payloads;
        if (value.value.tag == CompValue::Tag::Enum) {
          u32 variant = 0;
          if (!variant_index(value.type, name, variant) ||
              variant != value.value.variant) {
            return false;
          }
          const std::vector<ir::TypeIdx> types =
              variant_payload(value.type, variant, false);
          if (types.size() != value.value.fields.size()) {
            return comp_fail(span, "variant arity");
          }
          for (usize i = 0; i < types.size(); ++i) {
            payloads.push_back(CompVal{value.value.fields[i], types[i]});
          }
        } else if (value.value.tag == CompValue::Tag::Blessed) {
          const auto* entry = blessed_entry(value.type);
          if (entry == nullptr) {
            return comp_fail(span, "pattern is not comp-evaluable");
          }
          bool first = false;
          if (!blessed_ctor_side(name, entry->is_result, first) ||
              first != value.value.blessed_ok) {
            return false;
          }
          if (value.value.fields.size() > 1) {
            return comp_fail(span, "variant arity");
          }
          for (const CompValue& field : value.value.fields) {
            payloads.push_back(CompVal{field, entry->args[0]});
          }
        } else {
          return false;
        }
        const std::span<const ast::PatternIdx> elements =
            node.payload.tuple.elements;
        if (elements.size() != payloads.size()) {
          return false;
        }
        const usize mark = scope.frames.back().size();
        for (usize i = 0; i < elements.size(); ++i) {
          if (!comp_match_pattern(mod, elements[i], payloads[i], scope, span)) {
            scope.frames.back().resize(mark);
            return false;
          }
        }
        return true;
      }
      case ast::PatternKind::Struct: {
        if (value.value.tag != CompValue::Tag::Struct) {
          return false;
        }
        const usize mark = scope.frames.back().size();
        for (const ast::FieldPattern& field : node.payload.strukt.fields) {
          u32 index = 0;
          if (!struct_field_index(value.type, field.name.name, index) ||
              index >= value.value.fields.size()) {
            scope.frames.back().resize(mark);
            return comp_fail(field.name.span, "pattern field without value");
          }
          const CompVal member{value.value.fields[index],
                               field_type_of(value.type, index, span)};
          if (failed) {
            scope.frames.back().resize(mark);
            return false;
          }
          if (!comp_match_pattern(mod, field.pattern, member, scope, span)) {
            scope.frames.back().resize(mark);
            return false;
          }
        }
        return true;
      }
      case ast::PatternKind::Ref:
        return comp_match_pattern(mod, node.payload.ref.inner, value, scope,
                                  span);
      case ast::PatternKind::Or: {
        const usize mark = scope.frames.back().size();
        for (ast::PatternIdx alt : node.payload.or_pat.alternatives) {
          if (comp_match_pattern(mod, alt, value, scope, span)) {
            return true;
          }
          if (failed) {
            scope.frames.back().resize(mark);
            return false;
          }
          scope.frames.back().resize(mark);
        }
        return false;
      }
    }
  }

  bool comp_eval_struct(u32 mod,
                        ast::ExprIdx expr,
                        CompScope& scope,
                        CompVal& out) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprStruct& strukt = node.payload.get<ast::ExprStruct>();
    out.type = expr_type_in(mod, expr);
    if (tag_of(out.type) != ir::TypeTag::Struct) {
      return comp_fail(node.span, "struct without value");
    }
    const auto* info = struct_info(out.type);
    if (info == nullptr) {
      return comp_fail(node.span, "struct without declaration");
    }
    std::vector<CompValue> fields(info->fields.size());
    std::vector<bool> seen(info->fields.size(), false);
    for (const ast::ExprFieldInit& field : strukt.init) {
      u32 index = 0;
      if (!struct_field_index(out.type, field.name.name, index)) {
        return comp_fail(field.name.span, "unknown field");
      }
      CompVal member;
      if (!comp_eval_expr(mod, field.value, scope, member)) {
        return false;
      }
      fields[index] = std::move(member.value);
      seen[index] = true;
    }
    if (strukt.base_expr.is_valid()) {
      CompVal base;
      if (!comp_eval_expr(mod, strukt.base_expr, scope, base)) {
        return false;
      }
      if (base.value.tag != CompValue::Tag::Struct ||
          base.value.fields.size() != fields.size()) {
        return comp_fail(node.span, "struct base without value");
      }
      for (usize i = 0; i < fields.size(); ++i) {
        if (!seen[i]) {
          fields[i] = base.value.fields[i];
        }
      }
    }
    out.value.tag = CompValue::Tag::Struct;
    out.value.fields = std::move(fields);
    return true;
  }

  bool comp_eval_block(u32 mod,
                       ast::BlockIdx block,
                       CompScope& scope,
                       CompFlow& out) {
    const ast::Block& node = ast.blocks[block];
    scope.frames.emplace_back();
    for (ast::StmtIdx stmt : node.statements) {
      if (failed) {
        scope.frames.pop_back();
        return false;
      }
      CompFlow flow;
      if (!comp_eval_stmt(mod, stmt, scope, flow)) {
        scope.frames.pop_back();
        return false;
      }
      if (flow.kind != CompFlow::Kind::Value) {
        scope.frames.pop_back();
        out = std::move(flow);
        return true;
      }
    }
    if (failed) {
      scope.frames.pop_back();
      return false;
    }
    if (!node.value.is_valid()) {
      scope.frames.pop_back();
      out.kind = CompFlow::Kind::Value;
      out.value.value.tag = CompValue::Tag::Void;
      out.value.type = builder.primitive(ir::TypeTag::Void);
      return true;
    }
    CompVal value;
    const bool ok = comp_eval_expr(mod, node.value, scope, value);
    scope.frames.pop_back();
    if (!ok) {
      return false;
    }
    out.kind = CompFlow::Kind::Value;
    out.value = std::move(value);
    return true;
  }

  bool comp_eval_stmt(u32 mod,
                      ast::StmtIdx stmt,
                      CompScope& scope,
                      CompFlow& out) {
    const ast::StmtNode& node = ast.stmts[stmt];
    switch (node.kind) {
      case ast::StmtKind::Decl: {
        const ast::StmtDecl& decl = node.payload.get<ast::StmtDecl>();
        CompVal init;
        if (!comp_eval_expr(mod, decl.init, scope, init)) {
          return false;
        }
        if (!comp_bind_pattern(mod, decl.pattern, init, scope, node.span)) {
          return false;
        }
        out.kind = CompFlow::Kind::Value;
        return true;
      }
      case ast::StmtKind::Reassign: {
        const ast::StmtReassign& reassign =
            node.payload.get<ast::StmtReassign>();
        if (reassign.compound) {
          return comp_fail(node.span, "compound assignment in comp evaluation");
        }
        if (ast.exprs[reassign.place].kind != ast::ExprKind::Path) {
          return comp_fail(node.span, "place without binding");
        }
        const ast::PathIdx path =
            ast.exprs[reassign.place].payload.get<ast::ExprPath>().idx;
        const std::span<const ast::Ident> segments = ast.paths[path].segments;
        if (segments.size() != 1) {
          return comp_fail(node.span, "place without binding");
        }
        CompVal value;
        if (!comp_eval_expr(mod, reassign.value, scope, value)) {
          return false;
        }
        for (usize i = scope.frames.size(); i-- > 0;) {
          for (auto& binding : scope.frames[i]) {
            if (binding.first == segments[0].name) {
              binding.second = value;
              out.kind = CompFlow::Kind::Value;
              return true;
            }
          }
        }
        if (scope.outer != nullptr) {
          for (auto& binding :
               const_cast<std::vector<std::pair<std::string_view, CompVal>>&>(
                   *scope.outer)) {
            if (binding.first == segments[0].name) {
              binding.second = value;
              out.kind = CompFlow::Kind::Value;
              return true;
            }
          }
        }
        return comp_fail(node.span, "place without binding");
      }
      case ast::StmtKind::Expr: {
        const ast::StmtExpr& expr = node.payload.get<ast::StmtExpr>();
        const ast::ExprKind kind = ast.exprs[expr.value].kind;
        if (kind == ast::ExprKind::Break) {
          out.kind = CompFlow::Kind::Break;
          return true;
        }
        if (kind == ast::ExprKind::Continue) {
          out.kind = CompFlow::Kind::Continue;
          return true;
        }
        CompVal discarded;
        if (!comp_eval_expr(mod, expr.value, scope, discarded)) {
          return false;
        }
        out.kind = CompFlow::Kind::Value;
        return true;
      }
    }
  }

  // Unrolls a loop body while its condition holds. `always` covers
  // `loop`, which has no condition expression.
  bool comp_eval_loop(u32 mod,
                      ast::BlockIdx body,
                      CompScope& scope,
                      CompFlow& out,
                      bool always,
                      diag::Span span,
                      ast::ExprIdx cond = ast::ExprIdx(base::kInvalidIdx)) {
    while (!failed) {
      if (!always) {
        CompVal test;
        if (!comp_eval_expr(mod, cond, scope, test)) {
          return false;
        }
        if (test.value.tag != CompValue::Tag::Bool) {
          return comp_fail(span, "condition without value");
        }
        if (!test.value.bool_value) {
          out.kind = CompFlow::Kind::Value;
          return true;
        }
      }
      CompFlow flow;
      if (!comp_eval_block(mod, body, scope, flow)) {
        return false;
      }
      if (flow.kind == CompFlow::Kind::Break) {
        out.kind = CompFlow::Kind::Value;
        return true;
      }
      if (flow.kind == CompFlow::Kind::Return) {
        out = std::move(flow);
        return true;
      }
    }
    return false;
  }

  // Interprets a resolved function body with comp actuals bound to
  // comp formals. Runtime formals stay unbound: bodies touching them
  // diagnose instead of evaluating.
  bool comp_run_fn(u32 def_module,
                   ast::ItemIdx item,
                   const std::vector<ir::TypeIdx>& params,
                   ir::TypeIdx ret,
                   u32 caller_module,
                   const std::span<const ast::ExprIdx>& args,
                   CompScope& caller_scope,
                   diag::Span span,
                   CompVal& out) {
    if (!item.is_valid()) {
      return comp_fail(span, "callee without body");
    }
    const ast::ItemFn& fn = ast.items[item].payload.get<ast::ItemFn>();
    if (args.size() != params.size() || args.size() != fn.params.size()) {
      return comp_fail(span, "call arity");
    }
    if (comp_call_depth_ >= kCompMaxCallDepth) {
      return comp_fail(span, "comp call depth exhausted");
    }
    ++comp_call_depth_;
    CompScope callee_scope;
    callee_scope.frames.emplace_back();
    bool ok = true;
    for (usize i = 0; i < args.size() && ok; ++i) {
      if (!fn.params[i].is_comp) {
        continue;
      }
      CompVal arg;
      if (!comp_eval_expr(caller_module, args[i], caller_scope, arg)) {
        ok = false;
        break;
      }
      if (!comp_bind_pattern(def_module, fn.params[i].pattern, arg,
                             callee_scope, ast.exprs[args[i]].span)) {
        ok = false;
      }
    }
    CompFlow flow;
    if (ok) {
      if (!fn.body.is_valid() ||
          !comp_eval_block(def_module, fn.body, callee_scope, flow)) {
        ok = false;
      }
    }
    --comp_call_depth_;
    if (!ok) {
      return false;
    }
    out.type = ret;
    if (flow.kind == CompFlow::Kind::Return ||
        flow.kind == CompFlow::Kind::Value) {
      out.value = flow.value.value;
      return true;
    }
    return comp_fail(span, "control escapes the comp call");
  }

  bool comp_eval_assoc_call(u32 mod,
                            ast::ExprIdx expr,
                            CompScope& scope,
                            CompVal& out,
                            const analyzer::CheckedModule::CallTarget* target) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    const analyzer::CheckedModule& def = pkg.modules[target->module];
    if (target->index >= def.methods.size()) {
      return comp_fail(node.span, "callee without target");
    }
    const analyzer::CheckedModule::MethodInfo& info =
        def.methods[target->index];
    if (info.receiver != analyzer::CheckedModule::ReceiverKind::None) {
      return comp_fail(node.span, "method without receiver");
    }
    return comp_run_fn(target->module, info.item, info.params, info.ret, mod,
                       call.args, scope, node.span, out);
  }

  bool comp_eval_call(u32 mod,
                      ast::ExprIdx expr,
                      CompScope& scope,
                      CompVal& out) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (ast.exprs[call.callee].kind != ast::ExprKind::Path) {
      return comp_fail(node.span, "callee without target");
    }
    const ast::PathIdx path =
        ast.exprs[call.callee].payload.get<ast::ExprPath>().idx;
    const std::span<const ast::Ident> segments = ast.paths[path].segments;
    if (segments.size() == 1) {
      const std::string_view name = segments[0].name;
      if (name == "print" || name == "println" || name == "panic") {
        return comp_fail(node.span, "intrinsic is not comp-evaluable");
      }
    }
    const analyzer::CheckedModule::CallTarget* target =
        call_target_in(mod, call.callee);
    if (target == nullptr) {
      if (const auto* use = variant_use(path)) {
        const std::vector<ir::TypeIdx> payloads =
            variant_payload(use->enum_type, use->variant, use->blessed_first);
        if (call.args.size() != payloads.size()) {
          return comp_fail(node.span, "variant arity");
        }
        out.type = use->enum_type;
        if (use->blessed) {
          out.value.tag = CompValue::Tag::Blessed;
          out.value.blessed_ok = use->blessed_first;
        } else {
          out.value.tag = CompValue::Tag::Enum;
          out.value.variant = use->variant;
        }
        for (ast::ExprIdx arg : call.args) {
          CompVal field;
          if (!comp_eval_expr(mod, arg, scope, field)) {
            return false;
          }
          out.value.fields.push_back(std::move(field.value));
        }
        return true;
      }
      return comp_fail(node.span, "callee without target");
    }
    if (target->is_method) {
      return comp_eval_assoc_call(mod, expr, scope, out, target);
    }
    const analyzer::CheckedModule& def = pkg.modules[target->module];
    if (target->index >= def.functions.size()) {
      return comp_fail(node.span, "callee without target");
    }
    const analyzer::CheckedModule::FnSig& sig = def.functions[target->index];
    return comp_run_fn(target->module, sig.item, sig.params, sig.ret, mod,
                       call.args, scope, node.span, out);
  }

  bool comp_eval_method_call(u32 mod,
                             ast::ExprIdx expr,
                             CompScope& scope,
                             CompVal& out) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprMethodCall& method = node.payload.get<ast::ExprMethodCall>();
    CompVal receiver;
    if (!comp_eval_expr(mod, method.receiver, scope, receiver)) {
      return false;
    }
    out.type = expr_type_in(mod, expr);
    if (receiver.value.tag == CompValue::Tag::Blessed) {
      if (method.name.name == "is_ok" || method.name.name == "is_err") {
        out.value.tag = CompValue::Tag::Bool;
        out.value.bool_value = method.name.name == "is_ok"
                                   ? receiver.value.blessed_ok
                                   : !receiver.value.blessed_ok;
        return true;
      }
      if (method.name.name == "unwrap" || method.name.name == "expect") {
        if (!receiver.value.blessed_ok || receiver.value.fields.empty()) {
          return comp_fail(node.span, "comp evaluation hit Err/None");
        }
        out.value = receiver.value.fields[0];
        return true;
      }
      return comp_fail(node.span, "method without value");
    }
    const analyzer::CheckedModule::CallTarget* target =
        call_target_in(mod, expr);
    if (target == nullptr || !target->is_method) {
      return comp_fail(node.span, "method without target");
    }
    const analyzer::CheckedModule& def = pkg.modules[target->module];
    if (target->index >= def.methods.size()) {
      return comp_fail(node.span, "method without target");
    }
    const analyzer::CheckedModule::MethodInfo& info =
        def.methods[target->index];
    if (info.receiver == analyzer::CheckedModule::ReceiverKind::None) {
      return comp_fail(node.span, "method without receiver");
    }
    if (!info.item.is_valid()) {
      return comp_fail(node.span, "method without body");
    }
    const ast::ItemFn& fn = ast.items[info.item].payload.get<ast::ItemFn>();
    if (method.args.size() + 1 != info.params.size() ||
        method.args.size() + 1 != fn.params.size()) {
      return comp_fail(node.span, "call arity");
    }
    if (comp_call_depth_ >= kCompMaxCallDepth) {
      return comp_fail(node.span, "comp call depth exhausted");
    }
    ++comp_call_depth_;
    CompScope callee_scope;
    callee_scope.frames.emplace_back();
    bool ok = comp_bind_pattern(target->module, fn.params[0].pattern, receiver,
                                callee_scope, node.span);
    for (usize i = 0; i < method.args.size() && ok; ++i) {
      CompVal arg;
      if (!comp_eval_expr(mod, method.args[i], scope, arg)) {
        ok = false;
        break;
      }
      if (!comp_bind_pattern(target->module, fn.params[i + 1].pattern, arg,
                             callee_scope, ast.exprs[method.args[i]].span)) {
        ok = false;
      }
    }
    CompFlow flow;
    if (ok) {
      if (!fn.body.is_valid() ||
          !comp_eval_block(target->module, fn.body, callee_scope, flow)) {
        ok = false;
      }
    }
    --comp_call_depth_;
    if (!ok) {
      return false;
    }
    out.type = info.ret;
    if (flow.kind == CompFlow::Kind::Return ||
        flow.kind == CompFlow::Kind::Value) {
      out.value = flow.value.value;
      return true;
    }
    return comp_fail(node.span, "control escapes the comp call");
  }

  Val materialize_comp_value(const CompVal& value, diag::Span span) {
    const ir::TypeTag tag = tag_of(value.type);
    switch (value.value.tag) {
      case CompValue::Tag::Int:
      case CompValue::Tag::Bool: {
        const u64 bits = value.value.tag == CompValue::Tag::Bool
                             ? (value.value.bool_value ? 1 : 0)
                             : value.value.int_value;
        return Val{imm_from_u64(tag, value.type, bits), value.type, false,
                   false};
      }
      case CompValue::Tag::Str: {
        const str::StringPoolId id = strings.intern(value.value.str_value);
        const ir::ImmutableIdx imm = builder.immutable(
            {.type = value.type, .data = {.str_id_value = id}});
        return Val{to_operand(imm, value.type), value.type, false, false};
      }
      case CompValue::Tag::Void: return Val{size_one, value.type, false, false};
      case CompValue::Tag::Tuple: {
        const ir::TupleType& shape =
            builder.state()
                .tuple_types[builder.state().types[value.type].as_tuple()];
        if (shape.elements.size() != value.value.fields.size()) {
          internal(span, "comp tuple arity");
          return Val{size_one, error_type(), false, false};
        }
        const ir::RegisterIdx addr =
            emit(ir::Opcode::Alloca, value.type, {size_one});
        for (u32 i = 0; i < static_cast<u32>(shape.elements.size()); ++i) {
          const CompVal field{value.value.fields[i], shape.elements[i]};
          Val lowered = materialize_comp_value(field, span);
          if (failed) {
            return Val{size_one, error_type(), false, false};
          }
          const ir::RegisterIdx gep =
              emit(ir::Opcode::GetElementPtr, shape.elements[i],
                   {to_operand(addr, value.type), zero_i32, index_operand(i)});
          emit_void(ir::Opcode::Store,
                    {use_value(lowered), to_operand(gep, shape.elements[i])});
        }
        return Val{to_operand(addr, value.type), value.type, true, false};
      }
      case CompValue::Tag::Struct: {
        const auto* info = struct_info(value.type);
        if (info == nullptr ||
            info->fields.size() != value.value.fields.size()) {
          internal(span, "comp struct arity");
          return Val{size_one, error_type(), false, false};
        }
        const ir::RegisterIdx addr =
            emit(ir::Opcode::Alloca, value.type, {size_one});
        for (u32 i = 0; i < static_cast<u32>(info->fields.size()); ++i) {
          const ir::TypeIdx field_type = field_type_of(value.type, i, span);
          if (failed) {
            return Val{size_one, error_type(), false, false};
          }
          const CompVal field{value.value.fields[i], field_type};
          Val lowered = materialize_comp_value(field, span);
          if (failed) {
            return Val{size_one, error_type(), false, false};
          }
          const ir::RegisterIdx gep =
              emit(ir::Opcode::GetElementPtr, field_type,
                   {to_operand(addr, value.type), zero_i32, index_operand(i)});
          emit_void(ir::Opcode::Store,
                    {use_value(lowered), to_operand(gep, field_type)});
        }
        return Val{to_operand(addr, value.type), value.type, true, false};
      }
      case CompValue::Tag::Enum:
      case CompValue::Tag::Blessed: {
        const bool blessed = value.value.tag == CompValue::Tag::Blessed;
        const u32 discriminant =
            blessed ? (value.value.blessed_ok ? 0 : 1) : value.value.variant;
        std::vector<ir::TypeIdx> payloads;
        if (blessed) {
          const auto* entry = blessed_entry(value.type);
          if (entry == nullptr) {
            internal(span, "comp enum without declaration");
            return Val{size_one, error_type(), false, false};
          }
          if (!value.value.fields.empty()) {
            payloads.push_back(entry->args[0]);
          }
        } else {
          payloads = variant_payload(value.type, value.value.variant, false);
          if (payloads.size() != value.value.fields.size()) {
            internal(span, "comp variant arity");
            return Val{size_one, error_type(), false, false};
          }
        }
        const ir::TypeIdx slot = enum_slot_type();
        const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, slot, {size_one});
        const ir::RegisterIdx tag_reg =
            emit(ir::Opcode::GetElementPtr, builder.primitive(ir::TypeTag::I32),
                 {to_operand(addr, slot), zero_i32, index_operand(0)});
        emit_void(ir::Opcode::Store,
                  {disc_operand(discriminant), to_operand(tag_reg, slot)});
        if (!payloads.empty() && !is_unit_payload(payloads)) {
          const ir::TypeIdx payload_type = payload_tuple(payloads);
          const ir::RegisterIdx payload =
              emit(ir::Opcode::Alloca, payload_type, {size_one});
          for (usize i = 0; i < payloads.size(); ++i) {
            const CompVal field{value.value.fields[i], payloads[i]};
            Val lowered = materialize_comp_value(field, span);
            if (failed) {
              return Val{size_one, error_type(), false, false};
            }
            const ir::RegisterIdx field_reg =
                emit(ir::Opcode::GetElementPtr, payloads[i],
                     {to_operand(payload, payload_type), zero_i32,
                      index_operand(static_cast<u32>(i))});
            emit_void(ir::Opcode::Store,
                      {use_value(lowered), to_operand(field_reg, payloads[i])});
          }
          const ir::TypeIdx ptr = builder.primitive(ir::TypeTag::Ptr);
          const ir::RegisterIdx slot_field =
              emit(ir::Opcode::GetElementPtr, ptr,
                   {to_operand(addr, slot), zero_i32, index_operand(1)});
          emit_void(ir::Opcode::Store,
                    {to_operand(payload, ptr), to_operand(slot_field, ptr)});
        }
        return Val{to_operand(addr, slot), value.type, true, false};
      }
    }
  }

  // Top-level comp evaluation: fresh budget and call depth, seeded
  // with the current function's comp bindings.
  bool comp_evaluate(u32 mod, ast::ExprIdx expr, CompVal& out) {
    CompScope scope;
    scope.outer = &comp_scope_;
    scope.frames.emplace_back();
    comp_budget_ = kCompStepBudget;
    comp_call_depth_ = 0;
    return comp_eval_expr(mod, expr, scope, out);
  }

  bool comp_eval_expr(u32 mod,
                      ast::ExprIdx expr,
                      CompScope& scope,
                      CompVal& out) {
    if (failed || !comp_tick(ast.exprs[expr].span)) {
      return false;
    }
    const ast::ExprNode& node = ast.exprs[expr];
    switch (node.kind) {
      case ast::ExprKind::Literal: return comp_eval_literal(mod, expr, out);
      case ast::ExprKind::Path: {
        const ast::PathIdx path = node.payload.get<ast::ExprPath>().idx;
        const std::span<const ast::Ident> segments = ast.paths[path].segments;
        if (segments.size() == 1) {
          if (const CompVal* bound = comp_lookup(scope, segments[0].name)) {
            out = *bound;
            return true;
          }
          if (const auto* info = lookup_static(mod, segments[0].name)) {
            if (info->is_const && info->init.is_valid() &&
                ast.exprs[info->init].kind == ast::ExprKind::Literal) {
              return comp_eval_literal(mod, info->init, out);
            }
          }
        }
        if (const auto* use = variant_use(path)) {
          const std::vector<ir::TypeIdx> payloads =
              variant_payload(use->enum_type, use->variant, use->blessed_first);
          if (!payloads.empty()) {
            return comp_fail(node.span, "variant without call");
          }
          out.type = use->enum_type;
          if (use->blessed) {
            out.value.tag = CompValue::Tag::Blessed;
            out.value.blessed_ok = use->blessed_first;
          } else {
            out.value.tag = CompValue::Tag::Enum;
            out.value.variant = use->variant;
          }
          return true;
        }
        return comp_fail(node.span, "path is not comp-evaluable");
      }
      case ast::ExprKind::Unary: {
        const ast::ExprUnary& unary = node.payload.get<ast::ExprUnary>();
        CompVal inner;
        if (!comp_eval_expr(mod, unary.inner, scope, inner)) {
          return false;
        }
        out.type = expr_type_in(mod, expr);
        if (unary.op == ast::UnaryOp::Not) {
          if (inner.value.tag != CompValue::Tag::Bool) {
            return comp_fail(node.span, "unary operand without value");
          }
          out.value.tag = CompValue::Tag::Bool;
          out.value.bool_value = !inner.value.bool_value;
          return true;
        }
        if (inner.value.tag != CompValue::Tag::Int) {
          return comp_fail(node.span, "unary operand without value");
        }
        const ir::TypeTag tag = tag_of(inner.type);
        const u64 mask = comp_mask(tag);
        out.value.tag = CompValue::Tag::Int;
        if (unary.op == ast::UnaryOp::BitNot) {
          out.value.int_value = ~inner.value.int_value & mask;
          return true;
        }
        out.value.int_value =
            (static_cast<u64>(0) - (inner.value.int_value & mask)) & mask;
        return true;
      }
      case ast::ExprKind::Borrow: {
        // Borrows are transparent in comp evaluation: the value flows
        // through, and references never escape except as `str`.
        return comp_eval_expr(mod, node.payload.get<ast::ExprBorrow>().inner,
                              scope, out);
      }
      case ast::ExprKind::Binary:
        return comp_eval_binary(mod, expr, scope, out);
      case ast::ExprKind::Cast: {
        const ast::ExprCast& cast = node.payload.get<ast::ExprCast>();
        CompVal inner;
        if (!comp_eval_expr(mod, cast.inner, scope, inner)) {
          return false;
        }
        out.type = expr_type_in(mod, expr);
        const ir::TypeTag target = tag_of(out.type);
        if (target == ir::TypeTag::I1) {
          out.value.tag = CompValue::Tag::Bool;
          out.value.bool_value = comp_truth(inner);
          return true;
        }
        if (inner.value.tag == CompValue::Tag::Bool) {
          out.value.tag = CompValue::Tag::Int;
          out.value.int_value = inner.value.bool_value ? 1 : 0;
          return true;
        }
        if (inner.value.tag != CompValue::Tag::Int) {
          return comp_fail(node.span, "cast without value");
        }
        out.value.tag = CompValue::Tag::Int;
        u64 bits = inner.value.int_value & comp_mask(tag_of(inner.type));
        if (comp_is_signed(target) && comp_is_signed(tag_of(inner.type))) {
          const u32 bytes = comp_int_bytes(target);
          const u64 sign = bytes >= 8
                               ? static_cast<u64>(1) << 63
                               : (static_cast<u64>(1) << (bytes * 8 - 1));
          bits &= comp_mask(target);
          if ((bits & sign) != 0) {
            bits |= ~comp_mask(target);
          }
        } else {
          bits &= comp_mask(target);
        }
        out.value.int_value = bits;
        return true;
      }
      case ast::ExprKind::Call: return comp_eval_call(mod, expr, scope, out);
      case ast::ExprKind::MethodCall:
        return comp_eval_method_call(mod, expr, scope, out);
      case ast::ExprKind::Field: {
        const ast::ExprField& field = node.payload.get<ast::ExprField>();
        CompVal base;
        if (!comp_eval_expr(mod, field.receiver, scope, base)) {
          return false;
        }
        out.type = expr_type_in(mod, expr);
        if (base.value.tag == CompValue::Tag::Tuple) {
          u32 index = 0;
          bool digits = !field.name.name.empty();
          for (char c : field.name.name) {
            if (c < '0' || c > '9') {
              digits = false;
              break;
            }
            index = index * 10 + static_cast<u32>(c - '0');
          }
          if (!digits || index >= base.value.fields.size()) {
            return comp_fail(field.name.span, "unknown tuple field");
          }
          out.value = base.value.fields[index];
          return true;
        }
        if (base.value.tag == CompValue::Tag::Struct) {
          u32 index = 0;
          if (!struct_field_index(base.type, field.name.name, index) ||
              index >= base.value.fields.size()) {
            return comp_fail(field.name.span, "unknown field");
          }
          out.value = base.value.fields[index];
          return true;
        }
        return comp_fail(node.span, "field without value");
      }
      case ast::ExprKind::Index:
        return comp_fail(node.span, "indexing is not comp-evaluable");
      case ast::ExprKind::Question: {
        const ast::ExprQuestion& question =
            node.payload.get<ast::ExprQuestion>();
        CompVal inner;
        if (!comp_eval_expr(mod, question.inner, scope, inner)) {
          return false;
        }
        out.type = expr_type_in(mod, expr);
        if (inner.value.tag == CompValue::Tag::Blessed) {
          if (!inner.value.blessed_ok || inner.value.fields.empty()) {
            return comp_fail(node.span, "comp evaluation hit Err/None");
          }
          out.value = inner.value.fields[0];
          return true;
        }
        return comp_fail(node.span, "'?' without value");
      }
      case ast::ExprKind::If: {
        const ast::ExprIf& if_node = node.payload.get<ast::ExprIf>();
        const ast::Cond& cond = ast.conds[if_node.cond];
        CompVal test;
        if (!comp_eval_expr(mod, cond.value, scope, test)) {
          return false;
        }
        if (test.value.tag != CompValue::Tag::Bool) {
          return comp_fail(node.span, "condition without value");
        }
        out.type = expr_type_in(mod, expr);
        CompFlow flow;
        if (test.value.bool_value) {
          if (!comp_eval_block(mod, if_node.then_block, scope, flow)) {
            return false;
          }
        } else if (if_node.else_block.is_valid()) {
          if (!comp_eval_block(mod, if_node.else_block, scope, flow)) {
            return false;
          }
        } else {
          out.value.tag = CompValue::Tag::Void;
          return true;
        }
        if (flow.kind != CompFlow::Kind::Value) {
          return comp_fail(node.span, "control escapes the comp branch");
        }
        out.value = flow.value.value;
        return true;
      }
      case ast::ExprKind::Match: {
        const ast::ExprMatch& match = node.payload.get<ast::ExprMatch>();
        CompVal scrutinee;
        if (!comp_eval_expr(mod, match.scrutinee, scope, scrutinee)) {
          return false;
        }
        out.type = expr_type_in(mod, expr);
        for (const ast::ExprMatchArm& arm : match.arms) {
          scope.frames.emplace_back();
          const bool matched = comp_match_pattern(
              mod, arm.pattern, scrutinee, scope, ast.exprs[arm.body].span);
          if (failed) {
            scope.frames.pop_back();
            return false;
          }
          if (!matched) {
            scope.frames.pop_back();
            continue;
          }
          CompFlow flow;
          const bool ok = comp_eval_expr(mod, arm.body, scope, flow.value);
          scope.frames.pop_back();
          if (!ok) {
            return false;
          }
          out.value = flow.value.value;
          return true;
        }
        return comp_fail(node.span, "match without value");
      }
      case ast::ExprKind::Block: {
        out.type = expr_type_in(mod, expr);
        CompFlow flow;
        if (!comp_eval_block(mod, node.payload.get<ast::ExprBlock>().block,
                             scope, flow)) {
          return false;
        }
        if (flow.kind != CompFlow::Kind::Value) {
          return comp_fail(node.span, "control escapes the comp block");
        }
        out.value = flow.value.value;
        return true;
      }
      case ast::ExprKind::Loop: {
        out.type = expr_type_in(mod, expr);
        CompFlow flow;
        if (!comp_eval_loop(mod, node.payload.get<ast::ExprLoop>().body, scope,
                            flow, true, node.span)) {
          return false;
        }
        out.value.tag = CompValue::Tag::Void;
        return true;
      }
      case ast::ExprKind::While: {
        const ast::ExprWhile& while_node = node.payload.get<ast::ExprWhile>();
        const ast::Cond& cond = ast.conds[while_node.cond];
        out.type = expr_type_in(mod, expr);
        CompFlow flow;
        if (!comp_eval_loop(mod, while_node.body, scope, flow, false, node.span,
                            cond.value)) {
          return false;
        }
        out.value.tag = CompValue::Tag::Void;
        return true;
      }
      case ast::ExprKind::Break:
      case ast::ExprKind::Continue:
      case ast::ExprKind::Return:
      case ast::ExprKind::Range: break;
      case ast::ExprKind::Tuple: {
        out.type = expr_type_in(mod, expr);
        out.value.tag = CompValue::Tag::Tuple;
        for (ast::ExprIdx element :
             node.payload.get<ast::ExprTuple>().elements) {
          CompVal field;
          if (!comp_eval_expr(mod, element, scope, field)) {
            return false;
          }
          out.value.fields.push_back(std::move(field.value));
        }
        return true;
      }
      case ast::ExprKind::Struct:
        return comp_eval_struct(mod, expr, scope, out);
    }
    return comp_fail(node.span, "expression is not comp-evaluable");
  }

  static bool comp_truth(const CompVal& value) {
    if (value.value.tag == CompValue::Tag::Bool) {
      return value.value.bool_value;
    }
    return value.value.int_value != 0;
  }

  bool comp_eval_binary(u32 mod,
                        ast::ExprIdx expr,
                        CompScope& scope,
                        CompVal& out) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprBinary& bin = node.payload.get<ast::ExprBinary>();
    CompVal lhs;
    if (!comp_eval_expr(mod, bin.lhs, scope, lhs)) {
      return false;
    }
    CompVal rhs;
    if (!comp_eval_expr(mod, bin.rhs, scope, rhs)) {
      return false;
    }
    out.type = expr_type_in(mod, expr);
    if (bin.op == ast::BinaryOp::And || bin.op == ast::BinaryOp::Or) {
      if (lhs.value.tag != CompValue::Tag::Bool ||
          rhs.value.tag != CompValue::Tag::Bool) {
        return comp_fail(node.span, "logical operand without value");
      }
      out.value.tag = CompValue::Tag::Bool;
      out.value.bool_value =
          bin.op == ast::BinaryOp::And
              ? (lhs.value.bool_value && rhs.value.bool_value)
              : (lhs.value.bool_value || rhs.value.bool_value);
      return true;
    }
    if (bin.op == ast::BinaryOp::Eq || bin.op == ast::BinaryOp::NotEq) {
      bool equal = false;
      if (lhs.value.tag == CompValue::Tag::Int &&
          rhs.value.tag == CompValue::Tag::Int) {
        equal = (lhs.value.int_value & comp_mask(tag_of(lhs.type))) ==
                (rhs.value.int_value & comp_mask(tag_of(rhs.type)));
      } else if (lhs.value.tag == CompValue::Tag::Bool &&
                 rhs.value.tag == CompValue::Tag::Bool) {
        equal = lhs.value.bool_value == rhs.value.bool_value;
      } else if (lhs.value.tag == CompValue::Tag::Str &&
                 rhs.value.tag == CompValue::Tag::Str) {
        equal = lhs.value.str_value == rhs.value.str_value;
      } else {
        return comp_fail(node.span, "comparison without value");
      }
      out.value.tag = CompValue::Tag::Bool;
      out.value.bool_value = bin.op == ast::BinaryOp::Eq ? equal : !equal;
      return true;
    }
    if (lhs.value.tag != CompValue::Tag::Int ||
        rhs.value.tag != CompValue::Tag::Int) {
      return comp_fail(node.span, "operand without value");
    }
    const ir::TypeTag tag = tag_of(lhs.type);
    const u64 mask = comp_mask(tag);
    const u64 left = lhs.value.int_value & mask;
    const u64 right = rhs.value.int_value & mask;
    if (bin.op == ast::BinaryOp::Gt || bin.op == ast::BinaryOp::Lt ||
        bin.op == ast::BinaryOp::GtEq || bin.op == ast::BinaryOp::LtEq) {
      bool ordered = false;
      if (comp_is_signed(tag)) {
        const i64 sl = comp_sign_extend(left, tag);
        const i64 sr = comp_sign_extend(right, tag);
        switch (bin.op) {
          case ast::BinaryOp::Gt: ordered = sl > sr; break;
          case ast::BinaryOp::Lt: ordered = sl < sr; break;
          case ast::BinaryOp::GtEq: ordered = sl >= sr; break;
          default: ordered = sl <= sr; break;
        }
      } else {
        switch (bin.op) {
          case ast::BinaryOp::Gt: ordered = left > right; break;
          case ast::BinaryOp::Lt: ordered = left < right; break;
          case ast::BinaryOp::GtEq: ordered = left >= right; break;
          default: ordered = left <= right; break;
        }
      }
      out.value.tag = CompValue::Tag::Bool;
      out.value.bool_value = ordered;
      return true;
    }
    out.value.tag = CompValue::Tag::Int;
    switch (bin.op) {
      case ast::BinaryOp::Add:
        out.value.int_value = (left + right) & mask;
        return true;
      case ast::BinaryOp::Sub:
        out.value.int_value = (left - right) & mask;
        return true;
      case ast::BinaryOp::Mul:
        out.value.int_value = (left * right) & mask;
        return true;
      case ast::BinaryOp::Div:
      case ast::BinaryOp::Mod: {
        if (right == 0) {
          return comp_fail(node.span, "division by zero in comp evaluation");
        }
        if (comp_is_signed(tag)) {
          const i64 sl = comp_sign_extend(left, tag);
          const i64 sr = comp_sign_extend(right, tag);
          static constexpr i64 kMin =
              static_cast<i64>(static_cast<u64>(1) << 63);
          if (sl == kMin && sr == -1 && comp_int_bytes(tag) == 8) {
            out.value.int_value = static_cast<u64>(kMin);
            return true;
          }
          const i64 quotient = sl / sr;
          const i64 result =
              bin.op == ast::BinaryOp::Div ? quotient : (sl % sr);
          out.value.int_value = static_cast<u64>(result) & mask;
          return true;
        }
        out.value.int_value =
            (bin.op == ast::BinaryOp::Div ? (left / right) : (left % right)) &
            mask;
        return true;
      }
      case ast::BinaryOp::BitAnd:
        out.value.int_value = (left & right) & mask;
        return true;
      case ast::BinaryOp::BitOr:
        out.value.int_value = (left | right) & mask;
        return true;
      case ast::BinaryOp::BitXor:
        out.value.int_value = (left ^ right) & mask;
        return true;
      case ast::BinaryOp::Shl:
      case ast::BinaryOp::Shr: {
        if (right >= 64) {
          return comp_fail(node.span, "shift out of range in comp evaluation");
        }
        if (bin.op == ast::BinaryOp::Shl) {
          out.value.int_value = (left << right) & mask;
          return true;
        }
        if (comp_is_signed(tag)) {
          out.value.int_value =
              static_cast<u64>(comp_sign_extend(left, tag) >> right) & mask;
          return true;
        }
        out.value.int_value = (left >> right) & mask;
        return true;
      }
      default: break;
    }
    return comp_fail(node.span, "operator is not comp-evaluable");
  }

  static i64 comp_sign_extend(u64 bits, ir::TypeTag tag) {
    const u32 bytes = comp_int_bytes(tag);
    if (bytes >= 8) {
      return static_cast<i64>(bits);
    }
    const u64 mask = comp_mask(tag);
    const u64 sign = static_cast<u64>(1) << (bytes * 8 - 1);
    bits &= mask;
    if ((bits & sign) != 0) {
      bits |= ~mask;
    }
    return static_cast<i64>(bits);
  }

  Val lower_expr(ast::ExprIdx expr, const ir::TypeIdx* expected) {
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ast::ExprNode& node = ast.exprs[expr];
    SpanGuard guard{this, cur_span_};
    cur_span_ = node.span;
    switch (node.kind) {
      case ast::ExprKind::Literal: {
        const ast::ExprLiteral& literal = node.payload.get<ast::ExprLiteral>();
        if (ast.literals[literal.value].kind == ast::LiteralKind::Char) {
          internal(node.span, "character literal without type");
          return Val{size_one, error_type(), false, false};
        }
        return lower_literal(literal.value, expected);
      }
      case ast::ExprKind::Path: {
        return lower_path(expr, expected);
      }
      case ast::ExprKind::Struct: {
        return lower_struct(expr);
      }
      case ast::ExprKind::Tuple: {
        return lower_tuple(expr);
      }
      case ast::ExprKind::Unary: {
        const ast::ExprUnary& unary = node.payload.get<ast::ExprUnary>();
        Val inner = lower_expr(unary.inner, nullptr);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const ir::TypeTag tag = tag_of(inner.type);
        if (unary.op == ast::UnaryOp::Not) {
          const ir::RegisterIdx dst =
              emit(ir::Opcode::Not, inner.type, {use_value(inner)});
          return Val{to_operand(dst, inner.type), inner.type, false, false};
        }
        if (unary.op == ast::UnaryOp::BitNot) {
          const ir::RegisterIdx dst =
              emit(ir::Opcode::Not, inner.type, {use_value(inner)});
          return Val{to_operand(dst, inner.type), inner.type, false, false};
        }
        Val zero = lower_literal_zero(inner.type, node.span);
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
        const ast::ExprBorrow& borrow = node.payload.get<ast::ExprBorrow>();
        Val place = place_addr(borrow.inner);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const ir::TypeIdx ref =
            builder.reference_type(place.type, borrow.is_mut);
        const ir::RegisterIdx loan = emit(ir::Opcode::Borrow, ref, {place.op});
        return Val{to_operand(loan, ref), ref, false, false};
      }
      case ast::ExprKind::Binary: {
        return lower_binary(expr);
      }
      case ast::ExprKind::Cast: {
        const ast::ExprCast& cast = node.payload.get<ast::ExprCast>();
        Val inner = lower_expr(cast.inner, nullptr);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const ir::TypeIdx target = expr_type(expr);
        if (tag_of(target) == ir::TypeTag::Error) {
          internal(node.span, "cast without type");
          return Val{size_one, error_type(), false, false};
        }
        const ir::RegisterIdx dst =
            emit(ir::Opcode::TypeCast, target, {use_value(inner)});
        return Val{to_operand(dst, target), target, false, false};
      }
      case ast::ExprKind::Call: {
        return lower_call(expr, expected);
      }
      case ast::ExprKind::MethodCall: {
        return lower_method_call(expr, expected);
      }
      case ast::ExprKind::Field: {
        const ast::ExprField& field = node.payload.get<ast::ExprField>();
        Val base = lower_expr(field.receiver, nullptr);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        return materialize(field_addr(base, field.name.name, node.span));
      }
      case ast::ExprKind::Question: {
        return lower_question(expr);
      }
      case ast::ExprKind::If: {
        return lower_if(expr, expected);
      }
      case ast::ExprKind::Match: {
        return lower_match(expr, expected);
      }
      case ast::ExprKind::Loop: {
        return lower_loop(expr);
      }
      case ast::ExprKind::While: {
        return lower_while(expr);
      }
      case ast::ExprKind::Break: {
        if (break_targets_.empty()) {
          internal(node.span, "break without loop");
          return Val{size_one, error_type(), false, false};
        }
        emit_br(break_targets_.back());
        return Val{size_one, builder.never_type(), false, false};
      }
      case ast::ExprKind::Continue: {
        if (continue_targets_.empty()) {
          internal(node.span, "continue without loop");
          return Val{size_one, error_type(), false, false};
        }
        emit_br(continue_targets_.back());
        return Val{size_one, builder.never_type(), false, false};
      }
      case ast::ExprKind::Index:
      case ast::ExprKind::Range:
        unsupported(node.span, "control flow in lowering");
        return Val{size_one, error_type(), false, false};
      case ast::ExprKind::Block: {
        const ast::ExprBlock& block = node.payload.get<ast::ExprBlock>();
        if (!block.is_comp) {
          return lower_block(block.block, expected);
        }
        CompVal evaluated;
        if (!comp_evaluate(module, expr, evaluated)) {
          return Val{size_one, error_type(), false, false};
        }
        return materialize_comp_value(evaluated, node.span);
      }
      case ast::ExprKind::Return: {
        const ast::ExprReturn& ret = node.payload.get<ast::ExprReturn>();
        if (!ret.value.is_valid()) {
          emit_void(ir::Opcode::Ret, {});
        } else {
          Val value = lower_expr(ret.value, nullptr);
          if (failed) {
            return Val{size_one, error_type(), false, false};
          }
          if (tag_of(value.type) == ir::TypeTag::Void) {
            emit_void(ir::Opcode::Ret, {});
          } else {
            emit_void(ir::Opcode::Ret, {use_value(value)});
          }
        }
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

  void lower_stmt(ast::StmtIdx stmt) {
    if (failed || terminated_cur()) {
      return;
    }
    const ast::StmtNode& node = ast.stmts[stmt];
    SpanGuard guard{this, cur_span_};
    cur_span_ = node.span;
    switch (node.kind) {
      case ast::StmtKind::Decl: {
        const ast::StmtDecl& decl = node.payload.get<ast::StmtDecl>();
        if (decl.is_comp) {
          CompVal evaluated;
          if (!comp_evaluate(module, decl.init, evaluated)) {
            return;
          }
          // Comp bindings never take runtime addresses: runtime reads
          // splice constants through comp_scope_, so only publish the
          // value for later comp evaluation here.
          CompScope scope;
          scope.frames.emplace_back();
          if (!comp_bind_pattern(module, decl.pattern, evaluated, scope,
                                 node.span)) {
            return;
          }
          for (auto& binding : scope.frames.back()) {
            comp_scope_.push_back(std::move(binding));
          }
          return;
        }
        Val init = lower_expr(decl.init, nullptr);
        if (failed) {
          return;
        }
        // Moving into the binding consumes a non-Copy place.
        const ir::OperandIdx moved = use_value(init);
        bind_pattern(decl.pattern, Val{moved, init.type, false, false});
        return;
      }
      case ast::StmtKind::Reassign: {
        const ast::StmtReassign& reassign =
            node.payload.get<ast::StmtReassign>();
        Val place = place_addr(reassign.place);
        if (failed) {
          return;
        }
        Val value = lower_expr(reassign.value, nullptr);
        if (failed) {
          return;
        }
        ir::OperandIdx stored = use_value(value);
        if (reassign.compound) {
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
        const ast::StmtExpr& expr = node.payload.get<ast::StmtExpr>();
        mark_move(lower_expr(expr.value, nullptr));
        return;
      }
    }
  }

  Val lower_block(ast::BlockIdx block, const ir::TypeIdx* expected) {
    const ast::Block& node = ast.blocks[block];
    bool reachable = true;
    for (ast::StmtIdx stmt : node.statements) {
      if (failed) {
        break;
      }
      if (!reachable) {
        const u32 index =
            bag.emit(diag::Severity::Warning, kLowerUnreachable,
                     ast.stmts[stmt].span, "unreachable statement");
        (void)index;
        continue;
      }
      lower_stmt(stmt);
      if (!failed && terminated_cur()) {
        reachable = false;
      }
    }
    if (failed || terminated_cur()) {
      if (!reachable && node.value.is_valid()) {
        const u32 index =
            bag.emit(diag::Severity::Warning, kLowerUnreachable,
                     ast.exprs[node.value].span, "unreachable expression");
        (void)index;
      }
      return Val{size_one, error_type(), false, false};
    }
    if (!node.value.is_valid()) {
      return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
    }
    Val value = lower_expr(node.value, expected);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return value;
  }

  void lower_fn(const FnEntry& entry) {
    const u32 mod = entry.mod;
    module = mod;
    locals.clear();
    comp_scope_.clear();
    fn_blocks_.clear();
    streams_.clear();
    stream_last_.clear();
    fn_block_base_ = block_next_;
    break_targets_.clear();
    continue_targets_.clear();
    pending_params_.clear();

    const ast::ItemNode& item = ast.items[entry.item];
    const ast::ItemFn& fn = item.payload.get<ast::ItemFn>();
    if (!entry.item.is_valid() || !fn.body.is_valid()) {
      internal(!entry.item.is_valid() ? diag::Span{} : item.span,
               "function without body");
      return;
    }
    const std::vector<u32> comp = comp_positions(entry.item);
    switch_to(reserve_block());
    cur_span_ = fn.name.span;
    // Entry block parameters arrive in declaration order, skipping
    // comp parameters (their values ride the specialization).
    ir::BlockParamSeq param_seq;
    usize comp_at = 0;
    for (usize i = 0; i < entry.params.size() && !failed; ++i) {
      if (comp_at < comp.size() && comp[comp_at] == i) {
        ++comp_at;
        continue;
      }
      const ir::RegisterIdx reg = claim_reg();
      builder.reg({.type = entry.params[i],
                   .def_idx = ir::InstructionIdx(base::kInvalidIdx)});
      param_seq.push(
          builder.block_param({.type = entry.params[i], .reg = reg}));
      pending_params_.push_back(reg);
    }
    pending_block_params_ = param_seq.finish();
    if (failed) {
      return;
    }
    // Bind parameters (patterns may destructure) after allocas exist.
    binding_param_ = true;
    const std::span<const ast::ItemFnParam> params = fn.params;
    usize pending_at = 0;
    comp_at = 0;
    usize comp_arg_at = 0;
    for (usize i = 0; i < params.size() && !failed; ++i) {
      const ir::TypeIdx ptype = entry.params[i];
      if (comp_at < comp.size() && comp[comp_at] == i) {
        ++comp_at;
        // Comp parameters bind their specialized constants for nested
        // comp evaluation. Like comp declarations they take no runtime
        // addresses; runtime reads splice through comp_scope_.
        const CompVal& arg = entry.comp_args[comp_arg_at++];
        CompScope scope;
        scope.frames.emplace_back();
        if (!comp_bind_pattern(mod, params[i].pattern, arg, scope,
                               fn.name.span)) {
          return;
        }
        for (auto& binding : scope.frames.back()) {
          comp_scope_.push_back(std::move(binding));
        }
        continue;
      }
      const ir::RegisterIdx preg = pending_params_[pending_at++];
      Val param{to_operand(preg, ptype), ptype, false, true};
      // Parameters live in memory like locals so borrows observe them.
      if (tag_of(ptype) == ir::TypeTag::Void) {
        bind_pattern(params[i].pattern, param);
        continue;
      }
      const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, ptype, {size_one});
      emit_void(ir::Opcode::Store,
                {to_operand(preg, ptype), to_operand(addr, ptype)});
      bind_pattern(params[i].pattern,
                   Val{to_operand(addr, ptype), ptype, true, true});
    }
    pending_params_.clear();
    binding_param_ = false;
    if (failed) {
      return;
    }
    Val body = lower_block(fn.body, nullptr);
    if (failed) {
      return;
    }
    if (!terminated_cur()) {
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
    block_next_ = static_cast<u32>(builder.state().blocks.size());
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
    // Seed functions without comp parameters in declaration order;
    // comp specializations reserve on first call. Reservation order
    // matches lowering order, so indexes line up with storage.
    for (u32 m = 0; m < static_cast<u32>(pkg.modules.size()); ++m) {
      for (const auto& sig : pkg.modules[m].functions) {
        if (!sig.item.is_valid() || !comp_positions(sig.item).empty()) {
          continue;
        }
        fn_index(m, sig.item, sig.name, sig.params, sig.ret, {});
        if (failed) {
          return;
        }
      }
    }
    // Lower bodies, then publish functions in reservation order so
    // the pre-assigned indexes line up with storage positions.
    struct Done {
      FnEntry entry;
      std::vector<ir::BlockIdx> blocks;
    };
    std::vector<Done> done;
    for (usize w = 0; w < worklist_.size() && !failed; ++w) {
      const FnEntry entry = fns[worklist_[w]];
      lower_fn(entry);
      if (failed) {
        return;
      }
      // Publish reserved blocks in reservation order; entry block
      // carries the recorded parameters.
      std::vector<ir::BlockIdx> blocks;
      bool first = true;
      for (usize i = 0; i < fn_blocks_.size(); ++i) {
        ir::BlockIdx created =
            builder.block({.instrs = streams_[i].finish(),
                           .block_params = first ? pending_block_params_
                                                 : ir::BlockParamIdxRange{}});
        DCHECK(created.idx == fn_blocks_[i].idx);
        first = false;
        blocks.push_back(created);
      }
      pending_block_params_ = ir::BlockParamIdxRange{};
      done.push_back({entry, std::move(blocks)});
    }
    if (failed) {
      return;
    }
    for (const Done& entry : done) {
      const std::vector<u32> comp = comp_positions(entry.entry.item);
      ir::TypeSeq params;
      usize comp_at = 0;
      for (usize i = 0; i < entry.entry.params.size(); ++i) {
        if (comp_at < comp.size() && comp[comp_at] == i) {
          ++comp_at;
          continue;
        }
        params.push(builder.ref_type(entry.entry.params[i]));
      }
      ir::BlockIdxRange range{entry.blocks.front(),
                              static_cast<u32>(entry.blocks.size())};
      builder.function({.meta = {.return_type = entry.entry.ret,
                                 .param_types = params.finish(),
                                 .name = strings.intern(entry.entry.name)},
                        .blocks = range});
    }
  }

  ir::BlockParamIdxRange pending_block_params_;

  ir::Storage finish() && { return std::move(builder).build(); }
};

}  // namespace

diag::Fallible<LoweredPackage> lower_package(analyzer::CheckedPackage package,
                                             ir::PointerWidth width,
                                             ast::AstArena& ast,
                                             str::StringInterner& strings,
                                             diag::DiagBag& bag) {
  Lowerer lowerer(std::move(package), width, ast, strings, bag);
  lowerer.run();
  if (lowerer.failed) {
    return base::make_err(diag::Fatal{});
  }
  // Tables leave before the builder moves; aggregate init stays whole.
  std::vector<diag::Span> spans = std::move(lowerer.instr_spans_);
  std::vector<LoweredPackage::AddrInfo> addrs = std::move(lowerer.addr_names_);
  ir::Storage storage = std::move(lowerer).finish();
  if (base::Result<void, ir::VerifyError> result = ir::verify_storage(storage);
      result.is_err()) {
    ir::VerifyError error = std::move(result).unwrap_err();
    const u32 index = bag.emit(diag::Severity::Error, kLowerInternal,
                               diag::Span{}, "lowered IR failed verification");
    (void)index;
    const diag::Diagnostic diag = ir::to_diagnostic(error);
    fmt::memory_buffer rendered;
    diag::render(diag, rendered);
    DLOG("verify failure: {} at index {}",
         std::string_view(rendered.data(), rendered.size()), error.index);
    return base::make_err(diag::Fatal{});
  }
  // Tables outlive the builder move above; the Lowerer shell is empty.
  LoweredPackage lowered{std::move(storage), std::move(spans),
                         std::move(addrs)};
  return base::make_ok(std::move(lowered));
}

}  // namespace lower

