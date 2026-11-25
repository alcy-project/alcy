// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/completion.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/type_display.h"
#include "lexer/lexer.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

struct Item {
  std::string label;
  std::string detail;
  ast::ItemIdx declaration = ast::ItemIdx::invalid();
  CompletionItemKind kind = CompletionItemKind::Variable;
  // A variant is shown by its short name but inserted qualified, as the
  // language requires at a use site.
  std::string insert_text;
  // The item was matched to the qualifier the buffer already typed, so
  // it stays eligible after `Type::` even though it is a method.
  bool qualified = false;
};

std::string_view prefix_at(std::string_view text, usize offset) {
  usize start = offset;
  while (start > 0) {
    const char c = text[start - 1];
    // A qualified name is one prefix: replacing only its last segment
    // would double the qualifier for a variant item's insert text.
    const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == ':' ||
                      c == '<' || c == '>' || c == ',';
    if (!word) {
      break;
    }
    --start;
  }
  return text.substr(start, offset - start);
}

std::string function_detail(const pipeline::PipelineContext& context,
                            const ir::StorageState& state,
                            std::span<const ir::TypeIdx> params,
                            ir::TypeIdx ret,
                            std::string_view variadic_name,
                            ast::ItemIdx item) {
  std::string out = "(";
  bool first = true;
  for (usize i = 0; i < params.size(); ++i) {
    if (!first) {
      out += ", ";
    }
    first = false;
    const ParamSyntax param = param_syntax(context, item, i);
    if (param.is_comp) {
      out += "comp ";
    }
    if (param.is_mut) {
      out += "mut ";
    }
    if (!param.name.empty()) {
      out += param.name;
      out += ": ";
    }
    out += ir::type_display(state, context.strings, params[i]);
  }
  if (!variadic_name.empty()) {
    if (!first) {
      out += ", ";
    }
    out += variadic_name;
    out += ": ";
    out += variadic_param_element(context, item);
    out += "..";
  }
  out += ')';
  if (ret != ir::primitive_idx(ir::TypeTag::Void)) {
    out += " -> " + ir::type_display(state, context.strings, ret);
  }
  return out;
}

// The names the root module can actually spell: its own declarations and
// everything its `use` statements and the prelude brought into scope.
// For each module, the declared name -> the names the root module can
// spell for it. An `as` import contributes its alias, which is the only
// usable label for that declaration in the buffer.
using VisibleNames = std::unordered_map<
    u32,
    std::unordered_map<std::string_view, std::vector<std::string_view>>>;

VisibleNames visible_names(const DocumentAnalysis& analysis) {
  VisibleNames names;
  const analyzer::CheckedPackage& package = analysis.package();
  u32 root_module = analyzer::NO_MODULE;
  for (u32 i = 0; i < package.tree.modules.size(); ++i) {
    if (package.tree.modules[i]->file == analysis.root()) {
      root_module = i;
      break;
    }
  }
  if (root_module == analyzer::NO_MODULE) {
    return names;
  }
  for (const analyzer::Import& import :
       package.tree.modules[root_module]->imports) {
    names[import.target_module][import.member].push_back(import.name);
  }
  for (const analyzer::CheckedModule& module : package.modules) {
    if (module.module != root_module) {
      continue;
    }
    for (const analyzer::CheckedModule::FnSig& fn : module.functions) {
      names[root_module][fn.name].push_back(fn.name);
    }
    for (const analyzer::CheckedModule::MethodInfo& method : module.methods) {
      names[root_module][method.name].push_back(method.name);
    }
    for (const analyzer::CheckedModule::NamedType& named : module.types) {
      names[root_module][named.name].push_back(named.name);
    }
    for (const analyzer::CheckedModule::StaticInfo& info : module.statics) {
      names[root_module][info.name].push_back(info.name);
    }
  }
  // Methods are named at a receiver, not through a module scope. Offer
  // their declared names from every module so a call like `r.step_by(2)`
  // can complete even when the defining module is not imported by name.
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::MethodInfo& method : module.methods) {
      if (method.item.is_valid()) {
        names[module.module][method.name].push_back(method.name);
      }
    }
  }
  return names;
}

