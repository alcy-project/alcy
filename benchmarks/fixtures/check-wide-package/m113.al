struct Point113 {
  x: i32,
  y: i32,
}

impl Point113 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape113 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area113(s: Shape113) -> i32 {
  match s {
    Shape113::Circle(r) => { ret r * 3 * r }
    Shape113::Rect(w, h) => { ret w * h }
    Shape113::Empty => { ret 0 }
  }
}

fn walk113(p: Point113) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
