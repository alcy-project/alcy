// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/discover.h"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fpag/io/io_util.h"
#include "ssg/documents.h"
#include "ssg/markdown.h"
#include "ssg/paths.h"

namespace ssg {
namespace {

std::string read_whole_file(std::string_view path) {
  return io::read_file(std::string(path));
}

base::Result<Blocks, std::string> parse_source(std::string_view path,
                                               std::string_view source_rel) {
  base::Result<Blocks, std::string> blocks =
      parse_markdown(read_whole_file(path));
  if (blocks.is_err()) {
    return base::make_err(std::string(source_rel) + ": " +
                          std::move(blocks).unwrap_err());
  }
  return base::make_ok(std::move(blocks).unwrap());
}

std::string strip_md(std::string_view name) {
  return std::string(
      name.substr(0, name.size() - std::string_view(".md").size()));
}

base::Result<DocSource, std::string> make_doc_source(
    std::string_view dir,
    std::string_view route_prefix,
    std::string_view segment,
    std::string_view name,
    usize order) {
  base::Result<Blocks, std::string> parsed =
      parse_source(join_posix({dir, name}),
                   std::string(route_prefix) + "/" + std::string(name));
  if (parsed.is_err()) {
    return base::make_err(std::move(parsed).unwrap_err());
  }
  Blocks blocks = std::move(parsed).unwrap();
  base::Result<std::string, std::string> title =
      title_of(blocks, std::string(route_prefix) + std::string(name));
  if (title.is_err()) {
    return base::make_err(std::move(title).unwrap_err());
  }
  DocSource source;
  source.source_rel = std::string(name);
  source.path = join_posix({segment, strip_md(name) + ".html"});
  source.order = order;
  source.title = std::move(title).unwrap();
  source.blocks = std::move(blocks);
  return base::make_ok(std::move(source));
}

// The guide's tree is walked in name order, so the discovery is
// deterministic regardless of the OS's directory order: every file
// becomes a relative path under `dir`, and each level's names are
// sorted before the walk descends.
base::Result<void, std::string> walk_markdown_files(
    std::string_view dir,
    std::string_view prefix,
    std::vector<std::string>* files) {
  std::vector<std::string> subdirs;
  auto listed = list_directory(dir, &subdirs);
  if (listed.is_err()) {
    return base::make_err(std::move(listed).unwrap_err());
  }
  for (std::string& name : std::move(listed).unwrap()) {
    if (name.ends_with(".md")) {
      files->push_back(prefix.empty() ? std::move(name)
                                      : join_posix({prefix, name}));
    }
  }
  for (std::string& subdir : subdirs) {
    const std::string next =
        prefix.empty() ? subdir : join_posix({prefix, subdir});
    base::Result<void, std::string> walked =
        walk_markdown_files(join_posix({dir, subdir}), next, files);
    if (walked.is_err()) {
      return walked;
    }
  }
  return base::make_ok();
}

}  // namespace

// The prose of the reference and the tools tree names sibling pages as
// backticked file.md; the ones with a page become links, the rest stay
// text. A page that is not published is mentioned without a link.
Blocks link_sibling_references(Blocks blocks,
                               const std::set<std::string>& published) {
  const auto rewrite = [&published](std::string& text) {
    std::string out;
    out.reserve(text.size());
    usize index = 0;
    while (index < text.size()) {
      if (text[index] != '`') {
        out += text[index];
        index++;
        continue;
      }
      const usize close = text.find('`', index + 1);
      if (close == std::string::npos) {
        out += text[index];
        index++;
        continue;
      }
      const std::string name(text.substr(index + 1, close - index - 1));
      const bool is_file = name.ends_with(".md");
      const bool is_published =
          is_file && published.find(name) != published.end();
      if (is_published) {
        out += "[`" + name + "`](" + strip_md(name) + ".html)";
      } else {
        out += '`' + name + '`';
      }
      index = close + 1;
    }
    text = std::move(out);
  };
  for (Block& block : blocks) {
    if (block.type == BlockType::Code) {
      continue;
    }
    if (block.type == BlockType::List) {
      for (std::string& item : block.items) {
        rewrite(item);
      }
      continue;
    }
    rewrite(block.text);
  }
  return blocks;
}

base::Result<std::vector<DocSource>, std::string> discover_guide(
    std::string_view docs_dir) {
  std::vector<std::string> files;
  base::Result<void, std::string> walked =
      walk_markdown_files(docs_dir, "", &files);
  if (walked.is_err()) {
    return base::make_err(std::move(walked).unwrap_err());
  }
  std::sort(files.begin(), files.end());
  std::vector<DocSource> pages;
  std::unordered_map<usize, std::string> orders;
  for (const std::string& source_rel : files) {
    const std::string file_name = basename_posix(source_rel);
    const std::string stem =
        file_name.substr(0, file_name.size() - std::string_view(".md").size());
    const usize dash = stem.find('-');
    usize number_len = dash == std::string::npos ? stem.size() : dash;
    for (usize i = 0; i < number_len; i++) {
      const char c = stem[i];
      if (c < '0' || c > '9') {
        number_len = 0;
        break;
      }
    }
    if (number_len == 0) {
      return base::make_err(source_rel + ": guide files are named NN-slug.md");
    }
    const usize order = std::stoul(stem.substr(0, number_len));
    const auto previous = orders.find(order);
    if (previous != orders.end()) {
      return base::make_err(source_rel + ": the number " +
                            std::to_string(order) + " is already used by " +
                            previous->second);
    }
    orders[order] = source_rel;
    base::Result<Blocks, std::string> parsed =
        parse_source(join_posix({docs_dir, source_rel}), source_rel);
    if (parsed.is_err()) {
      return base::make_err(std::move(parsed).unwrap_err());
    }
    Blocks blocks = std::move(parsed).unwrap();
    base::Result<std::string, std::string> title = title_of(blocks, source_rel);
    if (title.is_err()) {
      return base::make_err(std::move(title).unwrap_err());
    }
    const std::string dir = dirname_posix(source_rel);
    const std::string file = std::string(stem.substr(dash + 1)) + ".html";
    DocSource page;
    page.source_rel = source_rel;
    page.path = dir == "." ? join_posix({"guide", file})
                           : join_posix({"guide", dir, file});
    page.order = order;
    page.title = std::move(title).unwrap();
    page.blocks = std::move(blocks);
    pages.push_back(std::move(page));
  }
  std::sort(
      pages.begin(), pages.end(), [](const DocSource& a, const DocSource& b) {
        return a.order < b.order || (a.order == b.order && a.path < b.path);
      });
  return base::make_ok(std::move(pages));
}

namespace {

base::Result<std::vector<DocSource>, std::string> discover_ordered(
    std::string_view dir,
    const std::vector<std::string>& order,
    std::string_view segment,
    std::string_view route_prefix) {
  base::Result<std::vector<std::string>, std::string> names =
      markdown_files(dir);
  if (names.is_err()) {
    return base::make_err(std::move(names).unwrap_err());
  }
  const std::vector<std::string> names_list = std::move(names).unwrap();
  const std::set<std::string> published(names_list.begin(), names_list.end());
  std::vector<DocSource> pages;
  pages.reserve(published.size());
  for (const std::string& name : published) {
    const std::string full_name = std::string(route_prefix) + "/" + name;
    base::Result<DocSource, std::string> source = make_doc_source(
        dir, route_prefix, segment, name,
        static_cast<usize>(
            std::find(order.begin(), order.end(), name) == order.end()
                ? order.size()
                : static_cast<usize>(
                      std::find(order.begin(), order.end(), name) -
                      order.begin())));
    if (source.is_err()) {
      return base::make_err(std::move(source).unwrap_err());
    }
    DocSource page = std::move(source).unwrap();
    page.blocks = link_sibling_references(std::move(page.blocks), published);
    pages.push_back(std::move(page));
  }
  std::sort(
      pages.begin(), pages.end(), [](const DocSource& a, const DocSource& b) {
        return a.order < b.order || (a.order == b.order && a.path < b.path);
      });
  return base::make_ok(std::move(pages));
}

}  // namespace

base::Result<std::vector<DocSource>, std::string> discover_spec(
    std::string_view spec_dir) {
  static const std::vector<std::string> SPEC_ORDER = {
      "overview.md",  "keywords.md",  "values.md",   "types.md",
      "grammar.md",   "items.md",     "modules.md",  "control.md",
      "ownership.md", "summaries.md", "ffi.md",      "fmt.md",
      "comp.md",      "errors.md",    "deferred.md",
  };
  return discover_ordered(spec_dir, SPEC_ORDER, "reference", "spec");
}

base::Result<std::vector<SpecAsset>, std::string> discover_spec_assets(
    std::string_view spec_dir) {
  std::vector<SpecAsset> assets;
  auto listed = list_directory(spec_dir, nullptr);
  if (listed.is_err()) {
    return base::make_err(std::move(listed).unwrap_err());
  }
  for (const std::string& name : std::move(listed).unwrap()) {
    const usize dot = name.rfind('.');
    const std::string extension =
        dot == std::string::npos ? std::string() : name.substr(dot + 1);
    if (extension != "ebnf" && extension != "txt") {
      continue;
    }
    assets.push_back(
        {.source_rel = name, .path = join_posix({"reference", name})});
  }
  std::sort(
      assets.begin(), assets.end(),
      [](const SpecAsset& a, const SpecAsset& b) { return a.path < b.path; });
  return base::make_ok(std::move(assets));
}

base::Result<std::vector<DocSource>, std::string> discover_tools(
    std::string_view tools_dir) {
  static const std::vector<std::string> TOOLS_ORDER = {
      "index.md", "build.md", "compile.md", "check.md", "test.md", "run.md",
      "fmt.md",   "lint.md",  "lsp.md",     "new.md",   "init.md",
  };
  return discover_ordered(tools_dir, TOOLS_ORDER, "tools", "tools");
}
}  // namespace ssg
