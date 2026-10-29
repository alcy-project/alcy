// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <string_view>
#include <vector>

#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "i18n/messages.h"
#include "linter/context.h"
#include "linter/engine.h"
#include "linter/rules/rules.h"

namespace linter::rules {

namespace {

// Whether a name is exempt from the reachability question: the entry
// point, the language's conventions, and anything the author marked as
// deliberate.
bool exempt_name(std::string_view name) {
  return name == "main" || name == "self" || name == "Self" || name == "new";
}

// One private signature the rule may report. The roots are the entry
// point and the public surface; everything they reach is live, and a
// signature no recorded call names is the finding.
struct Candidate {
  u32 module;
  u32 index;
  ast::ItemIdx item;
  std::string_view name;
};

}  // namespace

void DeadCode::run(LintContext& ctx) {
  // Reachability runs over the checker's call graph, which is already
  // resolved: a call target names the module and the signature index it
  // reached, so no path resolution happens here. An edge is a call that
  // was checked; a dynamic dispatch through a spec is an edge to every
  // implementation the checker recorded, which is what the table holds.
  //
  // The checker keys its targets by callee and instantiation, not by
  // the function the call sits in, so liveness is per callee: a
  // signature is live when any call anywhere names it. That keeps a
  // helper chain live when only its head is called from a dead caller,
  // which under-reports rather than invents a chain the rule cannot
  // prove.
  const analyzer::CheckedPackage& package = ctx.package;

  // The signatures that own a declared item, per module. A signature
  // without an item is synthesized and never a finding. The rule reads
  // the goal package alone: staged toolchain sources and path
  // dependencies have their own callers, and lint.toml does not
  // configure them.
  std::vector<Candidate> candidates;
  for (u32 module = 0; module < package.modules.size(); ++module) {
    if (!lintable(ctx.package.tree, module)) {
      continue;
    }
    const analyzer::CheckedModule& checked = package.modules[module];
    for (u32 index = 0; index < checked.functions.size(); ++index) {
      const analyzer::CheckedModule::FnSig& sig = checked.functions[index];
      if (!sig.item.is_valid() || exempt_name(sig.name)) {
        continue;
      }
      // A pub declaration is the package's surface: another package may
      // call it, and this run does not read them.
      const ast::ItemNode& node = ctx.ast.items[sig.item];
      if (node.is_pub) {
        continue;
      }
      candidates.push_back(Candidate{.module = module,
                                     .index = index,
                                     .item = sig.item,
                                     .name = sig.name});
    }
  }
  if (candidates.empty()) {
    return;
  }

  // Whether a call target names a signature. A target's (module, index)
  // pair is the resolution the checker recorded; a method call and a
  // bare call share the table. One pass over the targets marks what a
  // call reaches, so the candidate loop is a lookup rather than a scan
  // per signature.
  std::vector<std::vector<char>> called(package.modules.size());
  for (u32 module = 0; module < package.modules.size(); ++module) {
    called[module].assign(package.modules[module].functions.size(), 0);
  }
  for (const analyzer::CheckedModule& checked : package.modules) {
    for (const analyzer::CheckedModule::CallTarget& target :
         checked.call_targets) {
      if (target.module < called.size() &&
          target.index < called[target.module].size()) {
        called[target.module][target.index] = 1;
      }
    }
  }

  for (const Candidate& candidate : candidates) {
    if (called[candidate.module][candidate.index] != 0) {
      continue;
    }
    const ast::ItemNode& node = ctx.ast.items[candidate.item];
    const u32 finding =
        ctx.report<i18n::Key::LintDeadCode>(node.span, candidate.name);
    ctx.label<i18n::Key::LintDeadCodeNote>(finding, node.span);
  }
}

}  // namespace linter::rules
