// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

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

