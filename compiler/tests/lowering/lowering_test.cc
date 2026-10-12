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

#include "codegen_llvm/common.h"
#include "codegen_llvm/llvm_backend.h"
#include "codegen_llvm/llvm_ir_emitter.h"
#include "codegen_llvm/target.h"
#include "config/build_config.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profiler.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/verifier.h"
#include "lowering/diag_code.h"
#include "tests/lowering/lowering_fixture.h"

namespace lowering {

namespace {

// The host machine, which is what these cases emit for; the emitter reads
// the module's layout, so the module is configured the way the pipeline
// configures it.
codegen_llvm::Target host_target() {
  return codegen_llvm::Target{codegen_llvm::host_triple(),
                              ir::PointerWidth::W64};
}

}  // namespace

TEST_CASE("Lower evaluates a const item initializer") {
  VirtualDir dir;
  write_all(dir, {{"main.al",
                   "const N: i32 = 1 + 2\n"
                   "const M: i32 = N * 2\n"
                   "\n"
                   "fn main() -> i32 {\n"
                   "  ret M\n"
                   "}\n"}});
  Fixture f;
  const LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.lowered.has_value());
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Lower evaluates a comp fn through a const item") {
  VirtualDir dir;
  write_all(dir, {{"main.al",
                   "comp fn twice(x: i32) -> i32 {\n"
                   "  ret x * 2\n"
                   "}\n"
                   "const N: i32 = twice(21)\n"
                   "\n"
                   "fn main() -> i32 {\n"
                   "  ret N - 42\n"
                   "}\n"}});
  Fixture f;
  const LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.lowered.has_value());
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  // The IR engine answered the call; lowering's differential pass only
  // sees AST-answered calls, so this is where the switch shows.
  CHECK(result.lowered->comp_engine_answered >= 1);
}

TEST_CASE("Lower compares the checker's comp call against the IR engine") {
  VirtualDir dir;
  write_all(dir, {{"main.al",
                   "comp fn twice(x: i32) -> i32 {\n"
                   "  ret x * 2\n"
                   "}\n"
                   "const N: i32 = twice(2)\n"
                   "\n"
                   "fn main() -> i32 {\n"
                   "  a: [i32; N] := [1, 2, 3, 4]\n"
                   "  ret a[0]\n"
                   "}\n"}});
  Fixture f;
  const LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  // The array length forced the checker's evaluator to answer; the
  // comparison pass re-ran that call through the IR engine after
  // lowering built the storage, and the lowering itself did not
  // evaluate it again.
  CHECK(result.lowered->comp_engine_compared >= 1);
  CHECK(result.lowered->comp_engine_answered == 0);
}

TEST_CASE("Lower reports a const cycle instead of recursing") {
  VirtualDir dir;
  write_all(dir, {{"main.al",
                   "const A: i32 = B\n"
                   "const B: i32 = A\n"
                   "\n"
                   "fn main() -> i32 {\n"
                   "  ret A\n"
                   "}\n"}});
  Fixture f;
  const LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Lower straight-line arithmetic") {
  VirtualDir dir;
  write_all(dir, {{"main.al",
                   "fn add(a: i32, b: i32) -> i32 {\n"
                   "  ret a + b * 2\n"
                   "}\n"
                   "fn main() {\n"
                   "  _ := add(1, 2)\n"
                   "}\n"}});

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
  write_all(dir, {{"main.al",
                   "struct Point { x: i32, y: i32 }\n"
                   "fn get(p: &Point) -> i32 {\n"
                   "  ret p.x + p.y\n"
                   "}\n"
                   "fn main() {\n"
                   "  p := Point { x: 1, y: 2 }\n"
                   "  t := (p.x, true)\n"
                   "  _ := get(&p) + t.0\n"
                   "}\n"}});

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
  write_all(dir, {{"main.al",
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
  write_all(dir, {{"main.al",
                   "fn add(a: i32, b: i32) -> i32 {\n"
                   "  ret a + b\n"
                   "}\n"
                   "fn main() {\n"
                   "  _ := add(40, 2)\n"
                   "}\n"}});

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
  CHECK(codegen_llvm::configure_target(*module, host_target()).is_ok());
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, host_target(), true);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));
}

TEST_CASE("Lowered panic names the site it was written at") {
  VirtualDir dir;
  write_all(dir, {{"main.al",
                   "fn main() {\n"
                   "  panic(\"boom\")\n"
                   "}\n"
                   "intrinsic fn panic(msg: str) -> !;\n"}});

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("lower_panic_site_test", context);
  CHECK(codegen_llvm::configure_target(*module, host_target()).is_ok());
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, host_target(), true);
  std::move(emitter).emit();
  const std::string text = codegen_llvm::emit_ir(*module);
  CHECK(text.find("main.al:2:3") != std::string::npos);
  CHECK(text.find("boom") != std::string::npos);
}