std::vector<std::string_view> visible_labels(const VisibleNames& visible,
                                             u32 module,
                                             std::string_view name) {
  const auto found = visible.find(module);
  if (found == visible.end()) {
    return {};
  }
  const auto labels = found->second.find(name);
  return labels == found->second.end() ? std::vector<std::string_view>{}
                                       : labels->second;
}

std::string doc_text(const DocumentAnalysis& analysis, ast::ItemIdx item) {
  if (!item.is_valid()) {
    return {};
  }
  const auto& context = analysis.context();
  const ast::ItemNode& node = context.ast.items[item];
  std::string text;
  for (const diag::Span span : node.docs) {
    const std::optional<std::string_view> bytes =
        context.sources.bytes(span.file);
    if (!bytes.has_value() || span.offset + span.length > bytes->size()) {
      continue;
    }
    std::string_view line = bytes->substr(span.offset, span.length);
    if (line.starts_with("///")) {
      line.remove_prefix(3);
    }
    if (line.starts_with(' ')) {
      line.remove_prefix(1);
    }
    if (!text.empty()) {
      text.push_back('\n');
    }
    text.append(line);
  }
  return text;
}

struct PatternIdxHash {
  usize operator()(ast::PatternIdx index) const { return index.idx; }
};

bool span_contains(diag::Span span, usize offset) {
  return span.offset <= offset && offset < span.offset + span.length;
}

// The blocks and items a binding can belong to. A parameter's name sits
// before its function's body block, so the item's own span is a scope too.
std::vector<diag::Span> scope_spans(const pipeline::PipelineContext& context,
                                    source::FileId root) {
  std::vector<diag::Span> scopes;
  context.ast.blocks.for_each_node([&](ast::BlockIdx index) {
    const diag::Span span = context.ast.blocks[index].span;
    if (span.file == root && span.length != 0) {
      scopes.push_back(span);
    }
  });
  context.ast.items.for_each_node([&](ast::ItemIdx index) {
    const diag::Span span = context.ast.items[index].span;
    if (span.file == root && span.length != 0) {
      scopes.push_back(span);
    }
  });
  return scopes;
}

// A binding belongs to the cursor's scope when the smallest scope that
// holds its name also holds the cursor. A local in a block that already
// closed is outside, while one in an enclosing block is inside.
bool in_scope_of(const std::vector<diag::Span>& scopes,
                 diag::Span name,
                 usize offset) {
  const diag::Span* best = nullptr;
  for (const diag::Span& span : scopes) {
    if (!span_contains(span, name.offset) ||
        (best != nullptr && span.length >= best->length)) {
      continue;
    }
    best = &span;
  }
  return best != nullptr && span_contains(*best, offset);
}

