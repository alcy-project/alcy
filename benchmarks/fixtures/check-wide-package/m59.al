struct Point59 {
  x: i32,
  y: i32,
}

impl Point59 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape59 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area59(s: Shape59) -> i32 {
  match s {
    Shape59::Circle(r) => { ret r * 3 * r }
    Shape59::Rect(w, h) => { ret w * h }
    Shape59::Empty => { ret 0 }
  }
}

fn walk59(p: Point59) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
