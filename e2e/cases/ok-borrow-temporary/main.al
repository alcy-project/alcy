// A Copy temporary is materialized where a borrow needs an origin:
// a literal, an arithmetic result, a Copy struct literal, and an
// explicit spec call like `a.eq(&1)`.

struct P {
  x: i32,
}

fn take_p(p: &P) -> i32 {
  ret p.x
}

fn takes(v: &i32) -> i32 {
  ret *v
}

fn main() -> i32 {
  if takes(&(1 + 2)) != 3 {
    ret 1
  }
  if takes(&7) != 7 {
    ret 2
  }
  if take_p(&(P { x: 7 })) != 7 {
    ret 3
  }
  a := 1
  if !a.eq(&1) {
    ret 4
  }
  ret 0
}