TEST_CASE("Lower emits verifiable LLVM IR for print") {
  VirtualDir dir;
  write_all(dir, {{"main.al",
                   "fn main() {\n"
                   "  print(\"hi\")\n"
                   "}\n"},
                  {"io.al",
                   "pub intrinsic fn panic(msg: str) -> !;\n"
                   "\n"
                   "pub fn print(comp fmt: str, args: ..) {\n"
                   "  panic(\"must expand\")\n"
                   "}\n"}});

  Fixture f;
  LowerCase result =
      lower_case(dir, "main.al", {"main.al"}, f, {{"io", "io.al"}});
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("lower_emit_print_test", context);
  CHECK(codegen_llvm::configure_target(*module, host_target()).is_ok());
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, host_target(), true);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));

  std::string ir_str;
  llvm::raw_string_ostream os(ir_str);
  module->print(os, nullptr);

  // String globals carry an explicit NUL terminator for the runtime.
  CHECK(ir_str.find("alcy_print") != std::string::npos);
  CHECK(ir_str.find("c\"hi\\00\"") != std::string::npos);
}

#if !BUILD_FLAG(IS_OS_ASMJS)
TEST_CASE("Lower emits relocatable objects") {
  VirtualDir dir;
  write_all(dir, {{"main.al",
                   "fn add(a: i32, b: i32) -> i32 {\n"
                   "  ret a + b\n"
                   "}\n"
                   "fn main() {\n"
                   "  print(\"hi\")\n"
                   "  _ := add(40, 2)\n"
                   "}\n"},
                  {"io.al",
                   "pub intrinsic fn panic(msg: str) -> !;\n"
                   "\n"
                   "pub fn print(comp fmt: str, args: ..) {\n"
                   "  panic(\"must expand\")\n"
                   "}\n"}});

  Fixture f;
  LowerCase result =
      lower_case(dir, "main.al", {"main.al"}, f, {{"io", "io.al"}});
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("lower_emit_object_test", context);
  CHECK(codegen_llvm::configure_target(*module, host_target()).is_ok());
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, host_target(), true);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));

  base::Result<std::vector<u8>, codegen_llvm::ObjectEmitError> emitted =
      codegen_llvm::emit_object(*module, host_target());
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
        triple_emitted = codegen_llvm::emit_object(
            *module,
            codegen_llvm::Target{std::string(triple), ir::PointerWidth::W64});
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
  write_all(dir, {{"main.al",
                   "fn main() -> i32 {\n"
                   "  x := 40\n"
                   "  ret x + 2\n"
                   "}\n"}});

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
  CHECK(codegen_llvm::configure_target(*module, host_target()).is_ok());
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, host_target(), true);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));

  // The emitter spills every local; mem2reg in the O3 pipeline is what
  // promotes this one, so its absence says the pipeline ran.
  base::Result<void, codegen_llvm::ObjectEmitError> optimized =
      codegen_llvm::optimize_module(*module, host_target(), nullptr);
  CHECK(optimized.is_ok());
  CHECK(!llvm::verifyModule(*module));
  CHECK(codegen_llvm::emit_ir(*module).find("alloca") == std::string::npos);

  // Unknown triples fail the same way emission does.
  CHECK(codegen_llvm::optimize_module(
            *module,
            codegen_llvm::Target{"no-such-triple", ir::PointerWidth::W64},
            nullptr)
            .is_err());
}

TEST_CASE("Optimization records its passes when profiled") {
  VirtualDir dir;
  write_all(dir, {{"main.al",
                   "fn main() -> i32 {\n"
                   "  x := 40\n"
                   "  ret x + 2\n"
                   "}\n"}});

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("lower_optimize_profile_test", context);
  CHECK(codegen_llvm::configure_target(*module, host_target()).is_ok());
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, host_target(), true);
  std::move(emitter).emit();

  debug::Profiler profiler;
  profiler.start();
  CHECK(
      codegen_llvm::optimize_module(*module, host_target(), &profiler).is_ok());
  profiler.stop();

  // The timer behind these events has to outlive the callbacks that fill
  // it; a timer scoped to the block that registers them is a dead stack
  // slot by the time a pass runs, which is what ASan catches here.
  bool saw_pass = false;
  for (const debug::ProfileEvent& event : profiler.copy_events()) {
    if (profiler.name(event.category) == "llvm-pass") {
      saw_pass = true;
      CHECK(!profiler.name(event.name).empty());
    }
  }
  CHECK(saw_pass);
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
    write_all(dir, {{"main.al", source},
                    {"io.al",
                     "pub intrinsic fn panic(msg: str) -> !;\n"
                     "\n"
                     "pub fn print(comp fmt: str, args: ..) {\n"
                     "  panic(\"must expand\")\n"
                     "}\n"}});

    Fixture f;
    LowerCase result =
        lower_case(dir, "main.al", {"main.al"}, f, {{"io", "io.al"}});
    CHECK(result.ok);
    CHECK(result.lowered.has_value());
    if (!result.ok || !result.lowered.has_value()) {
      continue;
    }
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module =
        std::make_unique<llvm::Module>("lower_entry_test", context);
    CHECK(codegen_llvm::configure_target(*module, host_target()).is_ok());
    codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                        std::move(result.lowered->storage),
                                        &f.strings, host_target(), true);
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
  write_all(dir, {{"main.al",
                   "fn main() {\n"
                   "  ret\n"
                   "  _ := 2\n"
                   "}\n"}});

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(f.bag.warning_count() > 0);
}

