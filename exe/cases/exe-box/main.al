// `Box<T>` owns its pointee on the heap: the handle is move-only, and
// the destructor ends the pointee and releases the block.

struct Noisy {
  n: i32,
}

impl Noisy {
  fn drop(self: Noisy) {
    println("drop {}", self.n)
  }
}

fn main() -> i32 {
  mut b := Box::<i32>::new(7)
  if *b.get() != 7 {
    ret 1
  }
  mut room := b.get_mut()
  *room = 8
  if *b.get() != 8 {
    ret 2
  }
  println("{}", *b.get())

  boxed := Box::<Noisy>::new(Noisy { n: 3 })
  if boxed.get().n != 3 {
    ret 3
  }

  mut v: Vec<Box<Noisy>> := Vec::<Box<Noisy>>::new()
  v.push(Box::<Noisy>::new(Noisy { n: 1 }))
  v.push(Box::<Noisy>::new(Noisy { n: 2 }))
  v.clear()
  println("cleared")
  ret 0
}
