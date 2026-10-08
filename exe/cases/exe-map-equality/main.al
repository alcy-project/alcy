// `Map` equality compares live entries: the same keys with values that
// compare through `PartialEq` (ADR-0053).

fn main() -> i32 {
  mut a := Map::<i32>::new()
  _ := a.insert("x", 1)
  _ := a.insert("y", 2)

  mut b := Map::<i32>::new()
  _ := b.insert("x", 1)
  _ := b.insert("y", 2)
  if !(a == b) {
    ret 1
  }
  b["y"] = 5
  if a == b {
    ret 2
  }
  b["y"] = 2
  if !(a == b) {
    ret 3
  }

  // Insertion order does not matter.
  mut c := Map::<i32>::new()
  _ := c.insert("y", 2)
  _ := c.insert("x", 1)
  if !(a == c) {
    ret 4
  }

  mut short := Map::<i32>::new()
  _ := short.insert("x", 1)
  if a == short {
    ret 5
  }

  mut other := Map::<i32>::new()
  _ := other.insert("x", 1)
  _ := other.insert("z", 2)
  if a == other {
    ret 6
  }
  ret 0
}
