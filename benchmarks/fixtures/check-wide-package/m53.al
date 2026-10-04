struct Point53 {
  x: i32,
  y: i32,
}

impl Point53 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape53 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area53(s: Shape53) -> i32 {
  match s {
    Shape53::Circle(r) => { ret r * 3 * r }
    Shape53::Rect(w, h) => { ret w * h }
    Shape53::Empty => { ret 0 }
  }
}

fn walk53(p: Point53) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
