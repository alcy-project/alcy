// A popped element that owns its payload still borrows the vector: the
// copy holds the buffer's loan through its `String`, so a `push` that
// reallocates reports the conflict rather than invalidating it.
fn main() -> i32 {
  mut v := Vec::<String>::new()
  v.push(String::from("a"))
  live := v.pop()
  v.push(String::from("b"))
  match live {
    Option::Some(s) => {
      if s.len_bytes() != 1 {
        ret 1
      }
    }
    Option::None => {
      ret 2
    }
  }
  ret 0
}
