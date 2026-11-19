// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The command that stamps the site: one language tree of pages per
// language, from the sources under `docs/`. The CLI runs it once per
// build; nothing here persists state. Every flag is a path or a build
// stamp value; there is no configuration file.

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "playground/playground.h"
#include "ssg/catalog.h"
#include "ssg/discover.h"
#include "ssg/documents.h"
#include "ssg/highlight.h"
#include "ssg/links.h"
#include "ssg/markdown.h"
#include "ssg/paths.h"
#include "ssg/render.h"
#include "ssg/template.h"
#include "ssg/urls.h"

namespace fs = std::filesystem;

namespace {

const char* USAGE =
    "usage: alcy-docsite --docs <dir> --dist <dir> --repo-root <dir>\n"
    "                  [--site <dir>] [--spec <dir>] [--tools <dir>]\n"
    "                  [--stamp <stamp>] [--stamp-url <url>]\n";

void print_usage() {
  std::cerr << USAGE;
}

struct Options {
  std::string docs;
  std::string dist;
  std::string site = "site";
  std::string repo_root = ".";
  std::string spec = "docs/spec";
  std::string tools = "docs/tools";
  std::string stamp = "unknown";
  std::string stamp_url;
  std::string catalog;
};

base::Result<Options, std::string> parse_args(i32 argc, char** argv) {
  Options options;
  std::unordered_map<std::string, std::string> values;
  for (i32 index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) {
      return base::make_err(std::string("missing value for ") + argv[index]);
    }
    const std::string key = argv[index];
    if (!key.starts_with("--")) {
      return base::make_err(std::string("unexpected argument ") + key);
    }
    values[key] = argv[index + 1];
  }
  auto need = [&](const char* name) -> base::Result<std::string, std::string> {
    const auto found = values.find(name);
    if (found == values.end()) {
      return base::make_err(std::string("missing ") + name);
    }
    return base::make_ok(found->second);
  };
  auto docs = need("--docs");
  if (docs.is_err()) {
    return base::make_err(std::move(docs).unwrap_err());
  }
  options.docs = std::move(docs).unwrap();
  auto dist = need("--dist");
  if (dist.is_err()) {
    return base::make_err(std::move(dist).unwrap_err());
  }
  options.dist = std::move(dist).unwrap();
  auto root = need("--repo-root");
  if (root.is_err()) {
    return base::make_err(std::move(root).unwrap_err());
  }
  options.repo_root = std::move(root).unwrap();
  if (auto it = values.find("--site"); it != values.end()) {
    options.site = it->second;
  }
  if (auto it = values.find("--spec"); it != values.end()) {
    options.spec = it->second;
  }
  if (auto it = values.find("--tools"); it != values.end()) {
    options.tools = it->second;
  }
  if (auto it = values.find("--stamp"); it != values.end()) {
    options.stamp = it->second;
  }
  if (auto it = values.find("--stamp-url"); it != values.end()) {
    options.stamp_url = it->second;
  }
  if (auto it = values.find("--catalog"); it != values.end()) {
    options.catalog = it->second;
  }
  if (options.catalog.empty()) {
    options.catalog =
        (fs::path(options.site) / "shared" / "catalog.json").string();
  }
  return base::make_ok(std::move(options));
}

std::string read_whole_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream bytes;
  bytes << input.rdbuf();
  return bytes.str();
}

void write_string_to_file(const fs::path& root,
                          std::string_view route,
                          std::string_view content) {
  const fs::path target = root / fs::path(std::string(route));
  std::error_code error;
  fs::create_directories(target.parent_path(), error);
  std::ofstream output(target, std::ios::binary | std::ios::trunc);
  output << content;
}

bool site_exists_relative(const fs::path& root, std::string_view candidate) {
  if (candidate.empty()) {
    return true;
  }
  const fs::path target = root / fs::path(std::string(candidate));
  std::error_code error;
  if (candidate.ends_with('/')) {
    return fs::is_directory(target, error) && !error;
  }
  return fs::is_regular_file(target, error) && !error;
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 16);
  for (const char c : text) {
    switch (c) {
      case '"':
        out += '\\';
        out += '"';
        break;
      case '\\':
        out += '\\';
        out += '\\';
        break;
      case '\n':
        out += '\\';
        out += 'n';
        break;
      case '\r':
        out += '\\';
        out += 'r';
        break;
      case '\t':
        out += '\\';
        out += 't';
        break;
      default: out += c;
    }
  }
  return out;
}

