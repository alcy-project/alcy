struct Res { buf: &mut MaybeUninit<u8> }

impl Res {
  fn drop(mut self: &mut Self) {
    unsafe { dealloc(self.buf, 1 as usize) }
  }
}

fn main() {
  r := Res { buf: unsafe { alloc::<u8>(4) } }
  _ := r
}
