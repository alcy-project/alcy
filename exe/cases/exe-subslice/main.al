// Sub-slicing names a run of a container with a range: `&a[1..<3]` is
// the middle two elements, re-slicing narrows a view, and `str`
// narrows the same way. An exclusive borrow of a run writes through
// to the array behind it.
fn main() -> i32 {
  mut bad := 0
  mut a := [10i32, 20i32, 30i32, 40i32, 50i32]
  v := &a[1..<4]
  if slice_len(v) != 3 {
    bad = 1
  }
  if v[0] != 20 {
    bad = 2
  }
  if v[2] != 40 {
    bad = 2
  }
  r := 1..<3
  w := &a[r]
  if slice_len(w) != 2 {
    bad = 3
  }
  if w[1] != 30 {
    bad = 3
  }
  g := &a[..]
  if slice_len(g) != 5 {
    bad = 4
  }
  if g[4] != 50 {
    bad = 4
  }
  h := &a[2..=3]
  if slice_len(h) != 2 {
    bad = 5
  }
  if h[0] != 30 {
    bad = 5
  }
  t := &a[3..]
  if slice_len(t) != 2 {
    bad = 6
  }
  if t[1] != 50 {
    bad = 6
  }
  u := &a[..<2]
  if slice_len(u) != 2 {
    bad = 7
  }
  if u[0] != 10 {
    bad = 7
  }
  e := &a[2..<2]
  if slice_len(e) != 0 {
    bad = 8
  }
  // Re-slicing a view narrows it without touching the array.
  x := g[1..<4]
  if slice_len(x) != 3 {
    bad = 9
  }
  if x[2] != 40 {
    bad = 9
  }
  y := x[1..]
  if slice_len(y) != 2 {
    bad = 10
  }
  if y[0] != 30 {
    bad = 10
  }
  mut m := &mut a[0..<2]
  m[0] = 99i32
  if m[1] != 20 {
    bad = 11
  }
  if a[0] != 99 {
    bad = 11
  }
  // A `str` narrows to a `str`.
  mut s := String::new()
  s.push(104u8)
  s.push(105u8)
  s.push(106u8)
  s.push(107u8)
  t2 := s.as_str()[1..<3]
  if str_len(t2) != 2 {
    bad = 12
  }
  if str_byte(t2, 0) != 105u8 {
    bad = 13
  }
  if str_byte(t2, 1) != 106u8 {
    bad = 13
  }
  u2 := s.as_str()[2..]
  if str_len(u2) != 2 {
    bad = 14
  }
  if str_byte(u2, 1) != 107u8 {
    bad = 14
  }
  ret bad
}
