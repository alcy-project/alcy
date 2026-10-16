// The reservation activates at the call: a read that is still live
// there - a closure that captured the place - conflicts.
struct S {
  n: i32,
}

impl S {
  pub fn step(mut self: &mut Self, f: (i32) -> i32) -> i32 {
    ret f(0) + self.n
  }
}

fn main() -> i32 {
  mut s := S { n: 1 }
  r :=
    s
    .step(
      [&s](_: i32) ->
        {
          ret s.n + 4
        }
    )
  ret r - 6
}
