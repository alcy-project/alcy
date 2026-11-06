// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "testgen/tests.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"

namespace testgen {

// The name an item declares in the module's namespace, when the item is
// one that names itself: a block item takes a name of its own, so it may
// not repeat one of these.
std::optional<ast::Ident> declared_ident(const ast::ItemNode& item) {
  switch (item.kind) {
    case ast::ItemKind::Fn: return item.payload.get<ast::ItemFn>().name;
    case ast::ItemKind::Intrinsic:
      return item.payload.get<ast::ItemIntrinsic>().name;
    case ast::ItemKind::Struct: return item.payload.get<ast::ItemStruct>().name;
    case ast::ItemKind::Enum: return item.payload.get<ast::ItemEnum>().name;
    case ast::ItemKind::Spec: return item.payload.get<ast::ItemSpec>().name;
    case ast::ItemKind::Static: return item.payload.get<ast::ItemStatic>().name;
    case ast::ItemKind::Const: return item.payload.get<ast::ItemConst>().name;
    case ast::ItemKind::TestEntry:
      return item.payload.get<ast::ItemTestEntry>().name;
    default: return std::nullopt;
  }
}

namespace {

// The function a program starts at, which is the one a run replaces.
constexpr std::string_view ENTRY_POINT = "main";

std::string joined_label(std::string_view module, std::string_view name) {
  std::string label(module);
  label += "::";
  label += name;
  return label;
}

std::optional<std::string_view> declared_name(const ast::ItemNode& item) {
  if (const std::optional<ast::Ident> ident = declared_ident(item);
      ident.has_value()) {
    return ident->name;
  }
  return std::nullopt;
}

}  // namespace

SplicedModule splice_tests(std::span<const ast::ItemIdx> items,
                           std::string_view module,
                           ast::AstArena& ast) {
  SplicedModule spliced;
  spliced.items = items;
  std::vector<ast::ItemIdx> grown(items.begin(), items.end());
  // The names the module declares at its own level. A block item has to
  // pick a name none of them uses.
  std::vector<std::string_view> level;
  for (const ast::ItemIdx index : items) {
    if (!ast.items.bound(index)) {
      continue;
    }
    if (const std::optional<std::string_view> name =
            declared_name(ast.items[index]);
        name.has_value()) {
      level.push_back(*name);
    }
  }
  // The names the blocks have taken so far, in the order they took them.
  std::vector<std::string_view> blocks;
  // Records why `name` cannot be spliced, which is when it is not free; a
  // free name joins the blocks' names.
  const auto refused = [&spliced, &level, &blocks](const ast::Ident& name) {
    if (name.name == ENTRY_POINT) {
      spliced.refused.push_back(RefusedItem{Refusal::NamedMain, name});
      return true;
    }
    if (std::find(level.begin(), level.end(), name.name) != level.end()) {
      spliced.refused.push_back(RefusedItem{Refusal::Collides, name});
      return true;
    }
    if (std::find(blocks.begin(), blocks.end(), name.name) != blocks.end()) {
      spliced.refused.push_back(RefusedItem{Refusal::Duplicates, name});
      return true;
    }
    blocks.push_back(name.name);
    return false;
  };
  for (const ast::ItemIdx index : items) {
    if (!ast.items.bound(index)) {
      continue;
    }
    const ast::ItemNode& block = ast.items[index];
    if (block.kind != ast::ItemKind::Test) {
      continue;
    }
    for (const ast::ItemIdx entry_index :
         block.payload.get<ast::ItemTest>().items) {
      if (!ast.items.bound(entry_index)) {
        continue;
      }
      const ast::ItemNode& entry = ast.items[entry_index];
      if (entry.kind == ast::ItemKind::Fn) {
        if (refused(entry.payload.get<ast::ItemFn>().name)) {
          continue;
        }
        // A helper is a function the block's tests call, so it joins the
        // module the way an entry does. It is private: a block declares no
        // interface to anything outside the run.
        ast::ItemNode helper = entry;
        helper.is_pub = false;
        const ast::ItemIdx at = ast.items.push_back(helper);
        if (!at.is_valid()) {
          return spliced;
        }
        grown.push_back(at);
        continue;
      }
      if (entry.kind != ast::ItemKind::TestEntry) {
        continue;
      }
      const ast::ItemTestEntry& declared =
          entry.payload.get<ast::ItemTestEntry>();
      if (refused(declared.name)) {
        continue;
      }
      if (declared.return_type.is_valid()) {
        spliced.refused.push_back(RefusedItem{Refusal::Returns, declared.name});
        continue;
      }
      ast::ItemNode node;
      node.kind = ast::ItemKind::Fn;
      node.span = entry.span;
      // An entry is public where a helper is not: the driver calls it from
      // outside the module it was declared in, through the alias it
      // imported, or by its own name when the two share a module.
      node.is_pub = true;
      node.payload.set(ast::ItemFn{
          .name = declared.name,
          .generic = {},
          .params = {},
          .return_type = ast::TypeIdx::invalid(),
          .body = declared.body,
          .is_unsafe = false,
          .is_comp = false,
      });
      const ast::ItemIdx at = ast.items.push_back(node);
      if (!at.is_valid()) {
        // The arena is spent; the caller answers with the diagnostic that
        // names the reservation rather than with a half-spliced module.
        return spliced;
      }
      grown.push_back(at);
      spliced.tests.push_back(TestCase{
          .module = std::string(module),
          .name = std::string(declared.name.name),
          .label = joined_label(module, declared.name.name),
      });
    }
  }
  if (grown.size() != items.size()) {
    spliced.items = ast::copy_to_arena(ast.spans, grown);
  }
  return spliced;
}

std::span<const ast::ItemIdx> entry_items(std::span<const ast::ItemIdx> module,
                                          std::span<const ast::ItemIdx> driver,
                                          ast::AstArena& ast) {
  std::vector<ast::ItemIdx> joined;
  joined.reserve(module.size() + driver.size());
  for (const ast::ItemIdx index : module) {
    if (ast.items.bound(index) && ast.items[index].kind == ast::ItemKind::Fn &&
        ast.items[index].payload.get<ast::ItemFn>().name.name == ENTRY_POINT) {
      continue;
    }
    joined.push_back(index);
  }
  joined.insert(joined.end(), driver.begin(), driver.end());
  return ast::copy_to_arena(ast.spans, joined);
}

}  // namespace testgen