std::string route_dir(std::string_view route) {
  const usize slash = route.rfind('/');
  return slash == std::string_view::npos ? std::string(".")
                                         : std::string(route.substr(0, slash));
}

struct Context {
  Options options;
  ssg::Catalog catalog;
  ssg::Stamp stamp;
  std::string boot;
  ssg::Highlight highlight;
  std::unordered_set<std::string> planned;
};

std::string boot_text(const fs::path& site) {
  std::string boot = read_whole_file(site / "shared" / "boot.js");
  if (boot.find("</script") != std::string::npos) {
    return std::string();
  }
  return boot;
}

// The diagnostics the fence compiler answers with carry the whole
// site-visible message; the first error is the one that matters. The
// object's `message` string is pulled out with a focused scan, since
// the shape is fixed.
std::string first_error_message(std::string_view json) {
  const usize error_at = json.find("\"severity\":\"error\"");
  if (error_at == std::string_view::npos) {
    return std::string("the compiler refused the fence");
  }
  const usize message_at = json.find("\"message\":\"", error_at);
  if (message_at == std::string_view::npos) {
    return std::string("the compiler refused the fence");
  }
  const usize value_at = message_at + std::string_view("\"message\":\"").size();
  usize cursor = value_at;
  std::string out;
  while (cursor < json.size()) {
    const char c = json[cursor];
    if (c == '"') {
      break;
    }
    if (c != '\\') {
      out += c;
      cursor++;
      continue;
    }
    const char escape = cursor + 1 < json.size() ? json[cursor + 1] : '\0';
    switch (escape) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'u': {
        u32 code = 0;
        for (i32 i = 0; i < 4; i++) {
          const char hex =
              cursor + 2 + i < json.size() ? json[cursor + 2 + i] : '0';
          code <<= 4;
          code |= hex >= '0' && hex <= '9'   ? static_cast<u32>(hex - '0')
                  : hex >= 'a' && hex <= 'f' ? static_cast<u32>(hex - 'a' + 10)
                  : hex >= 'A' && hex <= 'F' ? static_cast<u32>(hex - 'A' + 10)
                                             : 0;
        }
        if (code < 0x80) {
          out += static_cast<char>(code);
        } else if (code < 0x800) {
          out += static_cast<char>(0xC0 | (code >> 6));
          out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
          out += static_cast<char>(0xE0 | (code >> 12));
          out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (code & 0x3F));
        }
        cursor += 6;
        continue;
      }
      default: out += c;
    }
    cursor += 2;
  }
  return out;
}

