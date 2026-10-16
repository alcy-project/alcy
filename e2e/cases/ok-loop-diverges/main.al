// A `loop` with no `break` runs forever, so it is `!` and the function
// needs no trailing `ret`.
fn f() -> i32 {
  mut i := 0
  loop {
    i = i + 1
    if i == 3 {
      ret i
    }
  }
}

fn main() -> i32 {
  ret f()
}
