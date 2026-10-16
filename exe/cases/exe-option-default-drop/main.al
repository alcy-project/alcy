// `unwrap_or` hands its default back only when the receiver is `None`.
// On the `Some` path the default still owns a value that has to end
// there, or every `unwrap_or` with an owning fallback leaks it.
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

fn main() -> i32 {
  a: Option<Res> := Option::Some(Res::new())
  d := Res::new()
  print("before\n")
  b := a.unwrap_or(d)
  print("after\n")
  ret 0
}
