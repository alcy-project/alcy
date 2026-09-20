struct Point { x: i32, y: i32 }

fn add(a: i32, b: i32) -> i32 {
  ret a + b
}

fn main() {
  p := Point { x: 1, y: 2 }
  _ := add(p.x, p.y)
}
