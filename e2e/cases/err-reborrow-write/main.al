// A place reached through a reference has to be nameable, or a write
// through it is invisible to a loan of the place it came from. See
// docs/adr/0012-reborrow-on-reference-read.md.
fn g(mut b: &mut i32) -> i32 {
  x := &*b
  *b = 1
  ret *x
}

fn main() {
  mut n := 0
  _ := g(&mut n)
}
