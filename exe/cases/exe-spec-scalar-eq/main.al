// Scalars implement the sealed `PartialEq`/`Eq` specs (ADR-0053):
// `.eq` reaches the core implementation for integers, floats, and
// `bool`, while `==` and `!=` keep the builtin comparison.

fn main() -> i32 {
  // `i32`.
  a := 3
  b := 3
  c := 4
  if !(a.eq(&b)) {
    ret 1
  }
  if a.eq(&c) {
    ret 2
  }
  if !(a == b) {
    ret 3
  }
  if a != b {
    ret 4
  }
  if !(a != c) {
    ret 5
  }

  // `u8`.
  d := 200 as u8
  e := 200 as u8
  f := 199 as u8
  if !(d.eq(&e)) {
    ret 6
  }
  if d.eq(&f) {
    ret 7
  }
  if d == f {
    ret 8
  }
  if !(d != f) {
    ret 9
  }

  // `usize` and `isize` are the pointer-width integer spellings.
  g := 7 as usize
  h := 7 as usize
  i := 8 as usize
  if !(g.eq(&h)) {
    ret 10
  }
  if g.eq(&i) {
    ret 11
  }
  j := -5 as isize
  k := -5 as isize
  l := 5 as isize
  if !(j.eq(&k)) {
    ret 12
  }
  if j.eq(&l) {
    ret 13
  }

  // `f64` has `PartialEq` only; the builtin operators stay builtin.
  x := 1.5
  y := 1.5
  z := 2.5
  if !(x.eq(&y)) {
    ret 14
  }
  if x.eq(&z) {
    ret 15
  }
  if !(x == y) {
    ret 16
  }
  if x != y {
    ret 17
  }
  if !(x != z) {
    ret 18
  }

  // `bool`.
  t := true
  u := true
  v := false
  if !(t.eq(&u)) {
    ret 19
  }
  if t.eq(&v) {
    ret 20
  }
  if t == v {
    ret 21
  }
  if !(t != v) {
    ret 22
  }
  ret 0
}
