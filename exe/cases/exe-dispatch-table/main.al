// A function value held in a struct field, the shape a dispatch table
// takes.
struct Handler {
  f: (i32) -> i32,
  name: String,
}

fn inc(x: i32) -> i32 {
  ret x + 1
}

fn main() -> i32 {
  mut table := Vec::<Handler>::new()
  table.push(Handler { f: inc, name: format("inc") })
  table.push(Handler { f: (x: i32) -> x * 2, name: format("double") })
  first := table.at(0).or_panic_with_message("expected a value")
  second := table.at(1).or_panic_with_message("expected a value")
  if (first.f)(1) != 2 {
    ret 1
  }
  if (second.f)(3) != 6 {
    ret 2
  }
  ret 0
}
