// A field cannot move out of a value whose `drop` method is the user's:
// the method would run on a value with a hole where the field was.
struct Res {
  buf: &mut MaybeUninit<u8>,
}

impl Res {
  fn new() -> Res {
    ret Res { buf: unsafe { alloc::<u8>(1) } }
  }

  fn drop(self: Res) {
    unsafe { dealloc(self.buf, 1 as usize) }
  }
}

struct Holder {
  inner: Res,
  tag: i32,
}

impl Holder {
  fn drop(self: Holder) {
    unsafe { dealloc(self.inner.buf, 1 as usize) }
  }
}

fn main() -> i32 {
  h := Holder { inner: Res::new(), tag: 1 }
  _ := h.tag
  x := h.inner
  ret 0
}
