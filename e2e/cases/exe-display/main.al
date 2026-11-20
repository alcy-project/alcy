// `{}` reaches a nominal argument's `Display` implementation: the
// method writes the prefix of its rendering that fits and returns the
// total length, so a narrowed buffer keeps the total honest (the
// `write` call below reports 6 for a 4-byte buffer).

struct Point {
  x: i32,
  y: i32,
}

impl Display for Point {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    s := format("({}, {})", self.x, self.y)
    ret s.display(out)
  }
}

fn main() -> i32 {
  p := Point { x: 3, y: 4 }
  println("point {}", p)
  println("{} + {} = {}", 1, 2, p)

  // The owned string formats through the same spec.
  s := format("[{}]", 7)
  println("{}", s)

  // A 4-byte buffer holds the prefix; the total is what a full one
  // would have taken.
  mut buf := [0u8; 4]
  out := write("{}", &mut buf, p)
  if out.written != 4 {
    ret 1
  }
  if out.total != 6 {
    ret 2
  }
  if buf[0] != 40u8 {
    ret 3
  }
  println("{} {}", out.written, out.total)
  ret 0
}
