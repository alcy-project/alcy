// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/new.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/io/file_handle.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/vcs.h"

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
  PipelineContext ctx{i18n::Language::EnUs};
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
  PipelineContext ctx{i18n::Language::EnUs};
  const std::string target = dir.join("myproj");
  CHECK(init_package(ctx, target, Vcs::Git).is_ok());
  // The default program prints, so the default manifest names the suite
  // that provides it.
  const std::string text = io::read_file(dir.join("myproj/alcy.toml"));
  CHECK(text.find("\"alcy/std/*\"") != std::string::npos);
}

TEST_CASE("Init refuses to overwrite an existing package") {
  io::TempDir dir = io::TempDir::create_unique("alcy_init_overwrite_test_");
  PipelineContext ctx{i18n::Language::EnUs};
  const std::string target = dir.join("myproj");
  CHECK(init_package(ctx, target, Vcs::Git).is_ok());
  CHECK(init_package(ctx, target, Vcs::Git).is_err());
  CHECK(ctx.bag.has_errors());
}

TEST_CASE("Init without a VCS writes no ignore file") {
  io::TempDir dir = io::TempDir::create_unique("alcy_init_novcs_test_");
  PipelineContext ctx{i18n::Language::EnUs};
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

  PipelineContext ctx{i18n::Language::EnUs};
  // The ignore file belongs to the user, so its presence is not a reason
  // to refuse: only the package files are alcy's to write.
  CHECK(init_package(ctx, dir.join("mine"), Vcs::None).is_ok());
  CHECK(io::read_file(dir.join("mine/.gitignore")) == "# mine\n");
}

TEST_CASE("New with a path creates the directories it names") {
  io::TempDir dir = io::TempDir::create_unique("alcy_new_path_test_");
  PipelineContext ctx{i18n::Language::EnUs};
  NewResult result =
      create_new_package(ctx, dir.join("first-party/cli"), Vcs::None);
  CHECK(result.is_ok());
  if (result.is_ok()) {
    CHECK(std::move(result).unwrap().name == "cli");
  }
  const std::string manifest =
      io::read_file(dir.join("first-party/cli/alcy.toml"));
  CHECK(manifest.find("name = \"cli\"") != std::string::npos);
}

TEST_CASE("A new suite writes its identity and ignores out") {
  io::TempDir dir = io::TempDir::create_unique("alcy_new_suite_test_");
  PipelineContext ctx{i18n::Language::EnUs};
  NewResult result =
      create_new_suite(ctx, dir.join("tools"), "acme/tools", Vcs::Git);
  CHECK(result.is_ok());
  if (result.is_ok()) {
    CHECK(std::move(result).unwrap().name == "tools");
  }
  const std::string manifest = io::read_file(dir.join("tools/alcy.toml"));
  CHECK(manifest.find("name = \"tools\"") != std::string::npos);
  CHECK(manifest.find("owner = \"acme\"") != std::string::npos);
  CHECK(manifest.find("packages = []") != std::string::npos);
  CHECK(io::read_file(dir.join("tools/.gitignore")) == "/out/\n");
}

TEST_CASE("A package created in a suite joins it") {
  io::TempDir dir = io::TempDir::create_unique("alcy_suite_member_test_");
  PipelineContext ctx{i18n::Language::EnUs};
  const std::string root = dir.join("tools");
  CHECK(create_new_suite(ctx, root, "acme/tools", Vcs::Git).is_ok());

  NewResult first = create_new_package(ctx, dir.join("tools/cli"), Vcs::Git);
  CHECK(first.is_ok());
  if (first.is_ok()) {
    const ScaffoldResult created = std::move(first).unwrap();
    CHECK(created.name == "cli");
    CHECK(created.suite == "acme/tools");
  }
  NewResult nested =
      create_new_package(ctx, dir.join("tools/first-party/other"), Vcs::None);
  CHECK(nested.is_ok());

  const std::string suite_text = io::read_file(dir.join("tools/alcy.toml"));
  CHECK(suite_text.find("packages = [\"cli\", \"first-party/other\"]") !=
        std::string::npos);
  const std::string member = io::read_file(dir.join("tools/cli/alcy.toml"));
  // A member takes the suite's identity by spelling the inheritance;
  // nothing travels between manifests otherwise (ADR-0057).
  CHECK(member.find("version.suite = true") != std::string::npos);
  CHECK(member.find("license.suite = true") != std::string::npos);
  // The suite root owns the build output its members share, so the
  // member writes no ignore file.
  io::FileHandle ignore;
  CHECK(!ignore.open(dir.join("tools/cli/.gitignore"), io::FileAccess::Read));
}

TEST_CASE("A suite refuses a member name it already holds") {
  io::TempDir dir = io::TempDir::create_unique("alcy_suite_clash_test_");
  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(create_new_suite(ctx, dir.join("tools"), "tools", Vcs::None).is_ok());
  CHECK(create_new_package(ctx, dir.join("tools/cli"), Vcs::None).is_ok());
  // Another path with the same last segment would be addressed by the
  // same name, and the refusal comes before any file is written.
  CHECK(create_new_package(ctx, dir.join("tools/vendor/cli"), Vcs::None)
            .is_err());
  CHECK(ctx.bag.has_errors());
  io::FileHandle manifest;
  CHECK(!manifest.open(dir.join("tools/vendor/cli/alcy.toml"),
                       io::FileAccess::Read));
}

}  // namespace pipeline
