// A debug build checks the source `+`; the wrap a release build gives
// is this abort (ADR-0065).
fn main() -> i32 {
  a := 2147483647i32
  ret a + 1
}
