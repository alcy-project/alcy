struct Point15 {
  x: i32,
  y: i32,
}

impl Point15 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape15 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area15(s: Shape15) -> i32 {
  match s {
    Shape15::Circle(r) => { ret r * 3 * r }
    Shape15::Rect(w, h) => { ret w * h }
    Shape15::Empty => { ret 0 }
  }
}

fn walk15(p: Point15) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
