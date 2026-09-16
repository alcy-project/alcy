// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "debug/dcheck.h"
#include "fpag/base/tagged_union.h"
#include "ir/common.h"
#include "ir/type.h"

namespace ir {

struct Operand {
  using Payload = base::AutoTaggedUnion<RegisterIdx,
                                        FunctionIdx,
                                        BlockIdx,
                                        ImmutableIdx,
                                        ExternalFunctionIdx,
                                        void>;
  using Tag = Payload::Tag;
  template <typename T>
  static constexpr Tag TagOf = Payload::TagOf<T>;

  Payload data;
  TypeIdx type;

  constexpr Tag tag() const { return data.tag(); }

  template <typename T>
  constexpr bool is() const {
    return data.is<T>();
  }

  static constexpr Operand from_register(RegisterIdx idx, TypeIdx type) {
    return Operand{Payload{idx}, type};
  }

  static constexpr Operand from_function(FunctionIdx idx, TypeIdx type) {
    return Operand{Payload{idx}, type};
  }

  static constexpr Operand from_block(BlockIdx idx, TypeIdx type) {
    return Operand{Payload{idx}, type};
  }

  static constexpr Operand from_immutable(ImmutableIdx idx, TypeIdx type) {
    return Operand{Payload{idx}, type};
  }

  static constexpr Operand from_external_function(ExternalFunctionIdx idx,
                                                  TypeIdx type) {
    return Operand{Payload{idx}, type};
  }

  static constexpr Operand invalid() {
    // Default-constructed payload holds void.
    return Operand{Payload{}, primitive_idx(TypeTag::Void)};
  }

  constexpr RegisterIdx as_register() const {
    DCHECK_MSG(is<RegisterIdx>(), "operand is not a register");
    return data.get<RegisterIdx>();
  }

  constexpr FunctionIdx as_function() const {
    DCHECK_MSG(is<FunctionIdx>(), "operand is not a function");
    return data.get<FunctionIdx>();
  }

  constexpr BlockIdx as_block() const {
    DCHECK_MSG(is<BlockIdx>(), "operand is not a block");
    return data.get<BlockIdx>();
  }

  constexpr ImmutableIdx as_immutable() const {
    DCHECK_MSG(is<ImmutableIdx>(), "operand is not an immutable");
    return data.get<ImmutableIdx>();
  }

  constexpr ExternalFunctionIdx as_external_function() const {
    DCHECK_MSG(is<ExternalFunctionIdx>(),
               "operand is not an external function");
    return data.get<ExternalFunctionIdx>();
  }
};

static_assert(sizeof(Operand) == 12);
static_assert(Operand{Operand::Payload{}, primitive_idx(TypeTag::Void)}.tag() ==
              Operand::TagOf<void>);

constexpr Operand kInvalidOperand = Operand::invalid();

}  // namespace ir
