// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

// Minimal POSIX path handling for the routes the site computes:
// normalize, join, dirname, basename, extension, relative. Paths here
// are always forward-slash separated; the OS side of every write goes
// through these helpers rather than a platform separator.

std::vector<std::string_view> split_segments(std::string_view path);
std::string normalize_posix(std::string_view path);
std::string join_posix(std::string_view first, std::string_view second);
std::string join_posix(std::initializer_list<std::string_view> parts);
std::string dirname_posix(std::string_view path);
std::string basename_posix(std::string_view path);
std::string extension_posix(std::string_view path);
std::string relative_posix(std::string_view from_dir, std::string_view to_path);

std::string trim_view(std::string_view text);
std::string lowercase(std::string_view text);

}  // namespace ssg
