// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace llvm {

class Module;
class Type;
class Function;
class BasicBlock;
class Value;
class Constant;

class ConstantFolder;
class IRBuilderDefaultInserter;

template <typename T, typename Inserter>
class IRBuilder;

}  // namespace llvm

