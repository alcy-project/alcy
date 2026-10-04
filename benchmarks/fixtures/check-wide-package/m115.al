struct Point115 {
  x: i32,
  y: i32,
}

impl Point115 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape115 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area115(s: Shape115) -> i32 {
  match s {
    Shape115::Circle(r) => { ret r * 3 * r }
    Shape115::Rect(w, h) => { ret w * h }
    Shape115::Empty => { ret 0 }
  }
}

fn walk115(p: Point115) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
