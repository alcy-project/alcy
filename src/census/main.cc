// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The compile-time evaluator against compiled packages. This driver
// loads each package the compiler wrote in the binary IR form, runs
// every function the interpreter can start, and fails while it
// reports a compiler bug (Internal) or a refusal it has not declared. A
// declared refusal is the boundary the corpus accepts as a program
// property, and the per-reason census is what shows the boundary move.
//
// Arguments are package files written by --emit ir-bc. A load failure is
// a failure of this run: the bytes come from the compiler, not from a
// hostile source.

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "comp/comp_value.h"
#include "comp/ir_interpreter.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "ir/block.h"
#include "ir/block_param.h"
#include "ir/common.h"
#include "ir/deserialize.h"
#include "ir/function.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type_util.h"

namespace {

constexpr const char* load_error_name(ir::IrLoadError error) {
  switch (error) {
    case ir::IrLoadError::Truncated: return "truncated";
    case ir::IrLoadError::BadMagic: return "bad magic";
    case ir::IrLoadError::UnsupportedVersion: return "unsupported version";
    case ir::IrLoadError::BadFlags: return "bad flags";
    case ir::IrLoadError::BadSection: return "bad section";
    case ir::IrLoadError::OutOfRange: return "out of range";
    case ir::IrLoadError::BadShape: return "bad shape";
    case ir::IrLoadError::BadChecksum: return "bad checksum";
    case ir::IrLoadError::Verification: return "verification";
  }
  return "unknown";
}

// What one package's functions did, as the census the summary prints.
// A refusal the engine has declared is counted under its wording, so a
// boundary the corpus exercises reads as a number, not a silence.
struct Census {
  usize functions = 0;
  usize evaluated = 0;
  usize budget = 0;
  usize overflow = 0;
  usize external = 0;
  usize params = 0;
  std::map<std::string_view, usize> declined;
};

// Whether the engine can start the function from zero-filled arguments.
// A parameter the engine cannot fabricate - a reference, a float, an
// aggregate - leaves the function uncalled rather than called with a
// value the language could never pass.
bool zeroable(const ir::StorageState& state, ir::TypeIdx type) {
  return ir::is_integer_type(state.types[type].tag);
}

bool eval_package(const char* path, Census& census) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "%s: cannot read\n", path);
    return false;
  }
  std::vector<u8> bytes((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  base::Result<ir::LoadedIr, ir::IrLoadError> loaded =
      ir::deserialize(std::span<const u8>(bytes));
  if (loaded.is_err()) {
    std::fprintf(stderr, "%s: load failed: %s\n", path,
                 load_error_name(std::move(loaded).unwrap_err()));
    return false;
  }
  ir::LoadedIr package = std::move(loaded).unwrap();
  const ir::StorageState& state = package.storage->state();
  const ir::SymbolTable& strings = *package.strings;

  bool ok = true;
  for (usize index = 0; index < state.functions.size(); ++index) {
    const ir::Function& fn =
        state.functions[ir::FunctionIdx(static_cast<u32>(index))];
    ++census.functions;
    if (fn.blocks.empty()) {
      // A declaration with no body has nothing to evaluate.
      ++census.external;
      continue;
    }
    const ir::Block& entry = state.blocks[fn.blocks.head()];
    std::vector<comp::CompVal> args;
    bool runnable = true;
    for (const ir::BlockParamIdx param_id : entry.block_params) {
      const ir::BlockParam& param = state.block_params[param_id];
      if (!zeroable(state, param.type)) {
        runnable = false;
        break;
      }
      comp::CompVal zero;
      zero.type = param.type;
      zero.value.tag = comp::CompValue::Tag::Int;
      zero.value.int_value = 0;
      args.push_back(zero);
    }
    if (!runnable) {
      ++census.params;
      continue;
    }
    u64 quota = comp::COMP_BRANCH_QUOTA;
    u32 depth = 0;
    base::Result<comp::CompVal, comp::IrRunError> result =
        comp::interpret_function(state, strings, package.width,
                                 ir::FunctionIdx(static_cast<u32>(index)), args,
                                 quota, depth);
    if (result.is_ok()) {
      ++census.evaluated;
      continue;
    }
    const comp::IrRunError error = std::move(result).unwrap_err();
    switch (error.kind) {
      case comp::IrRunError::Kind::Quota:
      case comp::IrRunError::Kind::Depth: ++census.budget; break;
      case comp::IrRunError::Kind::Overflow: ++census.overflow; break;
      case comp::IrRunError::Kind::DivisionByZero:
      case comp::IrRunError::Kind::OutOfBounds: {
        const std::string_view name = strings.get(fn.meta.name);
        std::fprintf(stderr, "%s: %.*s: source error: %.*s\n", path,
                     static_cast<i32>(name.size()), name.data(),
                     static_cast<i32>(error.what.size()), error.what.data());
        break;
      }
      case comp::IrRunError::Kind::Unsupported: {
        const std::span<const std::string_view> declared =
            comp::unsupported_refusals();
        if (std::find(declared.begin(), declared.end(), error.what) !=
            declared.end()) {
          ++census.declined[error.what];
          break;
        }
        const std::string_view name = strings.get(fn.meta.name);
        std::fprintf(stderr, "%s: %.*s: undeclared refusal: %.*s\n", path,
                     static_cast<i32>(name.size()), name.data(),
                     static_cast<i32>(error.what.size()), error.what.data());
        ok = false;
        break;
      }
      case comp::IrRunError::Kind::Internal: {
        const std::string_view name = strings.get(fn.meta.name);
        std::fprintf(stderr, "%s: %.*s: internal: %.*s\n", path,
                     static_cast<i32>(name.size()), name.data(),
                     static_cast<i32>(error.what.size()), error.what.data());
        ok = false;
        break;
      }
    }
  }
  return ok;
}

void print_census(std::string_view path, const Census& census) {
  std::printf(
      "%.*s: %zu functions, %zu evaluated, %zu budget, %zu overflow, "
      "%zu external, %zu params, %zu declined\n",
      static_cast<i32>(path.size()), path.data(), census.functions,
      census.evaluated, census.budget, census.overflow, census.external,
      census.params,
      census.functions - census.evaluated - census.budget - census.overflow -
          census.external - census.params);
  for (const auto& [what, count] : census.declined) {
    std::printf("    %zu %.*s\n", count, static_cast<i32>(what.size()),
                what.data());
  }
}

}  // namespace

i32 main(i32 argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: comp_census <package.irbc>...\n");
    return 2;
  }
  bool ok = true;
  Census total;
  for (i32 at = 1; at < argc; ++at) {
    Census census;
    if (!eval_package(argv[at], census)) {
      ok = false;
    }
    print_census(argv[at], census);
    total.functions += census.functions;
    total.evaluated += census.evaluated;
    total.budget += census.budget;
    total.overflow += census.overflow;
    total.external += census.external;
    total.params += census.params;
    for (const auto& [what, count] : census.declined) {
      total.declined[what] += count;
    }
  }
  std::printf(
      "comp_census: %d package(s), %zu functions, %zu evaluated, %zu budget, "
      "%zu overflow, %zu external, %zu params, %zu declined\n",
      argc - 1, total.functions, total.evaluated, total.budget, total.overflow,
      total.external, total.params,
      total.functions - total.evaluated - total.budget - total.overflow -
          total.external - total.params);
  for (const auto& [what, count] : total.declined) {
    std::printf("    %zu %.*s\n", count, static_cast<i32>(what.size()),
                what.data());
  }
  if (!ok) {
    std::fprintf(stderr, "comp_census: the engine and the corpus disagree\n");
    return 1;
  }
  return 0;
}
