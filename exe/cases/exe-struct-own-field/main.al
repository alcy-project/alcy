// A struct that holds an owning value retires it with the frame.
struct S {
  s: String,
}

fn main() -> i32 {
  x := S { s: format("hi") }
  if x.s.len_bytes() != 2 {
    ret 1
  }
  ret 0
}