// The values `{{placeholder}}` substitutes in the landing and
// playground templates.
std::unordered_map<std::string, std::string> template_values_for(
    const Context& context,
    std::string_view language,
    std::string_view tree_path) {
  const std::string route =
      ssg::prefix_of(context.catalog, language) + std::string(tree_path);
  const std::string dir = route_dir(route);
  const std::string dist = ssg::relative_posix(dir, ".");
  const std::string& assets = dist;
  std::string root = ssg::prefix_of(context.catalog, language);
  if (!root.empty() && root.back() == '/') {
    root.pop_back();
  }
  std::string home = ssg::relative_posix(dir, root);
  if (home.empty()) {
    home = "./";
  } else if (!home.ends_with('/')) {
    home += '/';
  }
  const std::vector<std::string> others =
      ssg::other_languages(context.catalog, language);
  std::string switch_href;
  if (!others.empty()) {
    switch_href =
        ssg::relative_posix(dir, ssg::prefix_of(context.catalog, others[0]) +
                                     std::string(tree_path));
  }
  std::unordered_map<std::string, std::string> values;
  values["lang"] = std::string(language);
  values["dist"] = assets;
  values["home"] = std::move(home);
  values["switch"] = std::move(switch_href);
  values["boot"] = "<script>" + context.boot + "</script>";
  values["catalog"] = ssg::catalog_map(assets, language);
  const bool is_playground = tree_path == "playground/index.html";
  const std::string current =
      is_playground ? std::string("playground") : std::string();
  values["header"] =
      ssg::site_header(context.catalog, route, language, tree_path, current);
  values["pageLinks"] = ssg::page_links(context.catalog, language, tree_path);
  const std::string title =
      is_playground ? "alcy " + ssg::translate(context.catalog, language,
                                               "site.playground")
                    : std::string("alcy");
  const std::string description =
      is_playground
          ? ssg::translate(context.catalog, language, "meta.playground")
          : std::string(
                "An experimental statically-typed programming language.");
  values["social"] = ssg::social_meta(title, description, route);
  values["speculation"] = ssg::speculation_rules();
  const std::string stamp_text = ssg::escape_html(context.stamp.text);
  values["stamp"] = context.stamp.url.empty()
                        ? stamp_text
                        : "<a href=\"" + context.stamp.url +
                              "\" rel=\"noopener\">" + stamp_text + "</a>";
  values["settings"] = "";
  return values;
}
namespace {
void write_spec_assets(Context& context,
                       const fs::path& site_root,
                       const fs::path& dist_root) {
  base::Result<std::vector<ssg::SpecAsset>, std::string> assets =
      ssg::discover_spec_assets(site_root / fs::path(context.options.spec));
  if (assets.is_err()) {
    return;  // An empty spec tree was caught earlier at the discover step.
  }
  const std::vector<ssg::SpecAsset> asset_list = std::move(assets).unwrap();
  for (const std::string& language : context.catalog.languages) {
    const std::string prefix = ssg::prefix_of(context.catalog, language);
    for (const ssg::SpecAsset& asset : asset_list) {
      write_string_to_file(
          dist_root, prefix + asset.path,
          read_whole_file(site_root / fs::path(context.options.spec) /
                          fs::path(asset.source_rel)));
    }
  }
}
struct FenceResult {
  usize count = 0;
  std::vector<std::string> errors;
};

}  // namespace

FenceResult fence_validate_all(const std::vector<ssg::DocSource>& sources);
namespace {
void write_article_tree(Context& context,
                        const std::vector<ssg::DocSource>& sources,
                        const ssg::Tree& tree,
                        const fs::path& dist,
                        ssg::LinkAudit* audit,
                        std::vector<std::vector<ssg::DocPage>>* by_language);
}  // namespace

