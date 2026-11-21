// `is_subset` reads every key against the other set.
fn main() -> i32 {
  mut a := HashSet::<String>::new()
  _ := a.insert(String::from("x"))
  mut b := HashSet::<String>::new()
  _ := b.insert(String::from("x"))
  _ := b.insert(String::from("y"))
  if !a.is_subset(&b) {
    ret 1
  }
  if b.is_subset(&a) {
    ret 2
  }
  mut e := HashSet::<String>::new()
  if !e.is_subset(&a) {
    ret 3
  }
  if !a.is_subset(&a) {
    ret 4
  }
  ret 0
}
