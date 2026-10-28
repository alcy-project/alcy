// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linter/config.h"

#include <optional>
#include <span>
#include <string_view>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/messages.h"
#include "linter/diag_code.h"
#include "linter/level.h"
#include "linter/rule.h"
#include "source/source.h"

// clang-format off
// Umbrella header provides the .inl implementations; keep it whole.
#include "toml++/toml.hpp"  // IWYU pragma: keep
// Other headers must be included after toml.hpp
#include "toml++/impl/node.hpp"
#include "toml++/impl/parse_error.hpp"
#include "toml++/impl/parse_result.hpp"
#include "toml++/impl/parser.hpp"
#include "toml++/impl/table.hpp"
// clang-format on

namespace linter {

namespace {

bool read_level(const toml::node& node, LintLevel& out) {
  const std::optional<std::string_view> text = node.value<std::string_view>();
  if (!text.has_value()) {
    return false;
  }
  const std::optional<LintLevel> level = lint_level_from_name(*text);
  if (!level.has_value()) {
    return false;
  }
  out = *level;
  return true;
}

}  // namespace

base::Result<LintConfig, diag::Reported> parse_lint_config(
    std::string_view bytes,
    std::string_view filename,
    source::FileId /*file*/,
    diag::DiagBag& bag,
    mem::Arena& arena) {
  toml::parse_result result = toml::parse(bytes, filename);
  if (!result) {
    // The lint file is read without a source location, so the message
    // names the file rather than pointing into it.
    const toml::parse_error& error = result.error();
    const u32 index = bag.emit<i18n::Key::LintConfigSyntaxError>(
        LintCode::ConfigSyntaxError, filename, error.description());
    (void)index;
    return base::make_err(diag::Reported{});
  }

  const std::span<const Rule> registered = all_rules();
  RuleSetting* const settings = static_cast<RuleSetting*>(arena.alloc(
      sizeof(RuleSetting) * registered.size(), alignof(RuleSetting)));
  if (settings == nullptr && !registered.empty()) {
    return base::make_err(diag::Reported{});
  }
  for (usize i = 0; i < registered.size(); ++i) {
    settings[i].level = registered[i].default_level;
    settings[i].max_args = 0;
  }

  LintConfig config;
  config.rules = std::span<const RuleSetting>(settings, registered.size());

  const toml::table& root = result.table();
  const toml::table* overrides = nullptr;
  for (const auto& [key, node] : root) {
    const std::string_view name = key.str();
    if (name == "level") {
      LintLevel level = LintLevel::Warn;
      if (!read_level(node, level)) {
        const u32 index = bag.emit<i18n::Key::LintConfigUnknownLevel>(
            LintCode::ConfigSemanticError,
            node.value<std::string_view>().value_or(""), filename);
        (void)index;
        return base::make_err(diag::Reported{});
      }
      // The top-level level is what a rule not named below uses, so it
      // replaces every rule's own default before the overrides apply.
      config.default_level = level;
      for (usize i = 0; i < registered.size(); ++i) {
        settings[i].level = level;
      }
      continue;
    }
    if (name == "rules") {
      if (!node.is_table()) {
        const u32 index = bag.emit<i18n::Key::LintConfigRuleNotTable>(
            LintCode::ConfigSemanticError, name, filename);
        (void)index;
        return base::make_err(diag::Reported{});
      }
      overrides = node.as_table();
      continue;
    }
  }

  if (overrides == nullptr) {
    return base::make_ok(config);
  }

  for (const auto& [key, node] : *overrides) {
    const u32 index = rule_index(key.str());
    if (index == NO_RULE) {
      const u32 emitted = bag.emit<i18n::Key::LintConfigUnknownRule>(
          LintCode::ConfigSemanticError, key.str(), filename);
      (void)emitted;
      return base::make_err(diag::Reported{});
    }
    if (node.is_string()) {
      LintLevel level = settings[index].level;
      if (!read_level(node, level)) {
        const u32 emitted = bag.emit<i18n::Key::LintConfigUnknownLevel>(
            LintCode::ConfigSemanticError,
            node.value<std::string_view>().value_or(""), filename);
        (void)emitted;
        return base::make_err(diag::Reported{});
      }
      settings[index].level = level;
      continue;
    }
    if (!node.is_table()) {
      const u32 emitted = bag.emit<i18n::Key::LintConfigRuleNotTable>(
          LintCode::ConfigSemanticError, key.str(), filename);
      (void)emitted;
      return base::make_err(diag::Reported{});
    }
    const toml::table& table = *node.as_table();
    for (const auto& [option_key, option] : table) {
      const std::string_view option_name = option_key.str();
      if (option_name == "level") {
        LintLevel level = settings[index].level;
        if (!option.is_string() || !read_level(option, level)) {
          const u32 emitted = bag.emit<i18n::Key::LintConfigUnknownLevel>(
              LintCode::ConfigSemanticError,
              option.value<std::string_view>().value_or(""), filename);
          (void)emitted;
          return base::make_err(diag::Reported{});
        }
        settings[index].level = level;
        continue;
      }
      if (option_name == "max-args") {
        const std::optional<i64> value = option.value<i64>();
        if (!value.has_value() || *value < 1) {
          const u32 emitted = bag.emit<i18n::Key::LintConfigOptionNotInteger>(
              LintCode::ConfigSemanticError, key.str(), filename);
          (void)emitted;
          return base::make_err(diag::Reported{});
        }
        settings[index].max_args = static_cast<u32>(*value);
        continue;
      }
    }
  }
  return base::make_ok(config);
}

}  // namespace linter
