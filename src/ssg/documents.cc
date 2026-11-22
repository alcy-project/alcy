// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/documents.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/build_config.h"
#include "fpag/base/result.h"
#include "ssg/catalog.h"
#include "ssg/markdown.h"

#if BUILD_FLAG(IS_OS_WIN)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace ssg {
namespace {

// The one directory listing the module needs: a name plus the kind,
// read straight from the OS. A shared temporary directory is not a
// failure mode here - the discovery reads the repository's own tree.
base::Result<std::vector<std::pair<std::string, bool>>, std::string>
list_entries(std::string_view dir) {
  std::vector<std::pair<std::string, bool>> entries;  // name, is_dir
  const std::string path(dir);
#if BUILD_FLAG(IS_OS_WIN)
  WIN32_FIND_DATAA data;
  const HANDLE handle = FindFirstFileA((path + "\\\\*").c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) {
    return base::make_err("cannot list the directory: " + path);
  }
  do {
    const std::string name(data.cFileName);
    if (name == "." || name == "..") {
      continue;
    }
    entries.emplace_back(
        name, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
  } while (FindNextFileA(handle, &data));
  FindClose(handle);
#else
  DIR* const handle = opendir(path.c_str());
  if (handle == nullptr) {
    return base::make_err("cannot list the directory: " + path);
  }
  while (const dirent* const entry = readdir(handle)) {
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    bool is_dir = entry->d_type == DT_DIR;
    if (entry->d_type == DT_UNKNOWN) {
      std::string full = path;
      full += '/';
      full += name;
      struct stat status;
      if (stat(full.c_str(), &status) != 0) {
        continue;
      }
      is_dir = S_ISDIR(status.st_mode);
    }
    entries.emplace_back(name, is_dir);
  }
  closedir(handle);
#endif
  return base::make_ok(std::move(entries));
}

}  // namespace

base::Result<std::vector<std::string>, std::string> list_directory(
    std::string_view dir,
    std::vector<std::string>* subdirs) {
  auto listed = list_entries(dir);
  if (listed.is_err()) {
    return base::make_err(std::move(listed).unwrap_err());
  }
  std::vector<std::string> files;
  for (auto& [name, is_directory] : std::move(listed).unwrap()) {
    if (is_directory) {
      if (subdirs != nullptr) {
        subdirs->push_back(std::move(name));
      }
      continue;
    }
    files.push_back(std::move(name));
  }
  std::sort(files.begin(), files.end());
  if (subdirs != nullptr) {
    std::sort(subdirs->begin(), subdirs->end());
  }
  return base::make_ok(std::move(files));
}

base::Result<std::vector<std::string>, std::string> markdown_files(
    std::string_view dir) {
  auto listed = list_directory(dir, nullptr);
  if (listed.is_err()) {
    return base::make_err(std::move(listed).unwrap_err());
  }
  std::vector<std::string> names = std::move(listed).unwrap();
  std::erase_if(names,
                [](const std::string& name) { return !name.ends_with(".md"); });
  return base::make_ok(std::move(names));
}

base::Result<std::string, std::string> title_of(const Blocks& blocks,
                                                std::string_view source_rel) {
  if (blocks.empty() || blocks.front().type != BlockType::Heading ||
      blocks.front().level != 1) {
    return base::make_err(std::string(source_rel) +
                          ": the file must open with a level-1 heading");
  }
  return base::make_ok(blocks.front().text);
}

std::vector<DocPage> pages_for_language(const Catalog& catalog,
                                        const std::vector<DocSource>& sources,
                                        std::string_view language) {
  std::vector<DocPage> pages;
  pages.reserve(sources.size());
  const std::string prefix(prefix_of(catalog, language));
  // The default language defines which pages exist. A translation
  // replaces its counterpart; one that has not been written yet leaves
  // the default text in place, so a new translation can land a page at
  // a time.
  for (const DocSource& fallback : sources) {
    if (fallback.language != catalog.default_language) {
      continue;
    }
    const DocSource* source = &fallback;
    for (const DocSource& candidate : sources) {
      if (candidate.language == language && candidate.path == fallback.path) {
        source = &candidate;
        break;
      }
    }
    DocPage page;
    page.source_rel = source->source_rel;
    page.path = source->path;
    // The default source fixes the page's position; a translation must
    // not reorder the tree by giving its file a different number.
    page.order = fallback.order;
    page.title = source->title;
    page.blocks = source->blocks;
    page.language = std::string(language);
    page.route = prefix + source->path;
    pages.push_back(std::move(page));
  }
  return pages;
}

}  // namespace ssg
