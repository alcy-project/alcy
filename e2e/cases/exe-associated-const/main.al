// An associated const is a compile-time value under its owner type: it
// names an array length and reads as an ordinary value.
struct Limits { x: i32 }

impl Limits {
  pub const N: i32 := 4i32
}

const DOUBLE: i32 := Limits::N * 2i32

fn main() -> i32 {
  values: [i32; Limits::N] := [1i32, 2i32, 3i32, 4i32]
  if Limits::N != 4i32 {
    ret 1
  }
  if values[3] != 4i32 {
    ret 2
  }
  if DOUBLE != 8i32 {
    ret 3
  }
  ret 0
}
