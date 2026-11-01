// A `comp fn` body is comp evaluation itself, so it may call another
// comp fn (docs/spec/comp.md).
comp fn twice(x: i32) -> i32 {
  ret x * 2
}

comp fn quad(x: i32) -> i32 {
  ret twice(twice(x))
}

fn main() -> i32 {
  ret comp { quad(3) } - 12
}
