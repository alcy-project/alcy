// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/deserialize.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "ir/binary_format.h"
#include "ir/common.h"
#include "ir/serialize.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/write_input.h"
#include "tests/ir/ir_fixtures.h"

namespace ir {

namespace {

// A write input aliasing a loaded package's own buffers, so a test can
// serialize what it just read back.
struct LoadedInput {
  std::vector<std::string_view> files;
  std::vector<AddrName> addr_names;
  WriteInput input;

  explicit LoadedInput(LoadedIr& loaded) {
    files.reserve(loaded.file_names.size());
    for (const std::string& name : loaded.file_names) {
      files.push_back(name);
    }
    addr_names.reserve(loaded.addr_names.size());
    for (const LoadedIr::AddrNameEntry& entry : loaded.addr_names) {
      addr_names.push_back(
          AddrName{entry.reg, entry.name, entry.is_param, entry.is_capture});
    }
    input = WriteInput{
        .storage = &*loaded.storage,
        .strings = loaded.strings.get(),
        .instr_spans = loaded.instr_spans,
        .files = FileTable{.names = files, .hashes = loaded.file_hashes},
        .addr_names = addr_names,
        .prelude_functions = loaded.prelude_functions,
        .width = loaded.width,
        .compiler_version = loaded.compiler_version,
    };
  }
};

}  // namespace

TEST_CASE("A serialized package reads back as the same bytes") {
  str::StringInterner strings;
  const Storage storage = test::composite_storage(strings);
  const std::vector<u8> bytes = test::composite_bytes(strings, storage);

  base::Result<LoadedIr, IrLoadError> loaded = deserialize(bytes);
  CHECK(loaded.is_ok());
  if (loaded.is_err()) {
    return;
  }
  LoadedIr package = std::move(loaded).unwrap();

  // The side tables came back.
  CHECK(package.width == PointerWidth::W64);
  CHECK(package.compiler_version == "0.0.0-test");
  CHECK(package.prelude_functions == 0);
  CHECK(package.file_names.size() == 1);
  CHECK(package.file_names[0] == "main.al");
  CHECK(package.instr_spans.size() == 4);
  CHECK(package.instr_spans[2].offset == 5);
  CHECK(package.instr_spans[2].length == 6);
  CHECK(package.addr_names.size() == 1);
  CHECK(package.addr_names[0].name == "x");
  CHECK(package.addr_names[0].reg.idx == 0);

  // The storage resolves through the interner the reader built.
  const Storage& read = *package.storage;
  CHECK(read.functions().size() == 1);
  CHECK(read.instrs().size() == 4);
  CHECK(package.strings->get(read.functions()[FunctionIdx(0)].meta.name) ==
        "main");

  // Reading and writing again produces the bytes it read.
  LoadedInput input(package);
  CHECK(serialize(input.input) == bytes);
}

TEST_CASE("A corrupt binary form is refused") {
  str::StringInterner strings;
  const Storage storage = test::composite_storage(strings);
  const std::vector<u8> bytes = test::composite_bytes(strings, storage);
  const std::span<const u8> file(bytes);

  {
    // Truncation, in the header and in the middle.
    CHECK(deserialize(file.first(10)).is_err());
    CHECK(deserialize(file.first(bytes.size() - 1)).is_err());
  }
  {
    std::vector<u8> broken = bytes;
    broken[0] = 'B';
    CHECK(std::move(deserialize(broken)).unwrap_err() == IrLoadError::BadMagic);
  }
  {
    std::vector<u8> broken = bytes;
    broken[4] = 2;  // A major version this reader does not know.
    CHECK(std::move(deserialize(broken)).unwrap_err() ==
          IrLoadError::UnsupportedVersion);
  }
  {
    std::vector<u8> broken = bytes;
    broken[8] = 0;  // Little-endian flag cleared.
    CHECK(std::move(deserialize(broken)).unwrap_err() == IrLoadError::BadFlags);
  }
  {
    // The second section starts where the first does.
    std::vector<u8> broken = bytes;
    const usize version_len = test::read_u32(file, 16);
    const usize table_at = 20 + version_len + 8;
    const u64 first_offset = test::read_u64(file, table_at + 8);
    broken[table_at + 24 + 8] = static_cast<u8>(first_offset);
    for (u32 i = 1; i < 8; ++i) {
      broken[table_at + 24 + 8 + i] = 0;
    }
    CHECK(std::move(deserialize(broken)).unwrap_err() ==
          IrLoadError::BadSection);
  }
  {
    // A span count that is neither zero nor the instruction count.
    std::vector<u8> broken = bytes;
    const usize spans_at = test::section_at(file, binary::Section::Spans);
    broken[spans_at] = 3;
    CHECK(std::move(deserialize(broken)).unwrap_err() == IrLoadError::BadShape);
  }
  {
    // A composite payload that addresses no row: the verifier refuses.
    std::vector<u8> broken = bytes;
    const usize types_at = test::section_at(file, binary::Section::Types);
    const u32 type_count = test::read_u32(broken, types_at);
    bool patched = false;
    for (u32 i = 0; i < type_count; ++i) {
      const usize at = types_at + 4 + static_cast<usize>(i) * 8;
      if (broken[at] == static_cast<u8>(TypeTag::Struct)) {
        broken[at + 4] = 0xFF;
        broken[at + 5] = 0xFF;
        patched = true;
        break;
      }
    }
    CHECK(patched);
    CHECK(std::move(deserialize(broken)).unwrap_err() ==
          IrLoadError::Verification);
  }
  {
    // A string count the section cannot hold, which would otherwise ask
    // for an allocation of the count's size.
    std::vector<u8> broken = bytes;
    const usize strings_at = test::section_at(file, binary::Section::Strings);
    broken[strings_at] = 0xFF;
    broken[strings_at + 1] = 0xFF;
    broken[strings_at + 2] = 0xFF;
    broken[strings_at + 3] = 0x7F;
    CHECK(std::move(deserialize(broken)).unwrap_err() == IrLoadError::BadShape);
  }
  {
    // An operand tag the format does not define.
    std::vector<u8> broken = bytes;
    const usize operands_at = test::section_at(file, binary::Section::Operands);
    broken[operands_at + 4] = 0xEE;
    CHECK(std::move(deserialize(broken)).unwrap_err() == IrLoadError::BadShape);
  }
}

}  // namespace ir
