struct Res { buf: &mut MaybeUninit<u8> }

impl Res {
  fn new() -> Res { ret Res { buf: alloc::<u8>(4) } }
  fn drop(self: Res) {
    print("drop\n")
    dealloc(self.buf, 1 as usize)
  }
}

// A branch out of the loop body skips the body's own drop point, so the
// value the body declared must end at the branch, nested block or not.
fn main() -> i32 {
  mut i := 0
  loop {
    x := Res::new()
    i = i + 1
    if i < 3 {
      continue
    }
    break
  }
  ret 0
}
