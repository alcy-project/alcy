// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The service worker caches only what it names, and a name it does
// not list is fetched straight from the network - which means it is
// absent when the network is. A module the guide loads but the worker
// does not name therefore works online and fails offline, silently.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ios>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "ssg/markdown.h"
#include "ssg/render.h"
#include "tests/ssg/cases.h"
using ssg::Block;

namespace {

std::string site_shared() {
  const std::filesystem::path shared = ssg_test::site_shared_path();
  return shared.empty() ? std::string() : shared.string();
}

std::string read(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream bytes;
  bytes << input.rdbuf();
  return bytes.str();
}

std::vector<std::string> quoted_strings(const std::string& body) {
  std::vector<std::string> names;
  const std::regex quoted(R"re("([^"]+)")re");
  for (std::sregex_iterator it(body.begin(), body.end(), quoted), end;
       it != end; ++it) {
    names.push_back((*it)[1].str());
  }
  return names;
}

std::vector<std::string> shell_names(const std::string& source) {
  const usize begin = source.find("const SHELL_NAMES = [");
  const usize end = source.find("];", begin);
  if (begin == std::string::npos || end == std::string::npos) {
    return {};
  }
  return quoted_strings(source.substr(begin, end - begin));
}

// The modules a page asks for at the end of its body.
std::vector<std::string> loaded_modules(const std::string& html) {
  std::vector<std::string> modules;
  const std::regex src_re(R"re(src="([^"]*shared/([A-Za-z0-9.-]+\.js))")re");
  for (std::sregex_iterator it(html.begin(), html.end(), src_re), end;
       it != end; ++it) {
    modules.push_back((*it)[2].str());
  }
  return modules;
}

// The shared modules a page's own modules import, followed to a fixed
// point: whatever the browser must fetch before the page can run. A
// module imported only inside a function body is deferred by the
// caller, not fetched with the page, so the walk follows top-level
// imports only.
std::vector<std::string> imported_modules(const std::string& entry) {
  std::vector<std::string> seen;
  std::vector<std::string> queue = {entry};
  const std::vector<std::string> catalogs = {"i18n.en.js", "i18n.ja.js"};
  const std::regex import_re(
      R"re(import[^;]*from "\./([A-Za-z0-9_-]+)\.js")re");
  while (!queue.empty()) {
    const std::string name = queue.back();
    queue.pop_back();
    if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
      continue;
    }
    seen.push_back(name);
    const std::string base = name.substr(0, name.rfind(".js"));
    const std::string source = read(site_shared() + "/" + base + ".ts");
    if (source.find("alcy:i18n") != std::string::npos) {
      for (const std::string& catalog : catalogs) {
        if (std::find(seen.begin(), seen.end(), catalog) == seen.end()) {
          seen.push_back(catalog);
        }
      }
    }
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line)) {
      std::smatch match;
      if (std::regex_search(line, match, import_re)) {
        queue.push_back(match[1].str() + ".js");
      }
    }
  }
  return seen;
}

}  // namespace

TEST_CASE("every module a guide page runs is one the worker caches") {
  const std::string shell = read(site_shared() + "/sw.ts");
  const ssg::Block alcy = ssg_test::code_block("alcy", "fn main() {}", 1);
  const ssg::Block sh = ssg_test::code_block("sh", "alcy run main.al", 3);
  const std::string html = ssg_test::render_article(
      {ssg_test::test_page("Title", "guide/x.html", "en")}, ssg::GUIDE_TREE,
      "en", {alcy, sh});
  for (const std::string& entry : loaded_modules(html)) {
    for (const std::string& name : imported_modules(entry)) {
      CHECK(shell.find(name) != std::string::npos);
    }
  }
  // A page reaches its catalog through the page's import map rather
  // than by path, so the walk has to be told where to look; without
  // that it would stop at the shared modules and check nothing about
  // the strings.
  const std::vector<std::string> shell_modules = imported_modules("shell.js");
  for (const std::string& catalog : {"i18n.en.js", "i18n.ja.js"}) {
    CHECK(std::find(shell_modules.begin(), shell_modules.end(), catalog) !=
          shell_modules.end());
  }
}

TEST_CASE("the worker lists no module that does not exist") {
  for (const std::string& name : shell_names(read(site_shared() + "/sw.ts"))) {
    if (name == "i18n.en.js" || name == "i18n.ja.js") {
      // Stamped from the catalog by the build; the authored module
      // does not exist under that name.
      continue;
    }
    if (name.ends_with(".css")) {
      CHECK(std::ifstream(site_shared() + "/" + name, std::ios::binary).good());
      continue;
    }
    const std::string source = name.substr(0, name.rfind(".js")) + ".ts";
    CHECK(std::ifstream(site_shared() + "/" + source, std::ios::binary).good());
  }
}