// Locals the checker never tables: statement bindings, parameters and
// pattern bindings in the scope the cursor sits in.
void collect_locals(const DocumentAnalysis& analysis,
                    std::string_view source,
                    usize offset,
                    std::vector<Item>* out) {
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();
  const ir::StorageState& state = analysis.types();
  const source::FileId root = analysis.root();
  const std::vector<diag::Span> scopes = scope_spans(context, root);

  std::unordered_map<ast::PatternIdx, ast::ExprIdx, PatternIdxHash>
      init_by_pattern;
  context.ast.stmts.for_each_node([&](ast::StmtIdx index) {
    const ast::StmtNode& statement = context.ast.stmts[index];
    if (statement.kind != ast::StmtKind::Decl || statement.span.file != root) {
      return;
    }
    const ast::StmtDecl& declaration = statement.payload.get<ast::StmtDecl>();
    if (declaration.pattern != ast::PatternIdx::invalid()) {
      init_by_pattern.emplace(declaration.pattern, declaration.init);
    }
  });

  std::unordered_map<ast::PatternIdx, std::string_view, PatternIdxHash>
      param_types;
  context.ast.items.for_each_node([&](ast::ItemIdx index) {
    const ast::ItemNode& item = context.ast.items[index];
    if (item.span.file != root || item.kind != ast::ItemKind::Fn) {
      return;
    }
    for (const ast::ItemFnParam& param :
         item.payload.get<ast::ItemFn>().params) {
      if (param.pattern == ast::PatternIdx::invalid() ||
          param.type == ast::TypeIdx::invalid()) {
        continue;
      }
      const diag::Span span = context.ast.types[param.type].span;
      if (span.offset + span.length <= source.size()) {
        param_types.emplace(param.pattern,
                            source.substr(span.offset, span.length));
      }
    }
  });

  context.ast.patterns.for_each_node([&](ast::PatternIdx index) {
    const ast::PatternNode& node = context.ast.patterns[index];
    diag::Span name;
    std::string_view spelling;
    if (node.kind == ast::PatternKind::Ident) {
      name = node.payload.ident.name.span;
      spelling = node.payload.ident.name.name;
    } else if (node.kind == ast::PatternKind::MutIdent) {
      name = node.payload.mut_ident.name.span;
      spelling = node.payload.mut_ident.name.name;
    } else {
      return;
    }
    // The `for` desugar names its cursor `_it` at the loop keyword.
    if (name.file != root || name.offset >= offset ||
        spelling.starts_with("_it") || !in_scope_of(scopes, name, offset)) {
      return;
    }
    std::string detail;
    const auto init = init_by_pattern.find(index);
    if (init != init_by_pattern.end() &&
        init->second != ast::ExprIdx::invalid()) {
      if (const std::optional<ir::TypeIdx> type =
              checked_expr_type(package, init->second);
          type.has_value()) {
        detail = ir::type_display(state, context.strings, *type);
      }
    }
    if (detail.empty()) {
      const auto param = param_types.find(index);
      if (param != param_types.end()) {
        detail = std::string(param->second);
      }
    }
    out->push_back(Item{std::string(spelling), std::move(detail),
                        ast::ItemIdx::invalid(), CompletionItemKind::Variable,
                        ""});
  });
}

bool has_label(const std::vector<Item>& items, std::string_view label) {
  for (const Item& item : items) {
    if (item.label == label) {
      return true;
    }
  }
  return false;
}

void collect(const DocumentAnalysis& analysis,
             const analyzer::CheckedPackage& package,
             const VisibleNames& visible,
             std::vector<Item>* out) {
  const pipeline::PipelineContext& context = analysis.context();
  const ir::StorageState& state = analysis.types();
  const auto emit = [&](u32 module, ast::ItemIdx item,
                        std::string_view declared, std::string_view detail,
                        CompletionItemKind kind) {
    for (const std::string_view label :
         visible_labels(visible, module, declared)) {
      if (has_label(*out, label)) {
        continue;
      }
      out->push_back(
          Item{std::string(label), std::string(detail), item, kind, ""});
    }
  };
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::FnSig& fn : module.functions) {
      emit(module.module, fn.item, fn.name,
           function_detail(context, state, fn.params, fn.ret,
                           variadic_param_name(context, fn.item), fn.item),
           fn.is_method ? CompletionItemKind::Method
                        : CompletionItemKind::Function);
    }
    for (const analyzer::CheckedModule::MethodInfo& method : module.methods) {
      // The definition-site check registers rows for a parameter's
      // bound ; they name no declaration, so there is
      // nothing here to offer.
      if (!method.item.is_valid()) {
        continue;
      }
      emit(module.module, method.item, method.name,
           function_detail(context, state, method.params, method.ret,
                           variadic_param_name(context, method.item),
                           method.item),
           CompletionItemKind::Method);
    }
    for (const analyzer::CheckedModule::NamedType& named : module.types) {
      const ir::TypeNode& type = state.types[named.type];
      ast::ItemIdx declaration = ast::ItemIdx::invalid();
      if (type.tag == ir::TypeTag::Enum) {
        for (const analyzer::CheckedModule::EnumInfo& info : module.enums) {
          if (info.type == named.type) {
            declaration = info.item;
            break;
          }
        }
      } else {
        for (const analyzer::CheckedModule::StructInfo& info : module.structs) {
          if (info.type == named.type) {
            declaration = info.item;
            break;
          }
        }
      }
      emit(module.module, declaration, named.name,
           ir::type_display(state, context.strings, named.type),
           type.tag == ir::TypeTag::Enum ? CompletionItemKind::Enum
                                         : CompletionItemKind::Struct);
    }
    for (const analyzer::CheckedModule::EnumInfo& info : module.enums) {
      const auto labels = visible_labels(visible, module.module, info.name);
      if (labels.empty()) {
        continue;
      }
      // A use alias is what the buffer can spell; inserting the
      // declaration name would be unresolved at the use site.
      const std::string qualifier(labels.front());
      for (const std::string_view variant : info.variants) {
        if (has_label(*out, variant)) {
          continue;
        }
        Item item{std::string(variant), qualifier + "::" + std::string(variant),
                  ast::ItemIdx::invalid(), CompletionItemKind::EnumMember, ""};
        item.insert_text = item.detail;
        out->push_back(std::move(item));
      }
    }
    for (const analyzer::CheckedModule::StaticInfo& info : module.statics) {
      emit(module.module, info.item, info.name,
           ir::type_display(state, context.strings, info.type),
           info.is_const ? CompletionItemKind::Constant
                         : CompletionItemKind::Variable);
    }
  }
}

