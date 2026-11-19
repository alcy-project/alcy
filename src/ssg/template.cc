// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/template.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "ssg/catalog.h"
#include "ssg/markdown.h"

namespace ssg {

base::Result<std::string, std::string> stamp_template(
    std::string_view source,
    const Catalog& catalog,
    std::string_view language,
    const std::unordered_map<std::string, std::string>& values) {
  std::string out;
  usize index = 0;
  while (index < source.size()) {
    const usize open = source.find("{{", index);
    if (open == std::string_view::npos) {
      out.append(source.substr(index));
      return base::make_ok(std::move(out));
    }
    out.append(source.substr(index, open - index));
    const usize close = source.find("}}", open + 2);
    if (close == std::string_view::npos) {
      return base::make_err(
          std::string("an unterminated placeholder in a template"));
    }
    const std::string name(source.substr(open + 2, close - open - 2));
    if (name.starts_with("t:")) {
      const std::string key = name.substr(2);
      if (catalog.messages.find(key) == catalog.messages.end()) {
        return base::make_err("unknown message key in template: " + key);
      }
      out += escape_html(translate(catalog, language, key));
      index = close + 2;
      continue;
    }
    const auto found = values.find(name);
    if (found == values.end()) {
      return base::make_err("unknown template placeholder: " + name);
    }
    out += found->second;
    index = close + 2;
  }
  return base::make_ok(std::move(out));
}

}  // namespace ssg
