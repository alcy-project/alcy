// `get_or_insert` borrows the entry it just made, so a caller writes
// through it; a value for a key already present is dropped.

fn main() -> i32 {
  mut m := Map::<i32>::new()
  {
    mut v := m.get_or_insert("a", 10)
    if *v != 10 {
      ret 1
    }
    *v = *v + 1
  }
  if *m.get("a").or_panic_with_message("expected a value") != 11 {
    ret 2
  }
  {
    mut w := m.get_or_insert("a", 99)
    if *w != 11 {
      ret 3
    }
  }
  mut n := Map::<String>::new()
  {
    mut x := n.get_or_insert("k", format("abc"))
    if x.len() != 3 {
      ret 4
    }
  }
  {
    mut y := n.get_or_insert("k", format("zz"))
    if y.len() != 3 {
      ret 5
    }
  }
  ret 0
}
