struct Res { buf: &mut MaybeUninit<u8> }

impl Res {
  fn new() -> Res { ret Res { buf: alloc::<u8>(4) } }
  fn drop(self: Res) {
    print("res\n")
    dealloc(self.buf, 1 as usize)
  }
}

struct Holder { inner: Res, tag: i32 }

struct G<T> { item: T, raw: &mut MaybeUninit<u8> }

impl<T> G<T> {
  fn wrap(v: T) -> G<T> { ret G { item: v, raw: alloc::<u8>(1) } }
  fn drop(self: G<T>) {
    print("gen\n")
    dealloc(self.raw, 1 as usize)
  }
}

fn early(n: i32) -> i32 {
  a := Res::new()
  if n == 1 {
    ret 7
  }
  b := Res::new()
  ret 0
}

fn main() -> i32 {
  h := Holder { inner: Res::new(), tag: 1 }
  _ := h.tag
  print("made-holder\n")
  g := G::<i32>::wrap(5)
  _ := g.item
  print("made-gen\n")
  if early(1) != 7 {
    ret 1
  }
  print("early-done\n")
  ret 0
}
