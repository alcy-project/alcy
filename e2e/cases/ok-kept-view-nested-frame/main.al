// Both places are locals of the same frame, and the buffer is declared
// before the container, so the closure shape is accepted.

fn main() -> i32 {
  outer := []() -> {
    a2 := String::from("abc")
    mut holder2 := Vec::<str>::new()
    inner := [&a2, &mut holder2]() -> {
      holder2.push(a2.as_str())
    }
    inner()
    w := holder2.at(0).or_panic_with_message("none")
    print("[{}]", *w)
  }
  outer()
  ret 0
}
