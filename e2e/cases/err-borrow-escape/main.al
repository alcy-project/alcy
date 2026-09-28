// A reborrow of a parameter has to reach the caller as a loan on the
// caller's own argument, or an accessor that returns `&T` derived from
// `&Self` hands back a pointer nothing records. The store below would
// change what the returned reference reads.
struct Box { n: i32 }

fn get(b: &Box) -> &i32 {
  ret &b.n
}

fn main() -> i32 {
  mut x := Box { n: 1 }
  r := get(&x)
  x.n = 9
  ret *r - 9
}
