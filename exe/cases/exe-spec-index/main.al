// Indexing reaches `Vec`, `String`, and `Map` through the sealed
// `Index`/`IndexMut` specs (ADR-0053).

fn bump(mut v: &mut Vec<i32>) {
  x := v[0]
  v[0] = x + 1
}

fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(40)
  v.push(1)
  bump(&mut v)
  if v[0] != 41 {
    ret 1
  }
  if v[0] + v[1] != 42 {
    ret 2
  }
  x := v[1]
  v[0] = x
  if v[0] != 1 {
    ret 3
  }
  mut q := &mut v[0]
  *q = 7
  if v[0] != 7 {
    ret 4
  }

  mut m := Map::<i32>::new()
  _ := m.insert("a", 1)
  _ := m.insert("b", 2)
  if m["a"] + m["b"] != 3 {
    ret 5
  }
  m["b"] = 5
  if m["b"] != 5 {
    ret 6
  }

  mut s := String::new()
  s.push_str("hi")
  if s[0] as i32 != 104 {
    ret 7
  }
  s[1] = 105
  if s[1] as i32 != 105 {
    ret 8
  }
  ret 0
}
