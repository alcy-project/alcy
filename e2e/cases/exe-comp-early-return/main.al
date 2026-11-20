// A body evaluated during compilation leaves through a `ret` reached
// inside a branch: the branch hands the return to the body that owns
// it instead of ending the evaluation at the arm.

comp fn clamp_high(n: i32) -> i32 {
  if n > 10 {
    ret 10
  }
  ret n
}

comp fn pick(kind: i32) -> i32 {
  match kind {
    0 => ret 7,
    _ => {}
  }
  ret kind
}

comp fn inner(n: i32) -> i32 {
  {
    if n > 0 {
      ret 5
    }
  }
  ret n
}

fn main() -> i32 {
  const a := clamp_high(42)
  const b := pick(0)
  const c := inner(2)
  const d := clamp_high(3)
  ret (a + b + c + d) - 25
}
