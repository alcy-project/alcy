use acme_limits::limits::Limits;

fn main() -> i32 {
  if Limits::N != 5i32 {
    ret 1
  }
  values: [i32; Limits::N] := [1i32, 2i32, 3i32, 4i32, 5i32]
  if values[4] != 5i32 {
    ret 2
  }
  ret 0
}
