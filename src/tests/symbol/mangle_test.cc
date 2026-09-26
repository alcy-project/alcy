// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "symbol/mangle.h"

#include <string>
#include <utility>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "ir/common.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace {

using ir::StorageBuilder;
using ir::TypeIdx;
using ir::TypeTag;

// A storage with a few shapes to name, and an interner for their names.
struct Fixture {
  StorageBuilder builder;
  // The interner's map needs a power-of-two initial capacity.
  str::StringInterner strings{16};
  TypeIdx i32 = builder.primitive(TypeTag::I32);
  TypeIdx u8 = builder.primitive(TypeTag::U8);
  TypeIdx str = builder.primitive(TypeTag::Str);
  TypeIdx mut_u8 = builder.reference_type(u8, true);
  TypeIdx ref_str = builder.reference_type(str, false);
  TypeIdx arr = builder.array_type(i32, 4);

  // `Pair<A, B>` with the given arguments, interned per argument pair.
  TypeIdx pair(TypeIdx a, TypeIdx b) {
    ir::TypeSeq fields;
    fields.push(builder.ref_type(a));
    fields.push(builder.ref_type(b));
    ir::TypeSeq params;
    params.push(builder.ref_type(a));
    params.push(builder.ref_type(b));
    return builder.struct_type(strings.intern("Pair"), fields.finish(),
                               params.finish());
  }

  ir::Storage build() { return std::move(builder).build().unwrap().unwrap(); }
};

symbol::Signature signature(std::string path,
                            std::string name,
                            symbol::Signature::Kind kind) {
  symbol::Signature out;
  out.path = std::move(path);
  out.name = std::move(name);
  out.kind = kind;
  return out;
}

}  // namespace

TEST_CASE("Mangle distinguishes signatures that share a source name") {
  Fixture f;
  symbol::Signature free_fn =
      signature("", "free", symbol::Signature::Kind::Free);
  symbol::Signature method =
      signature("", "free", symbol::Signature::Kind::Method);
  symbol::Signature assoc =
      signature("", "free", symbol::Signature::Kind::Assoc);
  symbol::Signature in_core =
      signature("core", "free", symbol::Signature::Kind::Free);
  const ir::Storage types = f.build();
  const std::string a = symbol::mangle(free_fn, types, f.strings);
  const std::string b = symbol::mangle(method, types, f.strings);
  const std::string c = symbol::mangle(assoc, types, f.strings);
  const std::string d = symbol::mangle(in_core, types, f.strings);
  CHECK(a != b);
  CHECK(a != c);
  CHECK(a != d);
  CHECK(b != c);
  // The prefix is what keeps a source name away from a C symbol.
  CHECK(a.starts_with("_A1"));
}

TEST_CASE("Mangle distinguishes instantiations of one item") {
  Fixture f;
  const TypeIdx over_i32 = f.pair(f.i32, f.str);
  const TypeIdx over_u8 = f.pair(f.u8, f.str);
  symbol::Signature a = signature("m", "get", symbol::Signature::Kind::Method);
  a.generics = {over_i32};
  symbol::Signature b = signature("m", "get", symbol::Signature::Kind::Method);
  b.generics = {over_u8};
  const ir::Storage types = f.build();
  CHECK(symbol::mangle(a, types, f.strings) !=
        symbol::mangle(b, types, f.strings));
}

TEST_CASE("Mangle is deterministic") {
  Fixture f;
  symbol::Signature sig =
      signature("a::b", "run", symbol::Signature::Kind::Free);
  sig.generics = {f.pair(f.i32, f.arr), f.mut_u8, f.ref_str};
  const ir::Storage types = f.build();
  const std::string first = symbol::mangle(sig, types, f.strings);
  for (i32 i = 0; i < 4; ++i) {
    CHECK(symbol::mangle(sig, types, f.strings) == first);
  }
}

