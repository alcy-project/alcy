// A type argument is sized, and a slice is not one.
fn main() {
  _ := Box::<[i32]>::new(1)
}