// The primitive spellings a qualifier can name directly. A type the
// checker would resolve through a scope is looked up in the checked
// package's named types below.
std::optional<ir::TypeIdx> primitive_of(std::string_view spelling) {
  using T = ir::TypeTag;
  const std::pair<std::string_view, T> spells[] = {
      {"bool", T::I1},     {"i8", T::I8},   {"i16", T::I16},
      {"i32", T::I32},     {"i64", T::I64}, {"u8", T::U8},
      {"u16", T::U16},     {"u32", T::U32}, {"u64", T::U64},
      {"f32", T::F32},     {"f64", T::F64}, {"char", T::Char},
      {"rune", T::Rune},   {"str", T::Str}, {"isize", T::Isize},
      {"usize", T::Usize},
  };
  for (const auto& [name, tag] : spells) {
    if (spelling == name) {
      return ir::primitive_idx(tag);
    }
  }
  return std::nullopt;
}

std::optional<ir::TypeIdx> resolve_type_spelling(
    const analyzer::CheckedPackage& package,
    std::string_view spelling) {
  if (const std::optional<ir::TypeIdx> primitive = primitive_of(spelling);
      primitive.has_value()) {
    return primitive;
  }
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::NamedType& named : module.types) {
      if (named.name == spelling) {
        return named.type;
      }
    }
  }
  return std::nullopt;
}

// Top-level arguments of `Name::<A, B>`. A nested argument cannot be
// resolved by name alone, so the caller keeps the wider candidate set
// rather than dropping applicable methods.
std::optional<std::vector<std::string_view>> type_arguments(
    std::string_view qualifier) {
  const usize open = qualifier.find('<');
  if (open == std::string_view::npos || !qualifier.ends_with('>')) {
    return std::vector<std::string_view>{};
  }
  std::vector<std::string_view> args;
  usize begin = open + 1;
  u32 depth = 0;
  for (usize i = begin; i + 1 < qualifier.size(); ++i) {
    const char c = qualifier[i];
    if (c == '<') {
      ++depth;
    } else if (c == '>') {
      if (depth == 0) {
        return std::nullopt;
      }
      --depth;
    } else if (c == ',' && depth == 0) {
      args.push_back(qualifier.substr(begin, i - begin));
      begin = i + 1;
    }
  }
  args.push_back(qualifier.substr(begin, qualifier.size() - 1 - begin));
  return args;
}

// The arguments a method's receiver was instantiated with, or nothing
// for a receiver that declares none.
ir::TypeIdxRange nominal_arguments(const ir::StorageState& state,
                                   ir::TypeIdx type) {
  const ir::TypeNode& node = state.types[type];
  if (node.tag == ir::TypeTag::Struct) {
    return state.struct_types[node.as_struct()].params;
  }
  if (node.tag == ir::TypeTag::Enum) {
    return state.enum_types[node.as_enum()].params;
  }
  return {};
}

