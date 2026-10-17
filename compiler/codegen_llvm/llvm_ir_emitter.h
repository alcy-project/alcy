// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codegen_llvm/declaration.h"
#include "codegen_llvm/llvm_ir_storage.h"
#include "codegen_llvm/target.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/profiler/profiler.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "overlay/overlay.h"

namespace llvm {
class BasicBlock;
class Function;
class Module;
}  // namespace llvm

namespace codegen_llvm {

// One program function of the runtime backtrace table the unwinder
// reads: the LLVM function and the source spelling a frame shows
// (ADR-0070).
struct BacktraceName {
  llvm::Function* function = nullptr;
  std::string name;
};

// Fills the table the panic walker dereferences, for a module built with
// backtrace on: every defined function gets an entry, the program's with
// the display name `visible` carries and every other one (the runtime,
// the entry thunk) with a null name, which the walker skips. Called
// after add_runtime_definitions and before optimization.
void define_backtrace_table(llvm::Module& module,
                            const std::vector<BacktraceName>& visible);

// One call a backtrace names: the block holding only that call, and the
// rendered site the frame shows beside the function name (ADR-0070).
struct CallSiteName {
  llvm::BasicBlock* block = nullptr;
  std::string site;
};

// Fills the table the panic walker reads for call sites, on the same
// terms as define_backtrace_table: one entry per recorded call, so a
// frame's return address resolves to the call it returns to.
void define_callsite_table(llvm::Module& module,
                           const std::vector<CallSiteName>& sites);

// What a backtrace build names: the program functions and the call
// sites, each with the spelling a frame shows. Both empty unless the
// emitter was built with backtrace on.
struct EmittedBacktrace {
  std::vector<BacktraceName> functions;
  std::vector<CallSiteName> sites;
};

class LlvmIrEmitter {
 public:
  using IRBuilder =
      llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter>;

  // `storage` carries its verification proof: the emitter never
  // sees unverified IR. Unreachable opcode and shape cases below
  // rely on this; the only way to build a proof is
  // StorageBuilder::build.
  LlvmIrEmitter(llvm::Module* module,
                ir::VerifiedStorage storage,
                ir::SymbolTable* interner,
                const Target& target,
                bool emit_entry,
                bool freestanding = false,
                const overlay::Overlay* overlay = nullptr,
                debug::Profiler* profiler = nullptr,
                bool backtrace = false,
                std::span<const diag::Span> instr_spans = {},
                std::span<const str::StringPoolId> site_names = {});
  ~LlvmIrEmitter() = default;

  LlvmIrEmitter(const LlvmIrEmitter&) = delete;
  LlvmIrEmitter& operator=(const LlvmIrEmitter&) = delete;

  LlvmIrEmitter(LlvmIrEmitter&&) noexcept = default;
  LlvmIrEmitter& operator=(LlvmIrEmitter&&) noexcept = default;

  // Emits every function body. Returns what a backtrace build names,
  // empty unless the emitter was built with backtrace on.
  EmittedBacktrace emit() && noexcept;

 private:
  void check_state();

  llvm::Type* type(ir::TypeIdx idx) const;
  llvm::Type* return_type(ir::TypeIdx idx) const;
  llvm::Type* build_type(ir::TypeIdx idx) const;
  llvm::Type* enum_payload_area_type(ir::TypeIdx idx) const;

  void emit_function(llvm::Function* llvm_function, const ir::Function& func);
  void emit_block(const ir::Block& block);
  void emit_instruction(ir::InstructionIdx instr_idx,
                        const ir::Instruction& instr);

  // Per-category instruction emission (see llvm_ir_emitter_compute.cc and
  // llvm_ir_emitter_memory.cc).
  void emit_compute(const ir::Instruction& instr);
  void emit_memory(const ir::Instruction& instr);
  void emit_control(const ir::Instruction& instr);
  // The checked forms of `+ - *`, `/ %`, and the shifts: each computes
  // the operation's overflow condition, calls the runtime check named
  // for it, and answers the operation's value. `emit_compute` takes
  // these only in a build whose `overflow_checks_` is set.
  // The build's checks (ADR-0068): an overflow entry replaces the
  // operation's own emission with the with-overflow form, and the other
  // kinds emit their condition and the runtime call before it.
  void emit_checked_arith(const ir::Instruction& instr,
                          const overlay::Entry& entry);
  void emit_check_prelude(const ir::Instruction& instr,
                          const overlay::Entry& entry);
  void emit_check(const overlay::Entry& entry,
                  llvm::Value* condition,
                  std::string_view helper);
  // The entry this instruction carries, or nothing when the build checks
  // nothing there.
  const overlay::Entry* overlay_entry(ir::InstructionIdx instr_idx) const;
  // The declaration of a runtime check by name, created on first use so
  // `runtime_ir` defines only the checks a build's code reaches.
  llvm::Function* check_function(std::string_view name);

