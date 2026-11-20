// `Box<T>` owns its pointee on the heap: the handle is move-only, the
// destructor ends the pointee and releases the block, and `into_inner`
// moves the pointee out, which is what a recursive walk needs.

enum List {
  Cons(i32, Box<List>),
  Nil,
}

impl List {
  // A consuming walk: `into_inner` moves each link's pointee out, and
  // the spent link it leaves behind ends nothing.
  fn sum(self: List) -> i32 {
    ret match self {
      List::Cons(v, rest) => v + rest.into_inner().sum(),
      List::Nil => 0,
    }
  }
}

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

  l := List::Cons(1, Box::<List>::new(List::Cons(2, Box::<List>::new(List::Nil))))
  if l.sum() != 3 {
    ret 4
  }
  println("{}", List::Cons(5, Box::<List>::new(List::Cons(6, Box::<List>::new(List::Nil)))).sum())

  inner := b.into_inner()
  if inner != 8 {
    ret 5
  }
  ret 0
}
