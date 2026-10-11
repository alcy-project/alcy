fn g() {
  panic("boom\n")
}

fn f() {
  g()
}

fn main() {
  f()
}
