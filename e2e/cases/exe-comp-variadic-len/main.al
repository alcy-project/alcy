comp fn count(args: ..) -> usize {
  ret args.len()
}

comp fn counted(args: ..) -> usize {
  mut total := 0usize
  comp for _ in 0..<args.len() {
    total += 1
  }
  ret total
}

fn main() -> i32 {
  const a := count()
  const b := count(1, 2, 3)
  const c := counted(1, 2, 3, 4, 5)
  println("{} {} {}", a, b, c)
  ret 0
}
