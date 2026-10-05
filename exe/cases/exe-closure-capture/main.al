fn main() -> i32 {
  // A moved copy keeps the value it saw; a shared borrow reads the
  // live one; an exclusive borrow writes through. v is read only
  // through the closure, which is what the exclusive borrow means.
  mut t := 1
  u := 10
  mut v := 100
  f := [t, &u, &mut v] (a: i32) -> {
    v = v + a
    a + t + u + v
  }
  t = 50
  if f(1) != 113 { ret 1 }
  if f(1) != 114 { ret 2 }
  ret 0
}
