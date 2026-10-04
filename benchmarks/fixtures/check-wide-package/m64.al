struct Point64 {
  x: i32,
  y: i32,
}

impl Point64 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape64 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area64(s: Shape64) -> i32 {
  match s {
    Shape64::Circle(r) => { ret r * 3 * r }
    Shape64::Rect(w, h) => { ret w * h }
    Shape64::Empty => { ret 0 }
  }
}

fn walk64(p: Point64) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
