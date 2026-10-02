// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lowering/lowering.h"

#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "codegen_llvm/common.h"
#include "codegen_llvm/llvm_ir_emitter.h"
#include "codegen_llvm/llvm_object_emitter.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "fpag/mem/page_allocator.h"
#include "fpag/str/string_interner.h"
#include "i18n/language.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/verifier.h"
#include "source/source.h"
#include "tests/util/virtual_source.h"

namespace lowering {

namespace {

struct Fixture {
  mem::Arena arena;
  ast::AstArena ast;
  diag::DiagBag bag{arena, i18n::Language::EnUs};
  source::SourceManager sources;
  str::StringInterner strings{mem::page_size()};

  Fixture() { arena.reserve(1u << 20); }
};

// The sources one case declared, held in memory. It stands in for a
// scratch directory and keeps the `dir` name so the cases below read the
// way they were written.
using VirtualDir = tests::DeclaredSources;

// Records the sources rather than writing them, so a case cannot fail for
// a reason other than what it asserts. Always succeeds, which keeps the
// call sites' guard meaningful to read.
bool write_all(
    VirtualDir& dir,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        files) {
  for (const auto& [name, bytes] : files) {
    dir.add(name, bytes);
  }
  return true;
}

struct LowerCase {
  std::optional<LoweredPackage> lowered;
  bool ok;
};

LowerCase lower_case(
    VirtualDir& dir,
    std::string_view root_rel,
    std::initializer_list<std::string_view> rels,
    Fixture& f,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        prelude = {}) {
  std::vector<analyzer::ModuleInput> inputs;
  source::FileId root = source::UNKNOWN_FILE;
  std::deque<std::string> name_storage;
  for (std::string_view rel : rels) {
    std::optional<analyzer::ModuleInput> input = tests::register_source(
        f.sources, dir, rel, rel == root_rel, name_storage);
    if (!input.has_value()) {
      continue;
    }
    if (rel == root_rel) {
      root = input->id;
    }
    inputs.push_back(*input);
  }
  std::deque<std::string> prelude_storage;
  std::vector<analyzer::ModuleInput> prelude_inputs;
  for (const auto& [name, rel] : prelude) {
    const tests::VirtualSource* const file = dir.find(rel);
    if (file == nullptr) {
      continue;
    }
    prelude_storage.emplace_back(name);
    // A staged prelude source is a package facade, so its public
    // surface is in scope without a `use`.
    prelude_inputs.push_back({prelude_storage.back(),
                              f.sources.add_virtual(file->name, file->bytes),
                              true});
  }
  base::Result<analyzer::ModuleTree, diag::Reported> tree_result =
      analyzer::resolve_modules(root, inputs, "testpkg", f.sources, f.ast,
                                f.bag, prelude_inputs);
  if (tree_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt, false};
  }
  analyzer::ModuleTree tree = std::move(tree_result).unwrap();
  base::Result<analyzer::CheckedPackage, diag::Reported> checked_result =
      analyzer::check_package(tree, ir::PointerWidth::W64, f.ast, f.bag,
                              f.strings);
  if (checked_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt, false};
  }
  analyzer::CheckedPackage checked = std::move(checked_result).unwrap();
  base::Result<LoweredPackage, diag::Reported> lowered_result = lower_package(
      std::move(checked), ir::PointerWidth::W64, f.ast, f.strings, f.bag);
  if (lowered_result.is_err()) {
    return {std::nullopt, false};
  }
  return {std::move(lowered_result).unwrap(), !f.bag.has_errors()};
}

}  // namespace

TEST_CASE("Lower straight-line arithmetic") {
  VirtualDir dir;
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
  CHECK(result.lowered->storage->functions().size() == 2);
  CHECK(ir::verify_storage(*result.lowered->storage).is_ok());
}

TEST_CASE("Lower structs tuples fields and borrows") {
  VirtualDir dir;
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
  CHECK(ir::verify_storage(*result.lowered->storage).is_ok());
}

