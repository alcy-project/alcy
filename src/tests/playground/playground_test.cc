// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "playground/playground.h"

#include <string>
#include <string_view>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"

namespace playground {
namespace {

AlcyResult check(std::string_view source) {
  AlcyResult result{};
  alcy_check(reinterpret_cast<const u8*>(source.data()), source.size(),
             &result);
  return result;
}

AlcyResult compile(std::string_view source) {
  AlcyResult result{};
  alcy_compile(reinterpret_cast<const u8*>(source.data()), source.size(),
               &result);
  return result;
}

std::string diagnostics_of(const AlcyResult& result) {
  return std::string(reinterpret_cast<const char*>(result.diagnostics),
                     result.diagnostics_len);
}

constexpr std::string_view HELLO = "fn main() {\n  println(\"hi\")\n}\n";
constexpr std::string_view BROKEN = "fn main() {\n  missing()\n}\n";

}  // namespace

TEST_CASE("A checked source answers the counts") {
  AlcyResult result = check(HELLO);
  CHECK(result.ok == 1);
  CHECK(result.file_count == 1);
  CHECK(result.module_count == 1);
  CHECK(result.function_count == 1);
  // Check produces no module, and an empty diagnostics array is still an
  // array the host can parse.
  CHECK(result.wasm == nullptr);
  CHECK(result.wasm_len == 0);
  CHECK(diagnostics_of(result) == "[]");

  alcy_release(&result);
  CHECK(result.diagnostics == nullptr);
  CHECK(result.diagnostics_len == 0);
  CHECK(result.ok == 0);
}

TEST_CASE("A compiled source answers a wasm module") {
  AlcyResult result = compile(HELLO);
  CHECK(result.ok == 1);
  CHECK(result.wasm_len > 8);
  if (result.wasm_len >= 4) {
    CHECK(result.wasm[0] == 0x00);
    CHECK(result.wasm[1] == 0x61);
    CHECK(result.wasm[2] == 0x73);
    CHECK(result.wasm[3] == 0x6D);
  }
  CHECK(result.file_count == 1);
  CHECK(result.module_count == 1);
  CHECK(result.function_count == 1);
  CHECK(diagnostics_of(result) == "[]");
  alcy_release(&result);
}

TEST_CASE("A rejected source reports the CLI's diagnostics array") {
  AlcyResult checked = check(BROKEN);
  CHECK(checked.ok == 0);
  const std::string diagnostics = diagnostics_of(checked);
  CHECK(diagnostics.front() == '[');
  CHECK(diagnostics.back() == ']');
  CHECK(diagnostics.find("\"severity\":\"error\"") != std::string::npos);
  CHECK(diagnostics.find("Unresolved value") != std::string::npos);
  CHECK(checked.wasm == nullptr);
  alcy_release(&checked);

  AlcyResult compiled = compile(BROKEN);
  CHECK(compiled.ok == 0);
  CHECK(compiled.wasm == nullptr);
  CHECK(diagnostics_of(compiled).find("\"severity\":\"error\"") !=
        std::string::npos);
  alcy_release(&compiled);
}

TEST_CASE("A null result pointer is refused") {
  CHECK(alcy_check(nullptr, 0, nullptr) == 0);
  CHECK(alcy_compile(nullptr, 0, nullptr) == 0);
  alcy_release(nullptr);
}

}  // namespace playground
