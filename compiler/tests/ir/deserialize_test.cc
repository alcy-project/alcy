// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/deserialize.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/hash.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "ir/binary_format.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/seq_builder.h"
#include "ir/serialize.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/summary.h"
#include "ir/symbol_table.h"
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
  std::span<const SummaryEntry> summary;
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
    summary = loaded.summary;
    input = WriteInput{
        .storage = &*loaded.storage,
        .strings = loaded.strings.get(),
        .instr_spans = loaded.instr_spans,
        .files = FileTable{.names = files, .hashes = loaded.file_hashes},
        .addr_names = addr_names,
        .summary = summary,
        .prelude_functions = loaded.prelude_functions,
        .width = loaded.width,
        .compiler_version = loaded.compiler_version,
    };
  }
};

// Restores the trailing checksum after a case patches the body, so the
// structural error under test is the one reported rather than the
// checksum mismatch the same patch also caused.
void reseal(std::vector<u8>& bytes) {
  const base::Digest128 hash =
      base::hash128(std::span<const u8>(bytes.data(), bytes.size() - 16));
  const std::array<u8, 16> raw = hash.bytes();
  for (u32 i = 0; i < 16; ++i) {
    bytes[bytes.size() - 16 + i] = raw[i];
  }
}

}  // namespace

TEST_CASE("A serialized package reads back as the same bytes") {
  ir::SymbolTable strings;
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

TEST_CASE("A const argument survives the binary form") {
  StorageBuilder builder;
  ir::SymbolTable strings{16};
  const TypeIdx i32 = builder.primitive(TypeTag::I32);
  const TypeIdx four = builder.const_type(4);
  const TypeIdx eight = builder.const_type(8);

  TypeSeq params;
  params.push(builder.ref_type(i32));
  params.push(builder.ref_type(four));
  params.push(builder.ref_type(eight));
  (void)builder.struct_type(strings.intern("ArrayVec"), TypeIdxRange{},
                            params.finish());

  const Storage storage = std::move(builder).build().unwrap().unwrap();
  const WriteInput input{
      .storage = &storage,
      .strings = &strings,
      .instr_spans = {},
      .files = {},
      .addr_names = {},
      .summary = {},
      .prelude_functions = 0,
      .width = PointerWidth::W64,
      .compiler_version = "0.0.0-test",
  };
  const std::vector<u8> bytes = serialize(input);

  base::Result<LoadedIr, IrLoadError> loaded = deserialize(bytes);
  CHECK(loaded.is_ok());
  if (loaded.is_err()) {
    return;
  }
  LoadedIr package = std::move(loaded).unwrap();
  const Storage& read = *package.storage;
  // Both values came back, and stayed apart: one row per distinct
  // argument, in the order they were interned.
  CHECK(read.const_types().size() == 2);
  if (read.const_types().size() == 2) {
    CHECK(read.const_types()[ConstTypeIdx(0)].value == 4);
    CHECK(read.const_types()[ConstTypeIdx(1)].value == 8);
  }

  LoadedInput reinput(package);
  CHECK(serialize(reinput.input) == bytes);
}

TEST_CASE("A corrupt binary form is refused") {
  ir::SymbolTable strings;
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
    broken[4] = 3;  // A major version this reader does not know.
    reseal(broken);
    CHECK(std::move(deserialize(broken)).unwrap_err() ==
          IrLoadError::UnsupportedVersion);
  }
  {
    std::vector<u8> broken = bytes;
    broken[8] = 0;  // Little-endian flag cleared.
    reseal(broken);
    CHECK(std::move(deserialize(broken)).unwrap_err() == IrLoadError::BadFlags);
  }
  {
    // The second section starts where the first does.
    std::vector<u8> broken = bytes;
    const usize version_len = test::read_u32(file, 16);
    const usize table_at = 20 + version_len + 8;
    const u64 first_offset = test::read_u64(file, table_at + 8);
    for (u32 i = 0; i < 8; ++i) {
      broken[table_at + 24 + 8 + i] = static_cast<u8>(first_offset >> (8 * i));
    }
    reseal(broken);
    CHECK(std::move(deserialize(broken)).unwrap_err() ==
          IrLoadError::BadSection);
  }
  {
    // A span count that is neither zero nor the instruction count.
    std::vector<u8> broken = bytes;
    const usize spans_at = test::section_at(file, binary::Section::Spans);
    broken[spans_at] = 3;
    reseal(broken);
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
    reseal(broken);
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
    reseal(broken);
    CHECK(std::move(deserialize(broken)).unwrap_err() == IrLoadError::BadShape);
  }
  {
    // An operand tag the format does not define.
    std::vector<u8> broken = bytes;
    const usize operands_at = test::section_at(file, binary::Section::Operands);
    broken[operands_at + 4] = 0xEE;
    reseal(broken);
    CHECK(std::move(deserialize(broken)).unwrap_err() == IrLoadError::BadShape);
  }
  {
    // One byte inside a section payload: every structural reader accepts
    // it, and only the footer says the file is not the one written.
    // Without the check this loaded with a corrupted name.
    std::vector<u8> broken = bytes;
    const usize strings_at = test::section_at(file, binary::Section::Strings);
    broken[strings_at + 8] ^= 0x01;
    CHECK(std::move(deserialize(broken)).unwrap_err() ==
          IrLoadError::BadChecksum);
  }
}

