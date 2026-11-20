// One unrolled `for` body is bounded by the expansion cap even when
// the global evaluation quota would still have room (docs/spec/comp.md).
comp fn total() -> i32 {
  mut s := 0
  for i in 0..<5000 {
    s += i
  }
  ret s
}

fn main() {
  const x := total()
  _ := x
}
