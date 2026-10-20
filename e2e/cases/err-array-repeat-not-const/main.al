// A repeat count must be comp-known: a runtime local is not a length.
fn main() -> i32 {
  n := 4
  a: [u8; 4] := [0u8; n]
  ret a.len() as i32
}