TEST_CASE("The FILES section keeps the content hashes it was given") {
  ir::SymbolTable strings;
  const Storage storage = test::composite_storage(strings);
  const std::array<base::Digest128, 1> hashes{
      base::Digest128{0x0123'4567'89ab'cdefull, 0x0fed'cba9'8765'4321ull}};
  const std::vector<u8> bytes = test::composite_bytes(strings, storage, hashes);
  base::Result<LoadedIr, IrLoadError> loaded = deserialize(bytes);
  CHECK(loaded.is_ok());
  if (!loaded.is_ok()) {
    return;
  }
  const std::vector<base::Digest128>& written = loaded.value().file_hashes;
  CHECK(written.size() == 1);
  if (written.size() == 1) {
    // The writer echoes what it is handed; computing the hash of a
    // source's bytes is the caller's business.
    CHECK(written[0] == hashes[0]);
  }
}

TEST_CASE("The reader hands back the identity the writer computed") {
  ir::SymbolTable strings;
  const Storage storage = test::composite_storage(strings);
  const std::array<base::Digest128, 1> hashes{
      base::Digest128{0x0123'4567'89ab'cdefull, 0x0fed'cba9'8765'4321ull}};
  const std::vector<u8> bytes = test::composite_bytes(strings, storage, hashes);
  base::Result<LoadedIr, IrLoadError> loaded = deserialize(bytes);
  CHECK(loaded.is_ok());
  if (!loaded.is_ok()) {
    return;
  }
  // The digest is over the same tables the reader just rebuilt, so a
  // cache compares one number instead of walking them.
  const std::array<std::string_view, 1> names{"main.al"};
  CHECK(loaded.value().identity ==
        identity_digest(FileTable{.names = names, .hashes = hashes},
                        PointerWidth::W64, "0.0.0-test"));
}

TEST_CASE("A summary section reads back the package's surface") {
  ir::SymbolTable strings;
  const Storage storage = test::composite_storage(strings);
  const std::array<SummaryEntry, 3> summary{
      SummaryEntry{FunctionIdx(0), strings.intern("main"), strings.intern(""),
                   SummaryKind::Free,
                   /*needs_body=*/false},
      SummaryEntry{FunctionIdx(base::INVALID_IDX), strings.intern("make"),
                   strings.intern("string"), SummaryKind::Free,
                   /*needs_body=*/true},
      SummaryEntry{FunctionIdx(3), strings.intern("String"),
                   strings.intern("string"), SummaryKind::Struct,
                   /*needs_body=*/false},
  };
  const std::vector<u8> bytes =
      test::composite_bytes(strings, storage, /*hashes=*/{}, summary);
  const std::span<const u8> file(bytes);

  // The section is written after the identity, where the kinds order
  // puts it.
  const usize version_len = test::read_u32(file, 16);
  const usize count = test::read_u64(file, 20 + version_len);
  const usize last_at = 20 + version_len + 8 + (count - 1) * 24;
  CHECK(test::read_u32(file, last_at) ==
        static_cast<u32>(binary::Section::Summary));

  base::Result<LoadedIr, IrLoadError> loaded = deserialize(bytes);
  CHECK(loaded.is_ok());
  if (loaded.is_err()) {
    return;
  }
  LoadedIr package = std::move(loaded).unwrap();
  CHECK(package.summary.size() == 3);
  if (package.summary.size() != 3) {
    return;
  }
  CHECK(package.summary[0].function.idx == 0);
  CHECK(package.strings->get(package.summary[0].name) == "main");
  CHECK(package.strings->get(package.summary[0].path).empty());
  CHECK(package.summary[0].kind == SummaryKind::Free);
  CHECK_FALSE(package.summary[0].needs_body);
  CHECK_FALSE(package.summary[1].function.is_valid());
  CHECK(package.strings->get(package.summary[1].name) == "make");
  CHECK(package.strings->get(package.summary[1].path) == "string");
  CHECK(package.summary[1].kind == SummaryKind::Free);
  CHECK(package.summary[1].needs_body);
  // A nominal row names a type rather than code.
  CHECK(package.summary[2].function.idx == 3);
  CHECK(package.strings->get(package.summary[2].name) == "String");
  CHECK(package.summary[2].kind == SummaryKind::Struct);
  CHECK_FALSE(package.summary[2].needs_body);

  // Reading and writing again produces the bytes it read, summary and
  // all: a dependent that reads and forwards keeps the surface.
  LoadedInput input(package);
  CHECK(serialize(input.input) == bytes);
}

TEST_CASE("A corrupt summary is refused") {
  ir::SymbolTable strings;
  const Storage storage = test::composite_storage(strings);
  const std::array<SummaryEntry, 1> summary{
      SummaryEntry{FunctionIdx(0), strings.intern("main"), strings.intern(""),
                   SummaryKind::Free,
                   /*needs_body=*/false},
  };
  const std::vector<u8> bytes =
      test::composite_bytes(strings, storage, /*hashes=*/{}, summary);
  const std::span<const u8> file(bytes);
  {
    // A function index no table row answers.
    std::vector<u8> broken = bytes;
    const usize summary_at = test::section_at(file, binary::Section::Summary);
    broken[summary_at + 4] = 9;
    reseal(broken);
    CHECK(std::move(deserialize(broken)).unwrap_err() == IrLoadError::BadShape);
  }
  {
    // A symbol kind outside the enum.
    std::vector<u8> broken = bytes;
    const usize summary_at = test::section_at(file, binary::Section::Summary);
    broken[summary_at + 16] = 99;
    reseal(broken);
    CHECK(std::move(deserialize(broken)).unwrap_err() == IrLoadError::BadShape);
  }
}

}  // namespace ir