i32 run(i32 argc, char** argv) {
  base::Result<Options, std::string> parsed = parse_args(argc, argv);
  if (parsed.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(parsed).unwrap_err() << "\n";
    print_usage();
    return 64;
  }
  Context context;
  context.options = std::move(parsed).unwrap();
  context.stamp = {.text = context.options.stamp,
                   .url = context.options.stamp_url};
  base::Result<ssg::Catalog, std::string> loaded =
      ssg::load_catalog(context.options.catalog);
  if (loaded.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(loaded).unwrap_err() << "\n";
    return 1;
  }
  context.catalog = std::move(loaded).unwrap();
  context.boot = boot_text(context.options.site);
  if (context.boot.empty()) {
    std::cerr << "alcy-docsite: shared/boot.js cannot be inlined: it contains "
                 "</script>\n";
    return 1;
  }

  const fs::path docs_dir =
      fs::path(context.options.repo_root) / fs::path(context.options.docs);
  const fs::path spec_dir =
      fs::path(context.options.repo_root) / fs::path(context.options.spec);
  const fs::path tools_dir =
      fs::path(context.options.repo_root) / fs::path(context.options.tools);
  base::Result<std::vector<ssg::DocSource>, std::string> guide =
      ssg::discover_guide(docs_dir);
  if (guide.is_err() || guide.value().empty()) {
    std::string message;
    if (guide.is_err()) {
      message = std::move(guide).unwrap_err();
    } else {
      message = docs_dir.string() + " has no guide pages";
    }
    std::cerr << "alcy-docsite: " << message << "\n";
    return 1;
  }
  base::Result<std::vector<ssg::DocSource>, std::string> spec =
      ssg::discover_spec(spec_dir);
  if (spec.is_err() || spec.value().empty()) {
    std::string message;
    if (spec.is_err()) {
      message = std::move(spec).unwrap_err();
    } else {
      message = spec_dir.string() + " has no specification pages";
    }
    std::cerr << "alcy-docsite: " << message << "\n";
    return 1;
  }
  base::Result<std::vector<ssg::DocSource>, std::string> tools =
      ssg::discover_tools(tools_dir);
  if (tools.is_err() || tools.value().empty()) {
    std::string message;
    if (tools.is_err()) {
      message = std::move(tools).unwrap_err();
    } else {
      message = tools_dir.string() + " has no tools pages";
    }
    std::cerr << "alcy-docsite: " << message << "\n";
    return 1;
  }
  std::vector<ssg::DocSource> guide_pages = std::move(guide).unwrap();
  std::vector<ssg::DocSource> spec_pages = std::move(spec).unwrap();
  std::vector<ssg::DocSource> tools_pages = std::move(tools).unwrap();

  std::vector<ssg::DocSource> all_sources;
  all_sources.insert(all_sources.end(), guide_pages.begin(), guide_pages.end());
  all_sources.insert(all_sources.end(), spec_pages.begin(), spec_pages.end());
  all_sources.insert(all_sources.end(), tools_pages.begin(), tools_pages.end());
  const FenceResult fenced = fence_validate_all(all_sources);

  const std::string highlights_path =
      (fs::path(context.options.repo_root) / "treesitter" / "queries" / "alcy" /
       "highlights.scm")
          .string();
  auto highlighter_result = ssg::create_highlighter(highlights_path);
  if (highlighter_result.is_err()) {
    std::cout << "guide: no highlight; code fences are shown as plain text\n";
    // Keep running: the plain-text path is still a build.
  } else {
    context.highlight = std::move(highlighter_result).unwrap();
  }

  const fs::path dist(context.options.dist);
  std::vector<ssg::DocPage> all_pages;
  for (const std::string& language : context.catalog.languages) {
    auto add = [&](const std::vector<ssg::DocSource>& sources) {
      std::vector<ssg::DocPage> pages =
          ssg::pages_for_language(context.catalog, sources, language);
      all_pages.insert(all_pages.end(), pages.begin(), pages.end());
    };
    add(guide_pages);
    add(spec_pages);
    add(tools_pages);
  }

  for (const ssg::DocPage& page : all_pages) {
    context.planned.insert(page.route);
  }
  // Landings and tree indexes exist before the readers reach them.
  for (const std::string& language : context.catalog.languages) {
    const std::string prefix = ssg::prefix_of(context.catalog, language);
    context.planned.insert(prefix + "index.html");
    context.planned.insert(prefix + "playground/index.html");
    context.planned.insert(prefix + "guide/index.html");
    context.planned.insert(prefix + "reference/index.html");
    context.planned.insert(prefix + "tools/index.html");
  }

  std::vector<std::string> fence_errors = fenced.errors;
  ssg::LinkAudit audit(
      {.pages = &all_pages,
       .site_exists =
           [&](std::string_view route) {
             return context.planned.count(std::string(route)) > 0 ||
                    site_exists_relative(dist, route);
           },
       .repo_exists =
           [&](std::string_view path) {
             std::error_code error;
             return fs::is_regular_file(fs::path(context.options.repo_root) /
                                            fs::path(std::string(path)),
                                        error) &&
                    !error;
           }});

  // Landing and playground stamped from their templates.
  const fs::path site_dir(context.options.site);
  const std::string landing = read_whole_file(site_dir / "index.html");
  const std::string playground =
      read_whole_file(site_dir / "playground" / "index.html");
  const std::vector<std::pair<std::string_view, std::string_view>> templates = {
      {"index.html", landing},
      {"playground/index.html", playground},
  };
  for (const std::string& language : context.catalog.languages) {
    const std::string prefix = ssg::prefix_of(context.catalog, language);
    for (const auto& [tree_path, source] : templates) {
      const std::unordered_map<std::string, std::string> values =
          template_values_for(context, language, tree_path);
      base::Result<std::string, std::string> stamped =
          ssg::stamp_template(source, context.catalog, language, values);
      if (stamped.is_err()) {
        std::cerr << "alcy-docsite: " << std::move(stamped).unwrap_err()
                  << "\n";
        return 1;
      }
      write_string_to_file(dist, prefix + std::string(tree_path),
                           std::move(stamped).unwrap());
    }
  }

  write_spec_assets(context, fs::path(context.options.repo_root), dist);

  // Articles in every tree, with the link audit watching every
  // resolve. The contents pages come out per tree-owned set.
  const std::vector<
      std::pair<const ssg::Tree*, const std::vector<ssg::DocSource>*>>
      trees = {
          {&ssg::GUIDE_TREE, &guide_pages},
          {&ssg::REFERENCE_TREE, &spec_pages},
          {&ssg::TOOLS_TREE, &tools_pages},
  };
  for (const auto& [tree, sources] : trees) {
    write_article_tree(context, *sources, *tree, dist, &audit, nullptr);
  }

  // The files every static site needs at the root, and the ones the
  // reference links to. The 404 carries one runnable block: the page
  // is missing, the compiler is not.
  const std::string og = read_whole_file(site_dir / "og.png");
  write_string_to_file(dist, "og.png", og);
  write_string_to_file(dist, "favicon.svg",
                       read_whole_file(site_dir / "favicon.svg"));
  write_string_to_file(
      dist, "robots.txt",
      "User-agent: *\nAllow: /\n\n"
      "Sitemap: https://alcy-project.github.io/alcy/sitemap.xml\n");
  std::string sitemap_sources = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  sitemap_sources +=
      "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n";
  std::vector<std::string> sitemap_routes;
  for (const std::string& language : context.catalog.languages) {
    const std::string prefix = ssg::prefix_of(context.catalog, language);
    sitemap_routes.push_back(prefix + "index.html");
    for (const std::vector<ssg::DocSource>* sources :
         {&guide_pages, &spec_pages, &tools_pages}) {
      for (const ssg::DocPage& page :
           ssg::pages_for_language(context.catalog, *sources, language)) {
        sitemap_routes.push_back(page.route);
      }
    }
    sitemap_routes.push_back(prefix + "guide/index.html");
    sitemap_routes.push_back(prefix + "playground/index.html");
  }
  for (const std::string& route : sitemap_routes) {
    sitemap_sources +=
        "  <url>\n    <loc>" + ssg::canonical_url(route) + "</loc>\n  </url>\n";
  }
  sitemap_sources += "</urlset>\n";
  write_string_to_file(dist, "sitemap.xml", sitemap_sources);

  // The search index covers the English trees once; the module that
  // reads it resolves a route into the reader's own tree.
  std::string search_json;
  search_json.reserve(static_cast<usize>(64) * 1024);
  search_json += "[\n";
  std::vector<ssg::DocSource> indexed;
  for (const std::vector<ssg::DocSource>* sources :
       {&guide_pages, &spec_pages, &tools_pages}) {
    indexed.insert(indexed.end(), sources->begin(), sources->end());
  }
  for (usize index = 0; index < indexed.size(); index++) {
    const ssg::DocSource& source = indexed[index];
    search_json += "  {\n    \"route\": \"" + source.path + "\",\n";
    search_json += "    \"title\": \"" + json_escape(source.title) + "\",\n";
    const std::vector<ssg::GuideSection> sections =
        ssg::sections_of(source.blocks);
    if (sections.empty()) {
      search_json += "    \"sections\": []\n  }";
    } else {
      search_json += "    \"sections\": [\n";
      for (usize s = 0; s < sections.size(); s++) {
        search_json += "      {\n        \"id\": \"" +
                       json_escape(sections[s].id) + "\",\n";
        search_json +=
            "        \"text\": \"" + json_escape(sections[s].text) + "\"\n";
        search_json += "      }";
        search_json += s + 1 == sections.size() ? "\n" : ",\n";
      }
      search_json += "    ]\n  }";
    }
    search_json += index + 1 == indexed.size() ? "\n" : ",\n";
  }
  search_json += "]\n";
  write_string_to_file(dist, "search-index.json", search_json);

  // The 404 page: GitHub Pages serves it under whatever path was
  // asked for, so its links are written from the deployment root.
  {
    const std::function<std::string(std::string_view)> renderer =
        [&](std::string_view code) {
          return context.highlight.highlight_code(code);
        };
    write_string_to_file(
        dist, "404.html",
        ssg::render_not_found(context.boot, context.stamp, renderer));
  }

  const std::vector<std::string>& checker_errors = audit.errors();
  if (!fence_errors.empty() || !checker_errors.empty()) {
    for (const std::string& error : fence_errors) {
      std::cerr << "alcy-docsite: " << error << "\n";
    }
    for (const std::string& error : checker_errors) {
      std::cerr << "alcy-docsite: " << error << "\n";
    }
    return 1;
  }
  std::cout << "alcy-docsite: " << guide_pages.size() << " guide, "
            << spec_pages.size() << " reference, " << tools_pages.size()
            << " tools pages x " << context.catalog.languages.size()
            << " languages, " << fenced.count << " alcy fences compiled\n";
  return 0;
}
namespace {

struct AuditContext {
  ssg::LinkAudit* audit = nullptr;
  const ssg::DocPage* page = nullptr;
};

std::string audit_link(std::string_view url, void* ctx) {
  const AuditContext* context = static_cast<const AuditContext*>(ctx);
  return context->audit->resolve(url, *context->page);
}

}  // namespace

