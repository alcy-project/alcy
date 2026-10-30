// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/navigation.h"

#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "lsp/analysis.h"
#include "lsp/ast_query.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace lsp {

namespace {

bool span_contains(diag::Span span, usize offset) {
  return span.offset <= offset && offset < span.offset + span.length;
}

// The declaration of a named type, when its name is the one the checker
// registered and the declaration is in the root file.
std::optional<diag::Span> declaration_of_name(
    const pipeline::PipelineContext& context,
    const analyzer::CheckedPackage& package,
    source::FileId root,
    std::string_view name) {
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::StructInfo& info : module.structs) {
      if (!info.item.is_valid()) {
        continue;
      }
      const ast::ItemStruct& declaration =
          context.ast.items[info.item].payload.get<ast::ItemStruct>();
      if (declaration.name.name == name && declaration.name.span.file == root) {
        return item_name_span(context.ast, context.ast.items[info.item]);
      }
    }
    for (const analyzer::CheckedModule::EnumInfo& info : module.enums) {
      if (!info.item.is_valid()) {
        continue;
      }
      const ast::ItemEnum& declaration =
          context.ast.items[info.item].payload.get<ast::ItemEnum>();
      if (declaration.name.name == name && declaration.name.span.file == root) {
        return item_name_span(context.ast, context.ast.items[info.item]);
      }
    }
  }
  return std::nullopt;
}

// The declaration of a checked type, looking through a reference. A
// primitive or a tuple has no declaration to jump to.
std::optional<diag::Span> declaration_of_type(
    const pipeline::PipelineContext& context,
    const analyzer::CheckedPackage& package,
    source::FileId root,
    ir::TypeIdx type) {
  const ir::StorageState& state = package.types->state();
  ir::TypeNode node = state.types[type];
  if (node.tag == ir::TypeTag::Ref || node.tag == ir::TypeTag::MutRef) {
    type = state.ref_types[node.as_ref()].pointee;
    node = state.types[type];
  }
  if (node.tag == ir::TypeTag::Struct) {
    return declaration_of_name(
        context, package, root,
        context.strings.get(state.struct_types[node.as_struct()].name));
  }
  if (node.tag == ir::TypeTag::Enum) {
    return declaration_of_name(
        context, package, root,
        context.strings.get(state.enum_types[node.as_enum()].name));
  }
  return std::nullopt;
}

// A binding's name, which the package does not table because it lives
// and dies with the checker's scope stack.
std::optional<diag::Span> pattern_name_at(
    const pipeline::PipelineContext& context,
    usize offset,
    source::FileId root) {
  std::optional<diag::Span> found;
  context.ast.patterns.for_each_node([&](const ast::PatternIdx index) {
    if (found.has_value()) {
      return;
    }
    const ast::PatternNode& node = context.ast.patterns[index];
    if (node.kind == ast::PatternKind::Ident) {
      if (span_contains(node.payload.ident.name.span, offset) &&
          node.payload.ident.name.span.file == root) {
        found = node.payload.ident.name.span;
      }
    } else if (node.kind == ast::PatternKind::MutIdent) {
      if (span_contains(node.payload.mut_ident.name.span, offset) &&
          node.payload.mut_ident.name.span.file == root) {
        found = node.payload.mut_ident.name.span;
      }
    }
  });
  return found;
}

// The declaration name the cursor sits on, from the AST. This is what
// lets references work on a declaration as well as on a use.
std::optional<diag::Span> declaration_name_at(
    const pipeline::PipelineContext& context,
    const analyzer::CheckedPackage& package,
    usize offset,
    source::FileId root) {
  using K = ast::ItemKind;
  for (const analyzer::ModuleNode* module : package.tree.modules) {
    for (const ast::ItemIdx index : module->items) {
      const ast::ItemNode& item = context.ast.items[index];
      const diag::Span name = item_name_span(context.ast, item);
      if (name.file != root) {
        continue;
      }
      if (span_contains(name, offset)) {
        return name;
      }
      if (item.kind == K::Struct) {
        for (const ast::ItemStructField& field :
             item.payload.get<ast::ItemStruct>().fields) {
          if (span_contains(field.name.span, offset)) {
            return field.name.span;
          }
        }
      } else if (item.kind == K::Enum) {
        for (const ast::ItemEnumVariant& variant :
             item.payload.get<ast::ItemEnum>().variants) {
          if (span_contains(variant.name.span, offset)) {
            return variant.name.span;
          }
        }
      } else if (item.kind == K::Impl) {
        for (const ast::ItemIdx method :
             item.payload.get<ast::ItemImpl>().methods) {
          const diag::Span method_name =
              item_name_span(context.ast, context.ast.items[method]);
          if (span_contains(method_name, offset)) {
            return method_name;
          }
        }
      } else if (item.kind == K::Spec) {
        for (const ast::SpecMethod& method :
             item.payload.get<ast::ItemSpec>().methods) {
          if (span_contains(method.name.span, offset)) {
            return method.name.span;
          }
        }
      }
    }
  }
  return pattern_name_at(context, offset, root);
}

}  // namespace

