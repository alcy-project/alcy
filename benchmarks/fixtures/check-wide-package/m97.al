struct Point97 {
  x: i32,
  y: i32,
}

impl Point97 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape97 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area97(s: Shape97) -> i32 {
  match s {
    Shape97::Circle(r) => { ret r * 3 * r }
    Shape97::Rect(w, h) => { ret w * h }
    Shape97::Empty => { ret 0 }
  }
}

fn walk97(p: Point97) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
