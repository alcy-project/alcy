// A debug build checks the source `+`; the wrap a release build gives
// is this abort.
fn main() -> i32 {
  a := 2147483647i32
  ret a + 1
}
