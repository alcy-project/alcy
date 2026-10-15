// A condition pattern that always matches is refused: the branch would
// enter whatever the initializer holds.
fn main() {
  if x := 1 > 2 {
    _ := x
  }
}
