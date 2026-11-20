comp fn pick() -> i32 {
  comp if true {
    ret 7
  } else {
    ret 9
  }
}

comp fn total() -> i32 {
  mut s := 0
  comp for i in 0..<4 {
    s += i
  }
  ret s
}

fn main() -> i32 {
  const a := pick()
  const b := total()
  if a != 7 || b != 6 {
    ret 1
  }
  ret 0
}
