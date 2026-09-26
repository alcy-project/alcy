// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "ir/storage.h"

// Deterministic encoding of a linkable symbol from the signature it
// names. Every symbol the compiler defines is encoded this way, so a
// source name can never collide with a C library entry point or with
// another alcy item. See docs/adr/0011.
namespace symbol {

// What a symbol names. Two signatures encode to the same string only if
// every field matches, so the encoding is injective over this domain.
struct Signature {
  // Module path, `::`-separated, empty for the root. Each segment is
  // encoded with its own byte length, so a separator inside a segment
  // is safe.
  std::string path;
  // Source name of the item.
  std::string name;
  enum class Kind : u8 {
    // Not encoded: a C entry point, or the synthesized program entry.
    Foreign,
    // A free function.
    Free,
    // An associated function in an impl block, with no receiver.
    Assoc,
    // An inherent method, which takes a receiver.
    Method,
  };
  Kind kind = Kind::Foreign;
  // Type arguments of the instantiation, empty for a non-generic item.
  std::vector<ir::TypeIdx> generics;
};

// A type recovered from a symbol. Decoding never consults or builds a
// type table: a decoded type is a description, so a symbol stays
// readable without a program to interpret it in.
struct DecodedType {
  enum class Kind : u8 {
    // A primitive, carried by its source spelling ("i32", "bool").
    Prim,
    Str,
    Ref,
    MutRef,
    Ptr,
    Array,
    Tuple,
    Nominal,
  };
  Kind kind = Kind::Prim;
  std::string spelling;
  // Ref and MutRef: the pointee. Array: the element.
  std::vector<DecodedType> parts;
  // Array length, or pointer width in bits.
  u64 count = 0;
  // Nominal: its module path segments, then its type arguments.
  std::vector<std::string> path;
  std::vector<DecodedType> args;
};

struct Demangled {
  Signature::Kind kind = Signature::Kind::Foreign;
  std::vector<std::string> path;
  std::string name;
  std::vector<DecodedType> generics;
};

// Encodes a signature. Deterministic: the same signature always yields
// the same string, independent of the order items were lowered in.
std::string mangle(const Signature& signature,
                   const ir::Storage& types,
                   const str::StringInterner& strings);

// Recovers a signature. A symbol that is truncated, carries an
// unknown version, or is not alcy's becomes a structured error
// rather than a partial decode.
enum class DemangleError : u8 {
  // Missing the alcy prefix: not our symbol.
  NotAlcySymbol,
  // The version byte disagrees with this decoder.
  UnknownVersion,
  // Truncated or undecodable body, including trailing bytes.
  Malformed,
};

base::Result<Demangled, DemangleError> demangle(std::string_view text);

// Renders a decoded type in source spelling, for diagnostics.
std::string display(const DecodedType& type);

}  // namespace symbol
