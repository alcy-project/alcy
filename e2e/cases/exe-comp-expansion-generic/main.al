fn show<T: Display>(x: T) {
  println("[{}]", x)
}

fn main() -> i32 {
  show(7i32)
  show("hi")
  ret 0
}
