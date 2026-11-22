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
// one that names itself. All names live in one namespace, so a gather
// compares them without regard to the kind that declared them.
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

GatheredModule gather_tests(std::span<const ast::ItemIdx> items,
                            std::string_view module,
                            ast::AstArena& ast) {
  GatheredModule gathered;
  // Keep the original list until a replacement is ready: an arena that
  // runs out of room must not leave the module with no items at all.
  gathered.items = items;
  std::vector<ast::ItemIdx> grown(items.begin(), items.end());
  // Names the module declares outside its tests: a test cannot take one
  // of those, because the run's driver calls it in that namespace.
  std::vector<std::string_view> module_names;
  for (const ast::ItemIdx index : items) {
    if (!ast.items.bound(index)) {
      continue;
    }
    const ast::ItemNode& node = ast.items[index];
    if (node.kind == ast::ItemKind::Fn &&
        node.payload.get<ast::ItemFn>().is_test) {
      continue;
    }
    if (const std::optional<std::string_view> name = declared_name(node);
        name.has_value()) {
      module_names.push_back(*name);
    }
  }
  // Names the tests already gathered. Two tests with one name are a
  // duplicate, which has its own diagnostic rather than a collision
  // with the module.
  std::vector<std::string_view> taken;
  std::vector<usize> refused_at;
  for (usize at = 0; at < items.size(); ++at) {
    const ast::ItemIdx index = items[at];
    if (!ast.items.bound(index)) {
      continue;
    }
    ast::ItemNode node = ast.items[index];
    if (node.kind != ast::ItemKind::Fn ||
        !node.payload.get<ast::ItemFn>().is_test) {
      continue;
    }
    const ast::ItemFn& fn = node.payload.get<ast::ItemFn>();
    if (fn.name.name == ENTRY_POINT) {
      gathered.refused.push_back(RefusedTest{Refusal::NamedMain, fn.name});
      refused_at.push_back(at);
      continue;
    }
    if (std::find(module_names.begin(), module_names.end(), fn.name.name) !=
        module_names.end()) {
      gathered.refused.push_back(RefusedTest{Refusal::Collides, fn.name});
      refused_at.push_back(at);
      continue;
    }
    if (std::find(taken.begin(), taken.end(), fn.name.name) != taken.end()) {
      gathered.refused.push_back(RefusedTest{Refusal::Duplicates, fn.name});
      refused_at.push_back(at);
      continue;
    }
    if (fn.return_type.is_valid()) {
      gathered.refused.push_back(RefusedTest{Refusal::Returns, fn.name});
      refused_at.push_back(at);
      continue;
    }
    taken.push_back(fn.name.name);
    // The driver calls the test from outside its module: the function
    // has to be visible where the module's other items need not be.
    node.is_pub = true;
    ast::ItemFn published = node.payload.get<ast::ItemFn>();
    published.is_test = false;
    node.payload.set(published);
    const ast::ItemIdx replacement = ast.items.push_back(node);
    if (!replacement.is_valid()) {
      // The arena is spent; the caller reports the reservation rather
      // than a half-gathered module.
      return gathered;
    }
    grown[at] = replacement;
    gathered.tests.push_back(TestCase{
        .module = std::string(module),
        .name = std::string(fn.name.name),
        .label = joined_label(module, fn.name.name),
    });
  }
  // A refused test is gone from the module, not left behind as an ordinary
  // function with the marker still set: that would add a second, unrelated
  // diagnostic to the one the refusal already reported.
  usize removed = 0;
  for (const usize at : refused_at) {
    grown.erase(grown.begin() + (at - removed));
    ++removed;
  }
  const bool changed = grown.size() != items.size() || [&] {
    for (usize i = 0; i < items.size(); ++i) {
      if (grown[i] != items[i]) {
        return true;
      }
    }
    return false;
  }();
  if (changed) {
    const std::span<const ast::ItemIdx> copied =
        ast::copy_to_arena(ast.spans, grown);
    if (!copied.empty()) {
      gathered.items = copied;
    }
  }
  return gathered;
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
