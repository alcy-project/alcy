// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/new.h"

#include <span>
#include <string>
#include <string_view>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/io/file_handle.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "pipeline/pipeline_context.h"

namespace pipeline {

TEST_CASE("Valid package names") {
  CHECK(valid_package_name("mypkg"));
  CHECK(valid_package_name("my-pkg_123"));
  CHECK(valid_package_name("A"));
}

TEST_CASE("Invalid package names") {
  CHECK(!valid_package_name(""));
  CHECK(!valid_package_name("my pkg"));
  CHECK(!valid_package_name("my/pkg"));
  CHECK(!valid_package_name("my.pkg"));
  CHECK(!valid_package_name("pkg!"));
}

TEST_CASE("Init derives the package name from the directory") {
  io::TempDir dir = io::TempDir::create_unique("alcy_init_name_test_");
  PipelineContext ctx;
  const std::string target = dir.join("myproj");
  CHECK(init_package(ctx, target, Vcs::Git).is_ok());
  io::FileHandle manifest;
  CHECK(manifest.open(dir.join("myproj/alcy.toml"), io::FileAccess::Read));
  io::FileHandle main;
  CHECK(main.open(dir.join("myproj/main.al"), io::FileAccess::Read));
  io::FileHandle gitignore;
  CHECK(gitignore.open(dir.join("myproj/.gitignore"), io::FileAccess::Read));
}

TEST_CASE("Init depends on the standard suite by default") {
  io::TempDir dir = io::TempDir::create_unique("alcy_init_deps_test_");
  PipelineContext ctx;
  const std::string target = dir.join("myproj");
  CHECK(init_package(ctx, target, Vcs::Git).is_ok());
  // The default program prints, so the default manifest names the suite
  // that provides it.
  const std::string text = io::read_file(dir.join("myproj/alcy.toml"));
  CHECK(text.find("\"alcy/std/*\"") != std::string::npos);
}

TEST_CASE("Init refuses to overwrite an existing package") {
  io::TempDir dir = io::TempDir::create_unique("alcy_init_overwrite_test_");
  PipelineContext ctx;
  const std::string target = dir.join("myproj");
  CHECK(init_package(ctx, target, Vcs::Git).is_ok());
  CHECK(init_package(ctx, target, Vcs::Git).is_err());
  CHECK(ctx.bag.has_errors());
}

TEST_CASE("Init without a VCS writes no ignore file") {
  io::TempDir dir = io::TempDir::create_unique("alcy_init_novcs_test_");
  PipelineContext ctx;
  const std::string target = dir.join("myproj");
  CHECK(init_package(ctx, target, Vcs::None).is_ok());
  io::FileHandle manifest;
  CHECK(manifest.open(dir.join("myproj/alcy.toml"), io::FileAccess::Read));
  io::FileHandle gitignore;
  CHECK(!gitignore.open(dir.join("myproj/.gitignore"), io::FileAccess::Read));
}

TEST_CASE("Init without a VCS leaves an existing ignore file alone") {
  io::TempDir dir = io::TempDir::create_unique("alcy_init_novcs_keep_test_");
  CHECK(dir.make_dir("mine"));
  const std::string_view MINE = "# mine\n";
  CHECK(
      io::write_file(std::span<const u8>(
                         reinterpret_cast<const u8*>(MINE.data()), MINE.size()),
                     dir.join("mine/.gitignore")));

  PipelineContext ctx;
  // The ignore file belongs to the user, so its presence is not a reason
  // to refuse: only the package files are alcy's to write.
  CHECK(init_package(ctx, dir.join("mine"), Vcs::None).is_ok());
  CHECK(io::read_file(dir.join("mine/.gitignore")) == "# mine\n");
}

}  // namespace pipeline