// The builder interns a primitive more than once under different
// instantiations, so identical spellings need not share an index.
// Compare the shape instead: tags for scalars, names and arguments for
// nominals, and the pointee behind each pointer kind.
bool same_type(const ir::StorageState& state,
               ir::TypeIdx left,
               ir::TypeIdx right) {
  if (left.idx == right.idx) {
    return true;
  }
  const ir::TypeNode& a = state.types[left];
  const ir::TypeNode& b = state.types[right];
  if (a.tag != b.tag) {
    return false;
  }
  using T = ir::TypeTag;
  switch (a.tag) {
    case T::Ref:
    case T::MutRef:
    case T::RawPtr:
    case T::RawMutPtr:
      return same_type(state, state.ref_types[a.as_ref()].pointee,
                       state.ref_types[b.as_ref()].pointee);
    case T::Struct: {
      const ir::StructType& x = state.struct_types[a.as_struct()];
      const ir::StructType& y = state.struct_types[b.as_struct()];
      if (x.name != y.name || x.params.size() != y.params.size()) {
        return false;
      }
      for (usize i = 0; i < x.params.size(); ++i) {
        if (!same_type(state, x.params[i], y.params[i])) {
          return false;
        }
      }
      return true;
    }
    case T::Enum: {
      const ir::EnumType& x = state.enum_types[a.as_enum()];
      const ir::EnumType& y = state.enum_types[b.as_enum()];
      if (x.name != y.name || x.params.size() != y.params.size()) {
        return false;
      }
      for (usize i = 0; i < x.params.size(); ++i) {
        if (!same_type(state, x.params[i], y.params[i])) {
          return false;
        }
      }
      return true;
    }
    default: return a.tag < T::Struct || a.tag == T::Param;
  }
}

bool arguments_match(const ir::StorageState& state,
                     ir::TypeIdx type,
                     std::span<const ir::TypeIdx> wanted) {
  const ir::TypeIdxRange actual = nominal_arguments(state, type);
  if (actual.size() != wanted.size()) {
    return false;
  }
  for (usize i = 0; i < actual.size(); ++i) {
    if (!same_type(state, actual[i], wanted[i])) {
      return false;
    }
  }
  return true;
}

std::string_view nominal_name(const pipeline::PipelineContext& context,
                              const ir::StorageState& state,
                              ir::TypeIdx type) {
  const ir::TypeNode& node = state.types[type];
  if (node.tag == ir::TypeTag::Struct) {
    return context.strings.get(state.struct_types[node.as_struct()].name);
  }
  if (node.tag == ir::TypeTag::Enum) {
    return context.strings.get(state.enum_types[node.as_enum()].name);
  }
  return {};
}

// Associated functions and variant constructors the typed qualifier
// names. A qualifier such as `Vec::<i32>::` still reads as `Vec` here:
// the arguments do not change which declarations are candidates.
std::string_view written_type(const pipeline::PipelineContext& context,
                              ast::TypeIdx type) {
  if (type == ast::TypeIdx::invalid()) {
    return {};
  }
  const diag::Span span = context.ast.types[type].span;
  const std::optional<std::string_view> source =
      context.sources.bytes(span.file);
  if (!source.has_value() || span.offset + span.length > source->size()) {
    return {};
  }
  return source->substr(span.offset, span.length);
}

std::string_view bound_name(const ast::AstArena& ast, ast::PatternIdx pattern) {
  if (pattern == ast::PatternIdx::invalid()) {
    return {};
  }
  const ast::PatternNode& node = ast.patterns[pattern];
  if (node.kind == ast::PatternKind::Ident) {
    return node.payload.ident.name.name;
  }
  if (node.kind == ast::PatternKind::MutIdent) {
    return node.payload.mut_ident.name.name;
  }
  return {};
}

// The signature as written, for a declaration the checker has not had
// to instantiate. A bounded impl's rows are created on demand, so the
// AST is the only place an unused associated function is visible.
std::string written_signature(const pipeline::PipelineContext& context,
                              ast::ItemIdx item) {
  const ast::ItemFn& fn = context.ast.items[item].payload.get<ast::ItemFn>();
  std::string out = "(";
  bool first = true;
  for (const ast::ItemFnParam& param : fn.params) {
    if (!first) {
      out += ", ";
    }
    first = false;
    const std::string_view name = bound_name(context.ast, param.pattern);
    if (!name.empty()) {
      out += name;
      out += ": ";
    }
    out += written_type(context, param.type);
  }
  out += ')';
  const std::string_view result = written_type(context, fn.return_type);
  if (!result.empty()) {
    out += " -> ";
    out += result;
  }
  return out;
}

