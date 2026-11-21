// An entry fills an absent key in one call.
fn main() -> i32 {
  mut m := HashMap::<String, i32>::new()
  {
    e := m.entry(String::from("a"))
    v := e.or_insert(1)
    *v = *v + 1
  }
  if *m.get(String::from("a")).or_panic_with_message("expected a value") != 2 {
    ret 1
  }
  if m.len() != 1 {
    ret 2
  }
  {
    e := m.entry(String::from("a"))
    v := e.or_insert(9)
    if *v != 2 {
      ret 3
    }
  }
  if *m.get(String::from("a")).or_panic_with_message("expected a value") != 2 {
    ret 4
  }
  {
    e := m.entry(String::from("b"))
    v := e.or_insert(5)
    if *v != 5 {
      ret 5
    }
  }
  if *m.get(String::from("b")).or_panic_with_message("expected a value") != 5 {
    ret 6
  }
  if m.len() != 2 {
    ret 7
  }
  ret 0
}
