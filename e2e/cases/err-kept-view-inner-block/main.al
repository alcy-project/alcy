// The element type is a type parameter, so the declaration does not say
// that the callee only borrows the argument: the container keeps the
// view of a buffer whose block ends before the container does.

fn main() -> i32 {
  mut views := Vec::<str>::new()
  {
    buf := String::from("abc")
    views.push(buf.as_str())
  }
  w := views.at(0).or_panic_with_message("none")
  print("[{}]\n", *w)
  ret 0
}
