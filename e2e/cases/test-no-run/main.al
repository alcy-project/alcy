// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

fn main() -> i32 {
  ret 0
}

#[when(testing)] {
  #[test] fn adds() {
    if 1 + 1 != 2 {
      panic("addition")
    }
  }
}
