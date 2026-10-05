struct Res { buf: &mut MaybeUninit<u8> }

impl Res {
  fn drop(self: Res) {
    unsafe { dealloc(self.buf, 1 as usize) }
  }
}

fn main() {
  r := Res { buf: unsafe { alloc::<u8>(4) } }
  _ := r
}
