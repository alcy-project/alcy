// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "comp/ir_interpreter.h"

#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/comp_evaluator.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "comp/comp_value.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/opcode.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "ir/verifier.h"
#include "lowering/lowering.h"
#include "tests/lowering/lowering_fixture.h"

namespace lowering {

namespace {

// The two engines read one budget; the interpreter keeps its own copy
// of the numbers because comp cannot depend on analyzer.
static_assert(comp::COMP_BRANCH_QUOTA == analyzer::COMP_BRANCH_QUOTA);
static_assert(comp::COMP_MAX_CALL_DEPTH == analyzer::COMP_MAX_CALL_DEPTH);

// The initializer of the const item `name`, or an invalid index.
ast::ExprIdx find_const_init(const ast::AstArena& ast, std::string_view name) {
  for (u32 i = 0; i < static_cast<u32>(ast.items.size()); ++i) {
    const ast::ItemNode& item = ast.items[ast::ItemIdx(i)];
    if (item.kind != ast::ItemKind::Const) {
      continue;
    }
    const ast::ItemConst& konst = item.payload.get<ast::ItemConst>();
    if (konst.name.name == name) {
      return konst.init;
    }
  }
  return ast::ExprIdx(base::INVALID_IDX);
}

// The AST engine's value for the const item `name`. The checked type
// table is moved into the evaluator's builder and put back through
// build(), so lowering reads the package it checked.
std::optional<comp::CompVal> ast_value(Fixture& f,
                                       analyzer::CheckedPackage& checked,
                                       std::string_view name) {
  const ast::ExprIdx init = find_const_init(f.ast, name);
  ir::StorageBuilder builder(std::move(checked.types).unwrap().take_state());
  bool ok = init.is_valid();
  comp::CompVal value;
  if (ok) {
    analyzer::CompEvaluator evaluator(
        analyzer::CompInputs{checked.tree, checked.modules,
                             checked.generic_insts, checked.type_origins},
        f.ast, builder, ir::PointerWidth::W64);
    ok = evaluator.evaluate_item(0, init, analyzer::NO_INST, value);
  }
  base::Result<ir::VerifiedStorage, ir::VerificationError> rebuilt =
      std::move(builder).build();
  if (rebuilt.is_err()) {
    return std::nullopt;
  }
  checked.types = std::move(rebuilt).unwrap();
  if (!ok) {
    return std::nullopt;
  }
  return value;
}

// The lowered function whose meta name is `name`. A specialization of a
// comp-parameter function keeps the source name; the cases use one
// instantiation each.
ir::FunctionIdx find_function(const ir::Storage& storage,
                              const ir::SymbolTable& strings,
                              std::string_view name) {
  for (ir::FunctionIdx idx(0); idx.idx < storage.functions().size(); ++idx) {
    if (strings.get(storage.functions()[idx].meta.name) == name) {
      return idx;
    }
  }
  return ir::FunctionIdx(base::INVALID_IDX);
}

comp::CompVal int_arg(ir::TypeIdx type, u64 bits) {
  comp::CompVal value;
  value.type = type;
  value.value.tag = comp::CompValue::Tag::Int;
  value.value.int_value = bits;
  return value;
}

comp::CompVal bool_arg(ir::TypeIdx type, bool bit) {
  comp::CompVal value;
  value.type = type;
  value.value.tag = comp::CompValue::Tag::Bool;
  value.value.bool_value = bit;
  return value;
}

// `values` as arguments typed by the function's parameters, or empty
// when the counts disagree.
std::vector<comp::CompVal> int_args(const ir::Storage& storage,
                                    ir::FunctionIdx fn,
                                    std::initializer_list<u64> values) {
  const ir::Function& function = storage.functions()[fn];
  std::vector<comp::CompVal> args;
  auto value = values.begin();
  for (ir::TypeIdx param : function.meta.param_types) {
    if (value == values.end()) {
      return {};
    }
    args.push_back(int_arg(param, *value));
    ++value;
  }
  if (value != values.end()) {
    return {};
  }
  return args;
}

std::vector<comp::CompVal> bool_args(const ir::Storage& storage,
                                     ir::FunctionIdx fn,
                                     std::initializer_list<bool> values) {
  const ir::Function& function = storage.functions()[fn];
  std::vector<comp::CompVal> args;
  auto value = values.begin();
  for (ir::TypeIdx param : function.meta.param_types) {
    if (value == values.end()) {
      return {};
    }
    args.push_back(bool_arg(param, *value));
    ++value;
  }
  if (value != values.end()) {
    return {};
  }
  return args;
}

struct RunResult {
  bool ok = false;
  comp::CompVal value;
  comp::IrRunError error;
};

RunResult run_function(const Fixture& f,
                       const ir::Storage& storage,
                       ir::FunctionIdx fn,
                       std::span<const comp::CompVal> args,
                       u64 quota,
                       u32 depth) {
  RunResult out;
  base::Result<comp::CompVal, comp::IrRunError> result =
      comp::interpret_function(storage, f.strings, ir::PointerWidth::W64, fn,
                               args, quota, depth);
  if (result.is_err()) {
    out.error = std::move(result).unwrap_err();
    return out;
  }
  out.ok = true;
  out.value = std::move(result).unwrap();
  return out;
}

// Both engines carry one value; integers wrap like the opcodes that
// produced them, so the comparison reads through the type's width.
void check_same_value(const ir::Storage& storage,
                      const comp::CompVal& expected,
                      const comp::CompVal& actual) {
  CHECK(expected.type.idx == actual.type.idx);
  CHECK(expected.value.tag == actual.value.tag);
  if (expected.value.tag != actual.value.tag) {
    return;
  }
  if (expected.value.tag == comp::CompValue::Tag::Bool) {
    CHECK(expected.value.bool_value == actual.value.bool_value);
    return;
  }
  if (expected.value.tag == comp::CompValue::Tag::Int) {
    const ir::TypeTag tag = storage.types()[expected.type].tag;
    CHECK((expected.value.int_value & comp::mask(tag)) ==
          (actual.value.int_value & comp::mask(tag)));
  }
}

void check_int_value(const ir::Storage& storage,
                     const comp::CompVal& value,
                     u64 expected) {
  CHECK(value.value.tag == comp::CompValue::Tag::Int);
  if (value.value.tag != comp::CompValue::Tag::Int) {
    return;
  }
  const ir::TypeTag tag = storage.types()[value.type].tag;
  CHECK((value.value.int_value & comp::mask(tag)) ==
        (expected & comp::mask(tag)));
}

// One checked-and-lowered package plus the fixture that owns the arenas
// its tables point into, and the AST engine's values for the const
// items the case named.
struct Scenario {
  Fixture fixture;
  CheckCase checked;
  std::vector<comp::CompVal> asts;
  LowerCase lowered;
  // The lowered storage the package owns, valid once `build_scenario`
  // returned true.
  const ir::Storage* storage = nullptr;
};

// Checks `source`, evaluates each const item in `const_names` through
// the AST engine, then lowers the same package.
bool build_scenario(VirtualDir& dir,
                    std::string_view source,
                    std::initializer_list<std::string_view> const_names,
                    Scenario& scenario) {
  write_all(dir, {{"main.al", source}});
  scenario.checked = check_case(dir, "main.al", {"main.al"}, scenario.fixture);
  if (!scenario.checked.ok || !scenario.checked.checked.has_value()) {
    return false;
  }
  for (std::string_view name : const_names) {
    std::optional<comp::CompVal> value =
        ast_value(scenario.fixture, *scenario.checked.checked, name);
    if (!value.has_value()) {
      return false;
    }
    scenario.asts.push_back(std::move(*value));
  }
  scenario.lowered = lower_checked_case(std::move(*scenario.checked.checked),
                                        scenario.fixture);
  if (!scenario.lowered.lowered.has_value()) {
    return false;
  }
  scenario.storage = &*scenario.lowered.lowered->storage;
  return true;
}

}  // namespace

TEST_CASE("IR interpreter agrees on branches and arithmetic") {
  VirtualDir dir;
  Scenario scenario;
  const bool built =
      build_scenario(dir,
                     "comp fn price(kind: i32, count: i32) -> i32 {\n"
                     "  mut unit := 0\n"
                     "  if kind < 3 {\n"
                     "    unit = 5\n"
                     "  } else {\n"
                     "    unit = 7\n"
                     "  }\n"
                     "  ret unit * count + kind\n"
                     "}\n"
                     "const R: i32 = price(4, 3)\n"
                     "fn main() {\n"
                     "}\n",
                     {"R"}, scenario);
  CHECK(built);
  if (!built) {
    return;
  }
  CHECK(scenario.storage != nullptr);
  if (scenario.storage == nullptr) {
    return;
  }
  const ir::Storage& storage = *scenario.storage;
  const ir::FunctionIdx fn =
      find_function(storage, scenario.fixture.strings, "price");
  CHECK(fn.is_valid());
  if (!fn.is_valid()) {
    return;
  }
  const std::vector<comp::CompVal> args = int_args(storage, fn, {4, 3});
  CHECK(args.size() == 2);
  if (args.size() != 2) {
    return;
  }
  const RunResult run = run_function(scenario.fixture, storage, fn, args,
                                     comp::COMP_BRANCH_QUOTA, 0);
  CHECK(run.ok);
  CHECK(scenario.asts.size() == 1);
  if (!run.ok || scenario.asts.size() != 1) {
    return;
  }
  check_same_value(storage, scenario.asts[0], run.value);
}

TEST_CASE("IR interpreter agrees on a while loop") {
  VirtualDir dir;
  Scenario scenario;
  const bool built = build_scenario(dir,
                                    "comp fn sum(n: i32) -> i32 {\n"
                                    "  mut i := 0\n"
                                    "  mut total := 0\n"
                                    "  while i < n {\n"
                                    "    total = total + i\n"
                                    "    i = i + 1\n"
                                    "  }\n"
                                    "  ret total\n"
                                    "}\n"
                                    "const R: i32 = sum(10)\n"
                                    "fn main() {\n"
                                    "}\n",
                                    {"R"}, scenario);
  CHECK(built);
  if (!built) {
    return;
  }
  CHECK(scenario.storage != nullptr);
  if (scenario.storage == nullptr) {
    return;
  }
  const ir::Storage& storage = *scenario.storage;
  const ir::FunctionIdx fn =
      find_function(storage, scenario.fixture.strings, "sum");
  CHECK(fn.is_valid());
  if (!fn.is_valid()) {
    return;
  }
  const std::vector<comp::CompVal> args = int_args(storage, fn, {10});
  CHECK(args.size() == 1);
  if (args.size() != 1) {
    return;
  }
  const RunResult run = run_function(scenario.fixture, storage, fn, args,
                                     comp::COMP_BRANCH_QUOTA, 0);
  CHECK(run.ok);
  CHECK(scenario.asts.size() == 1);
  if (!run.ok || scenario.asts.size() != 1) {
    return;
  }
  check_same_value(storage, scenario.asts[0], run.value);
}

TEST_CASE("IR interpreter agrees on a direct call") {
  VirtualDir dir;
  Scenario scenario;
  const bool built = build_scenario(dir,
                                    "fn double(comp n: i32) -> i32 {\n"
                                    "  ret n * 2\n"
                                    "}\n"
                                    "fn quad(comp n: i32) -> i32 {\n"
                                    "  ret double(n) * 2\n"
                                    "}\n"
                                    "const R: i32 = quad(21)\n"
                                    "const S: i32 = double(21)\n"
                                    "fn main() {\n"
                                    "  _ := quad(21)\n"
                                    "}\n",
                                    {"R", "S"}, scenario);
  CHECK(built);
  if (!built) {
    return;
  }
  CHECK(scenario.storage != nullptr);
  if (scenario.storage == nullptr) {
    return;
  }
  const ir::Storage& storage = *scenario.storage;
  const std::vector<comp::CompVal> no_args;
  const ir::FunctionIdx quad =
      find_function(storage, scenario.fixture.strings, "quad");
  const ir::FunctionIdx doubled =
      find_function(storage, scenario.fixture.strings, "double");
  CHECK(quad.is_valid());
  CHECK(doubled.is_valid());
  CHECK(scenario.asts.size() == 2);
  if (!quad.is_valid() || !doubled.is_valid() || scenario.asts.size() != 2) {
    return;
  }
  const RunResult quad_run = run_function(scenario.fixture, storage, quad,
                                          no_args, comp::COMP_BRANCH_QUOTA, 0);
  CHECK(quad_run.ok);
  if (quad_run.ok) {
    check_same_value(storage, scenario.asts[0], quad_run.value);
  }
  const RunResult double_run = run_function(
      scenario.fixture, storage, doubled, no_args, comp::COMP_BRANCH_QUOTA, 0);
  CHECK(double_run.ok);
  if (double_run.ok) {
    check_same_value(storage, scenario.asts[1], double_run.value);
  }
}

TEST_CASE("IR interpreter agrees on casts and signed shifts") {
  VirtualDir dir;
  Scenario scenario;
  const bool built = build_scenario(dir,
                                    "comp fn mix(x: i32) -> i32 {\n"
                                    "  y := x as i64\n"
                                    "  ret ((y * 3 - 1) as i32) >> 1\n"
                                    "}\n"
                                    "const R: i32 = mix(0 - 7)\n"
                                    "fn main() {\n"
                                    "}\n",
                                    {"R"}, scenario);
  CHECK(built);
  if (!built) {
    return;
  }
  CHECK(scenario.storage != nullptr);
  if (scenario.storage == nullptr) {
    return;
  }
  const ir::Storage& storage = *scenario.storage;
  const ir::FunctionIdx fn =
      find_function(storage, scenario.fixture.strings, "mix");
  CHECK(fn.is_valid());
  if (!fn.is_valid()) {
    return;
  }
  const std::vector<comp::CompVal> args =
      int_args(storage, fn, {static_cast<u64>(-7)});
  CHECK(args.size() == 1);
  if (args.size() != 1) {
    return;
  }
  const RunResult run = run_function(scenario.fixture, storage, fn, args,
                                     comp::COMP_BRANCH_QUOTA, 0);
  CHECK(run.ok);
  CHECK(scenario.asts.size() == 1);
  if (!run.ok || scenario.asts.size() != 1) {
    return;
  }
  check_same_value(storage, scenario.asts[0], run.value);
}

TEST_CASE("IR interpreter agrees on unsigned bitwise operations") {
  VirtualDir dir;
  Scenario scenario;
  const bool built =
      build_scenario(dir,
                     "comp fn weave(x: u32) -> u32 {\n"
                     "  ret ((x << 3) ^ (x >> 1)) | (x & 0xF0u32)\n"
                     "}\n"
                     "const R: u32 = weave(12u32)\n"
                     "fn main() {\n"
                     "}\n",
                     {"R"}, scenario);
  CHECK(built);
  if (!built) {
    return;
  }
  CHECK(scenario.storage != nullptr);
  if (scenario.storage == nullptr) {
    return;
  }
  const ir::Storage& storage = *scenario.storage;
  const ir::FunctionIdx fn =
      find_function(storage, scenario.fixture.strings, "weave");
  CHECK(fn.is_valid());
  if (!fn.is_valid()) {
    return;
  }
  const std::vector<comp::CompVal> args = int_args(storage, fn, {12});
  CHECK(args.size() == 1);
  if (args.size() != 1) {
    return;
  }
  const RunResult run = run_function(scenario.fixture, storage, fn, args,
                                     comp::COMP_BRANCH_QUOTA, 0);
  CHECK(run.ok);
  CHECK(scenario.asts.size() == 1);
  if (!run.ok || scenario.asts.size() != 1) {
    return;
  }
  check_same_value(storage, scenario.asts[0], run.value);
}

TEST_CASE("IR interpreter agrees on booleans") {
  VirtualDir dir;
  Scenario scenario;
  const bool built =
      build_scenario(dir,
                     "comp fn agree(a: bool, b: bool) -> bool {\n"
                     "  ret a && !b\n"
                     "}\n"
                     "const R: bool = agree(true, false)\n"
                     "fn main() {\n"
                     "}\n",
                     {"R"}, scenario);
  CHECK(built);
  if (!built) {
    return;
  }
  CHECK(scenario.storage != nullptr);
  if (scenario.storage == nullptr) {
    return;
  }
  const ir::Storage& storage = *scenario.storage;
  const ir::FunctionIdx fn =
      find_function(storage, scenario.fixture.strings, "agree");
  CHECK(fn.is_valid());
  if (!fn.is_valid()) {
    return;
  }
  const std::vector<comp::CompVal> args = bool_args(storage, fn, {true, false});
  CHECK(args.size() == 2);
  if (args.size() != 2) {
    return;
  }
  const RunResult run = run_function(scenario.fixture, storage, fn, args,
                                     comp::COMP_BRANCH_QUOTA, 0);
  CHECK(run.ok);
  CHECK(scenario.asts.size() == 1);
  if (!run.ok || scenario.asts.size() != 1) {
    return;
  }
  check_same_value(storage, scenario.asts[0], run.value);
}

TEST_CASE("IR interpreter returns from an early Ret") {
  VirtualDir dir;
  Scenario scenario;
  const bool built = build_scenario(dir,
                                    "comp fn clamp(x: i32) -> i32 {\n"
                                    "  if x < 0 {\n"
                                    "    ret 0\n"
                                    "  }\n"
                                    "  ret x\n"
                                    "}\n"
                                    "fn main() {\n"
                                    "}\n",
                                    {}, scenario);
  CHECK(built);
  if (!built) {
    return;
  }
  CHECK(scenario.storage != nullptr);
  if (scenario.storage == nullptr) {
    return;
  }
  const ir::Storage& storage = *scenario.storage;
  const ir::FunctionIdx fn =
      find_function(storage, scenario.fixture.strings, "clamp");
  CHECK(fn.is_valid());
  if (!fn.is_valid()) {
    return;
  }
  const std::vector<comp::CompVal> negative =
      int_args(storage, fn, {static_cast<u64>(-5)});
  const std::vector<comp::CompVal> positive = int_args(storage, fn, {3});
  CHECK(negative.size() == 1);
  CHECK(positive.size() == 1);
  if (negative.size() != 1 || positive.size() != 1) {
    return;
  }
  const RunResult clamped = run_function(scenario.fixture, storage, fn,
                                         negative, comp::COMP_BRANCH_QUOTA, 0);
  CHECK(clamped.ok);
  if (clamped.ok) {
    check_int_value(storage, clamped.value, 0);
  }
  const RunResult passed = run_function(scenario.fixture, storage, fn, positive,
                                        comp::COMP_BRANCH_QUOTA, 0);
  CHECK(passed.ok);
  if (passed.ok) {
    check_int_value(storage, passed.value, 3);
  }
}

TEST_CASE("IR interpreter agrees on recursion inside the depth cap") {
  VirtualDir dir;
  Scenario scenario;
  const bool built = build_scenario(dir,
                                    "fn fact(n: i32) -> i32 {\n"
                                    "  if n <= 1 {\n"
                                    "    ret 1\n"
                                    "  }\n"
                                    "  ret n * fact(n - 1)\n"
                                    "}\n"
                                    "fn main() {\n"
                                    "  _ := fact(5)\n"
                                    "}\n",
                                    {}, scenario);
  CHECK(built);
  if (!built) {
    return;
  }
  CHECK(scenario.storage != nullptr);
  if (scenario.storage == nullptr) {
    return;
  }
  const ir::Storage& storage = *scenario.storage;
  const ir::FunctionIdx fn =
      find_function(storage, scenario.fixture.strings, "fact");
  CHECK(fn.is_valid());
  if (!fn.is_valid()) {
    return;
  }
  const std::vector<comp::CompVal> args = int_args(storage, fn, {5});
  CHECK(args.size() == 1);
  if (args.size() != 1) {
    return;
  }
  const RunResult run = run_function(scenario.fixture, storage, fn, args,
                                     comp::COMP_BRANCH_QUOTA, 0);
  CHECK(run.ok);
  if (run.ok) {
    check_int_value(storage, run.value, 120);
  }
}

TEST_CASE("IR interpreter refuses past the call depth") {
  VirtualDir dir;
  Scenario scenario;
  const bool built = build_scenario(dir,
                                    "fn fact(n: i32) -> i32 {\n"
                                    "  if n <= 1 {\n"
                                    "    ret 1\n"
                                    "  }\n"
                                    "  ret n * fact(n - 1)\n"
                                    "}\n"
                                    "fn main() {\n"
                                    "  _ := fact(5)\n"
                                    "}\n",
                                    {}, scenario);
  CHECK(built);
  if (!built) {
    return;
  }
  CHECK(scenario.storage != nullptr);
  if (scenario.storage == nullptr) {
    return;
  }
  const ir::Storage& storage = *scenario.storage;
  const ir::FunctionIdx fn =
      find_function(storage, scenario.fixture.strings, "fact");
  CHECK(fn.is_valid());
  if (!fn.is_valid()) {
    return;
  }
  const std::vector<comp::CompVal> args = int_args(storage, fn, {100});
  CHECK(args.size() == 1);
  if (args.size() != 1) {
    return;
  }
  const RunResult run = run_function(scenario.fixture, storage, fn, args,
                                     comp::COMP_BRANCH_QUOTA, 0);
  CHECK(!run.ok);
  CHECK(run.error.kind == comp::IrRunError::Kind::Depth);
  CHECK(!run.error.what.empty());
}

TEST_CASE("IR interpreter refuses past the branch quota") {
  VirtualDir dir;
  Scenario scenario;
  const bool built = build_scenario(dir,
                                    "comp fn sum(n: i32) -> i32 {\n"
                                    "  mut i := 0\n"
                                    "  mut total := 0\n"
                                    "  while i < n {\n"
                                    "    total = total + i\n"
                                    "    i = i + 1\n"
                                    "  }\n"
                                    "  ret total\n"
                                    "}\n"
                                    "const R: i32 = sum(100)\n"
                                    "fn main() {\n"
                                    "}\n",
                                    {"R"}, scenario);
  CHECK(built);
  if (!built) {
    return;
  }
  CHECK(scenario.storage != nullptr);
  if (scenario.storage == nullptr) {
    return;
  }
  const ir::Storage& storage = *scenario.storage;
  const ir::FunctionIdx fn =
      find_function(storage, scenario.fixture.strings, "sum");
  CHECK(fn.is_valid());
  if (!fn.is_valid()) {
    return;
  }
  const std::vector<comp::CompVal> args = int_args(storage, fn, {100});
  CHECK(args.size() == 1);
  if (args.size() != 1) {
    return;
  }
  const RunResult run =
      run_function(scenario.fixture, storage, fn, args, 10, 0);
  CHECK(!run.ok);
  CHECK(run.error.kind == comp::IrRunError::Kind::Quota);
  CHECK(run.error.instr.is_valid());
  CHECK(!run.error.what.empty());
}

TEST_CASE("IR interpreter refuses a float operation") {
  VirtualDir dir;
  Scenario scenario;
  const bool built = build_scenario(dir,
                                    "fn scale(x: i32) -> f32 {\n"
                                    "  ret x as f32\n"
                                    "}\n"
                                    "fn main() {\n"
                                    "  _ := scale(1)\n"
                                    "}\n",
                                    {}, scenario);
  CHECK(built);
  if (!built) {
    return;
  }
  CHECK(scenario.storage != nullptr);
  if (scenario.storage == nullptr) {
    return;
  }
  const ir::Storage& storage = *scenario.storage;
  const ir::FunctionIdx fn =
      find_function(storage, scenario.fixture.strings, "scale");
  CHECK(fn.is_valid());
  if (!fn.is_valid()) {
    return;
  }
  const std::vector<comp::CompVal> args = int_args(storage, fn, {1});
  CHECK(args.size() == 1);
  if (args.size() != 1) {
    return;
  }
  const RunResult run = run_function(scenario.fixture, storage, fn, args,
                                     comp::COMP_BRANCH_QUOTA, 0);
  CHECK(!run.ok);
  CHECK(run.error.kind == comp::IrRunError::Kind::Unsupported);
  CHECK(run.error.instr.is_valid());
  CHECK(run.error.what.find("float") != std::string_view::npos);
}

namespace {

// `f() -> i32 { ret max + 1 }` with `op` as the add, built by hand so
// one instruction's behavior is read without a source program around
// it.
ir::VerifiedStorage overflow_add_ir(ir::SymbolTable& strings, ir::Opcode op) {
  ir::StorageBuilder builder;
  const ir::TypeIdx i32 = ir::primitive_idx(ir::TypeTag::I32);
  const ir::ImmutableIdx maximum =
      builder.immutable({.type = i32, .data = {.i32_value = 2147483647}});
  const ir::ImmutableIdx one =
      builder.immutable({.type = i32, .data = {.i32_value = 1}});
  ir::OperandSeq args;
  args.push(builder.operand(ir::Operand::from_immutable(maximum, i32)));
  args.push(builder.operand(ir::Operand::from_immutable(one, i32)));
  const ir::InstructionIdx sum =
      builder.instr({.op = op,
                     .flags = {},
                     .dst = ir::RegisterIdx(0),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = args.finish()});
  builder.reg({.type = i32, .def_idx = sum});
  const ir::OperandIdx result =
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(0), i32));
  const ir::InstructionIdx ret =
      builder.instr({.op = ir::Opcode::Ret,
                     .flags = {},
                     .dst = ir::RegisterIdx(base::INVALID_IDX),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {result, 1}});
  ir::InstrSeq instrs;
  instrs.push(sum);
  instrs.push(ret);
  const ir::BlockIdx block =
      builder.block({.instrs = instrs.finish(), .block_params = {}});
  builder.function({.meta = {.return_type = i32,
                             .param_types = {},
                             .name = strings.intern("f"),
                             .path = str::EMPTY_STRING_ID,
                             .kind = ir::SymbolKind::Foreign,
                             .generics = ir::TypeIdxRange{}},
                    .blocks = {block, 1}});
  return std::move(builder).build().unwrap();
}

}  // namespace

TEST_CASE("IR interpreter refuses an overflowing source add") {
  Fixture f;
  ir::VerifiedStorage storage = overflow_add_ir(f.strings, ir::Opcode::IntAdd);
  const RunResult run = run_function(f, *storage, ir::FunctionIdx(0), {},
                                     comp::COMP_BRANCH_QUOTA, 0);
  CHECK(!run.ok);
  CHECK(run.error.kind == comp::IrRunError::Kind::Overflow);
}

TEST_CASE("IR interpreter wraps where the opcode says so") {
  Fixture f;
  ir::VerifiedStorage storage =
      overflow_add_ir(f.strings, ir::Opcode::IntAddWrap);
  const RunResult run = run_function(f, *storage, ir::FunctionIdx(0), {},
                                     comp::COMP_BRANCH_QUOTA, 0);
  CHECK(run.ok);
  CHECK(run.value.value.tag == comp::CompValue::Tag::Int);
  CHECK(run.value.value.int_value == 2147483648ull);
}

}  // namespace lowering
