// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The command that stamps the site: one language tree of pages per
// language, from the sources under `docs/`. The CLI runs it once per
// build; nothing here persists state. Every flag is a path or a build
// stamp value; there is no configuration file.

#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "fpag/arg/parser.h"
#include "fpag/io/io_util.h"
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

namespace {

// The one command the binary runs: every flag is a path or a build
// stamp value, so the parse has no subcommands and no choices.
arg::Command build_command() {
  arg::CommandBuilder builder("alcy-docsite");
  builder.about("Stamp the site's page trees from the sources under docs/.");
  const auto path_arg = [](const char* name, const char* help) {
    return arg::ArgBuilder(name).help(help).value_name("PATH").build();
  };
  builder.add_arg(arg::ArgBuilder("docs")
                      .help("the guide's source directory, under --repo-root")
                      .value_name("DIR")
                      .required()
                      .build());
  builder.add_arg(arg::ArgBuilder("dist")
                      .help("the directory the site is written to")
                      .value_name("DIR")
                      .required()
                      .build());
  builder.add_arg(arg::ArgBuilder("repo-root")
                      .help("the repository root the sources live under")
                      .value_name("DIR")
                      .required()
                      .build());
  builder.add_arg(path_arg("site", "the site's templates and shared assets"));
  builder.add_arg(path_arg(
      "spec", "the specification's source directory, under --repo-root"));
  builder.add_arg(path_arg(
      "tools", "the tools pages' source directory, under --repo-root"));
  builder.add_arg(arg::ArgBuilder("stamp")
                      .help("the build stamp the footer names")
                      .value_name("STAMP")
                      .default_value("unknown")
                      .build());
  builder.add_arg(
      arg::ArgBuilder("stamp-url")
          .help("the address the stamp links to; empty links nowhere")
          .value_name("URL")
          .default_value("")
          .build());
  builder.add_arg(
      arg::ArgBuilder("catalog")
          .help("the catalog; the default is --site/shared/catalog.json")
          .value_name("FILE")
          .default_value("")
          .build());
  return std::move(builder).build();
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

base::Result<std::optional<Options>, std::string> parse_args(i32 argc,
                                                             char** argv) {
  arg::Parser parser(build_command());
  arg::Matches matches;
  const arg::ParseStatus status = parser.parse(argc, argv, &matches);
  if (status == arg::ParseStatus::HelpRequested) {
    std::cout << parser.help_message() << "\n";
    return base::make_ok(std::optional<Options>());
  }
  if (status == arg::ParseStatus::Error) {
    return base::make_err(parser.error_message());
  }
  const auto get = [&matches](const char* name) {
    return std::string(matches.get<std::string_view>(name).unwrap_or(""));
  };
  Options options;
  options.docs = get("docs");
  options.dist = get("dist");
  options.repo_root = get("repo-root");
  options.site = get("site");
  options.spec = get("spec");
  options.tools = get("tools");
  options.stamp = get("stamp");
  options.stamp_url = get("stamp-url");
  options.catalog = get("catalog");
  if (options.catalog.empty()) {
    options.catalog = ssg::join_posix({options.site, "shared", "catalog.json"});
  }
  return base::make_ok(std::optional<Options>(std::move(options)));
}

// io::create_directory is one level deep, and a route sits a few
// levels under the dist root, so the parents are made one by one. A
// level that exists already is fine; a level that cannot be made is
// not, and the caller hears about it.
bool create_directories(const std::string& path) {
  usize index = 0;
  while (index < path.size()) {
    const usize slash = path.find('/', index);
    if (slash == std::string::npos) {
      break;
    }
    // An absolute path's first component is empty; the root exists.
    if (slash > index && !io::create_directory(path.substr(0, slash))) {
      return false;
    }
    index = slash + 1;
  }
  return io::create_directory(path);
}

// Reads a file the build depends on: a template, an image, a script.
// An empty read is a missing or unreadable file, and both stop the
// build - half a site published as a success is the one failure the
// reader cannot see past.
base::Result<std::string, std::string> read_input_file(std::string_view path) {
  if (!io::is_file(std::string(path))) {
    return base::make_err("cannot read " + std::string(path) +
                          ": it does not exist");
  }
  std::string bytes = io::read_file(std::string(path));
  if (bytes.empty()) {
    return base::make_err("cannot read " + std::string(path) +
                          ": the file is empty");
  }
  return base::make_ok(std::move(bytes));
}

base::Result<void, std::string> write_string_to_file(std::string_view root,
                                                     std::string_view route,
                                                     std::string_view content) {
  const std::string target = ssg::join_posix({root, route});
  if (!create_directories(ssg::dirname_posix(target))) {
    return base::make_err("cannot create the directory for " + target);
  }
  const std::string bytes(content);
  const std::span<const u8> view(reinterpret_cast<const u8*>(bytes.data()),
                                 bytes.size());
  if (!io::write_file(view, target)) {
    return base::make_err("cannot write " + target);
  }
  return base::make_ok();
}

bool site_exists_relative(std::string_view root, std::string_view candidate) {
  if (candidate.empty()) {
    return true;
  }
  const std::string target = ssg::join_posix({root, candidate});
  if (candidate.ends_with('/')) {
    return io::is_dir(target);
  }
  return io::is_file(target);
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

base::Result<std::string, std::string> boot_text(std::string_view site) {
  base::Result<std::string, std::string> boot =
      read_input_file(ssg::join_posix({site, "shared", "boot.js"}));
  if (boot.is_err()) {
    return boot;
  }
  if (boot.value().find("</script") != std::string::npos) {
    return base::make_err(
        std::string("shared/boot.js cannot be inlined: it contains </script>"));
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
                        : "<a href=\"" +
                              ssg::escape_attribute(context.stamp.url) +
                              "\" rel=\"noopener\">" + stamp_text + "</a>";
  values["settings"] = "";
  return values;
}
namespace {
base::Result<void, std::string> write_spec_assets(Context& context,
                                                  std::string_view site_root,
                                                  std::string_view dist_root) {
  base::Result<std::vector<ssg::SpecAsset>, std::string> assets =
      ssg::discover_spec_assets(
          ssg::join_posix({site_root, context.options.spec}));
  if (assets.is_err()) {
    return base::make_ok();  // An empty spec tree was caught at discover.
  }
  const std::vector<ssg::SpecAsset> asset_list = std::move(assets).unwrap();
  for (const std::string& language : context.catalog.languages) {
    const std::string prefix = ssg::prefix_of(context.catalog, language);
    for (const ssg::SpecAsset& asset : asset_list) {
      base::Result<std::string, std::string> bytes = read_input_file(
          ssg::join_posix({site_root, context.options.spec, asset.source_rel}));
      if (bytes.is_err()) {
        return base::make_err(std::move(bytes).unwrap_err());
      }
      base::Result<void, std::string> written = write_string_to_file(
          dist_root, prefix + asset.path, std::move(bytes).unwrap());
      if (written.is_err()) {
        return written;
      }
    }
  }
  return base::make_ok();
}
struct FenceResult {
  usize count = 0;
  std::vector<std::string> errors;
};

}  // namespace

FenceResult fence_validate_all(const std::vector<ssg::DocSource>& sources);
namespace {
base::Result<void, std::string> write_article_tree(
    Context& context,
    const std::vector<ssg::DocSource>& sources,
    const ssg::Tree& tree,
    std::string_view dist,
    ssg::LinkAudit* audit);
}  // namespace

i32 run(i32 argc, char** argv) {
  base::Result<std::optional<Options>, std::string> parsed =
      parse_args(argc, argv);
  if (parsed.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(parsed).unwrap_err() << "\n";
    return 64;
  }
  std::optional<Options> parsed_options = std::move(parsed).value();
  if (!parsed_options.has_value()) {
    return 0;  // --help: the parse printed the usage on standard output.
  }
  Context context;
  context.options = std::move(*parsed_options);
  context.stamp = {.text = context.options.stamp,
                   .url = context.options.stamp_url};
  base::Result<ssg::Catalog, std::string> loaded =
      ssg::load_catalog(context.options.catalog);
  if (loaded.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(loaded).unwrap_err() << "\n";
    return 1;
  }
  context.catalog = std::move(loaded).unwrap();
  base::Result<std::string, std::string> boot = boot_text(context.options.site);
  if (boot.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(boot).unwrap_err() << "\n";
    return 1;
  }
  context.boot = std::move(boot).unwrap();

  const std::string docs_dir =
      ssg::join_posix({context.options.repo_root, context.options.docs});
  const std::string spec_dir =
      ssg::join_posix({context.options.repo_root, context.options.spec});
  const std::string tools_dir =
      ssg::join_posix({context.options.repo_root, context.options.tools});
  base::Result<std::vector<ssg::DocSource>, std::string> guide =
      ssg::discover_guide(docs_dir);
  if (guide.is_err() || guide.value().empty()) {
    std::string message;
    if (guide.is_err()) {
      message = std::move(guide).unwrap_err();
    } else {
      message = docs_dir + " has no guide pages";
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
      message = spec_dir + " has no specification pages";
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
      message = tools_dir + " has no tools pages";
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
      ssg::join_posix({context.options.repo_root, "treesitter", "queries",
                       "alcy", "highlights.scm"});
  auto highlighter_result = ssg::create_highlighter(highlights_path);
  if (highlighter_result.is_err()) {
    std::cout << "guide: no highlight; code fences are shown as plain text\n";
    // Keep running: the plain-text path is still a build.
  } else {
    context.highlight = std::move(highlighter_result).unwrap();
  }

  const std::string& dist = context.options.dist;
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
  ssg::LinkAudit audit({.pages = &all_pages,
                        .site_exists =
                            [&](std::string_view route) {
                              return context.planned.count(std::string(route)) >
                                         0 ||
                                     site_exists_relative(dist, route);
                            },
                        .repo_exists =
                            [&](std::string_view path) {
                              return io::is_file(ssg::join_posix(
                                  {context.options.repo_root, path}));
                            }});

  // Landing and playground stamped from their templates.
  const std::string& site_dir = context.options.site;
  base::Result<std::string, std::string> landing =
      read_input_file(ssg::join_posix({site_dir, "index.html"}));
  if (landing.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(landing).unwrap_err() << "\n";
    return 1;
  }
  base::Result<std::string, std::string> playground =
      read_input_file(ssg::join_posix({site_dir, "playground", "index.html"}));
  if (playground.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(playground).unwrap_err() << "\n";
    return 1;
  }
  const std::vector<std::pair<std::string_view, std::string_view>> templates = {
      {"index.html", landing.value()},
      {"playground/index.html", playground.value()},
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
      base::Result<void, std::string> written = write_string_to_file(
          dist, prefix + std::string(tree_path), std::move(stamped).unwrap());
      if (written.is_err()) {
        std::cerr << "alcy-docsite: " << std::move(written).unwrap_err()
                  << "\n";
        return 1;
      }
    }
  }

  base::Result<void, std::string> assets =
      write_spec_assets(context, context.options.repo_root, dist);
  if (assets.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(assets).unwrap_err() << "\n";
    return 1;
  }

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
    base::Result<void, std::string> written =
        write_article_tree(context, *sources, *tree, dist, &audit);
    if (written.is_err()) {
      std::cerr << "alcy-docsite: " << std::move(written).unwrap_err() << "\n";
      return 1;
    }
  }

  // The files every static site needs at the root, and the ones the
  // reference links to. The 404 carries one runnable block: the page
  // is missing, the compiler is not.
  base::Result<std::string, std::string> og =
      read_input_file(ssg::join_posix({site_dir, "og.png"}));
  if (og.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(og).unwrap_err() << "\n";
    return 1;
  }
  base::Result<std::string, std::string> favicon =
      read_input_file(ssg::join_posix({site_dir, "favicon.svg"}));
  if (favicon.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(favicon).unwrap_err() << "\n";
    return 1;
  }
  base::Result<void, std::string> copied =
      write_string_to_file(dist, "og.png", og.value());
  if (copied.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(copied).unwrap_err() << "\n";
    return 1;
  }
  copied = write_string_to_file(dist, "favicon.svg", favicon.value());
  if (copied.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(copied).unwrap_err() << "\n";
    return 1;
  }
  base::Result<void, std::string> written = write_string_to_file(
      dist, "robots.txt",
      "User-agent: *\nAllow: /\n\n"
      "Sitemap: https://alcy-project.github.io/alcy/sitemap.xml\n");
  if (written.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(written).unwrap_err() << "\n";
    return 1;
  }
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
  written = write_string_to_file(dist, "sitemap.xml", sitemap_sources);
  if (written.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(written).unwrap_err() << "\n";
    return 1;
  }

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
    search_json +=
        "  {\n    \"route\": \"" + json_escape(source.path) + "\",\n";
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
  written = write_string_to_file(dist, "search-index.json", search_json);
  if (written.is_err()) {
    std::cerr << "alcy-docsite: " << std::move(written).unwrap_err() << "\n";
    return 1;
  }

  // The 404 page: GitHub Pages serves it under whatever path was
  // asked for, so its links are written from the deployment root.
  {
    const std::function<std::string(std::string_view)> renderer =
        [&](std::string_view code) {
          return context.highlight.highlight_code(code);
        };
    base::Result<void, std::string> written = write_string_to_file(
        dist, "404.html",
        ssg::render_not_found(context.boot, context.stamp, renderer));
    if (written.is_err()) {
      std::cerr << "alcy-docsite: " << std::move(written).unwrap_err() << "\n";
      return 1;
    }
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
base::Result<void, std::string> write_article_tree(
    Context& context,
    const std::vector<ssg::DocSource>& sources,
    const ssg::Tree& tree,
    std::string_view dist,
    ssg::LinkAudit* audit) {
  for (const std::string& language : context.catalog.languages) {
    std::vector<ssg::DocPage> pages =
        ssg::pages_for_language(context.catalog, sources, language);
    const std::string prefix = ssg::prefix_of(context.catalog, language);
    base::Result<void, std::string> written = write_string_to_file(
        dist, prefix + std::string(tree.segment) + "/index.html",
        ssg::render_tree_index(pages, context.catalog, language, context.boot,
                               context.stamp, tree));
    if (written.is_err()) {
      return written;
    }
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
      written = write_string_to_file(
          dist, page.route, ssg::render_guide_page(page, page_options));
      if (written.is_err()) {
        return written;
      }
    }
  }
  return base::make_ok();
}

}  // namespace
}  // namespace

i32 main(i32 argc, char** argv) {
  return run(argc, argv);
}
