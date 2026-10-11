// A live variant's payload ends when the enum does.

struct D {
  buf: &mut MaybeUninit<u8>
}

impl D {
  fn drop(self: D) {
    println("drop")
    unsafe { dealloc(self.buf, 1 as usize) }
  }
}

enum Holder {
  One(D),
  Two(i32),
}

fn make() -> D { ret D { buf: unsafe { alloc::<u8>(1) } } }

fn main() -> i32 {
  h := Holder::One(make())
  ret 0
}
