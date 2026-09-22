// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lower/lower.h"

#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "codegen_llvm/common.h"
#include "codegen_llvm/llvm_ir_emitter.h"
#include "codegen_llvm/llvm_object_emitter.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/file_handle.h"
#include "fpag/io/temp_dir.h"
#include "fpag/mem/arena.h"
#include "fpag/mem/page_allocator.h"
#include "fpag/str/string_interner.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/verifier.h"
#include "source/source.h"

namespace lower {

namespace {

struct Fixture {
  mem::Arena arena;
  diag::DiagBag bag{arena};
  source::SourceManager sources;
  str::StringInterner strings{mem::page_size()};

  Fixture() { arena.reserve(1u << 20); }
};

bool write_all(
    io::TempDir& dir,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        files) {
  for (const auto& [rel, content] : files) {
    if (!dir.write_file(rel, content)) {
      return false;
    }
  }
  return true;
}

struct LowerCase {
  std::optional<LoweredPackage> lowered;
  bool ok;
};

LowerCase lower_case(io::TempDir& dir,
                     std::string_view root_rel,
                     std::initializer_list<std::string_view> rels,
                     Fixture& f) {
  std::vector<analyzer::ModuleInput> inputs;
  source::FileId root = source::kUnknownFile;
  for (std::string_view rel : rels) {
    base::Result<source::FileId, source::SourceError> loaded =
        f.sources.load(dir.join(rel));
    if (loaded.is_err()) {
      continue;
    }
    const source::FileId id = std::move(loaded).unwrap();
    if (rel == root_rel) {
      root = id;
      inputs.push_back({"", id});
    } else {
      std::string_view name = rel;
      constexpr std::string_view suffix = ".al";
      if (name.size() > suffix.size() &&
          name.substr(name.size() - suffix.size()) == suffix) {
        name.remove_suffix(suffix.size());
      }
      inputs.push_back({name, id});
    }
  }
  diag::Fallible<analyzer::ModuleTree> tree_result = analyzer::resolve_modules(
      root, inputs, "testpkg", f.sources, f.arena, f.bag);
  if (tree_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt, false};
  }
  analyzer::ModuleTree tree = std::move(tree_result).unwrap();
  diag::Fallible<analyzer::CheckedPackage> checked_result =
      analyzer::check_package(tree, ir::PointerWidth::W64, f.bag);
  if (checked_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt, false};
  }
  analyzer::CheckedPackage checked = std::move(checked_result).unwrap();
  diag::Fallible<LoweredPackage> lowered_result = lower_package(
      std::move(checked), ir::PointerWidth::W64, f.strings, f.bag);
  if (lowered_result.is_err()) {
    return {std::nullopt, false};
  }
  return {std::move(lowered_result).unwrap(), !f.bag.has_errors()};
}

}  // namespace

