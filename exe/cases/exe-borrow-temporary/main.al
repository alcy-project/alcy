// A temporary that owns nothing is borrowed where it lies: the slot
// holds it for the reference's life, which is the enclosing
// expression's, so a call may take `&2`, `&(1 + 1)`, or a temporary of
// its own making.

fn takes(p: &i32) -> i32 {
  ret *p + 1
}

spec Sum {
  fn plus(self: &Self, other: &Self) -> i32;
}

struct P { base: i32 }

fn make() -> P {
  ret P { base: 6 }
}

impl Sum for P {
  fn plus(self: &Self, other: &Self) -> i32 {
    ret self.base + other.base
  }
}

fn main() -> i32 {
  a := 2
  if !a.eq(&2) {
    ret 1
  }
  if takes(&7) != 8 {
    ret 2
  }
  if takes(&(3 + 4)) != 8 {
    ret 3
  }
  p := P { base: 5 }
  if p.plus(&make()) != 11 {
    ret 4
  }
  println("{}", takes(&40))
  ret 0
}
