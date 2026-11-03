// The block hands out a container that kept a view of a buffer the
// block declares, so the view would reach a slot that has ended.

fn main() -> i32 {
  views := {
    buf := String::from("abc")
    mut v := Vec::<str>::new()
    v.push(buf.as_str())
    v
  }
  w := views.at(0).or_panic_with_message("none")
  print("[{}]\n", *w)
  ret 0
}
