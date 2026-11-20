comp fn count(args: ..) -> usize {
  mut total := 0usize
  comp for i in 0..<args.len() {
    total += 1
  }
  ret total
}

fn main() -> i32 {
  const n := count(1, 2, 3, 4, 5)
  if n != 5 {
    ret 1
  }
  ret 0
}
