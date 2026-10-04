struct Res { buf: &mut MaybeUninit<u8> }

impl Res {
  fn new() -> Res { ret Res { buf: alloc::<u8>(4) } }
  fn drop(self: Res) {
    print("drop-res\n")
    dealloc(self.buf, 1 as usize)
  }
}

enum E { Ok(i32), Err(Res) }

impl E {
  fn drop(self: E) {
    print("drop-E\n")
  }
}

// `?` on the error path hands the scrutinee to the caller, so the local
// must not be ended there; and what the success path declared must still
// end when the function returns.
fn f(e: E) -> E {
  r := Res::new()
  v := e?
  ret E::Ok(v)
}

fn main() -> i32 {
  a := f(E::Ok(9))
  print("mid\n")
  b := f(E::Err(Res::new()))
  print("end\n")
  ret 0
}
