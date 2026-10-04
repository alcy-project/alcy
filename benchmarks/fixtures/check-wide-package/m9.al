struct Point9 {
  x: i32,
  y: i32,
}

impl Point9 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape9 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area9(s: Shape9) -> i32 {
  match s {
    Shape9::Circle(r) => { ret r * 3 * r }
    Shape9::Rect(w, h) => { ret w * h }
    Shape9::Empty => { ret 0 }
  }
}

fn walk9(p: Point9) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
