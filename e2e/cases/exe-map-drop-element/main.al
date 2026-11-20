// `Map::drop` ends the live values: each live pair moves out of its
// slot into a binding the drop pass ends, and the map owns its keys as
// Strings. The sanitized run checks that nothing is left allocated.

struct D {
  buf: &mut MaybeUninit<u8>,
}

impl D {
  fn drop(self: D) {
    println("drop")
    unsafe { dealloc(self.buf, 1 as usize) }
  }
}

fn make() -> D {
  ret D { buf: unsafe { alloc::<u8>(1) } }
}

fn main() {
  mut m := Map::<D>::new()
  _ := m.insert("a", make())
  _ := m.insert("b", make())
  println("exit")
}