TEST_CASE("Mangle splits a module path into segments") {
  Fixture f;
  symbol::Signature nested =
      signature("a::b::c", "f", symbol::Signature::Kind::Free);
  const ir::Storage types = f.build();
  const std::string encoded = symbol::mangle(nested, types, f.strings);
  base::Result<symbol::Demangled, symbol::DemangleError> decoded =
      symbol::demangle(encoded);
  CHECK(decoded.is_ok());
  if (decoded.is_err()) {
    return;
  }
  symbol::Demangled out = std::move(decoded).unwrap();
  CHECK(out.path.size() == 3);
  CHECK(out.path[0] == "a");
  CHECK(out.path[1] == "b");
  CHECK(out.path[2] == "c");
  CHECK(out.name == "f");
}

TEST_CASE("Mangle leaves a foreign name alone") {
  Fixture f;
  symbol::Signature foreign;
  foreign.name = "alcy_alloc";
  foreign.kind = symbol::Signature::Kind::Foreign;
  const ir::Storage types = f.build();
  // A foreign signature encodes as its own name, so a C entry point is
  // never rewritten.
  CHECK(symbol::mangle(foreign, types, f.strings) == "alcy_alloc");
}

TEST_CASE("Demangle recovers a signature") {
  Fixture f;
  symbol::Signature sig =
      signature("core::util", "parse", symbol::Signature::Kind::Method);
  sig.generics = {f.pair(f.i32, f.arr), f.mut_u8};
  const ir::Storage types = f.build();
  const std::string encoded = symbol::mangle(sig, types, f.strings);

  base::Result<symbol::Demangled, symbol::DemangleError> decoded =
      symbol::demangle(encoded);
  CHECK(decoded.is_ok());
  if (decoded.is_err()) {
    return;
  }
  symbol::Demangled out = std::move(decoded).unwrap();
  CHECK(out.kind == symbol::Signature::Kind::Method);
  CHECK(out.path.size() == 2);
  CHECK(out.path[0] == "core");
  CHECK(out.path[1] == "util");
  CHECK(out.name == "parse");
  CHECK(out.generics.size() == 2);
  CHECK(out.generics[0].kind == symbol::DecodedType::Kind::Nominal);
  CHECK(out.generics[0].path.size() == 1);
  CHECK(out.generics[0].path[0] == "Pair");
  CHECK(out.generics[0].args.size() == 2);
  CHECK(out.generics[0].args[0].spelling == "i32");
  CHECK(out.generics[0].args[1].kind == symbol::DecodedType::Kind::Array);
  CHECK(out.generics[0].args[1].count == 4);
  CHECK(out.generics[1].kind == symbol::DecodedType::Kind::MutRef);
  CHECK(symbol::display(out.generics[1]) == "&mut u8");
}

TEST_CASE("Demangle rejects what it does not understand") {
  // Not ours.
  CHECK(symbol::demangle("free").is_err());
  CHECK(symbol::demangle("alcy_main").is_err());
  // Ours, but an unknown version.
  CHECK(symbol::demangle("_A9f03free").is_err());
  // Unknown kind.
  CHECK(symbol::demangle("_A1z03free").is_err());
  // Truncated: the name claims more bytes than remain.
  CHECK(symbol::demangle("_A1f09free").is_err());
  // Truncated inside a type argument.
  CHECK(symbol::demangle("_A1f03free4core").is_err());
  // Trailing bytes mean the encoding and the decoder disagree.
  CHECK(symbol::demangle("_A1f03free!!").is_err());
  // A count larger than what is left cannot be honest.
  CHECK(symbol::demangle("_A1f03free99i").is_err());
}

TEST_CASE("Display renders source spelling") {
  symbol::DecodedType ref;
  ref.kind = symbol::DecodedType::Kind::Ref;
  ref.parts.resize(1);
  ref.parts[0].kind = symbol::DecodedType::Kind::Prim;
  ref.parts[0].spelling = "str";
  CHECK(symbol::display(ref) == "&str");

  symbol::DecodedType tuple;
  tuple.kind = symbol::DecodedType::Kind::Tuple;
  tuple.parts.resize(2);
  tuple.parts[0].kind = symbol::DecodedType::Kind::Str;
  tuple.parts[1].kind = symbol::DecodedType::Kind::Prim;
  tuple.parts[1].spelling = "bool";
  CHECK(symbol::display(tuple) == "(str,bool)");
}
