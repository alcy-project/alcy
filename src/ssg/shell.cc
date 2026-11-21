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

constexpr std::string_view CHEVRON_ICON =
    "<svg viewBox=\"0 0 16 16\" aria-hidden=\"true\" focusable=\"false\" "
    "fill=\"currentColor\"><path "
    "d=\"M4.2 6.3 8 10l3.8-3.7z\"/></svg>";

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
  std::string current_name;
  for (const LanguageOption& item : options.languages) {
    if (item.current) {
      current_name = item.name;
      break;
    }
  }
  std::string language_items;
  for (const LanguageOption& item : options.languages) {
    language_items += "          <a class=\"language-option\" href=\"" +
                      escape_attribute_text(item.href) + "\" hreflang=\"" +
                      escape_attribute_text(item.language) + "\" lang=\"" +
                      escape_attribute_text(item.language) + "\"" +
                      (item.current ? " aria-current=\"true\"" : "") + ">" +
                      item.name + "</a>\n";
  }
  const std::string language_menu =
      "<details class=\"language-menu\">\n"
      "            <summary aria-label=\"" +
      escape_attribute_text(language_label + ": " + current_name) + "\">" +
      current_name + std::string(CHEVRON_ICON) +
      "</summary>\n"
      "            <div class=\"language-menu-list\">\n" +
      language_items +
      "            </div>\n"
      "          </details>";
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
  out += "            " + language_menu + "\n";
  out += "          </div>\n";
  out += "        </div>\n";
  out += "      </details>";
  return out;
}
}  // namespace ssg
