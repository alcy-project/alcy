struct R { x: i32, m: &mut i32 }

fn take(r: R) -> i32 {
  ret r.x
}

fn main() -> i32 {
  mut n := 0
  r := R { x: 1, m: &mut n }
  f := [r] () -> { ret take(r) }
  ret f()
}
