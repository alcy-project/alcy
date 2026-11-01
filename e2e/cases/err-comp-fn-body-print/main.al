// A `comp fn` body is comp evaluation, so the compile-time ban on
// I/O reaches it (docs/spec/comp.md).
comp fn describe(n: i32) -> i32 {
  println("{}", n)
  ret n
}

fn main() {
  const x := comp { describe(1) }
  _ := x
}
