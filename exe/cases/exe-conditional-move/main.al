// A value consumed on one arm and not the other is ended on the path
// that still owns it, and the borrow check accepts the end a guard
// placed.

struct D {
  buf: &mut MaybeUninit<u8>,
}

impl D {
  fn drop(self: D) {
    println("drop")
    unsafe { dealloc(self.buf, 1 as usize) }
  }
}

fn take(d: D) {}

fn main() -> i32 {
  d := D { buf: unsafe { alloc::<u8>(1) } }
  c := true
  if c {
    take(d)
  }
  println("after")
  ret 0
}
