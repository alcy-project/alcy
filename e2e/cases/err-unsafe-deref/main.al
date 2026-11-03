// Dereferencing a raw pointer is an operation the opt-in covers.
fn main() {
  p := 0 as *i32
  _ := *p
}