TEST_CASE("Lower straight-line arithmetic") {
  io::TempDir dir("alcy_lower_arith_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn add(a: i32, b: i32) -> i32 {\n"
                                      "  ret a + b * 2\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  _ := add(1, 2)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  CHECK(result.lowered->storage.functions().size() == 2);
  CHECK(ir::verify_storage(result.lowered->storage).is_ok());
}

TEST_CASE("Lower structs tuples fields and borrows") {
  io::TempDir dir("alcy_lower_aggregate_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Point { x: i32, y: i32 }\n"
                                      "fn get(p: &Point) -> i32 {\n"
                                      "  ret p.x + p.y\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  p := Point { x: 1, y: 2 }\n"
                                      "  t := (p.x, true)\n"
                                      "  _ := get(&p) + t.0\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  CHECK(ir::verify_storage(result.lowered->storage).is_ok());
}

TEST_CASE("Lower lowers control flow to verifiable blocks") {
  io::TempDir dir("alcy_lower_control_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn f(b: bool) -> i32 {\n"
                                      "  r := match b {\n"
                                      "    true => 1,\n"
                                      "    false => 0,\n"
                                      "  }\n"
                                      "  mut i := 0\n"
                                      "  while i < r {\n"
                                      "    i = i + 1\n"
                                      "  }\n"
                                      "  loop {\n"
                                      "    if i <= 0 {\n"
                                      "      break\n"
                                      "    }\n"
                                      "    i = i - 1\n"
                                      "  }\n"
                                      "  ret i\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  CHECK(ir::verify_storage(result.lowered->storage).is_ok());
}

TEST_CASE("Lower lowers enums matches and question propagation") {
  io::TempDir dir("alcy_lower_enum_test");
  const bool setup =
      write_all(dir, {{"main.al",
                       "enum Shape { Circle(i32), Rect }\n"
                       "fn area(s: Shape) -> i32 {\n"
                       "  r := match s {\n"
                       "    Shape::Circle(x) => x,\n"
                       "    Shape::Rect => 0,\n"
                       "  }\n"
                       "  ret r\n"
                       "}\n"
                       "fn calc(o: Option<i32>) -> Option<i32> {\n"
                       "  v := o?\n"
                       "  ret Some(v + 1)\n"
                       "}\n"
                       "fn main() {\n"
                       "  a := area(Shape::Circle(3))\n"
                       "  o: Option<i32> := Some(7)\n"
                       "  y := o.unwrap()\n"
                       "  _ := a\n"
                       "  _ := y\n"
                       "  _ := calc(o)\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  CHECK(ir::verify_storage(result.lowered->storage).is_ok());
}

TEST_CASE("Lower emits verifiable LLVM IR") {
  io::TempDir dir("alcy_lower_emit_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn add(a: i32, b: i32) -> i32 {\n"
                                      "  ret a + b\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  _ := add(40, 2)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("lower_emit_test", context);
  codegen_llvm::LlvmIrEmitter emitter(
      module.get(), std::move(result.lowered->storage), &f.strings);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));
}

TEST_CASE("Lower emits verifiable LLVM IR for print") {
  io::TempDir dir("alcy_lower_emit_print_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  print(\"hi\")\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("lower_emit_print_test", context);
  codegen_llvm::LlvmIrEmitter emitter(
      module.get(), std::move(result.lowered->storage), &f.strings);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));

  std::string ir_str;
  llvm::raw_string_ostream os(ir_str);
  module->print(os, nullptr);

  // String globals carry an explicit NUL terminator for the runtime.
  CHECK(ir_str.find("alcy_print") != std::string::npos);
  CHECK(ir_str.find("c\"hi\\00\"") != std::string::npos);
}

#if !defined(OS_ASMJS)
TEST_CASE("Lower emits relocatable objects") {
  io::TempDir dir("alcy_lower_emit_object_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn add(a: i32, b: i32) -> i32 {\n"
                                      "  ret a + b\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  print(\"hi\")\n"
                                      "  _ := add(40, 2)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("lower_emit_object_test", context);
  codegen_llvm::LlvmIrEmitter emitter(
      module.get(), std::move(result.lowered->storage), &f.strings);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));

  const std::string object_path = dir.join("main.o");
  base::Result<void, codegen_llvm::ObjectEmitError> emitted =
      codegen_llvm::emit_object(*module, "", object_path);
  CHECK(emitted.is_ok());
  if (emitted.is_err()) {
    return;
  }
  io::FileHandle object;
  CHECK(object.open(object_path, io::FileAccess::Read));
  // Non-empty relocatable output.
  CHECK(object.get_size() > 0);

  // Every linked backend emits independently of the host: any alcy
  // binary produces objects for any supported architecture.
  const std::pair<std::string_view, std::string_view> triples[] = {
      {"x86_64-unknown-linux-gnu", "main_x64.o"},
      {"aarch64-unknown-linux-gnu", "main_a64.o"},
      {"riscv64-unknown-linux-gnu", "main_r64.o"},
  };
  for (const auto& [triple, name] : triples) {
    const std::string path = dir.join(name);
    base::Result<void, codegen_llvm::ObjectEmitError> triple_emitted =
        codegen_llvm::emit_object(*module, triple, path);
    CHECK(triple_emitted.is_ok());
    if (triple_emitted.is_err()) {
      continue;
    }
    io::FileHandle triple_object;
    CHECK(triple_object.open(path, io::FileAccess::Read));
    CHECK(triple_object.get_size() > 0);
  }
}
#endif

TEST_CASE("Lower wraps all main forms in a C entry") {
  const std::pair<std::string_view, std::string_view> cases[] = {
      {"alcy_entry_void_test",
       "fn main() {\n"
       "  print(\"hi\")\n"
       "}\n"},
      {"alcy_entry_i32_test",
       "fn main() -> i32 {\n"
       "  ret 3\n"
       "}\n"},
      {"alcy_entry_result_test",
       "fn main() -> Result<(), i32> {\n"
       "  ret Ok(if true {\n"
       "  } else {\n"
       "  })\n"
       "}\n"},
  };
  for (const auto& [name, source] : cases) {
    io::TempDir dir(name);
    const bool setup = write_all(dir, {{"main.al", source}});
    CHECK(setup);
    if (!setup) {
      continue;
    }

    Fixture f;
    LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
    CHECK(result.ok);
    CHECK(result.lowered.has_value());
    if (!result.ok || !result.lowered.has_value()) {
      continue;
    }
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module =
        std::make_unique<llvm::Module>("lower_entry_test", context);
    codegen_llvm::LlvmIrEmitter emitter(
        module.get(), std::move(result.lowered->storage), &f.strings);
    std::move(emitter).emit();
    CHECK(!llvm::verifyModule(*module));

    std::string ir_str;
    llvm::raw_string_ostream os(ir_str);
    module->print(os, nullptr);

    // The user entry is renamed; a C-ABI `main` adapts its return.
    CHECK(ir_str.find("alcy_main") != std::string::npos);
    CHECK(ir_str.find("define i32 @main()") != std::string::npos);
  }
}

TEST_CASE("Lower warns on unreachable statements") {
  io::TempDir dir("alcy_lower_unreachable_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  ret\n"
                                      "  _ := 2\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(f.bag.warning_count() > 0);
}

TEST_CASE("Lowering emits no Drop markers") {
  io::TempDir dir("alcy_lower_no_drop_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct H { r: &mut i32 }\n"
                                      "fn main() {\n"
                                      "  mut x := 1\n"
                                      "  h := H { r: &mut x }\n"
                                      "  mut i := 0\n"
                                      "  while i < 2 {\n"
                                      "    g := h\n"
                                      "    _ := g\n"
                                      "    i = i + 1\n"
                                      "  }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  // Drop stays a no-op ruling: destruction needs no markers.
  for (const ir::Instruction& instr : result.lowered->storage.instrs()) {
    CHECK(instr.op != ir::Opcode::Drop);
  }
}

TEST_CASE("Lower emits verifiable LLVM IR for control flow") {
  io::TempDir dir("alcy_lower_emit_control_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn f(b: bool) -> i32 {\n"
                                      "  r := match b {\n"
                                      "    true => 1,\n"
                                      "    false => 0,\n"
                                      "  }\n"
                                      "  mut i := 0\n"
                                      "  while i < r {\n"
                                      "    i = i + 1\n"
                                      "  }\n"
                                      "  loop {\n"
                                      "    if i <= 0 {\n"
                                      "      break\n"
                                      "    }\n"
                                      "    i = i - 1\n"
                                      "  }\n"
                                      "  ret i\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("lower_emit_control_test", context);
  codegen_llvm::LlvmIrEmitter emitter(
      module.get(), std::move(result.lowered->storage), &f.strings);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));
}

TEST_CASE("Lower emits verifiable LLVM IR for enums and calls") {
  io::TempDir dir("alcy_lower_emit_enum_test");
  const bool setup =
      write_all(dir, {{"main.al",
                       "enum Shape { Circle(i32), Rect }\n"
                       "fn area(s: Shape) -> i32 {\n"
                       "  r := match s {\n"
                       "    Shape::Circle(x) => x,\n"
                       "    Shape::Rect => 0,\n"
                       "  }\n"
                       "  ret r\n"
                       "}\n"
                       "fn calc(o: Option<i32>) -> Option<i32> {\n"
                       "  v := o?\n"
                       "  ret Some(v + 1)\n"
                       "}\n"
                       "fn main() {\n"
                       "  a := area(Shape::Circle(3))\n"
                       "  o: Option<i32> := Some(7)\n"
                       "  y := o.unwrap()\n"
                       "  _ := a\n"
                       "  _ := y\n"
                       "  _ := calc(o)\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("lower_emit_enum_test", context);
  codegen_llvm::LlvmIrEmitter emitter(
      module.get(), std::move(result.lowered->storage), &f.strings);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));
}

}  // namespace lower
