struct Point19 {
  x: i32,
  y: i32,
}

impl Point19 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape19 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area19(s: Shape19) -> i32 {
  match s {
    Shape19::Circle(r) => { ret r * 3 * r }
    Shape19::Rect(w, h) => { ret w * h }
    Shape19::Empty => { ret 0 }
  }
}

fn walk19(p: Point19) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
