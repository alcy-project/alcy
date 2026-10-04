struct Point72 {
  x: i32,
  y: i32,
}

impl Point72 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape72 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area72(s: Shape72) -> i32 {
  match s {
    Shape72::Circle(r) => { ret r * 3 * r }
    Shape72::Rect(w, h) => { ret w * h }
    Shape72::Empty => { ret 0 }
  }
}

fn walk72(p: Point72) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
