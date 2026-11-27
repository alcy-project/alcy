// A local of an enclosing closure frame lives less long than the outer
// container it is stored into, so the store is refused however many
// closure levels the borrow passes through.

fn main() -> i32 {
  mut holder := Vec::<str>::new()
  outer := [&mut holder]() -> {
    a2 := String::from("abc")
    inner := [&a2, &mut holder]() -> {
      holder.push(a2.as_str())
    }
    inner()
  }
  outer()
  w := holder.at(0).or_panic("none")
  print("[{}]", *w)
  ret 0
}
