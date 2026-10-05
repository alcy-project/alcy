// `&T` never becomes a writable pointer: the cast would forge the
// exclusive half the reference does not have.
fn main() {
  mut x := 1
  _ := &x as *mut i32
}
