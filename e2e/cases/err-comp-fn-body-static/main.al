// A `comp fn` body cannot read a static either: storage exists only
// at runtime, and the body runs during compilation.
static COUNT: i32 = 3

comp fn bump(n: i32) -> i32 {
  ret n + COUNT
}

fn main() {
  const x := comp { bump(1) }
  _ := x
}
