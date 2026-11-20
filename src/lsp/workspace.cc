// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/workspace.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
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

u64 hash_bytes(std::string_view bytes) {
  u64 hash = 1469598103934665603ULL;
  for (char c : bytes) {
    hash ^= static_cast<u8>(c);
    hash *= 1099511628211ULL;
  }
  return hash;
}

void append_hash(std::string& out, std::string_view bytes) {
  const u64 hash = hash_bytes(bytes);
  for (u32 shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((hash >> shift) & 0xff));
  }
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
  std::string signature;
  signature.append(root);
  signature.push_back('\0');

  base::Result<path::Path, path::PathError> parsed =
      path::Path::from_native(root);
  if (parsed.is_err()) {
    return signature;
  }
  const path::Path root_path = std::move(parsed).unwrap();
  append_hash(signature,
              read_or_empty(root_path.join(pkg::MANIFEST_FILE_NAME)));
  append_hash(
      signature,
      read_or_empty(
          root_path.join(pkg::CONFIG_DIR_NAME).join(linter::LINT_FILE_NAME)));

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
  const bool have_sources = discovered.is_ok();
  if (have_sources) {
    pipeline::DiscoveredSourcePaths listed = std::move(discovered).unwrap();
    for (const std::string& file : listed.files) {
      signature.append(file);
      signature.push_back(':');
      if (const BufferView* buffer = buffer_for(file); buffer != nullptr) {
        signature.append(std::to_string(buffer->version));
        signature.push_back(':');
        append_hash(signature, buffer->text);
      } else {
        append_hash(signature, io::read_file(file));
      }
      signature.push_back('\0');
    }
  }
  for (const BufferView& buffer : buffers) {
    const std::optional<std::string> path = uri_to_path(buffer.uri);
    if (!path.has_value() || !under_root(*path, root) || have_sources) {
      continue;
    }
    signature.append(buffer.uri);
    signature.push_back(':');
    signature.append(std::to_string(buffer.version));
    signature.push_back('\0');
  }
  return signature;
}

}  // namespace lsp::workspace
