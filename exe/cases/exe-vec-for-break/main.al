// A loop that stops early ends the elements it never took.

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
  mut v: Vec<D> := Vec::<D>::new()
  v.push(make())
  v.push(make())
  v.push(make())
  mut n := 0
  for d in v {
    n = n + 1
    if n == 2 { break }
  }
  ret 0
}
