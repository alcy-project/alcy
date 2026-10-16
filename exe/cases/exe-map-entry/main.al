// An entry fills an absent key in one call.
fn main() -> i32 {
  mut m := Map::<i32>::new()
  {
    e := m.entry("a")
    v := e.or_insert(1)
    *v = *v + 1
  }
  if *m.get("a").unwrap() != 2 {
    ret 1
  }
  if m.len() != 1 {
    ret 2
  }
  {
    e := m.entry("a")
    v := e.or_insert(9)
    if *v != 2 {
      ret 3
    }
  }
  if *m.get("a").unwrap() != 2 {
    ret 4
  }
  {
    e := m.entry("b")
    v := e.or_insert(5)
    if *v != 5 {
      ret 5
    }
  }
  if *m.get("b").unwrap() != 5 {
    ret 6
  }
  if m.len() != 2 {
    ret 7
  }
  ret 0
}
