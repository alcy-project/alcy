// A run is a view, not a place: assigning to one has no address to
// write through.
fn main() -> i32 {
  mut a := [10i32, 20i32, 30i32]
  mut s := &mut a[..]
  s[0..<2] = &a[..]
  ret s[0]
}
