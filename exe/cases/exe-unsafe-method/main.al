// An unsafe method is declared like a free unsafe function; a call
// needs the gate.
struct S {
  n: i32,
}

impl S {
  unsafe fn poke(mut self: &mut Self) {
    self.n = 2
  }
}

fn main() -> i32 {
  mut s := S { n: 1 }
  unsafe { s.poke() }
  if s.n != 2 {
    ret 1
  }
  ret 0
}