// Associated functions declared for the nominated type, including
// those whose impl has not been instantiated in the buffer.
void collect_declared_methods(const pipeline::PipelineContext& context,
                              std::string_view name,
                              std::vector<Item>* out) {
  context.ast.items.for_each_node([&](ast::ItemIdx index) {
    const ast::ItemNode& item = context.ast.items[index];
    if (item.kind != ast::ItemKind::Impl) {
      return;
    }
    const ast::ItemImpl& impl = item.payload.get<ast::ItemImpl>();
    if (impl.type == ast::TypeIdx::invalid()) {
      return;
    }
    const ast::TypeNode& self = context.ast.types[impl.type];
    if (self.kind != ast::TypeKind::Path) {
      return;
    }
    const ast::TypePath& path = self.payload.get<ast::TypePath>();
    if (path.path == ast::PathIdx::invalid() ||
        context.ast.paths[path.path].segments.empty() ||
        context.ast.paths[path.path].segments.back().name != name) {
      return;
    }
    for (const ast::ItemIdx method : impl.methods) {
      const ast::ItemNode& node = context.ast.items[method];
      if (node.kind != ast::ItemKind::Fn) {
        continue;
      }
      const ast::ItemFn& fn = node.payload.get<ast::ItemFn>();
      if (!fn.params.empty() &&
          bound_name(context.ast, fn.params[0].pattern) == "self") {
        continue;
      }
      out->push_back(Item{std::string(fn.name.name),
                          written_signature(context, method), method,
                          CompletionItemKind::Method, "", true});
    }
  });
}

void collect_qualified(const DocumentAnalysis& analysis,
                       std::string_view qualifier,
                       std::string_view spelling,
                       std::vector<Item>* out) {
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();
  const ir::StorageState& state = analysis.types();
  const usize generic = qualifier.find('<');
  std::string_view name = qualifier.substr(0, generic);
  // `Vec::<i32>::` spells the module-style `::` before the argument
  // list; the type name itself ends at `Vec`.
  while (!name.empty() && name.back() == ':') {
    name.remove_suffix(1);
  }

  // A qualifier that spells arguments matches the receiver's arguments
  // by type identity, not by display text. Definition-site rows still
  // name every parameter and apply to each instantiation.
  std::optional<std::vector<ir::TypeIdx>> wanted;
  const std::optional<std::vector<std::string_view>> spelled =
      type_arguments(qualifier);
  if (spelled.has_value() && !spelled->empty()) {
    wanted.emplace();
    bool resolved_all = true;
    for (const std::string_view argument : *spelled) {
      const std::optional<ir::TypeIdx> type =
          resolve_type_spelling(package, argument);
      if (!type.has_value()) {
        resolved_all = false;
        break;
      }
      wanted->push_back(*type);
    }
    if (!resolved_all) {
      wanted.reset();
    }
  }

  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::MethodInfo& method : module.methods) {
      if (!method.item.is_valid() ||
          method.receiver != analyzer::CheckedModule::ReceiverKind::None ||
          nominal_name(context, state, method.self_type) != name) {
        continue;
      }
      if (wanted.has_value() && method.method_inst != analyzer::PARAM_INST &&
          !arguments_match(state, method.self_type, *wanted)) {
        continue;
      }
      out->push_back(
          Item{std::string(method.name),
               function_detail(context, state, method.params, method.ret,
                               variadic_param_name(context, method.item),
                               method.item),
               method.item, CompletionItemKind::Method, "", true});
    }
  }
  collect_declared_methods(context, name, out);
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::EnumInfo& info : module.enums) {
      if (info.name != name) {
        continue;
      }
      for (const std::string_view variant : info.variants) {
        Item item{std::string(variant),
                  std::string(spelling) + std::string(variant),
                  ast::ItemIdx::invalid(),
                  CompletionItemKind::EnumMember,
                  "",
                  true};
        item.insert_text = item.detail;
        out->push_back(std::move(item));
      }
    }
  }
}

