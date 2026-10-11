// `Vec::clear` and `Vec::drop` end the live elements.

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
  v.clear()
  v.push(make())
  ret 0
}
