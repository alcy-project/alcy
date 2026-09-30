// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Properties that need no oracle beyond the code under test.
//
// The bug hunt behind this file found roughly nine semantic defects that
// no sanitizer and no crash-only fuzzer can see, because each one
// *answered*: a pattern that matched the wrong value, a format that
// printed twenty digits, a manifest diagnostic one column off. What
// catches those is a relation between two computations, not a
// predicate about one. Each test below states such a relation, so it
// either holds for every input or names a counterexample.

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "symbol/mangle.h"

namespace {

using ir::StorageBuilder;
using ir::TypeIdx;
using ir::TypeTag;

struct Fixture {
  StorageBuilder builder;
  str::StringInterner strings{16};
  TypeIdx i32 = builder.primitive(TypeTag::I32);
  TypeIdx u8 = builder.primitive(TypeTag::U8);
  TypeIdx str = builder.primitive(TypeTag::Str);

  ir::Storage build() { return std::move(builder).build().unwrap().unwrap(); }
};

// The decoder is written independently of the encoder, so a round trip
// through it is a real oracle rather than a tautology: if the encoder
// lost or conflated a field, the decoder has no way to invent it back.
symbol::Signature sig(std::string path,
                      std::string name,
                      symbol::Signature::Kind kind) {
  symbol::Signature out;
  out.path = std::move(path);
  out.name = std::move(name);
  out.kind = kind;
  return out;
}

constexpr symbol::Signature::Kind KINDS[] = {
    symbol::Signature::Kind::Free,
    symbol::Signature::Kind::Assoc,
    symbol::Signature::Kind::Method,
};

// Paths chosen to break a naive `::` split: empty segments, separators
// inside a segment, leading and trailing separators, and non-ASCII.
constexpr const char* PATHS[] = {
    "",
    "a",
    "a::b",
    "a::b::c",
    "a::::b",  // an empty segment between two separators
    "::::",    // only empty segments
    "::a",     // leading separator
    "a::",     // trailing separator
    "a::b::",  //
    "a:::b",   // three separators in a row
    "a::b:c",  // a colon inside a segment
    ":::",     //
    "core::mem::io",
    "\xc3\xa9",        // a non-ASCII segment
    "a::\xc3\xa9::b",  //
    "very::deep::nested::module::path",
};

constexpr const char* NAMES[] = {
    "f", "ff", "fff", "", "a::b", "a:b", "\xc3\xa9", "_ZN", "1abc",
};

}  // namespace

TEST_CASE("Property: a mangled symbol decodes back to its signature") {
  Fixture f;
  const ir::Storage types = f.build();
  for (const char* path : PATHS) {
    for (const char* name : NAMES) {
      for (const symbol::Signature::Kind kind : KINDS) {
        const symbol::Signature original = sig(path, name, kind);
        const std::string encoded = symbol::mangle(original, types, f.strings);
        INFO("path: " << std::string(path) << " name: " << std::string(name));

        base::Result<symbol::Demangled, symbol::DemangleError> decoded =
            symbol::demangle(encoded);
        CHECK(decoded.is_ok());
        if (decoded.is_err()) {
          continue;
        }
        const symbol::Demangled back = std::move(decoded).unwrap();
        CHECK(back.kind == original.kind);
        CHECK(back.name == original.name);
        // The path comes back as segments, so compare the join rather
        // than re-splitting: the point is that no segment was lost or
        // invented, and `::`-joining an empty segment list is the empty
        // path either way.
        std::string rejoined;
        for (usize i = 0; i < back.path.size(); ++i) {
          if (i != 0) {
            rejoined += "::";
          }
          rejoined += back.path[i];
        }
        CHECK(rejoined == original.path);
      }
    }
  }
}

TEST_CASE("Property: mangling is injective over paths and names") {
  // Two distinct signatures sharing a symbol would silently overwrite
  // one definition with another, so distinctness is the property that
  // matters and the decoder cannot supply it.
  Fixture f;
  const ir::Storage types = f.build();
  std::set<std::string> seen;
  for (const char* path : PATHS) {
    for (const char* name : NAMES) {
      for (const symbol::Signature::Kind kind : KINDS) {
        const std::string encoded =
            symbol::mangle(sig(path, name, kind), types, f.strings);
        INFO("path: " << std::string(path) << " name: " << std::string(name));
        CHECK(seen.insert(encoded).second);
      }
    }
  }
}

TEST_CASE("Property: mangling does not depend on the order items were built") {
  // Lowering order varies with the module graph, so a symbol must be a
  // function of the signature alone.
  Fixture a;
  const ir::Storage types_a = a.build();
  const std::string first = symbol::mangle(
      sig("m::n", "get", symbol::Signature::Kind::Method), types_a, a.strings);

  Fixture b;
  // A differently-shaped storage, so the index tables differ.
  (void)b.builder.array_type(b.i32, 7);
  (void)b.builder.reference_type(b.str, true);
  const ir::Storage types_b = b.build();
  const std::string second = symbol::mangle(
      sig("m::n", "get", symbol::Signature::Kind::Method), types_b, b.strings);
  CHECK(first == second);
}
