// `for d in a` consumes an array; a break ends what the loop never took.

struct D {
  buf: &mut MaybeUninit<u8>
}

impl D {
  fn drop(self: D) {
    println("drop")
    unsafe { dealloc(self.buf, 1 as usize) }
  }
}

fn make() -> D { ret D { buf: unsafe { alloc::<u8>(1) } } }

fn main() -> i32 {
  a := [make(), make(), make()]
  mut n := 0
  for d in a {
    n = n + 1
    if n == 1 {
      break
    }
  }
  if n != 1 {
    ret 1
  }

  b := [1i32, 2i32, 3i32]
  mut total := 0
  for x in b {
    total = total + x
  }
  if total != 6 {
    ret 2
  }
  ret 0
}
