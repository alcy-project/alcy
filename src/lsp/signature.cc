// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/signature.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/typing.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

bool span_contains(diag::Span span, usize offset) {
  return span.offset <= offset && offset < span.offset + span.length;
}

// The call the position sits in: the smallest call expression whose
// span holds it, so a nested call picks the inner one.
struct CallSite {
  ast::ExprIdx call;
  ast::ExprIdx target;
  bool is_method = false;
};

std::optional<CallSite> call_at(const pipeline::PipelineContext& context,
                                usize offset,
                                source::FileId root) {
  std::optional<CallSite> best;
  usize best_length = 0;
  context.ast.exprs.for_each_node([&](const ast::ExprIdx index) {
    const ast::ExprNode& node = context.ast.exprs[index];
    if (node.kind != ast::ExprKind::Call &&
        node.kind != ast::ExprKind::MethodCall) {
      return;
    }
    if (node.span.file != root || !span_contains(node.span, offset)) {
      return;
    }
    if (best.has_value() && node.span.length >= best_length) {
      return;
    }
    best = CallSite{index,
                    node.kind == ast::ExprKind::MethodCall
                        ? index
                        : node.payload.get<ast::ExprCall>().callee,
                    node.kind == ast::ExprKind::MethodCall};
    best_length = node.span.length;
  });
  return best;
}

const analyzer::CheckedModule::CallTarget* find_target(
    const analyzer::CheckedPackage& package,
    ast::ExprIdx callee) {
  const analyzer::CheckedModule::CallTarget* fallback = nullptr;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::CallTarget& target :
         module.call_targets) {
      if (target.callee != callee) {
        continue;
      }
      if (target.inst == analyzer::NO_INST) {
        return &target;
      }
      if (fallback == nullptr) {
        fallback = &target;
      }
    }
  }
  return fallback;
}

std::string signature_label(const pipeline::PipelineContext& context,
                            const analyzer::CheckedPackage& package,
                            std::string_view name,
                            std::span<const ir::TypeIdx> params,
                            ir::TypeIdx ret) {
  const ir::StorageState& state = package.types->state();
  std::string out = "fn " + std::string(name) + "(";
  bool first = true;
  for (const ir::TypeIdx param : params) {
    if (!first) {
      out += ", ";
    }
    first = false;
    out += type_name(state, context.strings, param);
  }
  out += ')';
  if (ret != ir::primitive_idx(ir::TypeTag::Void)) {
    out += " -> " + type_name(state, context.strings, ret);
  }
  return out;
}

}  // namespace

text::JsonValue signature_help(const Document& document,
                               const DocumentAnalysis& analysis,
                               Position position) {
  if (!analysis.ok()) {
    return {};
  }
  const usize offset = document.index().byte_offset(document.text(), position);
  const pipeline::PipelineContext& context = analysis.context();
  const source::FileId root = analysis.root();
  const analyzer::CheckedPackage& package = analysis.package();

  const std::optional<CallSite> site = call_at(context, offset, root);
  if (!site.has_value()) {
    return {};
  }
  const analyzer::CheckedModule::CallTarget* target =
      find_target(package, site->target);
  if (target == nullptr || target->module >= package.modules.size()) {
    return {};
  }
  const analyzer::CheckedModule& module = package.modules[target->module];

  std::string_view name;
  std::span<const ir::TypeIdx> params;
  ir::TypeIdx ret = ir::primitive_idx(ir::TypeTag::Void);
  if (target->is_method) {
    if (target->index >= module.methods.size()) {
      return {};
    }
    const analyzer::CheckedModule::MethodInfo& method =
        module.methods[target->index];
    name = method.name;
    params = method.params;
    ret = method.ret;
  } else {
    if (target->index >= module.functions.size()) {
      return {};
    }
    const analyzer::CheckedModule::FnSig& fn = module.functions[target->index];
    name = fn.name;
    params = fn.params;
    ret = fn.ret;
  }

  const ast::ExprNode& call = context.ast.exprs[site->call];
  const std::span<const ast::ExprIdx> args =
      site->is_method ? call.payload.get<ast::ExprMethodCall>().args
                      : call.payload.get<ast::ExprCall>().args;
  u32 active = 0;
  for (const ast::ExprIdx arg : args) {
    const diag::Span span = context.ast.exprs[arg].span;
    if (span.offset + span.length <= offset) {
      ++active;
    }
  }
  if (!params.empty() && active >= params.size()) {
    active = static_cast<u32>(params.size() - 1);
  }

  const ir::StorageState& state = package.types->state();
  text::JsonValue parameters = text::JsonValue::make_array();
  for (const ir::TypeIdx param : params) {
    text::JsonValue entry = text::JsonValue::make_object();
    entry.set("label", text::JsonValue::make_string(
                           type_name(state, context.strings, param)));
    parameters.push(std::move(entry));
  }

  text::JsonValue signature = text::JsonValue::make_object();
  signature.set("label", text::JsonValue::make_string(signature_label(
                             context, package, name, params, ret)));
  signature.set("parameters", std::move(parameters));

  text::JsonValue signatures = text::JsonValue::make_array();
  signatures.push(std::move(signature));

  text::JsonValue result = text::JsonValue::make_object();
  result.set("signatures", std::move(signatures));
  result.set("activeSignature", text::JsonValue::make_number(0));
  result.set("activeParameter", text::JsonValue::make_number(active));
  return result;
}

}  // namespace lsp
