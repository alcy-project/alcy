struct Point10 {
  x: i32,
  y: i32,
}

impl Point10 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape10 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area10(s: Shape10) -> i32 {
  match s {
    Shape10::Circle(r) => { ret r * 3 * r }
    Shape10::Rect(w, h) => { ret w * h }
    Shape10::Empty => { ret 0 }
  }
}

fn walk10(p: Point10) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
