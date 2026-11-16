// A nested closure names the same outer place through each level, so a
// view of a block-scoped buffer stored into a borrowed container is
// refused however deep the chain is.

fn main() -> i32 {
  mut holder := Vec::<str>::new()
  {
    a := String::from("abc")
    outer := [&a, &mut holder]() -> {
      inner := [&a, &mut holder]() -> {
        holder.push(a.as_str())
      }
      inner()
    }
    outer()
  }
  w := holder.at(0).or_panic_with_message("none")
  print("[{}]", *w)
  ret 0
}
