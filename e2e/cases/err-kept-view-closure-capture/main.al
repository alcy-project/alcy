// A closure's reference capture names the outer place, so a view of a
// block-scoped buffer stored into a borrowed container is refused
// whatever order the capture list gives.

fn main() -> i32 {
  mut holder := Vec::<str>::new()
  {
    a := String::from("abc")
    f := [&a, &mut holder]() -> {
      holder.push(a.as_str())
    }
    f()
  }
  w := holder.at(0).or_panic_with_message("none")
  print("[{}]", *w)
  ret 0
}