TEST_CASE("Lower lowers control flow to verifiable blocks") {
  VirtualDir dir;
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
  CHECK(ir::verify_storage(*result.lowered->storage).is_ok());
}

TEST_CASE("Lower lowers enums matches and question propagation") {
  VirtualDir dir;
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
                       "}\n"
                       "enum Option<T> { Some(T), None }\n"
                       "impl<T> Option<T> {\n"
                       "  fn unwrap(self: Self) -> T {\n"
                       "    ret match self {\n"
                       "      Option::Some(v) => v,\n"
                       "      Option::None => panic(\"unreachable\"),\n"
                       "    }\n"
                       "  }\n"
                       "}\n"
                       "intrinsic fn panic(msg: str) -> !;\n"}});
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
  CHECK(ir::verify_storage(*result.lowered->storage).is_ok());
}

TEST_CASE("Lower emits verifiable LLVM IR") {
  VirtualDir dir;
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
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, ir::PointerWidth::W64, true);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));
}

TEST_CASE("Lower emits verifiable LLVM IR for print") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "fn main() {\n"
                       "  print(\"hi\")\n"
                       "}\n"},
                      {"core.al", "pub intrinsic fn print(msg: str);\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result =
      lower_case(dir, "main.al", {"main.al"}, f, {{"core", "core.al"}});
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("lower_emit_print_test", context);
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, ir::PointerWidth::W64, true);
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
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "fn add(a: i32, b: i32) -> i32 {\n"
                       "  ret a + b\n"
                       "}\n"
                       "fn main() {\n"
                       "  print(\"hi\")\n"
                       "  _ := add(40, 2)\n"
                       "}\n"},
                      {"core.al", "pub intrinsic fn print(msg: str);\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result =
      lower_case(dir, "main.al", {"main.al"}, f, {{"core", "core.al"}});
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("lower_emit_object_test", context);
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, ir::PointerWidth::W64, true);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));

  base::Result<std::vector<u8>, codegen_llvm::ObjectEmitError> emitted =
      codegen_llvm::emit_object(*module, "");
  CHECK(emitted.is_ok());
  if (emitted.is_err()) {
    return;
  }
  std::vector<u8> object = std::move(emitted).unwrap();
  CHECK(object.size() > 0);

  // Emission is target-independent, so non-host triples are covered here.
  const std::pair<std::string_view, std::string_view> triples[] = {
      {"x86_64-unknown-linux-gnu", "main_x64.o"},
      {"aarch64-unknown-linux-gnu", "main_a64.o"},
      {"riscv64-unknown-linux-gnu", "main_r64.o"},
  };
  for (const auto& [triple, name] : triples) {
    base::Result<std::vector<u8>, codegen_llvm::ObjectEmitError>
        triple_emitted = codegen_llvm::emit_object(*module, triple);
    CHECK(triple_emitted.is_ok());
    if (triple_emitted.is_err()) {
      continue;
    }
    const std::vector<u8>& buf = std::move(triple_emitted).unwrap();
    CHECK(buf.size() > 0);
  }
}

TEST_CASE("Optimization promotes stack allocas") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  x := 40\n"
                                      "  ret x + 2\n"
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
      std::make_unique<llvm::Module>("lower_optimize_test", context);
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, ir::PointerWidth::W64, true);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));

  // The emitter spills every local; mem2reg in the O3 pipeline is what
  // promotes this one, so its absence says the pipeline ran.
  base::Result<void, codegen_llvm::ObjectEmitError> optimized =
      codegen_llvm::optimize_module(*module, "");
  CHECK(optimized.is_ok());
  CHECK(!llvm::verifyModule(*module));
  CHECK(codegen_llvm::emit_ir(*module).find("alloca") == std::string::npos);

  // Unknown triples fail the same way emission does.
  CHECK(codegen_llvm::optimize_module(*module, "no-such-triple").is_err());
}
#endif

