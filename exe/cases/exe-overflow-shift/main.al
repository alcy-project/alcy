// A shift amount at or past the width is checked in a debug build.
fn main() -> i32 {
  n := 33i32
  ret 1i32 << n
}
