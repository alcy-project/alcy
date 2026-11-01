// Reading a reference-typed field yields the reference the field
// stores, so `r` names the buffer `x.b` points at - not the field
// itself. Writing through `*x.b` while `r` is live changes what
// `r` reads, which a shared borrow's read-only promise rules out.
//
// No function boundary is needed: the loan the field's copy carries
// is never charged to the place `*x.b` writes. A shared receiver
// reaches the same hole from the other side - it may be called any
// number of times, so two `&mut` to one buffer live at once.

struct W {
  b: &mut i32,
}

fn main() -> i32 {
  mut n := 1
  mut x := W { b: &mut n }
  r := x.b
  *x.b = 9
  ret *r
}
