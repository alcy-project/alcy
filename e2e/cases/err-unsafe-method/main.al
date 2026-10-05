// Unsafe methods wait for their own slice; the marker is refused
// rather than read past.
struct S { n: i32 }

impl S {
  unsafe fn poke(self: &Self) {}
}

fn main() {
  s := S { n: 1 }
  _ := s.n
}
