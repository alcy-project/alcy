struct Point91 {
  x: i32,
  y: i32,
}

impl Point91 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape91 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area91(s: Shape91) -> i32 {
  match s {
    Shape91::Circle(r) => { ret r * 3 * r }
    Shape91::Rect(w, h) => { ret w * h }
    Shape91::Empty => { ret 0 }
  }
}

fn walk91(p: Point91) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
