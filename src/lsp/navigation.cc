// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/navigation.h"

#include <optional>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/language.h"
#include "lsp/analysis.h"
#include "lsp/ast_query.h"
#include "lsp/document.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace lsp {

namespace {

bool span_contains(diag::Span span, usize offset) {
  return span.offset <= offset && offset < span.offset + span.length;
}

bool same_span(diag::Span left, diag::Span right) {
  return left.file == right.file && left.offset == right.offset &&
         left.length == right.length;
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
                                        Position position) {
  const usize offset = document.index().byte_offset(document.text(), position);
  pipeline::PipelineContext context{i18n::Language::EnUs};
  source::FileId root = source::UNKNOWN_FILE;
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      analyze_document(context, document, &root);
  if (checked.is_err()) {
    return std::nullopt;
  }
  const analyzer::CheckedPackage& package = checked.value();
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

std::vector<diag::Span> references_at(const Document& document,
                                      Position position) {
  const usize offset = document.index().byte_offset(document.text(), position);
  pipeline::PipelineContext context{i18n::Language::EnUs};
  source::FileId root = source::UNKNOWN_FILE;
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      analyze_document(context, document, &root);
  if (checked.is_err()) {
    return {};
  }
  const analyzer::CheckedPackage& package = checked.value();

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
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::Reference& reference :
         module.references) {
      if (reference.site.file != root ||
          !same_span(reference.declaration, *declaration)) {
        continue;
      }
      bool duplicate = false;
      for (const diag::Span site : out) {
        if (same_span(site, reference.site)) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        out.push_back(reference.site);
      }
    }
  }
  return out;
}

}  // namespace lsp
