// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace ast {

// The lane the calling thread appends to.
//
// A node table several parsers fill at once gives each parser a lane of its
// own, so an append moves a cursor only its own thread can move and needs no
// atomic operation. Which lane is a thread's is scheduling rather than syntax,
// so the stage that spreads the work says it once per unit of work and the
// tables read it here rather than being handed it: the parser appends from
// dozens of places and none of them is about threads.
//
// A thread that no stage has spoken for is lane 0, and a table with no lanes
// answers for lane 0 on the path a single parser takes.
//
// The variable is at namespace scope rather than inside the accessor: a
// function-local `static thread_local` in an inline function is one variable
// per thread only where the toolchain agrees on what an inline function's
// local static is, and a toolchain that does not gives two translation units
// two variables and therefore two lanes. Nothing here is a lock, so that
// failure is silent until two parsers write one lane's cursor.
extern thread_local u32 current_lane_storage;

inline void set_current_lane(u32 lane) {
  current_lane_storage = lane;
}

inline u32 current_lane() {
  return current_lane_storage;
}

}  // namespace ast
