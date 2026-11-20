// Writing over an owned place ends the value it replaces; a store
// into a moved place re-initializes it.

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
  mut r := make()
  r = make()
  mut i := 0
  while i < 3 {
    i = i + 1
    if i == 2 {
      r.drop()
    }
    r = make()
  }
  ret 0
}
