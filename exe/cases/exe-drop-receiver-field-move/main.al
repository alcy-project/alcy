// A destructor's body moves a field of its receiver into a binding and
// ends another in place: each value ends once, and the binding ends what
// it took where the body's scope exits (docs/spec/values.md, ADR-0137).

struct Res {
  buf: &mut MaybeUninit<u8>,
}

impl Res {
  fn new() -> Res {
    ret Res { buf: unsafe { alloc::<u8>(1) } }
  }

  fn drop(self: Res) {
    print("res\n")
    unsafe { dealloc(self.buf, 1 as usize) }
  }
}

struct Holder {
  first: Res,
  second: Res,
}

impl Holder {
  fn drop(self: Holder) {
    print("holder\n")
    moved := self.first
    unsafe { drop_in_place::<Res>(&mut self.second) }
    print("body done\n")
  }
}

fn main() -> i32 {
  h := Holder { first: Res::new(), second: Res::new() }
  print("built\n")
  ret 0
}
