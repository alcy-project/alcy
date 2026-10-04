struct Point61 {
  x: i32,
  y: i32,
}

impl Point61 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape61 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area61(s: Shape61) -> i32 {
  match s {
    Shape61::Circle(r) => { ret r * 3 * r }
    Shape61::Rect(w, h) => { ret w * h }
    Shape61::Empty => { ret 0 }
  }
}

fn walk61(p: Point61) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
