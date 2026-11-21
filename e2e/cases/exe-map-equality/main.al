// `Map` equality compares live entries: the same keys with values that
// compare through `PartialEq`.

fn main() -> i32 {
  mut a := HashMap::<String, i32>::new()
  _ := a.insert(String::from("x"), 1)
  _ := a.insert(String::from("y"), 2)

  mut b := HashMap::<String, i32>::new()
  _ := b.insert(String::from("x"), 1)
  _ := b.insert(String::from("y"), 2)
  if !(a == b) {
    ret 1
  }
  b[String::from("y")] = 5
  if a == b {
    ret 2
  }
  b[String::from("y")] = 2
  if !(a == b) {
    ret 3
  }

  // Insertion order does not matter.
  mut c := HashMap::<String, i32>::new()
  _ := c.insert(String::from("y"), 2)
  _ := c.insert(String::from("x"), 1)
  if !(a == c) {
    ret 4
  }

  mut short := HashMap::<String, i32>::new()
  _ := short.insert(String::from("x"), 1)
  if a == short {
    ret 5
  }

  mut other := HashMap::<String, i32>::new()
  _ := other.insert(String::from("x"), 1)
  _ := other.insert(String::from("z"), 2)
  if a == other {
    ret 6
  }
  ret 0
}
