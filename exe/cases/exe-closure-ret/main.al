fn call(f: (i32) -> i32, x: i32) -> i32 {
  ret f(x)
}

// `ret` inside a closure returns from the closure, not from the function
// that called it. If it escaped, `main` would exit with the closure's
// value and the checks below would never run.
fn main() -> i32 {
  a := call((x: i32) -> { ret x * 2
    0 }, 21)
  if a != 42 { ret 1 }
  b := call((x: i32) -> { if x == 1 { ret 7 }
    0 }, 1)
  if b != 7 { ret 2 }
  ret 0
}
