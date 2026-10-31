// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/checker.h"
#include "analyzer/diag_code.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_pool_id.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "ir/type_util.h"

namespace analyzer {

std::string_view Checker::nominal_name(ir::TypeIdx idx) const {
  const auto declaring = nominal_by_type_.find(idx.idx);
  if (declaring != nominal_by_type_.end()) {
    return nominals[declaring->second].name;
  }
  for (const GenericInstance& instance : generic_instances) {
    if (instance.type.idx == idx.idx) {
      return nominals[instance.nominal].name;
    }
  }
  return "type";
}

// Expression checking

ir::TypeTag Checker::tag_of(ir::TypeIdx idx) const {
  return builder.types()[idx].tag;
}

bool Checker::is_integer_tag(ir::TypeTag tag) const {
  switch (tag) {
    case ir::TypeTag::I8:
    case ir::TypeTag::I16:
    case ir::TypeTag::I32:
    case ir::TypeTag::I64:
    case ir::TypeTag::U8:
    case ir::TypeTag::U16:
    case ir::TypeTag::U32:
    case ir::TypeTag::U64: return true;
    default: return false;
  }
}

bool Checker::is_float_tag(ir::TypeTag tag) const {
  return tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64;
}

bool Checker::is_void(ir::TypeIdx idx) const {
  return tag_of(idx) == ir::TypeTag::Void;
}

bool Checker::is_never(ir::TypeIdx idx) const {
  return tag_of(idx) == ir::TypeTag::Never;
}

bool Checker::is_error(ir::TypeIdx idx) const {
  return tag_of(idx) == ir::TypeTag::Error;
}

// Whether the backend compares the type directly. A nominal type or
// `str` compares through `PartialEq` (ADR-0053); a tuple or array does
// so when each element can be compared directly, because the builtin
// comparison is element-wise.

bool Checker::is_builtin_comparable(ir::TypeIdx type) const {
  const ir::TypeTag tag = tag_of(type);
  if (is_integer_tag(tag) || is_float_tag(tag) || tag == ir::TypeTag::I1 ||
      tag == ir::TypeTag::Ptr || tag == ir::TypeTag::Ref ||
      tag == ir::TypeTag::MutRef || ir::is_raw_ptr_type(tag)) {
    return true;
  }
  if (tag == ir::TypeTag::Tuple) {
    const ir::TupleType tuple =
        builder.tuple_types()[builder.types()[type].as_tuple()];
    for (const ir::TypeIdx element : tuple.elements) {
      if (!is_builtin_comparable(element)) {
        return false;
      }
    }
    return true;
  }
  if (tag == ir::TypeTag::Array) {
    const ir::ArrayType array =
        builder.array_types()[builder.types()[type].as_array()];
    return is_builtin_comparable(array.element);
  }
  return false;
}

// User-facing type names: the table spells bool as `i1` and unit as
// `void`, which read poorly in diagnostics.

const char* Checker::pretty_tag(ir::TypeTag tag) {
  switch (tag) {
    case ir::TypeTag::Void: return "()";
    case ir::TypeTag::I1: return "bool";
    case ir::TypeTag::Never: return "!";
    case ir::TypeTag::Str: return "str";
    default: return ir::type_to_str(tag);
  }
}

// Structural type equality. Field slots hold storage copies, so index
// equality under-compares: both sides normalize to their origin first,
// primitives compare by tag, references/tuples/arrays recurse, and
// nominals compare by index. Every cycle passes through a nominal, but a
// seen-pair set guards regardless.

bool Checker::types_equal(ir::TypeIdx a, ir::TypeIdx b) {
  std::vector<u64> seen;
  return types_equal_inner(type_origin(a), type_origin(b), seen);
}

bool Checker::types_equal_inner(ir::TypeIdx a,
                                ir::TypeIdx b,
                                std::vector<u64>& seen) {
  // Slot copies nest: tuple and array elements are copies of their own,
  // so each level normalizes before comparing, not just the top one.
  a = type_origin(a);
  b = type_origin(b);
  if (a.idx == b.idx) {
    return true;
  }
  const u64 key = (static_cast<u64>(a.idx) << 32) | static_cast<u64>(b.idx);
  for (u64 prior : seen) {
    if (prior == key) {
      return true;
    }
  }
  seen.push_back(key);
  const ir::TypeTag ta = tag_of(a);
  const ir::TypeTag tb = tag_of(b);
  if (ta != tb) {
    return false;
  }
  switch (ta) {
    case ir::TypeTag::Ref:
    case ir::TypeTag::MutRef:
    case ir::TypeTag::RawPtr:
    case ir::TypeTag::RawMutPtr: {
      const ir::TypeIdx pa =
          builder.ref_types()[builder.types()[a].as_ref()].pointee;
      const ir::TypeIdx pb =
          builder.ref_types()[builder.types()[b].as_ref()].pointee;
      return types_equal_inner(pa, pb, seen);
    }
    case ir::TypeTag::Array: {
      const ir::ArrayType aa =
          builder.array_types()[builder.types()[a].as_array()];
      const ir::ArrayType ab =
          builder.array_types()[builder.types()[b].as_array()];
      return aa.count == ab.count &&
             types_equal_inner(aa.element, ab.element, seen);
    }
    case ir::TypeTag::Slice: {
      const ir::SliceType sa =
          builder.slice_types()[builder.types()[a].as_slice()];
      const ir::SliceType sb =
          builder.slice_types()[builder.types()[b].as_slice()];
      return types_equal_inner(sa.element, sb.element, seen);
    }
    case ir::TypeTag::Tuple: {
      const ir::TupleType ta_t =
          builder.tuple_types()[builder.types()[a].as_tuple()];
      const ir::TupleType tb_t =
          builder.tuple_types()[builder.types()[b].as_tuple()];
      if (ta_t.elements.size() != tb_t.elements.size()) {
        return false;
      }
      for (u32 i = 0; i < ta_t.elements.size(); ++i) {
        if (!types_equal_inner(ta_t.elements[i], tb_t.elements[i], seen)) {
          return false;
        }
      }
      return true;
    }
    case ir::TypeTag::Func: {
      const ir::FuncType fa =
          builder.func_types()[builder.types()[a].as_func()];
      const ir::FuncType fb =
          builder.func_types()[builder.types()[b].as_func()];
      if (fa.params.size() != fb.params.size()) {
        return false;
      }
      for (u32 i = 0; i < fa.params.size(); ++i) {
        if (!types_equal_inner(fa.params[i], fb.params[i], seen)) {
          return false;
        }
      }
      return types_equal_inner(fa.ret, fb.ret, seen);
    }
    case ir::TypeTag::Const: {
      // Equal tags do not make equal arguments: the value is the type.
      const u64 left =
          builder.const_types()[builder.types()[a].as_const()].value;
      const u64 right =
          builder.const_types()[builder.types()[b].as_const()].value;
      return left == right;
    }
    case ir::TypeTag::Param: {
      // The index is the identity: one node serves every item's
      // position with that index, and each item's own scope says what
      // the position means (ADR-0079).
      return builder.types()[a].as_param().value ==
             builder.types()[b].as_param().value;
    }
    case ir::TypeTag::Struct:
    case ir::TypeTag::Enum: return false;
    default: return true;
  }
}

// `&mut T` coerces to `&T`, and `*mut T` to `*T`: one is the other
// with the unique half dropped, which is a shared reborrow of the
// same referent or the same address without the write permission.

bool Checker::coerces_to_shared(ir::TypeIdx expected, ir::TypeIdx actual) {
  const ir::TypeTag etag = tag_of(expected);
  const ir::TypeTag atag = tag_of(actual);
  const bool ref_pair = etag == ir::TypeTag::Ref && atag == ir::TypeTag::MutRef;
  const bool raw_pair =
      etag == ir::TypeTag::RawPtr && atag == ir::TypeTag::RawMutPtr;
  if (!ref_pair && !raw_pair) {
    return false;
  }
  return builder.ref_types()[builder.types()[expected].as_ref()].pointee.idx ==
         builder.ref_types()[builder.types()[actual].as_ref()].pointee.idx;
}

bool Checker::coerces_array_to_slice(ir::TypeIdx expected, ir::TypeIdx actual) {
  const ir::TypeTag etag = tag_of(expected);
  const ir::TypeTag atag = tag_of(actual);
  if (etag != ir::TypeTag::Ref && etag != ir::TypeTag::MutRef) {
    return false;
  }
  if (atag != ir::TypeTag::Ref && atag != ir::TypeTag::MutRef) {
    return false;
  }
  if (etag == ir::TypeTag::MutRef && atag != ir::TypeTag::MutRef) {
    return false;
  }
  const ir::TypeIdx epointee =
      builder.ref_types()[builder.types()[expected].as_ref()].pointee;
  const ir::TypeIdx apointee =
      builder.ref_types()[builder.types()[actual].as_ref()].pointee;
  if (tag_of(epointee) != ir::TypeTag::Slice) {
    return false;
  }
  if (tag_of(apointee) != ir::TypeTag::Array) {
    return false;
  }
  const ir::SliceType slice =
      builder.slice_types()[builder.types()[epointee].as_slice()];
  const ir::ArrayType array =
      builder.array_types()[builder.types()[apointee].as_array()];
  return types_equal(slice.element, array.element);
}

// Unifies actual against expected, emitting a mismatch diagnostic.
// The coercions it accepts are the two the language has here, a shared
// reborrow and an array-to-slice view; the result is the actual type.
// Error suppresses follow-on diagnostics.

ir::TypeIdx Checker::unify(ir::TypeIdx expected,
                           ir::TypeIdx actual,
                           diag::Span span,
                           std::string_view what) {
  if (is_error(expected)) {
    return actual;
  }
  if (is_error(actual)) {
    return expected;
  }
  if (types_equal(expected, actual)) {
    return actual;
  }
  if (is_never(expected)) {
    return actual;
  }
  if (is_never(actual)) {
    return expected;
  }
  // `&mut T` where `&T` is expected is a shared reborrow, not a
  // mismatch. The exclusive reference is consumed at the call, so
  // nothing can write through it while the shared one is live.
  if (coerces_to_shared(expected, actual)) {
    return actual;
  }
  if (coerces_array_to_slice(expected, actual)) {
    return actual;
  }
  // A closure value coerces to the function type its environment fits:
  // the view borrows the environment the value owns (ADR-0064).
  if (const ClosureSig* closure = closure_sig_of_type(actual);
      closure != nullptr && tag_of(expected) == ir::TypeTag::Func) {
    const ir::FuncType shape =
        builder.func_types()[builder.types()[expected].as_func()];
    bool matches = shape.params.size() == closure->params.size() &&
                   types_equal(shape.ret, closure->ret);
    for (usize i = 0; matches && i < closure->params.size(); ++i) {
      matches = types_equal(shape.params[i], closure->params[i]);
    }
    if (matches) {
      return expected;
    }
  }
  // An unwritten slot is the one mismatch with an obvious fix, so it
  // gets its own message instead of two type names.
  const ir::TypeIdx uninit_side = uninit_payload(expected).is_valid() ? expected
                                  : uninit_payload(actual).is_valid()
                                      ? actual
                                      : ir::TypeIdx::invalid();
  if (uninit_side.is_valid()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerUninitializedValueUsed>(
        DiagCode::TypeMismatch, span,
        pretty_tag(tag_of(uninit_side == expected ? actual : expected)));
    (void)index;
    return error_type();
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerTypeMismatchExpectedFound>(
      DiagCode::TypeMismatch, span, what, type_display(expected),
      type_display(actual));
  (void)index;
  return error_type();
}

}  // namespace analyzer
