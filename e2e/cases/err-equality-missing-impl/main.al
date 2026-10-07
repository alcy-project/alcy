struct Nope {
  x: i32,
}

fn main() -> i32 {
  a := Nope { x: 1 }
  b := Nope { x: 1 }
  if a == b {
    ret 1
  }
  ret 0
}
