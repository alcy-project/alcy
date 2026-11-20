// An enum formats through its Display implementation, whose match
// reads the variant through a reference, and a narrowed buffer keeps the
// total honest for the rendering too.

enum Shape {
  Circle(i32),
  Square(i32),
}

impl Display for Shape {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    s := match self {
      Shape::Circle(r) => format("circle {}", *r),
      Shape::Square(q) => format("square {}", *q),
    }
    ret s.display(out)
  }
}

fn main() -> i32 {
  println("{}", Shape::Circle(3))
  println("{}", Shape::Square(4))
  mut buf := [0u8; 4]
  out := write("{}", &mut buf, Shape::Circle(3))
  if out.written != 4 {
    ret 1
  }
  if out.total != 8 {
    ret 2
  }
  if buf[0] != 99u8 {
    ret 3
  }
  ret 0
}
