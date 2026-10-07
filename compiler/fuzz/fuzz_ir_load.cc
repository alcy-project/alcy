// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Arbitrary bytes through the IR binary form's reader.
//
// A cache or a tool hands the reader bytes the compiler did not just
// write, so this is an entry point for untrusted input and ADR-0015's
// property applies: it refuses with an error or hands back verified
// storage, and never crashes or reads out of bounds. A package that
// loads is also normalized: writing what was read and loading that
// again must produce the same bytes, and a disagreement aborts for
// libFuzzer to minimize.

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "ir/deserialize.h"
#include "ir/serialize.h"
#include "ir/write_input.h"

namespace {

// The write input for a loaded package; `files` and `addr_names` are the
// caller's storage for the views the input needs.
ir::WriteInput make_input(ir::LoadedIr& package,
                          std::vector<std::string_view>& files,
                          std::vector<ir::AddrName>& addr_names) {
  files.clear();
  files.reserve(package.file_names.size());
  for (const std::string& name : package.file_names) {
    files.push_back(name);
  }
  addr_names.clear();
  addr_names.reserve(package.addr_names.size());
  for (const ir::LoadedIr::AddrNameEntry& entry : package.addr_names) {
    addr_names.push_back(
        ir::AddrName{entry.reg, entry.name, entry.is_param, entry.is_capture});
  }
  return ir::WriteInput{
      .storage = &*package.storage,
      .strings = package.strings.get(),
      .instr_spans = package.instr_spans,
      .files = ir::FileTable{.names = files, .hashes = package.file_hashes},
      .addr_names = addr_names,
      .prelude_functions = package.prelude_functions,
      .width = package.width,
      .compiler_version = package.compiler_version,
  };
}

}  // namespace

extern "C" i32 LLVMFuzzerTestOneInput(const u8* data, usize size) {
  const std::span<const u8> bytes(data, size);
  base::Result<ir::LoadedIr, ir::IrLoadError> loaded = ir::deserialize(bytes);
  if (loaded.is_err()) {
    return 0;
  }
  ir::LoadedIr package = std::move(loaded).unwrap();

  std::vector<std::string_view> files;
  std::vector<ir::AddrName> addr_names;
  const std::vector<u8> normalized =
      ir::serialize(make_input(package, files, addr_names));

  base::Result<ir::LoadedIr, ir::IrLoadError> again =
      ir::deserialize(normalized);
  if (again.is_err()) {
    // The reader refused bytes its own writer wrote.
    __builtin_trap();
  }
  ir::LoadedIr second = std::move(again).unwrap();
  std::vector<std::string_view> second_files;
  std::vector<ir::AddrName> second_names;
  if (ir::serialize(make_input(second, second_files, second_names)) !=
      normalized) {
    // Normalization is not idempotent: a table or a string moved on the
    // second pass.
    __builtin_trap();
  }
  return 0;
}
