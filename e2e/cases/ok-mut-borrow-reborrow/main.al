// `mut` gates rebinding: a binding declared without it still has
// storage, so a reference to it mutates that storage, and a reborrow
// takes its permission from the reference it reads through.
struct P {
  n: i32,
}

fn seven() -> i32 {
  ret 7
}

fn through_reference() -> i32 {
  x := 1
  r := &mut x
  *r = 9
  ret x - 9
}

fn through_reborrow() -> i32 {
  x := 1
  r := &mut x
  q := &mut * r
  *q = 5
  ret x - 5
}

fn through_field_reborrow() -> i32 {
  p := P { n: 1 }
  s := &mut p
  t := &mut (*s).n
  *t = 4
  ret p.n - 4
}

fn through_temporary() -> i32 {
  a := &mut seven()
  *a = 8
  ret *a - 8
}

fn main() -> i32 {
  if through_reference() != 0 {
    ret 1
  }
  if through_reborrow() != 0 {
    ret 2
  }
  if through_field_reborrow() != 0 {
    ret 3
  }
  if through_temporary() != 0 {
    ret 4
  }
  ret 0
}
