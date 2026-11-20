// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "fpag/base/numeric.h"
#include "path/path.h"

namespace lsp::workspace {

// One in-memory buffer an analysis may have to read instead of the file
// on disk. `version` is the protocol version, so a saved and an unsaved
// buffer at the same version still compare equal only when the text does.
struct BufferView {
  std::string_view uri;
  std::string_view text;
  i64 version = 0;
};

// Translates a `file://` URI to a path the source manager can load.
// Percent escapes are decoded; other schemes have no path.
std::optional<std::string> uri_to_path(std::string_view uri);

// The inverse spelling, for buffers a client names by path.
std::string path_to_uri(std::string_view path);

// Nearest enclosing directory with a package alcy.toml, starting at the
// directory of `file_path` and walking upwards. Suite manifests are not
// package roots.
std::optional<path::Path> find_package_root(std::string_view file_path);

// Whether `file_path` sits under `root` (not merely shares a prefix).
bool under_root(std::string_view file_path, std::string_view root);

// Everything whose change invalidates an analysis of `root`: the manifest,
// the lint config, every discovered source, and every open buffer under
// the root. Disk content is hashed so a save made outside the client is
// noticed too.
std::string fingerprint(std::string_view root,
                        std::span<const BufferView> buffers);

}  // namespace lsp::workspace
