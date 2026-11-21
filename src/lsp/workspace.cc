// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/workspace.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/hash.h"
#include "diag/bag.h"
#include "fingerprint/fingerprint.h"
#include "fpag/io/io_util.h"
#include "i18n/language.h"
#include "linter/config.h"
#include "path/path.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_context.h"
#include "pkg/config.h"
#include "pkg/manifest.h"

namespace lsp::workspace {

namespace {

constexpr std::string_view FILE_SCHEME = "file://";

bool hex_digit(char c, u8& out) {
  if (c >= '0' && c <= '9') {
    out = static_cast<u8>(c - '0');
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    out = static_cast<u8>(c - 'a' + 10);
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    out = static_cast<u8>(c - 'A' + 10);
    return true;
  }
  return false;
}

std::string read_or_empty(const path::Path& path) {
  return io::read_file(std::string(path.as_view()));
}

}  // namespace

std::optional<std::string> uri_to_path(std::string_view uri) {
  if (!uri.starts_with(FILE_SCHEME)) {
    return std::nullopt;
  }
  const std::string_view rest = uri.substr(FILE_SCHEME.size());
  std::string out;
  out.reserve(rest.size());
  for (usize i = 0; i < rest.size(); ++i) {
    const char c = rest[i];
    if (c == '%' && i + 2 < rest.size()) {
      u8 high = 0;
      u8 low = 0;
      if (!hex_digit(rest[i + 1], high) || !hex_digit(rest[i + 2], low)) {
        return std::nullopt;
      }
      out.push_back(static_cast<char>((high << 4) | low));
      i += 2;
      continue;
    }
    out.push_back(c);
  }
  // `file:///C:/x` names a drive, not a root followed by `C:`.
  if (out.size() >= 3 && out[0] == '/' && out[2] == ':') {
    out.erase(out.begin());
  }
  if (out.empty()) {
    return std::nullopt;
  }
  return out;
}

std::string path_to_uri(std::string_view path) {
  constexpr std::string_view unreserved =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/";
  std::string out(FILE_SCHEME);
  out.reserve(FILE_SCHEME.size() + path.size());
  for (char c : path) {
    if (c == '\\') {
      out.push_back('/');
    } else if (unreserved.find(c) != std::string_view::npos || c == ':') {
      out.push_back(c);
    } else {
      static constexpr char DIGITS[] = "0123456789ABCDEF";
      const u8 byte = static_cast<u8>(c);
      out.push_back('%');
      out.push_back(DIGITS[byte >> 4]);
      out.push_back(DIGITS[byte & 0x0f]);
    }
  }
  const usize start = FILE_SCHEME.size();
  const bool drive =
      out.size() > start + 1 && out[start] != '/' && out[start + 1] == ':';
  if (!drive && (out.size() == start || out[start] != '/')) {
    out.insert(start, 1, '/');
  } else if (drive) {
    out.insert(start, 1, '/');
  }
  return out;
}

std::optional<path::Path> find_package_root(std::string_view file_path) {
  base::Result<path::Path, path::PathError> parsed =
      path::Path::from_native(file_path);
  if (parsed.is_err()) {
    return std::nullopt;
  }
  path::Path at = std::move(parsed).unwrap().parent();
  while (true) {
    const path::Path manifest = at.join(pkg::MANIFEST_FILE_NAME);
    if (pkg::probe_manifest_kind(read_or_empty(manifest)) ==
        pkg::ManifestKind::Package) {
      return at;
    }
    const path::Path parent = at.parent();
    if (parent == at) {
      return std::nullopt;
    }
    at = parent;
  }
}

bool under_root(std::string_view file_path, std::string_view root) {
  if (file_path.size() <= root.size() || !file_path.starts_with(root)) {
    return false;
  }
  const char separator = file_path[root.size()];
  return separator == '/' || separator == '\\';
}

std::string fingerprint(std::string_view root,
                        std::span<const BufferView> buffers) {
  pipeline::PipelineContext context(i18n::Language::EnUs);
  fingerprint::Builder builder;
  builder.bytes(root).u8(0);

  base::Result<path::Path, path::PathError> parsed =
      path::Path::from_native(root);
  if (parsed.is_err()) {
    return builder.finish().hex();
  }
  const path::Path root_path = std::move(parsed).unwrap();
  builder.digest(
      base::hash128(read_or_empty(root_path.join(pkg::MANIFEST_FILE_NAME))));
  builder.digest(base::hash128(read_or_empty(
      root_path.join(pkg::CONFIG_DIR_NAME).join(linter::LINT_FILE_NAME))));

  const auto buffer_for = [&](std::string_view file) -> const BufferView* {
    for (const BufferView& buffer : buffers) {
      if (const std::optional<std::string> path = uri_to_path(buffer.uri);
          path.has_value() && *path == file) {
        return &buffer;
      }
    }
    return nullptr;
  };

  base::Result<pipeline::DiscoveredSourcePaths, diag::Reported> discovered =
      pipeline::discover_source_paths(root, context.bag);
  std::unordered_set<std::string> discovered_files;
  if (discovered.is_ok()) {
    pipeline::DiscoveredSourcePaths listed = std::move(discovered).unwrap();
    for (const std::string& file : listed.files) {
      discovered_files.insert(file);
      builder.bytes(file).u8(':');
      if (const BufferView* buffer = buffer_for(file); buffer != nullptr) {
        builder.bytes(std::to_string(buffer->version))
            .u8(':')
            .digest(base::hash128(buffer->text));
      } else {
        builder.digest(base::hash128(io::read_file(file)));
      }
      builder.u8(0);
    }
  }
  // A buffer under the root that discovery never saw - an unsaved new
  // file - is still loaded as a virtual source by analyze_package. Its
  // bytes and version must steer the signature or edits go unnoticed.
  for (const BufferView& buffer : buffers) {
    const std::optional<std::string> path = uri_to_path(buffer.uri);
    if (!path.has_value() || !under_root(*path, root) ||
        discovered_files.contains(*path)) {
      continue;
    }
    builder.bytes(buffer.uri)
        .u8(':')
        .bytes(std::to_string(buffer.version))
        .u8(':')
        .digest(base::hash128(buffer.text))
        .u8(0);
  }
  return builder.finish().hex();
}

}  // namespace lsp::workspace
