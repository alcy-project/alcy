// Every element of an array ends when the array does.

struct D {
  buf: &mut MaybeUninit<u8>,
}

impl D {
  fn drop(self: D) {
    println("drop")
    unsafe { dealloc(self.buf, 1 as usize) }
  }
}

fn make() -> D {
  ret D { buf: unsafe { alloc::<u8>(1) } }
}

fn main() -> i32 {
  a := [make(), make(), make()]
  ret 0
}
