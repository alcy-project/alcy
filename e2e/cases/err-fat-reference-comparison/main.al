// A reference to a slice carries its length, so a tuple holding one has
// no direct comparison either.
fn main() -> i32 {
  a := [1i32, 2i32]
  s := a.as_slice()
  if (s, 1i32) == (s, 1i32) {
    ret 0
  }
  ret 1
}
