// Reading a reference-typed field twice yields the reference the
// field stores both times, so `r` and `s` name one buffer while the
// field is not written between them. Two live `&mut` to one place is
// what the borrow rule refuses.

struct W {
  b: &mut i32,
}

fn main() -> i32 {
  mut n := 1
  mut x := W { b: &mut n }
  r := x.b
  s := x.b
  *r = 1
  *s = 2
  ret *r
}
