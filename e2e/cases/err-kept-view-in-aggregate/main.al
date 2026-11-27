// A view the callee stores inside an aggregate it builds is kept the
// same way as a bare view parameter: the aggregate lands in the
// caller's container, and the buffer's block ends before it does.

fn store(mut v: &mut Vec<(str, i32)>, s: str) {
  v.push((s, 1))
}

fn main() -> i32 {
  mut holder := Vec::<(str, i32)>::new()
  {
    buf := String::from("abc")
    store(&mut holder, buf.as_str())
  }
  w := holder.at(0).or_panic("none")
  print("[{}]", w.0)
  ret 0
}
