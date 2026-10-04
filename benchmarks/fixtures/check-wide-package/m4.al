struct Point4 {
  x: i32,
  y: i32,
}

impl Point4 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape4 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area4(s: Shape4) -> i32 {
  match s {
    Shape4::Circle(r) => { ret r * 3 * r }
    Shape4::Rect(w, h) => { ret w * h }
    Shape4::Empty => { ret 0 }
  }
}

fn walk4(p: Point4) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
