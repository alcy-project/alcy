fn main() -> i32 {
  // The closure keeps its own state across calls, and a copy shares
  // that state: one exclusive borrow, however many handles.
  mut total := 0
  f := [&mut total] (x: i32) -> {
    total = total + x
    total
  }
  if f(10) != 10 { ret 1 }
  if f(32) != 42 { ret 2 }
  g := f
  _ := f(1)
  if g(0) != 43 { ret 3 }
  ret 0
}
