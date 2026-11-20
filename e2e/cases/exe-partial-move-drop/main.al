// Moving a droppable field out of an aggregate leaves the aggregate's
// other fields intact: the moved field ends with its new owner and the
// one left behind ends at the scope exit, once each.
struct Res {
  buf: &mut MaybeUninit<u8>,
}

impl Res {
  fn new() -> Res {
    ret Res { buf: unsafe { alloc::<u8>(1) } }
  }

  fn drop(self: Res) {
    print("res\n")
    unsafe { dealloc(self.buf, 1 as usize) }
  }
}

struct Holder {
  a: Res,
  b: Res,
}

fn main() -> i32 {
  h := Holder { a: Res::new(), b: Res::new() }
  x := h.a
  print("moved\n")
  ret 0
}