  llvm::Function* create_function(const ir::FunctionMeta& function_meta) const;
  std::string linkable_name(const ir::FunctionMeta& function_meta) const;
  // The source spelling of a function for a backtrace frame: its module
  // path and name, without the signature the linkable name encodes.
  std::string display_name(const ir::FunctionMeta& function_meta) const;

  llvm::Value* resolve_operand_value(const ir::Operand& operand) const;
  // The value an argument carries, as the callee's parameter type
  // wants it. A zero-sized parameter has no bits to fill, and the unit
  // type lowers to a dummy operand, so the empty struct is what goes
  // there whatever the operand holds.
  llvm::Value* argument_value(const ir::Operand& operand,
                              llvm::Type* wanted) const;
  llvm::Function* resolve_operand_function(const ir::Operand& operand) const;

  // Whether an instruction is a call a backtrace names: a call whose
  // site the emission carries. Anything else emits inline, and the
  // site table stays without it.
  [[nodiscard]] bool has_call_site(ir::InstructionIdx instr_idx) const;
  // Emits a named call in a block of its own, so the site table maps
  // the call's address and not its neighbors'. Records the split for
  // the fixup below: successor PHIs still name the block as it was.
  void emit_sited_call(ir::InstructionIdx instr_idx,
                       const ir::Instruction& instr);

  void setup_immutables();
  void setup_external_functions();

  // True for a zero-parameter `main` whose return form maps to an
  // exit code: `()`, `i32`, or a two-variant enum whose first variant
  // holds `()` (see docs/adr/0009-result-option-library-enums.md).
  bool is_entry_candidate(const ir::Function& function) const;
  // Emits the C-ABI `main` wrapper around a renamed user entry.
  void emit_entry(llvm::Function* entry_function, ir::TypeTag ret);
  // Ends a freestanding program through the target's exit syscall.
  // Answers false for a target with no sequence here, which the
  // pipeline refuses before emission.
  bool emit_exit(llvm::Value* code32);

  llvm::Module* module_;
  ir::VerifiedStorage storage_;
  std::unique_ptr<IRBuilder> builder_;
  ir::SymbolTable* interner_;
  ir::PointerWidth width_;
  // The target's triple, kept for the freestanding exit sequence.
  std::string triple_;
  // Where the trace events go, or nothing. The function bodies emit one
  // region each under "emit-fn".
  debug::Profiler* profiler_ = nullptr;
  // Entry synthesis belongs to binaries; a library's `main` stays an
  // ordinary item, wrapped in nothing.
  bool emit_entry_;
  // Whether the entry is `_start`, ending through the exit syscall
  // instead of returning to a C runtime (ADR-0052).
  bool freestanding_ = false;
  // The build's arithmetic checks, parallel to the storage's
  // instructions; null when the build checks nothing (ADR-0068).
  const overlay::Overlay* overlay_ = nullptr;
  // Whether the panic walker gets a table to read; the emitter creates
  // the slot it lands in, and define_backtrace_table fills it once the
  // runtime exists (ADR-0070).
  bool backtrace_ = false;
  std::vector<BacktraceName> visible_functions_;
  // Source spans parallel to storage instructions, and the rendered
  // site spellings beside them. Both empty unless a backtrace asked
  // for call sites.
  std::span<const diag::Span> instr_spans_;
  std::span<const str::StringPoolId> site_names_;
  // Calls this emission gave a block of their own, with the block that
  // continues after each. Successor PHIs are rewritten from the former
  // to the latter once the function is whole.
  std::vector<CallSiteName> call_sites_;
  std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>> split_blocks_;
  LlvmIrStorage values_;
  // LLVM types are immutable once built, and one shape is requested for
  // every register, operand, and signature that names it, so each is
  // constructed once and shared.
  mutable std::vector<llvm::Type*> type_cache_;

  static constexpr usize FUNCTION_ARGS_SOO_SIZE = 8;
};

}  // namespace codegen_llvm
