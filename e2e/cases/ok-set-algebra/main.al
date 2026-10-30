// `union_with` and `intersection` build a new set from two, and the
// empty set is the identity on either side of a union.
fn main() -> i32 {
  mut a := Set::new()
  _ := a.insert("x")
  _ := a.insert("y")
  mut b := Set::new()
  _ := b.insert("y")
  _ := b.insert("z")
  u := a.union_with(&b)
  if u.len() != 3 {
    ret 1
  }
  if !u.contains("x") {
    ret 2
  }
  if !u.contains("z") {
    ret 3
  }
  i := a.intersection(&b)
  if i.len() != 1 {
    ret 4
  }
  if !i.contains("y") {
    ret 5
  }
  e := Set::new()
  if e.union_with(&a).len() != 2 {
    ret 6
  }
  if !e.intersection(&a).is_empty() {
    ret 7
  }
  if !a.intersection(&b).is_subset(&a) {
    ret 8
  }
  ret 0
}
