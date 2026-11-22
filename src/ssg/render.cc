// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/render.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ssg/catalog.h"
#include "ssg/documents.h"
#include "ssg/markdown.h"
#include "ssg/paths.h"
#include "ssg/shell.h"
#include "ssg/urls.h"

namespace ssg {
namespace {

std::string asset_prefix(std::string_view route) {
  const usize depth =
      static_cast<usize>(std::count(route.begin(), route.end(), '/'));
  std::string out;
  out.reserve(depth * 3);
  for (usize index = 0; index < depth; index++) {
    out += "../";
  }
  return out;
}

std::string tree_home(const Catalog& catalog,
                      std::string_view route,
                      std::string_view language) {
  const std::string root(prefix_of(catalog, language));
  const std::string directory = dirname_posix(route);
  const std::string home =
      relative_posix(directory.empty() ? std::string(".") : directory, root);
  if (home.empty()) {
    return "./";
  }
  return home.back() == '/' ? home : home + "/";
}

std::string relative_route(std::string_view from, std::string_view to) {
  const std::string base = dirname_posix(from);
  return relative_posix(base.empty() ? std::string(".") : base, to);
}

std::string align_attribute(CellAlign align) {
  switch (align) {
    case CellAlign::Left: return " style=\"text-align:left\"";
    case CellAlign::Center: return " style=\"text-align:center\"";
    case CellAlign::Right: return " style=\"text-align:right\"";
    default: return std::string();
  }
}

// The table renderer: thead from row zero, scope="col" on every
// header cell, one tbody row per body row. No span of attributes;
// a cell's alignment hangs off `style` when the separator asked.
std::string render_table(const Block& block,
                         ResolveLink resolve,
                         void* resolve_context) {
  std::string out;
  out.reserve(1024);
  out = "    <table>\n";
  out += "      <thead>\n        <tr>\n";
  const std::vector<std::string>& header = block.rows[0];
  for (usize column = 0; column < header.size(); column++) {
    const CellAlign align =
        column < block.aligns.size() ? block.aligns[column] : CellAlign::None;
    out += "          <th scope=\"col\"" + align_attribute(align) + ">" +
           render_inline(header[column], resolve, resolve_context) + "</th>\n";
  }
  out += "        </tr>\n      </thead>\n";
  out += "      <tbody>\n";
  for (usize row = 1; row < block.rows.size(); row++) {
    out += "        <tr>\n";
    const std::vector<std::string>& cells = block.rows[row];
    for (usize column = 0; column < cells.size(); column++) {
      const CellAlign align =
          column < block.aligns.size() ? block.aligns[column] : CellAlign::None;
      out += "          <td" + align_attribute(align) + ">" +
             render_inline(cells[column], resolve, resolve_context) + "</td>\n";
    }
    out += "        </tr>\n";
  }
  out += "      </tbody>\n    </table>";
  return out;
}

std::string block_html(
    const Block& block,
    const PageOptions& options,
    const std::unordered_map<const Block*, std::string>& heading_ids) {
  const Catalog& catalog = *options.catalog;
  const bool allow_links = options.resolve_link != nullptr;
  std::string (*link)(std::string_view, void*) = options.resolve_link;
  const auto t = [&catalog, &options](std::string_view key) {
    return translate(catalog, options.language, key);
  };
  (void)allow_links;
  switch (block.type) {
    case BlockType::Heading: {
      const auto found = heading_ids.find(&block);
      const std::string anchor = found == heading_ids.end()
                                     ? std::string()
                                     : " id=\"" + found->second + "\"";
      return "<h" + std::to_string(block.level) + anchor + ">" +
             render_inline(block.text, link, options.resolve_context) + "</h" +
             std::to_string(block.level) + ">";
    }
    case BlockType::Paragraph:
      return "<p>" + render_inline(block.text, link, options.resolve_context) +
             "</p>";
    case BlockType::Table:
      return render_table(block, link, options.resolve_context);
    case BlockType::Code: {
      const bool is_alcy = block.language == "alcy";
      const std::string code = is_alcy && options.highlight
                                   ? options.highlight(block.text)
                                   : escape_html(block.text);
      std::string actions = "<button type=\"button\" data-action=\"copy\">" +
                            t("guide.copy") + "</button>";
      if (is_alcy) {
        actions +=
            "\n          <button type=\"button\" data-action=\"edit\" "
            "aria-pressed=\"false\">" +
            t("guide.edit") + "</button>";
        actions +=
            "\n          <button type=\"button\" data-action=\"reset\" "
            "hidden>" +
            t("guide.reset") + "</button>";
        actions += "\n          <button type=\"button\" data-action=\"run\">" +
                   t("button.run") + "</button>";
        actions +=
            "\n          <button type=\"button\" data-action=\"open\" "
            "title=\"" +
            t("guide.openTitle") + "\">" + t("guide.open") + "</button>";
      }
      const char* output =
          is_alcy ? "\n        <div class=\"code-output\" hidden></div>" : "";
      const char* language = is_alcy ? " data-language=\"alcy\"" : "";
      std::string out;
      out += "<div class=\"code-block\"";
      out += language;
      out += ">\n";
      out += "        <div class=\"code-view\">\n";
      out += "          <pre><code>" + code + "</code></pre>\n";
      out += "        </div>\n";
      out += "        <div class=\"code-actions\" hidden>\n";
      out += "          " + actions + "\n";
      out += "        </div>";
      out += output;
      out += "\n      </div>";
      return out;
    }
    case BlockType::List: {
      const char* tag = block.ordered ? "ol" : "ul";
      std::string out = std::string("<") + tag + ">\n";
      for (const std::string& item : block.items) {
        out += "        <li>" +
               render_inline(item, link, options.resolve_context) + "</li>\n";
      }
      out += std::string("      </") + tag + ">";
      return out;
    }
    case BlockType::Blockquote:
      return "<blockquote><p>" +
             render_inline(block.text, link, options.resolve_context) +
             "</p></blockquote>";
  }
  return std::string();
}

std::string chapter_list(const DocPage& page, const PageOptions& options) {
  const Catalog& catalog = *options.catalog;
  const std::string label =
      translate(catalog, options.language, options.tree->label_key);
  std::string out = "<nav class=\"guide-nav\" aria-label=\"" + label + "\">\n";
  out += "      <h2>" + label + "</h2>\n      <ol>\n";
  for (const DocPage& other : *options.pages) {
    const std::string current =
        &other == &page ? " aria-current=\"page\"" : std::string();
    out += "        <li><a href=\"" + relative_route(page.route, other.route) +
           "\"" + current + ">" + escape_html(other.title) + "</a></li>\n";
  }
  out += "      </ol>\n    </nav>";
  return out;
}

}  // namespace
// Page-chrome glyphs the stylesheet styles and no catalog translates.
// The bytes are UTF-8 so this file stays ASCII.
const std::string MIDDLE_DOT = "\xc2\xb7";
const std::string ARROW_LEFT = "\xe2\x86\x90";
const std::string ARROW_RIGHT = "\xe2\x86\x92";

std::string heading_slug(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool dash = false;
  for (const char raw : text) {
    const u8 c = static_cast<u8>(raw);
    // Non-ASCII bytes stay verbatim: a Japanese heading keeps its own
    // characters as the anchor instead of collapsing to an empty id.
    if (c >= 0x80) {
      out += raw;
      dash = false;
      continue;
    }
    const u8 lower = static_cast<u8>(std::tolower(c));
    if (std::isalnum(lower)) {
      out += static_cast<char>(lower);
      dash = false;
      continue;
    }
    if (!out.empty() && !dash) {
      out += '-';
      dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') {
    out.pop_back();
  }
  return out;
}

std::vector<GuideSection> sections_of(const Blocks& blocks) {
  std::vector<GuideSection> sections;
  std::set<std::string> used;
  for (const Block& block : blocks) {
    if (block.type != BlockType::Heading || block.level != 2) {
      continue;
    }
    const std::string base = heading_slug(block.text);
    std::string id = base;
    for (usize count = 2; used.contains(id); count++) {
      id = base + "-" + std::to_string(count);
    }
    used.insert(id);
    GuideSection section;
    section.id = std::move(id);
    section.text = block.text;
    section.block = &block;
    sections.push_back(std::move(section));
  }
  const Block* current = nullptr;
  for (const Block& block : blocks) {
    if (block.type == BlockType::Heading && block.level <= 2) {
      current = &block;
      continue;
    }
    if (current != nullptr && block.type == BlockType::Code &&
        block.language == "alcy") {
      for (GuideSection& section : sections) {
        if (section.block == current) {
          section.programs += 1;
          break;
        }
      }
    }
  }
  return sections;
}
namespace {

std::string outline(const std::vector<GuideSection>& sections,
                    const PageOptions& options) {
  if (sections.size() < 2) {
    return "";
  }
  const Catalog& catalog = *options.catalog;
  const auto t = [&catalog, &options](std::string_view key) {
    return translate(catalog, options.language, key);
  };
  std::string out = "<details class=\"guide-outline\">\n      <summary>" +
                    t("guide.onThisPage") + "</summary>\n      <ol>\n";
  for (const GuideSection& section : sections) {
    std::string tally;
    if (section.programs > 0) {
      tally = " <span class=\"outline-tally\">";
      tally += translate(
          *options.catalog, options.language,
          section.programs == 1 ? "guide.programs.one" : "guide.programs.other",
          {{"count", std::to_string(section.programs)}});
      tally += "</span>";
    }
    out += "        <li><a href=\"#" + section.id + "\">" +
           render_inline(section.text, options.resolve_link,
                         options.resolve_context) +
           tally + "</a></li>\n";
  }
  out += "      </ol>\n    </details>";
  return out;
}

std::string pager(const DocPage& page, const PageOptions& options) {
  const Catalog& catalog = *options.catalog;
  const auto t = [&catalog, &options](std::string_view key) {
    return translate(catalog, options.language, key);
  };
  usize found = std::string::npos;
  for (usize i = 0; i < options.pages->size(); i++) {
    if (&(*options.pages)[i] == &page) {
      found = i;
      break;
    }
  }
  std::string back;
  if (found != 0 && found != std::string::npos) {
    const DocPage& previous = (*options.pages)[found - 1];
    back = "<a href=\"" + relative_route(page.route, previous.route) + "\">" +
           ARROW_LEFT + " " + t("site.previous") + " " + MIDDLE_DOT + " " +
           escape_html(previous.title) + "</a>";
  } else {
    back = "<span class=\"empty\"></span>";
  }
  std::string forward;
  const bool has_next =
      found != std::string::npos && found + 1 < options.pages->size();
  if (has_next) {
    const DocPage& next = (*options.pages)[found + 1];
    forward = "<a href=\"" + relative_route(page.route, next.route) + "\">" +
              t("site.next") + " " + MIDDLE_DOT + " " +
              escape_html(next.title) + " " + ARROW_RIGHT + "</a>";
  } else if (!options.tree->continues_into_segment.empty()) {
    const std::string prefix = page.language == catalog.default_language
                                   ? std::string()
                                   : std::string(page.language) + '/';
    forward = "<a class=\"pager-onward\" href=\"" +
              relative_route(
                  page.route,
                  prefix + std::string(options.tree->continues_into_segment) +
                      "/index.html") +
              "\">" + t("site.next") + " " + MIDDLE_DOT + " " +
              t(options.tree->continues_into_label_key) + " " + ARROW_RIGHT +
              "</a>";
  } else {
    forward = "<span class=\"empty\"></span>";
  }
  return "<nav class=\"guide-pager\" aria-label=\"" + t("site.pager") +
         "\">\n        " + back + "\n        " + forward + "\n      </nav>";
}

}  // namespace

std::string footer(const Stamp& stamp) {
  const std::string text = escape_html(stamp.text);
  const std::string mark = stamp.url.empty()
                               ? text
                               : "<a href=\"" + escape_attribute(stamp.url) +
                                     "\" rel=\"noopener\">" + text + "</a>";
  return "<footer class=\"site-footer\">\n      <span>alcy build " + mark +
         "</span>\n      <a href=\"https://github.com/alcy-project/alcy\" "
         "rel=\"noopener\">GitHub</a>\n    </footer>";
}

std::string site_header(const Catalog& catalog,
                        std::string_view route,
                        std::string_view language,
                        std::string_view tree_path,
                        std::string_view current_segment) {
  const std::string home = tree_home(catalog, route, language);
  const auto t = [&catalog, language](std::string_view key) {
    return translate(catalog, language, key);
  };
  std::vector<LanguageOption> languages;
  for (const std::string& target : catalog.languages) {
    const LanguageInfo info = language_info_of(catalog, target);
    languages.push_back(
        {.language = target,
         .name = info.name,
         .href = language_href(catalog, route, target, tree_path),
         .current = target == language});
  }
  const std::string settings = render_settings(
      {.catalog = &catalog, .language = language, .languages = languages});
  const auto current = [&current_segment](std::string_view segment) {
    return segment == current_segment ? std::string(" aria-current=\"page\"")
                                      : std::string();
  };
  std::string out;
  out += "<header class=\"site-header\">\n";
  out += "      <a class=\"site-brand\" href=\"" + home + "\">alcy</a>\n";
  out +=
      "      <nav class=\"site-nav\" aria-label=\"" + t("site.nav") + "\">\n";
  out += "        <a href=\"" + home + "guide/\"" + current("guide") + ">" +
         t("site.guide") + "</a>\n";
  out += "        <a href=\"" + home + "reference/\"" + current("reference") +
         ">" + t("site.reference") + "</a>\n";
  out += "        <a href=\"" + home + "tools/\"" + current("tools") + ">" +
         t("site.tools") + "</a>\n";
  out += "        <a href=\"" + home + "playground/\"" + current("playground") +
         ">" + t("site.playground") + "</a>\n";
  out +=
      "        <button type=\"button\" class=\"site-search-open\" "
      "data-action=\"search\">" +
      t("site.search") + "</button>\n";
  out += "      </nav>\n      " + settings + "\n    </header>";
  return out;
}
std::string render_guide_page(const DocPage& page, const PageOptions& options) {
  const Catalog& catalog = *options.catalog;
  const auto t = [&catalog, &options](std::string_view key) {
    return translate(catalog, options.language, key);
  };
  const std::string assets = asset_prefix(page.route);
  const bool has_code = std::any_of(
      page.blocks.begin(), page.blocks.end(),
      [](const Block& block) { return block.type == BlockType::Code; });
  const bool has_runnable = std::any_of(
      page.blocks.begin(), page.blocks.end(), [](const Block& block) {
        return block.type == BlockType::Code && block.language == "alcy";
      });
  const std::string guide_css = has_code
                                    ? "\n    <link rel=\"stylesheet\" href=\"" +
                                          assets + "shared/guide.css\" />"
                                    : std::string();
  std::string guide_js;
  if (has_code) {
    guide_js += "\n    <script type=\"module\" src=\"" + assets +
                "shared/guide-copy.js\"></script>";
  }
  if (has_runnable) {
    guide_js += "\n    <script type=\"module\" src=\"" + assets +
                "shared/guide.js\"></script>";
  }
  const std::vector<GuideSection> sections = sections_of(page.blocks);
  std::unordered_map<const Block*, std::string> heading_ids;
  for (const GuideSection& section : sections) {
    heading_ids.emplace(section.block, section.id);
  }
  std::string article;
  for (const Block& block : page.blocks) {
    if (!article.empty()) {
      article += "\n      ";
    }
    article += block_html(block, options, heading_ids);
  }
  const std::string description =
      std::string(page.title) + " " + MIDDLE_DOT + " " +
      translate(catalog, options.language, options.tree->summary_key);
  std::string out;
  // One page is tens of kilobytes; one reservation beats the growth
  // reallocations of the dozens of appends below.
  out.reserve(static_cast<usize>(32) * 1024);
  out += "<!doctype html>\n";
  out += "<!-- Copyright 2026 The Alcy Project Authors -->\n";
  out += "<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->\n";
  out += "<html lang=\"" + std::string(options.language) + "\">\n";
  out += "  <head>\n";
  out += "    <meta charset=\"utf-8\" />\n";
  out +=
      "    <meta name=\"viewport\" content=\"width=device-width, "
      "initial-scale=1\" />\n";
  out += "    <meta name=\"description\" content=\"" +
         escape_html(description) + "\" />\n";
  out += "    <title>" + escape_html(page.title) + " " + MIDDLE_DOT +
         " alcy</title>\n";
  out += "    <script>" + std::string(options.boot) + "</script>\n";
  out += "    " + catalog_map(assets, options.language) + "\n";
  out += "    <link rel=\"stylesheet\" href=\"" + assets +
         "shared/site.css\" />" + guide_css + "\n";
  out += "    <link rel=\"modulepreload\" href=\"" + assets +
         "shared/shell.js\" />\n";
  if (has_code) {
    out += "    <link rel=\"modulepreload\" href=\"" + assets +
           "shared/guide-copy.js\" />\n";
  }
  if (has_runnable) {
    out += "    <link rel=\"modulepreload\" href=\"" + assets +
           "shared/guide.js\" />\n";
  }
  out += "    <link rel=\"icon\" href=\"" + assets + "favicon.svg\" />\n";
  out += "    " + page_links(catalog, options.language, page.path) + "\n";
  out += "    " +
         social_meta(std::string(page.title) + " " + MIDDLE_DOT + " alcy",
                     description, page.route) +
         "\n";
  out += "    " + speculation_rules() + "\n";
  out += "  </head>\n";
  out += "  <body class=\"site\">\n";
  out += "    <a class=\"skip-link\" href=\"#content\">" + t("site.skip") +
         "</a>\n";
  out += "    " +
         site_header(catalog, page.route, options.language, page.path,
                     options.tree->segment) +
         "\n\n";
  out += "    <div class=\"guide-layout\">\n";
  out += "      <div class=\"guide-aside\">\n";
  out += "        " + chapter_list(page, options) + "\n";
  const std::string outline_html = outline(sections, options);
  if (!outline_html.empty()) {
    out += "        " + outline_html + "\n";
  }
  out += "      </div>\n\n";
  out +=
      "      <main class=\"guide-article\" id=\"content\" tabindex=\"-1\" "
      "lang=\"" +
      std::string(options.language) + "\">\n";
  out += "        " + article + "\n";
  out += "        " + pager(page, options) + "\n";
  out += "      </main>\n";
  out += "    </div>\n\n";
  out += "    " + footer(options.stamp) + "\n\n";
  out += "    <script type=\"module\" src=\"" + assets +
         "shared/shell.js\"></script>" + guide_js + "\n";
  out += "  </body>\n</html>\n";
  return out;
}

std::string render_tree_index(const std::vector<DocPage>& pages,
                              const Catalog& catalog,
                              std::string_view language,
                              std::string_view boot,
                              const Stamp& stamp,
                              const Tree& tree) {
  const std::string route =
      prefix_of(catalog, language) + std::string(tree.segment) + "/index.html";
  const std::string assets = asset_prefix(route);
  const auto t = [&catalog, &language](std::string_view key) {
    return translate(catalog, language, key);
  };
  const std::string label = t(tree.label_key);
  std::string out;
  // One page is tens of kilobytes; one reservation beats the growth
  // reallocations of the dozens of appends below.
  out.reserve(static_cast<usize>(32) * 1024);
  out += "<!doctype html>\n";
  out += "<!-- Copyright 2026 The Alcy Project Authors -->\n";
  out += "<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->\n";
  out += "<html lang=\"" + std::string(language) + "\">\n";
  out += "  <head>\n";
  out += "    <meta charset=\"utf-8\" />\n";
  out +=
      "    <meta name=\"viewport\" content=\"width=device-width, "
      "initial-scale=1\" />\n";
  out += "    <meta name=\"description\" content=\"" +
         escape_html(t(tree.summary_key)) + "\" />\n";
  out += "    <title>" + label + " " + MIDDLE_DOT + " alcy</title>\n";
  out += "    <script>" + std::string(boot) + "</script>\n";
  out += "    " + catalog_map(assets, language) + "\n";
  out += "    <link rel=\"stylesheet\" href=\"" + assets +
         "shared/site.css\" />\n";
  out += "    <link rel=\"modulepreload\" href=\"" + assets +
         "shared/shell.js\" />\n";
  out += "    <link rel=\"icon\" href=\"" + assets + "favicon.svg\" />\n";
  out +=
      "    " +
      page_links(catalog, language, std::string(tree.segment) + "/index.html") +
      "\n";
  out += "    " +
         social_meta(label + " " + MIDDLE_DOT + " alcy", t(tree.summary_key),
                     route) +
         "\n";
  out += "    " + speculation_rules() + "\n";
  out += "  </head>\n";
  out += "  <body class=\"site\">\n";
  out += "    <a class=\"skip-link\" href=\"#content\">" + t("site.skip") +
         "</a>\n";
  out += "    " +
         site_header(catalog, route, language,
                     std::string(tree.segment) + "/index.html", tree.segment) +
         "\n\n";
  out +=
      "    <main class=\"site-main guide-contents\" id=\"content\" "
      "tabindex=\"-1\">\n";
  out += "      <h1>" + label + "</h1>\n      <ol>\n";
  for (const DocPage& page : pages) {
    out += "        <li><a href=\"" + relative_route(route, page.route) +
           "\">" + escape_html(page.title) + "</a></li>\n";
  }
  out += "      </ol>\n    </main>\n\n";
  out += "    " + footer(stamp) + "\n\n";
  out += "    <script type=\"module\" src=\"" + assets +
         "shared/shell.js\"></script>\n";
  out += "  </body>\n</html>\n";
  return out;
}

std::string render_not_found(
    std::string_view boot,
    const Stamp& stamp,
    const std::function<std::string(std::string_view)>& highlight) {
  const std::string source =
      "fn main() -> i32 {\n"
      "  println(\"Page not found\")\n"
      "  ret 404\n"
      "}";
  std::string out;
  // One page is tens of kilobytes; one reservation beats the growth
  // reallocations of the dozens of appends below.
  out.reserve(static_cast<usize>(32) * 1024);
  out += "<!doctype html>\n";
  out += "<!-- Copyright 2026 The Alcy Project Authors -->\n";
  out += "<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->\n";
  out += "<html lang=\"en\">\n";
  out += "  <head>\n";
  out += "    <meta charset=\"utf-8\" />\n";
  out +=
      "    <meta name=\"viewport\" content=\"width=device-width, "
      "initial-scale=1\" />\n";
  out += "    <meta name=\"robots\" content=\"noindex\" />\n";
  out += "    <title>Page not found " + MIDDLE_DOT + " alcy</title>\n";
  out += "    <script>\n";
  out += "      (() => {\n";
  out += "        const base = document.createElement(\"base\");\n";
  out +=
      "        base.href = location.pathname.startsWith(\"/alcy/\") ? "
      "\"/alcy/\" : \"/\";\n";
  out += "        document.head.append(base);\n";
  out += "      })();\n";
  out += "    </script>\n";
  out += "    <script>" + std::string(boot) + "</script>\n";
  out += "    " + catalog_map("", "en") + "\n";
  out += "    <link rel=\"stylesheet\" href=\"shared/site.css\" />\n";
  out += "    <link rel=\"stylesheet\" href=\"shared/guide.css\" />\n";
  out += "    <link rel=\"icon\" href=\"favicon.svg\" />\n";
  out += "  </head>\n";
  out += "  <body class=\"site\">\n";
  out += "    <a class=\"skip-link\" href=\"#content\">Skip to content</a>\n";
  out += "    <header class=\"site-header\">\n";
  out += "      <a class=\"site-brand\" href=\"./\">alcy</a>\n";
  out += "      <nav class=\"site-nav\" aria-label=\"Site\">\n";
  out += "        <a href=\"guide/\">Guide</a>\n";
  out += "        <a href=\"reference/\">Reference</a>\n";
  out += "        <a href=\"playground/\">Playground</a>\n";
  out += "      </nav>\n";
  out += "    </header>\n\n";
  out += "    <main class=\"site-main\" id=\"content\" tabindex=\"-1\">\n";
  out += "      <h1>Page not found</h1>\n";
  out += "      <p>\n";
  out +=
      "        The page you asked for does not exist. It may have moved, or "
      "the link\n";
  out += "        that brought you here may be wrong.\n";
  out += "      </p>\n";
  out += "      <div class=\"actions\">\n";
  out += "        <a class=\"primary\" href=\"./\">Go home</a>\n";
  out += "        <a href=\"guide/\">read the guide</a>\n";
  out += "        <a href=\"reference/\">Read the reference</a>\n";
  out += "        <a href=\"playground/\">Open the playground</a>\n";
  out += "      </div>\n";
  out += "      <div class=\"code-block\" data-language=\"alcy\">\n";
  out += "        <div class=\"code-view\">\n";
  out += "          <pre><code>" + highlight(source) + "</code></pre>\n";
  out += "        </div>\n";
  out += "        <div class=\"code-actions\" hidden>\n";
  out +=
      "          <button type=\"button\" data-action=\"copy\">Copy</button>\n";
  out += "          <button type=\"button\" data-action=\"run\">run</button>\n";
  out += "        </div>\n";
  out += "        <div class=\"code-output\" hidden></div>\n";
  out += "      </div>\n";
  out += "    </main>\n\n";
  out += "    " + footer(stamp) + "\n";
  out += "    <script type=\"module\" src=\"shared/guide-copy.js\"></script>\n";
  out += "    <script type=\"module\" src=\"shared/guide.js\"></script>\n";
  out += "  </body>\n</html>\n";
  return out;
}
}  // namespace ssg
