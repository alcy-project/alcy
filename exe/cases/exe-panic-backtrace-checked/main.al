fn h() -> i32 {
  x := 2147483647i32
  ret x + 1i32
}

fn g() -> i32 {
  y := h()
  ret y + 1i32
}

fn f() -> i32 {
  z := g()
  ret z + 1i32
}

fn main() -> i32 {
  n := f()
  ret n
}
