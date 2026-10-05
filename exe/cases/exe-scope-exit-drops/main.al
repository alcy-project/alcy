struct D {
  buf: &mut MaybeUninit<u8>
}

impl D {
  fn drop(self: D) {
    println("drop")
    unsafe { dealloc(self.buf, 4 as usize) }
  }
}

fn sink(x: &D) {}

enum R {
  Ok(i32),
  Err(i32)
}

fn propagates(r: R) -> R {
  d := D { buf: unsafe { alloc::<u8>(4) } }
  sink(&d)
  v := r?
  ret R::Ok(v)
}

fn main() -> i32 {
  // `break` out of a loop body must end what the body declared.
  loop {
    d := D { buf: unsafe { alloc::<u8>(4) } }
    sink(&d)
    break
  }
  // `continue` restarts the body, so each pass ends its own value.
  mut i := 0
  while i < 2 {
    d := D { buf: unsafe { alloc::<u8>(4) } }
    sink(&d)
    i = i + 1
    continue
  }
  // `?` propagating out is a return, so it ends the live values too.
  r := propagates(R::Err(0))
  m := match r { R::Ok(x) => x, R::Err(e) => e }
  ret m
}
