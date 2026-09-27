// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "source/source.h"

namespace source {

namespace {

// Writes a file into the scratch directory and reports whether it landed,
// so a test can leave early when the setup failed.
bool write(io::TempDir& dir, std::string_view name, std::string_view bytes) {
  return dir.write_file(name, bytes);
}

}  // namespace

TEST_CASE("A virtual source is readable and named") {
  SourceManager sources;
  // A short name on purpose: a short string is held inside the string
  // object rather than on the heap, so it is the name whose view a
  // reallocating container would invalidate.
  const FileId id = sources.add_virtual("a.al", "fn main() {}\n");
  CHECK(sources.bytes(id).value_or("") == "fn main() {}\n");
  CHECK(sources.name(id).value_or("") == "a.al");
  CHECK(!sources.is_mapped(id));
  CHECK(sources.file_count() == 1);
}

TEST_CASE("A virtual source owns its bytes") {
  SourceManager sources;
  {
    // The caller's buffer dies at the end of the scope; the manager
    // copied, so the view is still good afterwards.
    std::string temporary = "fn main() {}\n";
    sources.add_virtual("a.al", temporary);
    temporary.assign("clobbered");
    temporary.clear();
    temporary.shrink_to_fit();
  }
  const FileId id = 0;
  CHECK(sources.bytes(id).value_or("") == "fn main() {}\n");
}

TEST_CASE("A virtual name is one id") {
  SourceManager sources;
  const FileId first = sources.add_virtual("a.al", "one");
  const FileId second = sources.add_virtual("a.al", "two");
  CHECK(first == second);
  CHECK(sources.file_count() == 1);
  // The first content wins, so a name never shows two different bodies.
  const std::optional<std::string_view> body = sources.bytes(first);
  CHECK(body.has_value());
  CHECK(body.value_or("") == "one");
}

TEST_CASE("A virtual name may be any label") {
  SourceManager sources;
  // Not a path: the manager does not require one, because nothing
  // downstream of it opens the name.
  CHECK(sources.add_virtual("<stdin>", "fn main() {}\n") == 0);
  CHECK(sources.name(0).value_or("") == "<stdin>");
  CHECK(sources.add_virtual("untitled:1", "") == 1);
  CHECK(sources.bytes(1).value_or("x").empty());
}

TEST_CASE("A view survives a later load") {
  // Loading the standard library adds dozens of files, which reallocated
  // a vector of entries. A view taken beforehand has to outlive that.
  io::TempDir dir = io::TempDir::create_unique("alcy_source_view_");
  CHECK(write(dir, "b.al", "fn b() {}\n"));

  SourceManager sources;
  const FileId first = sources.add_virtual("a.al", "fn main() {}\n");
  const std::optional<std::string_view> maybe_name = sources.name(first);
  const std::optional<std::string_view> maybe_body = sources.bytes(first);
  CHECK(maybe_name.has_value());
  CHECK(maybe_body.has_value());
  const std::string_view name =
      maybe_name.has_value() ? maybe_name.value() : std::string_view{};
  const std::string_view body =
      maybe_body.has_value() ? maybe_body.value() : std::string_view{};
  CHECK(name == "a.al");
  CHECK(body == "fn main() {}\n");
  if (name != "a.al" || body != "fn main() {}\n") {
    return;
  }

  for (int i = 0; i < 64; ++i) {
    const std::string file = "generated" + std::to_string(i) + ".al";
    CHECK(write(dir, file, "fn g() {}\n"));
    base::Result<FileId, SourceError> loaded =
        sources.load(dir.join(file).c_str());
    CHECK(loaded.is_ok());
  }
  CHECK(sources.file_count() > 1);
  CHECK(sources.name(first).value_or("") == "a.al");
  CHECK(sources.bytes(first).value_or("") == "fn main() {}\n");
  // The views taken before the loads grew the manager still read back the
  // same bytes, which is the whole point.
  CHECK(name == "a.al");
  CHECK(body == "fn main() {}\n");
}

TEST_CASE("A mapped source and a virtual one coexist") {
  io::TempDir dir = io::TempDir::create_unique("alcy_source_mixed_");
  CHECK(write(dir, "real.al", "fn real() {}\n"));

  SourceManager sources;
  base::Result<FileId, SourceError> loaded =
      sources.load(dir.join("real.al").c_str());
  CHECK(loaded.is_ok());
  if (loaded.is_err()) {
    return;
  }
  const FileId real = std::move(loaded).unwrap();
  const FileId virt = sources.add_virtual("imaginary.al", "fn imag() {}\n");

  CHECK(sources.is_mapped(real));
  CHECK(!sources.is_mapped(virt));
  CHECK(sources.bytes(real).value_or("") == "fn real() {}\n");
  CHECK(sources.bytes(virt).value_or("") == "fn imag() {}\n");
}

TEST_CASE("An unknown id is distinguishable from an empty one") {
  SourceManager sources;
  sources.add_virtual("empty.al", "");
  const std::optional<std::string_view> empty = sources.bytes(0);
  CHECK(empty.has_value());
  CHECK(empty.value_or("x").empty());
  CHECK(!sources.bytes(1).has_value());
  CHECK(!sources.name(1).has_value());
  CHECK(sources.file_count() == 1);
}

}  // namespace source
