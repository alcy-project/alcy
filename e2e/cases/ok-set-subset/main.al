// `is_subset` reads every key against the other set.
fn main() -> i32 {
  mut a := Set::new()
  _ := a.insert("x")
  mut b := Set::new()
  _ := b.insert("x")
  _ := b.insert("y")
  if !a.is_subset(&b) { ret 1 }
  if b.is_subset(&a) { ret 2 }
  mut e := Set::new()
  if !e.is_subset(&a) { ret 3 }
  if !a.is_subset(&a) { ret 4 }
  ret 0
}
