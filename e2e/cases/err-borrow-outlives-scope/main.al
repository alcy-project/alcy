// A borrow of a local declared inside a block, stored into a binding
// declared outside it, would outlive its referent: the slot ends with
// the block while the destination keeps reading it. The store is
// refused through a struct field as well as through a plain binding.
struct S {
  p: &i32,
}

fn main() -> i32 {
  mut s := S { p: &0 }
  {
    n := 5
    s.p = &n
  }
  ret *s.p
}
