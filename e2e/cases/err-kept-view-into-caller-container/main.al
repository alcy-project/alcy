// A callee that stores its view parameter reaches the caller's buffer.
// The container keeps a view of a buffer whose block ends before the
// container does, so the store is refused at the call site (ADR-0135).

fn store(mut v: &mut Vec<str>, s: str) {
  v.push(s)
}

fn main() -> i32 {
  mut views := Vec::<str>::new()
  {
    buf := String::from("abc")
    store(&mut views, buf.as_str())
  }
  w := views.at(0).or_panic_with_message("none")
  print("[{}]", *w)
  ret 0
}
