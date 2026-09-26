struct Res { buf: &mut MaybeUninit<u8> }

impl Res {
  fn drop(self: Res) {
    dealloc(self.buf, 1 as usize)
  }
}

fn main() {
  r := Res { buf: alloc::<u8>(4) }
  _ := r
}
