// A raw pointer is thin, so a slice cannot be its pointee.
fn main() {
  _ := 0 as *[i32]
}
