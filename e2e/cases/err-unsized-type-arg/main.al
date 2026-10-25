// A type argument is sized, and a slice is not one (ADR-0083).
fn main() {
  _ := Box::<[i32]>::new(1)
}