bool starts_with(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

text::JsonValue completion_items(const Document& document,
                                 const DocumentAnalysis& analysis,
                                 Position position) {
  const usize offset = document.index().byte_offset(document.text(), position);
  const std::string_view prefix = prefix_at(document.text(), offset);
  const usize qualifier_end = prefix.rfind("::");
  const bool qualified = qualifier_end != std::string_view::npos;
  const std::string_view qualifier =
      qualified ? prefix.substr(0, qualifier_end) : std::string_view{};
  const std::string_view spelling =
      qualified ? prefix.substr(0, qualifier_end + 2) : std::string_view{};
  const std::string_view name_prefix =
      qualified ? prefix.substr(qualifier_end + 2) : prefix;
  const usize prefix_start = offset - prefix.size();
  const bool after_dot =
      prefix_start > 0 && document.text()[prefix_start - 1] == '.';

  std::vector<Item> items;
  if (analysis.ok()) {
    std::vector<Item> matched;
    if (qualified) {
      collect_qualified(analysis, qualifier, spelling, &matched);
    }
    if (!matched.empty()) {
      items = std::move(matched);
    } else {
      if (!qualified) {
        collect_locals(analysis, document.text(), offset, &items);
      }
      collect(analysis, analysis.package(), visible_names(analysis), &items);
    }
  }
  for (const lexer::KeywordInfo& keyword : lexer::keyword_infos()) {
    if (keyword.category != lexer::KeywordCategory::Keyword) {
      continue;
    }
    items.push_back(Item{std::string(keyword.spelling), "",
                         ast::ItemIdx::invalid(), CompletionItemKind::Keyword,
                         ""});
  }

  std::sort(items.begin(), items.end(),
            [](const Item& left, const Item& right) {
              if (left.label != right.label) {
                return left.label < right.label;
              }
              return left.qualified && !right.qualified;
            });
  items.erase(std::unique(items.begin(), items.end(),
                          [](const Item& left, const Item& right) {
                            return left.label == right.label;
                          }),
              items.end());

  text::JsonValue array = text::JsonValue::make_array();
  for (const Item& item : items) {
    if (!starts_with(item.label, name_prefix)) {
      continue;
    }
    // After a receiver's dot only methods can apply. After `Type::`, a
    // method or a variant is a candidate only when it matched the type
    // the buffer already spelled.
    if (qualified) {
      if ((item.kind == CompletionItemKind::Method ||
           item.kind == CompletionItemKind::EnumMember) &&
          !item.qualified) {
        continue;
      }
    } else if (after_dot != (item.kind == CompletionItemKind::Method)) {
      continue;
    }
    text::JsonValue entry = text::JsonValue::make_object();
    entry.set("label", text::JsonValue::make_string(item.label));
    entry.set("kind",
              text::JsonValue::make_number(static_cast<f64>(item.kind)));
    if (!item.detail.empty()) {
      entry.set("detail", text::JsonValue::make_string(item.detail));
    }
    std::string documentation = doc_text(analysis, item.declaration);
    if (!documentation.empty()) {
      text::JsonValue doc = text::JsonValue::make_object();
      doc.set("kind", text::JsonValue::make_string("markdown"));
      doc.set("value", text::JsonValue::make_string(documentation));
      entry.set("documentation", std::move(doc));
    }
    text::JsonValue edit_range =
        range_json(document, static_cast<u32>(offset - prefix.size()),
                   static_cast<u32>(prefix.size()));
    text::JsonValue text_edit = text::JsonValue::make_object();
    text_edit.set("range", std::move(edit_range));
    text_edit.set("newText", text::JsonValue::make_string(
                                 item.insert_text.empty() ? item.label
                                                          : item.insert_text));
    entry.set("textEdit", std::move(text_edit));
    array.push(std::move(entry));
  }

  text::JsonValue list = text::JsonValue::make_object();
  list.set("isIncomplete", text::JsonValue::make_bool(false));
  list.set("items", std::move(array));
  return list;
}

}  // namespace lsp
