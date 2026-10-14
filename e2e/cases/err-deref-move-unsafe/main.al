// An ordinary `unsafe` function returns a borrow like any other: the
// gate is a spelling, not a second move rule (ADR-0050). The
// allocator's own accessors are the intrinsics that hand out storage
// the caller owns.
unsafe fn alias(p: &mut String) -> &mut String {
  ret p
}

fn main() {
  mut s := String::new()
  s.push_str("hi")
  w := unsafe { *alias(&mut s) }
  _ := w
  ret
}
