// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/storage_builder.h"

#include "doctest/doctest.h"
#include "ir/common.h"
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

}  // namespace

}  // namespace ir
