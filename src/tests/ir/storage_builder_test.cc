// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/storage_builder.h"

#include <string>
#include <utility>

#include "doctest/doctest.h"
#include "ir/common.h"
#include "ir/seq_builder.h"
#include "ir/type.h"

namespace ir {

namespace {

TEST_CASE("A payload-free tag is the same type every time") {
  StorageBuilder builder;
  // Both are found by looking for a tag they do not carry, and both are
  // asked for from the type checker often enough that remembering where one
  // landed is worth it. A cache that answered a second time with a new
  // index would make two spellings of the same type compare unequal.
  const TypeIdx never = builder.never_type();
  CHECK(never.is_valid());
  CHECK(builder.never_type() == never);
  CHECK(builder.types()[never].tag == TypeTag::Never);

  const TypeIdx error = builder.error_type();
  CHECK(error.is_valid());
  CHECK(builder.error_type() == error);
  CHECK(builder.types()[error].tag == TypeTag::Error);
  CHECK(never != error);

  // A later ask still finds the first, even once the table has grown past
  // where it was answered.
  builder.reference_type(builder.primitive(TypeTag::I32), false);
  builder.primitive(TypeTag::I1);
  CHECK(builder.never_type() == never);
  CHECK(builder.error_type() == error);
}

TEST_CASE("Rollback removes the interned reference types it added") {
  StorageBuilder builder;
  const TypeIdx i32 = builder.primitive(TypeTag::I32);
  const TypeIdx u8 = builder.primitive(TypeTag::U8);
  const StorageBuilder::Checkpoint cp = builder.checkpoint();
  const TypeIdx speculative = builder.reference_type(i32, false);
  CHECK(speculative.is_valid());
  const usize types_with_speculation = builder.state().types.size();

  builder.rollback(cp);
  CHECK(builder.state().types.size() == types_with_speculation - 1);

  const TypeIdx ref_u8 = builder.reference_type(u8, false);
  const TypeNode& u8_node = builder.types()[ref_u8];
  CHECK(u8_node.tag == TypeTag::Ref);
  CHECK(builder.state().ref_types[u8_node.as_ref()].pointee == u8);

  const TypeIdx ref_i32 = builder.reference_type(i32, false);
  const TypeNode& i32_node = builder.types()[ref_i32];
  CHECK(builder.state().ref_types[i32_node.as_ref()].pointee == i32);
  CHECK(std::move(builder).build().is_ok());
}

TEST_CASE("Rollback rebuilds every index derived from the type table") {
  StorageBuilder builder;
  const TypeIdx i32 = builder.primitive(TypeTag::I32);
  ir::TypeSeq elements;
  elements.push(i32);
  const TypeIdx live_ref = builder.reference_type(i32, false);
  const TypeIdx live_mut = builder.reference_type(i32, true);
  const TypeIdx live_raw = builder.raw_pointer_type(i32, false);
  const TypeIdx live_raw_mut = builder.raw_pointer_type(i32, true);
  const TypeIdx live_param = builder.param_type(7);
  const TypeIdx live_tuple = builder.tuple_type(elements.finish());
  const TypeIdx live_const = builder.const_type(42);
  const TypeIdx live_error = builder.error_type();
  const StorageBuilder::Checkpoint cp = builder.checkpoint();

  // A duplicate node shares the live reference's payload: dropping the
  // speculative nodes must not drop the entry that names `live_ref`.
  builder.ref_type(live_ref);
  CHECK(builder.raw_pointer_type(i32, false).is_valid());
  CHECK(builder.raw_pointer_type(i32, true).is_valid());
  CHECK(builder.const_type(99).is_valid());
  CHECK(builder.slice_type(i32).is_valid());
  CHECK(builder.never_type().is_valid());

  builder.rollback(cp);
  CHECK(builder.state().types.size() == cp.types);
  CHECK(builder.reference_type(i32, false) == live_ref);
  CHECK(builder.reference_type(i32, true) == live_mut);
  CHECK(builder.raw_pointer_type(i32, false) == live_raw);
  CHECK(builder.raw_pointer_type(i32, true) == live_raw_mut);
  CHECK(builder.param_type(7) == live_param);
  ir::TypeSeq again;
  again.push(i32);
  CHECK(builder.tuple_type(again.finish()) == live_tuple);
  CHECK(builder.const_type(42) == live_const);
  CHECK(builder.never_type().is_valid());
  CHECK(builder.error_type() == live_error);
  CHECK(std::move(builder).build().is_ok());
}

TEST_CASE("A reference shape is interned once") {
  StorageBuilder builder;
  const TypeIdx i32 = builder.primitive(TypeTag::I32);
  const TypeIdx shared = builder.reference_type(i32, false);
  const TypeIdx mutable_ref = builder.reference_type(i32, true);
  // Structural interning is what makes type equality index equality, so
  // the same shape asked twice must be one type and the two reference
  // flavours must stay apart.
  CHECK(builder.reference_type(i32, false) == shared);
  CHECK(builder.types()[shared].tag == TypeTag::Ref);
  CHECK(builder.types()[mutable_ref].tag == TypeTag::MutRef);
  CHECK(shared != mutable_ref);
}

TEST_CASE("A const argument is interned by value") {
  StorageBuilder builder;
  const TypeIdx four = builder.const_type(4);
  const TypeIdx eight = builder.const_type(8);
  CHECK(builder.types()[four].tag == TypeTag::Const);
  // One definition interned for 4 cannot serve 8, so the two arguments
  // must stay distinct types the way two different field lists do.
  CHECK(builder.const_type(4) == four);
  CHECK(four != eight);
  CHECK(builder.const_types()[builder.types()[four].as_const()].value == 4);
  CHECK(builder.const_types()[builder.types()[eight].as_const()].value == 8);
  CHECK(std::string(type_to_str(TypeTag::Const)) == "const");
}

}  // namespace

}  // namespace ir