FenceResult fence_validate_all(const std::vector<ssg::DocSource>& sources) {
  FenceResult result;
  for (const ssg::DocSource& source : sources) {
    for (const ssg::Block& block : source.blocks) {
      if (block.type != ssg::BlockType::Code || block.language != "alcy") {
        continue;
      }
      result.count++;
      AlcyResult compiled{};
      alcy_compile(reinterpret_cast<const u8*>(block.text.data()),
                   block.text.size(), &compiled);
      if (compiled.ok == 1) {
        alcy_release(&compiled);
        continue;
      }
      const std::string diagnostics(
          reinterpret_cast<const char*>(compiled.diagnostics),
          compiled.diagnostics_len);
      result.errors.push_back(source.source_rel + ":" +
                              std::to_string(block.line) + ": " +
                              first_error_message(diagnostics));
      alcy_release(&compiled);
    }
  }
  return result;
}

namespace {

// One pass writes one tree of pages: its contents indexes and its
// articles, per language. The contents pages are written first, so
// a build that stops midway has not published half a tree.
void write_article_tree(Context& context,
                        const std::vector<ssg::DocSource>& sources,
                        const ssg::Tree& tree,
                        const fs::path& dist,
                        ssg::LinkAudit* audit,
                        std::vector<std::vector<ssg::DocPage>>* by_language) {
  for (usize language_index = 0;
       language_index < context.catalog.languages.size(); language_index++) {
    const std::string& language = context.catalog.languages[language_index];
    std::vector<ssg::DocPage> pages =
        ssg::pages_for_language(context.catalog, sources, language);
    if (by_language != nullptr) {
      while (by_language->size() <= language_index) {
        by_language->emplace_back();
      }
      (*by_language)[language_index] = pages;
    }
    const std::string prefix = ssg::prefix_of(context.catalog, language);
    write_string_to_file(
        dist, prefix + std::string(tree.segment) + "/index.html",
        ssg::render_tree_index(pages, context.catalog, language, context.boot,
                               context.stamp, tree));
    for (const ssg::DocPage& page : pages) {
      AuditContext audit_ctx{.audit = audit, .page = &page};
      ssg::PageOptions page_options;
      page_options.catalog = &context.catalog;
      page_options.pages = &pages;
      page_options.tree = &tree;
      page_options.language = page.language;
      page_options.boot = context.boot;
      page_options.stamp = context.stamp;
      page_options.highlight = [&context](std::string_view code) {
        return context.highlight.highlight_code(code);
      };
      page_options.resolve_link = &audit_link;
      page_options.resolve_context = &audit_ctx;
      write_string_to_file(dist, page.route,
                           ssg::render_guide_page(page, page_options));
    }
  }
}

}  // namespace
}  // namespace

i32 main(i32 argc, char** argv) {
  return run(argc, argv);
}