TEST_CASE("Lower wraps all main forms in a C entry") {
  const std::string_view cases[] = {
      "fn main() {\n"
      "  print(\"hi\")\n"
      "}\n",
      "fn main() -> i32 {\n"
      "  ret 3\n"
      "}\n",
      "enum Result<T, E> { Ok(T), Err(E) }\n"
      "fn main() -> Result<(), i32> {\n"
      "  ret Result::Ok(if true {\n"
      "  } else {\n"
      "  })\n"
      "}\n",
  };
  for (const std::string_view source : cases) {
    VirtualDir dir;
    const bool setup =
        write_all(dir, {{"main.al", source},
                        {"core.al", "pub intrinsic fn print(msg: str);\n"}});
    CHECK(setup);
    if (!setup) {
      continue;
    }

    Fixture f;
    LowerCase result =
        lower_case(dir, "main.al", {"main.al"}, f, {{"core", "core.al"}});
    CHECK(result.ok);
    CHECK(result.lowered.has_value());
    if (!result.ok || !result.lowered.has_value()) {
      continue;
    }
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module =
        std::make_unique<llvm::Module>("lower_entry_test", context);
    codegen_llvm::LlvmIrEmitter emitter(
        module.get(), std::move(result.lowered->storage), &f.strings,
        ir::PointerWidth::W64, true);
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  for (const ir::Instruction& instr : result.lowered->storage->instrs()) {
    CHECK(instr.op != ir::Opcode::Drop);
  }
}

TEST_CASE("Lower emits verifiable LLVM IR for control flow") {
  VirtualDir dir;
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
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, ir::PointerWidth::W64, true);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));
}

TEST_CASE("Lower emits verifiable LLVM IR for enums and calls") {
  VirtualDir dir;
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
                       "}\n"
                       "enum Option<T> { Some(T), None }\n"
                       "impl<T> Option<T> {\n"
                       "  fn unwrap(self: Self) -> T {\n"
                       "    ret match self {\n"
                       "      Option::Some(v) => v,\n"
                       "      Option::None => panic(\"unreachable\"),\n"
                       "    }\n"
                       "  }\n"
                       "}\n"
                       "intrinsic fn panic(msg: str) -> !;\n"}});
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
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, ir::PointerWidth::W64, true);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));
}

// A `match` or `if` whose every arm ends the path produces no value, so
// there is no join and no arm ever stored into the result slot. Reading
// that slot anyway emitted an instruction after the `ret` or
// `Unreachable` the last arm left behind, which is two terminators in one
// block and fails verification.
TEST_CASE("Lower answers an all-terminated match or if with never") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "enum Opt {\n"
                                      "  Some(i32),\n"
                                      "  None,\n"
                                      "}\n"
                                      "fn classify(o: Opt) -> i32 {\n"
                                      "  match o {\n"
                                      "    Opt::Some(v) => { ret v * 2 }\n"
                                      "    Opt::None => { ret 0 - 1 }\n"
                                      "  }\n"
                                      "}\n"
                                      "fn pick(x: i32) -> i32 {\n"
                                      "  if x > 0 {\n"
                                      "    ret 1\n"
                                      "  } else {\n"
                                      "    ret 0\n"
                                      "  }\n"
                                      "}\n"
                                      "fn pick_let(o: Opt) -> i32 {\n"
                                      "  if Opt::Some(v) := o {\n"
                                      "    ret v\n"
                                      "  } else {\n"
                                      "    ret 0\n"
                                      "  }\n"
                                      "}\n"
                                      "fn mixed(o: Opt) -> i32 {\n"
                                      "  match o {\n"
                                      "    Opt::Some(v) => { ret v }\n"
                                      "    Opt::None => 0\n"
                                      "  }\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  _ := classify(Opt::Some(2))\n"
                                      "  _ := pick(1)\n"
                                      "  _ := pick_let(Opt::Some(3))\n"
                                      "  _ := mixed(Opt::Some(4))\n"
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
  CHECK(ir::verify_storage(*result.lowered->storage).is_ok());
}

}  // namespace lowering
