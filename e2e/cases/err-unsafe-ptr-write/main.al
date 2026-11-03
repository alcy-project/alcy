// Writing through a raw pointer is an operation the opt-in covers too.
fn main() {
  mut p := 0 as *mut i32
  *p = 1
}
