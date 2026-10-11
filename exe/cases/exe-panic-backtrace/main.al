fn g() {
  panic("boom\n")
}

fn f() {
  mut n := 0
  while n < 3 {
    n = n + 1
    if n == 2 {
      g()
    }
  }
}

fn main() {
  f()
}
