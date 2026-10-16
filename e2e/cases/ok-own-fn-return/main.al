// A return-position owning function type infers its witness from the
// body's first `ret`: a closure owns its environment, a named function
// carries its type, and a parameter's witness can be forwarded. A
// method answers the same way.
fn adder(base: i32) -> fn (i32) -> i32 {
  ret [base](x: i32) -> x + base
}

fn inc(x: i32) -> i32 {
  ret x + 1
}

fn passthrough(f: fn (i32) -> i32) -> fn (i32) -> i32 {
  ret f
}

struct Maker {
  base: i32,
}

impl Maker {
  fn make(self: &Self) -> fn (i32) -> i32 {
    base := self.base
    ret [base](a: i32) -> a + base
  }
}

fn main() -> i32 {
  a := adder(3)
  if a(4) != 7 {
    ret 1
  }
  b := passthrough(inc)
  if b(1) != 2 {
    ret 2
  }
  m := Maker { base: 21 }
  f := m.make()
  if f(1) != 22 {
    ret 3
  }
  ret 0
}
