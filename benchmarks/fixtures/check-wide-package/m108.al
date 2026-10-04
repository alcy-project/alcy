struct Point108 {
  x: i32,
  y: i32,
}

impl Point108 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape108 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area108(s: Shape108) -> i32 {
  match s {
    Shape108::Circle(r) => { ret r * 3 * r }
    Shape108::Rect(w, h) => { ret w * h }
    Shape108::Empty => { ret 0 }
  }
}

fn walk108(p: Point108) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
