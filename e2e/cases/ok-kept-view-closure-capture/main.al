// The buffer outlives the container, so the same closure shape is
// accepted when the outer places are in the safe order.

fn main() -> i32 {
  a := String::from("abc")
  mut holder := Vec::<str>::new()
  f := [&a, &mut holder]() -> {
    holder.push(a.as_str())
  }
  f()
  w := holder.at(0).or_panic_with_message("none")
  print("[{}]", *w)
  ret 0
}
