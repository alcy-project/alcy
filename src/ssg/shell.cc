// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/shell.h"

#include <string>
#include <string_view>
#include <vector>

#include "ssg/catalog.h"
#include "ssg/markdown.h"

namespace ssg {
namespace {

constexpr std::string_view MENU_ICON =
    "<svg viewBox=\"0 0 24 24\" aria-hidden=\"true\" focusable=\"false\" "
    "fill=\"none\" "
    "stroke=\"currentColor\" stroke-width=\"1.8\" "
    "stroke-linecap=\"round\"><path "
    "d=\"M3.5 6.5h17M3.5 12h17M3.5 17.5h17\"/></svg>";

std::string escape_attribute_text(std::string_view text) {
  return escape_attribute(text);
}

}  // namespace

std::string render_settings(const SettingsOptions& options) {
  const Catalog& catalog = *options.catalog;
  const std::string label_settings =
      translate(catalog, options.language, "label.settings");
  const std::string theme = translate(catalog, options.language, "label.theme");
  const std::string language_label =
      translate(catalog, options.language, "label.language");
  const std::string auto_mode =
      translate(catalog, options.language, "theme.auto");
  const std::vector<std::string> others =
      other_languages(catalog, options.language);
  std::string language_link;
  if (!others.empty()) {
    const std::string& other = others[0];
    const auto name = catalog.language_names.find(other);
    language_link =
        "<a class=\"language-link\" href=\"" +
        escape_attribute_text(options.switch_href) + "\" hreflang=\"" + other +
        "\" lang=\"" + other + "\">" +
        (name != catalog.language_names.end() ? name->second : other) + "</a>";
  }
  std::string out;
  out += "<details class=\"settings\">\n";
  out += "        <summary aria-label=\"" +
         escape_attribute_text(label_settings) + "\" title=\"" +
         escape_attribute_text(label_settings) + "\">" +
         std::string(MENU_ICON) + "</summary>\n";
  out += "        <div class=\"settings-panel\">\n";
  out += "          <div class=\"settings-row\">\n";
  out += "            <span class=\"settings-label\">" + theme + "</span>\n";
  out +=
      "            <button id=\"theme\" type=\"button\" class=\"theme-toggle\" "
      "data-theme-mode=\"auto\" aria-label=\"" +
      escape_attribute_text(auto_mode) + "\">\n";
  out +=
      "              <span class=\"theme-icon\" aria-hidden=\"true\"></span>\n";
  out += "              <span class=\"theme-name\">" + auto_mode + "</span>\n";
  out += "            </button>\n";
  out += "          </div>\n";
  out += "          <div class=\"settings-row\">\n";
  out += "            <span class=\"settings-label\">" + language_label +
         "</span>\n";
  out += "            " + language_link + "\n";
  out += "          </div>\n";
  out += "        </div>\n";
  out += "      </details>";
  return out;
}
}  // namespace ssg
