// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "cfg/build_config.h"
#include "fpag/base/numeric.h"
#include "source/source.h"

#if BUILD_FLAG(IS_OS_WIN)
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace test_fs {

// RAII scratch directory for tests, built on C/POSIX/Win32 APIs.
// Best-effort cleanup.
class TempDir {
 public:
  explicit TempDir(std::string_view name) {
    std::string base;
#if BUILD_FLAG(IS_OS_WIN)
    char buf[1024];
    const DWORD len = ::GetTempPathA(sizeof(buf), buf);
    base = (len > 0 && len < sizeof(buf)) ? std::string(buf, len) : ".\\";
#else
    const char* tmp = std::getenv("TMPDIR");
    base = (tmp != nullptr && tmp[0] != '\0') ? tmp : "/tmp";
    if (!base.empty() && base.back() != '/') {
      base.push_back('/');
    }
#endif
    root_ = base + std::string(name);
    // Canonical separator is '/': valid on Windows file APIs too, so test
    // expectations stay identical across platforms. Untouched on POSIX,
// where backslash is a valid filename character.
#if BUILD_FLAG(IS_OS_WIN)
    for (char& c : root_) {
      if (c == source::kWindowsPathSeparator) {
        c = source::kDefaultPathSeparator;
      }
    }
#endif
    remove_all(root_);
    make_dirs(root_);
  }

  ~TempDir() {
    for (const std::string& file : std::views::reverse(files_)) {
      std::remove(file.c_str());
    }
    for (const std::string& dir : std::views::reverse(dirs_)) {
      remove_dir(dir);
    }
    remove_dir(root_);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return root_; }

  std::string join(std::string_view child) const {
    std::string out = root_;
    out.push_back(source::kDefaultPathSeparator);
    out.append(child);
    return out;
  }

  bool make_dir(std::string_view rel) {
    const std::string full = join(rel);
    if (!make_dirs(full)) {
      return false;
    }
    dirs_.push_back(full);
    return true;
  }

  bool write_file(std::string_view rel, std::string_view bytes) {
    const std::string full = join(rel);
#if BUILD_FLAG(IS_OS_WIN)
    const usize slash = full.find_last_of("/\\");
#else
    const usize slash = full.find_last_of('/');
#endif
    if (slash != std::string::npos && !make_dirs(full.substr(0, slash))) {
      return false;
    }
    std::FILE* file = std::fopen(full.c_str(), "wb");
    if (file == nullptr) {
      return false;
    }
    const usize written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (written != bytes.size()) {
      return false;
    }
    files_.push_back(full);
    return true;
  }

 private:
  static bool is_sep(char c) {
#if BUILD_FLAG(IS_OS_WIN)
    return c == '/' || c == '\\';
#else
    // Backslash is a valid filename character on POSIX; never split on it.
    return c == '/';
#endif
  }

  static bool make_dirs(const std::string& path) {
    std::string current;
    usize index = 0;
#if BUILD_FLAG(IS_OS_WIN)
    // Preserve drive-letter roots like "C:".
    if (path.size() >= 2 && path[1] == ':') {
      current = path.substr(0, 2);
      index = 2;
    }
#endif
    for (; index <= path.size(); ++index) {
      if (index == path.size() || is_sep(path[index])) {
        if (!current.empty()) {
#if BUILD_FLAG(IS_OS_WIN)
          ::_mkdir(current.c_str());
#else
          ::mkdir(current.c_str(), 0755);
#endif
        }
      }
      if (index < path.size()) {
        current.push_back(path[index]);
      }
    }
    return true;
  }

  static void remove_all(const std::string& path) {
#if BUILD_FLAG(IS_OS_WIN)
    WIN32_FIND_DATAA found;
    HANDLE handle = ::FindFirstFileA((path + "/*").c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE) {
      return;
    }
    do {
      const std::string_view name(found.cFileName);
      if (name == "." || name == "..") {
        continue;
      }
      const std::string full = path + "/" + std::string(name);
      if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
          (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        remove_all(full);
        ::_rmdir(full.c_str());
      } else {
        std::remove(full.c_str());
      }
    } while (::FindNextFileA(handle, &found) != 0);
    ::FindClose(handle);
#else
    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) {
      return;
    }
    while (dirent* entry = ::readdir(dir)) {
      const std::string_view name(entry->d_name);
      if (name == "." || name == "..") {
        continue;
      }
      const std::string full = path + "/" + std::string(name);
      struct stat info;
      // lstat: never follow symlinks, so link cycles are impossible.
      if (::lstat(full.c_str(), &info) != 0) {
        continue;
      }
      if (S_ISDIR(info.st_mode)) {
        remove_all(full);
        ::rmdir(full.c_str());
      } else {
        std::remove(full.c_str());
      }
    }
    ::closedir(dir);
#endif
  }

  static void remove_dir(const std::string& path) {
#if BUILD_FLAG(IS_OS_WIN)
    ::_rmdir(path.c_str());
#else
    ::rmdir(path.c_str());
#endif
  }

  std::string root_;
  std::vector<std::string> files_;
  std::vector<std::string> dirs_;
};

}  // namespace test_fs