std::optional<diag::Span> definition_at(const Document& document,
                                        const DocumentAnalysis& analysis,
                                        Position position) {
  if (!analysis.ok()) {
    return std::nullopt;
  }
  const usize offset = document.index().byte_offset(document.text(), position);
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();
  const source::FileId root = analysis.root();
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::Reference& reference :
         module.references) {
      if (reference.site.file == root &&
          span_contains(reference.site, offset)) {
        return reference.declaration;
      }
    }
  }
  return declaration_name_at(context, package, offset, root);
}

std::optional<diag::Span> type_definition_at(const Document& document,
                                             const DocumentAnalysis& analysis,
                                             Position position) {
  if (!analysis.ok()) {
    return std::nullopt;
  }
  const usize offset = document.index().byte_offset(document.text(), position);
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();
  const source::FileId root = analysis.root();
  if (const std::optional<ast::ExprIdx> expr =
          innermost_expr(context, offset, root);
      expr.has_value()) {
    if (const std::optional<ir::TypeIdx> type =
            checked_expr_type(package, *expr);
        type.has_value()) {
      const std::optional<diag::Span> declaration =
          declaration_of_type(context, package, root, *type);
      if (declaration.has_value()) {
        return declaration;
      }
    }
  }
  if (const std::optional<ast::TypeIdx> type =
          innermost_type(context, offset, root);
      type.has_value()) {
    const ast::TypeNode& node = context.ast.types[*type];
    if (node.kind == ast::TypeKind::Path) {
      const ast::TypePath& path = node.payload.get<ast::TypePath>();
      if (path.path != ast::PathIdx::invalid() &&
          !context.ast.paths[path.path].segments.empty()) {
        return declaration_of_name(
            context, package, root,
            context.ast.paths[path.path].segments.back().name);
      }
    }
  }
  return std::nullopt;
}

std::vector<diag::Span> implementation_at(const Document& document,
                                          const DocumentAnalysis& analysis,
                                          Position position) {
  if (!analysis.ok()) {
    return {};
  }
  const usize offset = document.index().byte_offset(document.text(), position);
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();
  const source::FileId root = analysis.root();

  // A call or an impl method resolves to the method the checker bound;
  // its spec field names the spec the implementation belongs to.
  u32 spec = analyzer::NO_SPEC;
  std::string_view method;
  if (const std::optional<diag::Span> declaration =
          definition_at(document, analysis, position);
      declaration.has_value()) {
    for (const analyzer::CheckedModule& module : package.modules) {
      for (const analyzer::CheckedModule::MethodInfo& info : module.methods) {
        if (info.item.is_valid() &&
            same_span(item_name_span(context.ast, context.ast.items[info.item]),
                      *declaration)) {
          spec = info.spec;
          method = info.name;
          break;
        }
      }
      if (spec != analyzer::NO_SPEC) {
        break;
      }
    }
  }
  // A spec declaration names its spec and the method the cursor is on.
  if (spec == analyzer::NO_SPEC) {
    for (u32 i = 0; i < package.specs.size(); ++i) {
      const ast::ItemNode& item = context.ast.items[package.specs[i].item];
      if (item.kind != ast::ItemKind::Spec) {
        continue;
      }
      const ast::ItemSpec& declaration = item.payload.get<ast::ItemSpec>();
      if (span_contains(declaration.name.span, offset)) {
        spec = i;
        break;
      }
      for (const ast::SpecMethod& candidate : declaration.methods) {
        if (span_contains(candidate.name.span, offset)) {
          spec = i;
          method = candidate.name.name;
          break;
        }
      }
      if (spec != analyzer::NO_SPEC) {
        break;
      }
    }
  }
  if (spec == analyzer::NO_SPEC) {
    return {};
  }

  std::vector<diag::Span> out;
  std::unordered_set<u64> seen;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::MethodInfo& info : module.methods) {
      if (info.spec != spec || !info.item.is_valid()) {
        continue;
      }
      if (!method.empty() && info.name != method) {
        continue;
      }
      const diag::Span span =
          item_name_span(context.ast, context.ast.items[info.item]);
      if (span.file != root) {
        continue;
      }
      if (seen.insert(span_key(span)).second) {
        out.push_back(span);
      }
    }
  }
  return out;
}

std::vector<diag::Span> references_at(const Document& document,
                                      const DocumentAnalysis& analysis,
                                      Position position) {
  if (!analysis.ok()) {
    return {};
  }
  const usize offset = document.index().byte_offset(document.text(), position);
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();
  const source::FileId root = analysis.root();

  std::optional<diag::Span> declaration;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::Reference& reference :
         module.references) {
      if (reference.site.file == root &&
          span_contains(reference.site, offset)) {
        declaration = reference.declaration;
        break;
      }
    }
    if (declaration.has_value()) {
      break;
    }
  }
  if (!declaration.has_value()) {
    declaration = declaration_name_at(context, package, offset, root);
  }
  if (!declaration.has_value()) {
    return {};
  }

  std::vector<diag::Span> out;
  std::unordered_set<u64> seen;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::Reference& reference :
         module.references) {
      if (reference.site.file != root ||
          !same_span(reference.declaration, *declaration)) {
        continue;
      }
      if (seen.insert(span_key(reference.site)).second) {
        out.push_back(reference.site);
      }
    }
  }
  return out;
}

}  // namespace lsp
