struct Point114 {
  x: i32,
  y: i32,
}

impl Point114 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape114 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area114(s: Shape114) -> i32 {
  match s {
    Shape114::Circle(r) => { ret r * 3 * r }
    Shape114::Rect(w, h) => { ret w * h }
    Shape114::Empty => { ret 0 }
  }
}

fn walk114(p: Point114) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
