// `Set` equality compares membership only: insertion order does not
// matter, and lengths and keys must match (ADR-0053).
fn main() -> i32 {
  mut a := Set::new()
  _ := a.insert("x")
  _ := a.insert("y")

  mut b := Set::new()
  _ := b.insert("y")
  _ := b.insert("x")
  if !(a == b) {
    ret 1
  }

  mut short := Set::new()
  _ := short.insert("x")
  if a == short {
    ret 2
  }

  mut other := Set::new()
  _ := other.insert("x")
  _ := other.insert("z")
  if a == other {
    ret 3
  }

  mut left := Set::new()
  mut right := Set::new()
  if !(left == right) {
    ret 4
  }
  _ := left.insert("x")
  if left == right {
    ret 5
  }

  _ := a.remove("x")
  _ := b.remove("x")
  if !(a == b) {
    ret 6
  }
  ret 0
}
