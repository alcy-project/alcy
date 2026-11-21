// `Set` equality compares membership only: insertion order does not
// matter, and lengths and keys must match.
fn main() -> i32 {
  mut a := HashSet::<String>::new()
  _ := a.insert(String::from("x"))
  _ := a.insert(String::from("y"))

  mut b := HashSet::<String>::new()
  _ := b.insert(String::from("y"))
  _ := b.insert(String::from("x"))
  if !(a == b) {
    ret 1
  }

  mut short := HashSet::<String>::new()
  _ := short.insert(String::from("x"))
  if a == short {
    ret 2
  }

  mut other := HashSet::<String>::new()
  _ := other.insert(String::from("x"))
  _ := other.insert(String::from("z"))
  if a == other {
    ret 3
  }

  mut left := HashSet::<String>::new()
  mut right := HashSet::<String>::new()
  if !(left == right) {
    ret 4
  }
  _ := left.insert(String::from("x"))
  if left == right {
    ret 5
  }

  _ := a.remove(String::from("x"))
  _ := b.remove(String::from("x"))
  if !(a == b) {
    ret 6
  }
  ret 0
}