TEST_CASE("Lower emits verifiable LLVM IR for control flow") {
  VirtualDir dir;
  write_all(dir, {{"main.al",
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
  CHECK(codegen_llvm::configure_target(*module, host_target()).is_ok());
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, host_target(), true);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));
}

TEST_CASE("Lower emits verifiable LLVM IR for enums and calls") {
  VirtualDir dir;
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
  CHECK(codegen_llvm::configure_target(*module, host_target()).is_ok());
  codegen_llvm::LlvmIrEmitter emitter(module.get(),
                                      std::move(result.lowered->storage),
                                      &f.strings, host_target(), true);
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
  write_all(dir, {{"main.al",
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
                   "    Opt::None => ret 0\n"
                   "  }\n"
                   "}\n"
                   "fn main() {\n"
                   "  _ := classify(Opt::Some(2))\n"
                   "  _ := pick(1)\n"
                   "  _ := pick_let(Opt::Some(3))\n"
                   "  _ := mixed(Opt::Some(4))\n"
                   "}\n"}});

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  CHECK(ir::verify_storage(*result.lowered->storage).is_ok());
}

// A package whose names outgrow the shared table is refused with a
TEST_CASE("Checked indexing and field access mark addresses inbounds") {
  VirtualDir dir;
  write_all(dir, {{"main.al",
                   "struct Pair {\n"
                   "  a: i32,\n"
                   "  b: i32,\n"
                   "}\n"
                   "fn main() -> i32 {\n"
                   "  arr := [10i32, 20i32, 30i32]\n"
                   "  p := Pair { a: 1, b: 2 }\n"
                   "  mut i := 1\n"
                   "  ret arr[i] + p.b\n"
                   "}\n"}});
  Fixture f;
  const LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  // The checked index, the array literal's elements, and the field
  // projection each promise the address stays inside its object, which
  // is what the emitter turns into an inbounds GEP.
  usize inbounds = 0;
  for (const ir::Instruction& instr : result.lowered->storage->instrs()) {
    if (instr.op != ir::Opcode::GetElementPtr &&
        instr.op != ir::Opcode::ElemOffset) {
      continue;
    }
    if (instr.flags.inbounds) {
      ++inbounds;
    }
  }
  CHECK(inbounds >= 3);
}

TEST_CASE("A literal index inside the array's bounds needs no check") {
  VirtualDir dir;
  write_all(dir, {{"main.al",
                   "fn main() -> i32 {\n"
                   "  a := [1i32, 2i32]\n"
                   "  const i := 1\n"
                   "  ret a[1] + a[i]\n"
                   "}\n"}});
  Fixture f;
  const LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  // Both indexes are compile-time values the array admits, so no
  // compare or panic path is emitted at all.
  usize branches = 0;
  usize inbounds = 0;
  for (const ir::Instruction& instr : result.lowered->storage->instrs()) {
    if (instr.op == ir::Opcode::CondBr) {
      ++branches;
    }
    if (instr.op == ir::Opcode::GetElementPtr && instr.flags.inbounds) {
      ++inbounds;
    }
  }
  CHECK(branches == 0);
  CHECK(inbounds >= 2);
}

// diagnostic: the interner answers with nothing so a caller that can
// report exhaustion does, rather than aborting.
TEST_CASE("Lowering reports a spent name table") {
  VirtualDir dir;
  std::string source = "fn sink(s: str) {}\nfn main() {\n";
  for (u32 i = 0; i < 64; ++i) {
    source += "  sink(\"m" + std::to_string(i) + "\")\n";
  }
  source += "}\n";
  write_all(dir, {{"main.al", source}});

  Fixture f{16};
  const LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.ok);
  CHECK(!result.lowered.has_value());
  CHECK(f.bag.has_errors());
  bool reported = false;
  for (u32 i = 0; i < f.bag.size(); ++i) {
    const diag::Diagnostic* const diag = f.bag.at(i);
    if (diag != nullptr &&
        diag->code ==
            diag::Code{diag::Stage::Lowering,
                       static_cast<u8>(DiagCode::NameTableExhausted)}) {
      reported = true;
    }
  }
  CHECK(reported);
}

// A constant expression the compiler evaluates overflows its type: the
// program is refused with the check's own code rather than a
// not-supported report (ADR-0062).
TEST_CASE("Lowering reports an overflow in a constant") {
  VirtualDir dir;
  write_all(dir, {{"main.al",
                   "const N: i32 = 2147483647 + 1\n"
                   "fn main() -> i32 {\n"
                   "  ret N\n"
                   "}\n"}});

  Fixture f;
  const LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
  bool reported = false;
  for (u32 i = 0; i < f.bag.size(); ++i) {
    const diag::Diagnostic* const diag = f.bag.at(i);
    if (diag != nullptr &&
        diag->code == diag::Code{diag::Stage::Lowering,
                                 static_cast<u8>(DiagCode::CompOverflow)}) {
      reported = true;
    }
  }
  CHECK(reported);
}

}  // namespace lowering
