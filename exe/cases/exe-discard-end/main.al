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
  ret 0
}
