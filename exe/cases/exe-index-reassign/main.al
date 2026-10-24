// A read through an index runs before the assignment place asks for
// its exclusive borrow, so `v[i] = v[i] + 1` and the compound form
// compile.

fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  v.push(10i32)
  v[0] = v[0] + 1
  if *v.at(0).or_panic_with_message("expected a value") != 2 {
    ret 1
  }
  v[1] = v[0] + v[1]
  if *v.at(1).or_panic_with_message("expected a value") != 12 {
    ret 2
  }
  v[1] += 5
  if *v.at(1).or_panic_with_message("expected a value") != 17 {
    ret 3
  }
  mut m := Map::<i32>::new()
  _ := m.insert("k", 1)
  m["k"] = m["k"] + 1
  if *m.get("k").or_panic_with_message("expected a value") != 2 {
    ret 4
  }
  m["k"] += 3
  if *m.get("k").or_panic_with_message("expected a value") != 5 {
    ret 5
  }
  ret 0
}
