// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/new.h"

#include <string>

#include "doctest/doctest.h"
#include "fpag/io/file_handle.h"
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
  io::TempDir dir("alcy_init_name_test");
  PipelineContext ctx;
  const std::string target = dir.join("myproj");
  CHECK(init_package(ctx, target).is_ok());
  io::FileHandle manifest;
  CHECK(manifest.open(dir.join("myproj/alcy.toml"), io::FileAccess::Read));
  io::FileHandle main;
  CHECK(main.open(dir.join("myproj/main.al"), io::FileAccess::Read));
}

TEST_CASE("Init refuses to overwrite an existing package") {
  io::TempDir dir("alcy_init_overwrite_test");
  PipelineContext ctx;
  const std::string target = dir.join("myproj");
  CHECK(init_package(ctx, target).is_ok());
  CHECK(init_package(ctx, target).is_err());
  CHECK(ctx.bag.has_errors());
}

}  // namespace pipeline
