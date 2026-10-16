// A 64-bit count past the width still aborts under the direct-wasm
// backend: the check compares the count against 64 in the wide shape,
// the mirror of the valid-count case `hash-parity` runs.
fn main() -> i32 {
  n: u64 := 100u64
  x: u64 := 1u64
  _ := x << n
  ret 0
}
