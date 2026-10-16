// A closure owns what it moves in: the environment lives in the value,
// and the drop walk ends it with the value (ADR-0064).
struct R {
  x: i32,
  m: &mut i32,
}

fn main() {
  mut n := 0
  r := R { x: 1, m: &mut n }
  f := [r]() -> {
    ret r.x
  }
  _ := f()
}
