#[inline(always)]
fn always_inline() -> i32 {
  ret 1
}

#[inline(hint)]
fn hint_inline() -> i32 {
  ret 2
}

#[inline(never)]
fn never_inline() -> i32 {
  ret 3
}

fn main() -> i32 {
  ret always_inline() + hint_inline() + never_inline() - 6
}
