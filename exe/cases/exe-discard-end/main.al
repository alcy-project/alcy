// Discarding ends the value: the explicit spelling and the bare one.

struct D {
  buf: &mut MaybeUninit<u8>
}

impl D {
  fn drop(self: D) {
    println("drop")
    unsafe { dealloc(self.buf, 1 as usize) }
  }
}

fn make() -> D { ret D { buf: unsafe { alloc::<u8>(1) } } }

fn main() -> i32 {
  _ := make()
  make()
  // The `fmt` expansion builds its result on the heap, and a discarded
  // one ends like any other value.
  _ := format("x")
  format("y")
  ret 0
}
