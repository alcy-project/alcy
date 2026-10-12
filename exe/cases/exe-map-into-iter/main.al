// A loop that stops early ends the entries it never took.

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
  mut m: Map<D> := Map::<D>::new()
  _ := m.insert("a", make())
  _ := m.insert("b", make())
  mut n := 0
  for (k, v) in m {
    n = n + 1
    if n == 1 {
      break
    }
  }
  ret 0
}
