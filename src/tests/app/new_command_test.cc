// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "app/new_command.h"

#include "doctest/doctest.h"

namespace app {

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

}  // namespace app
