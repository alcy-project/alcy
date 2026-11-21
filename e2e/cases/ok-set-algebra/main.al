// `union_with` and `intersection` build a new set from two; both inputs
// are consumed, so a caller makes fresh sets for each check.

fn pair() -> (HashSet<String>, HashSet<String>) {
  mut a := HashSet::<String>::new()
  _ := a.insert(String::from("x"))
  _ := a.insert(String::from("y"))
  mut b := HashSet::<String>::new()
  _ := b.insert(String::from("y"))
  _ := b.insert(String::from("z"))
  ret (a, b)
}

fn main() -> i32 {
  (a, b) := pair()
  u := a.union_with(b)
  if u.len() != 3 {
    ret 1
  }
  if !u.contains(String::from("x")) {
    ret 2
  }
  if !u.contains(String::from("z")) {
    ret 3
  }

  (c, d) := pair()
  i := c.intersection(d)
  if i.len() != 1 {
    ret 4
  }
  if !i.contains(String::from("y")) {
    ret 5
  }

  (e, f) := pair()
  empty := HashSet::<String>::new()
  if empty.union_with(e).len() != 2 {
    ret 6
  }
  (g, h) := pair()
  if !empty2().intersection(g).is_empty() {
    ret 7
  }
  (j, k) := pair()
  if !j.intersection(k).is_subset(&u) {
    ret 8
  }
  ret 0
}

fn empty2() -> HashSet<String> {
  ret HashSet::<String>::new()
}
