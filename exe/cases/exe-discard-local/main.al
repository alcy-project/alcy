// Discarding a binding ends it where it stands, and the scope exit
// that follows does not end it again.

struct Res {
  buf: &mut MaybeUninit<u8>
}

impl Res {
  fn drop(self: Res) {
    println("drop")
    unsafe { dealloc(self.buf, 1 as usize) }
  }
}

fn main() -> i32 {
  r := Res { buf: unsafe { alloc::<u8>(1) } }
  _ := r
  ret 0
}
