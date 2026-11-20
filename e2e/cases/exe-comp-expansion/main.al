fn pick(comp i: usize, args: ..) -> i32 {
  ret args[i]
}

fn total(comp n: usize, args: ..) -> i32 {
  mut acc := 0
  comp for j in 0..<args.len() {
    acc += args[j]
  }
  ret acc
}

fn main() -> i32 {
  const first := 0usize
  const third := 2usize
  const three := 3usize
  a := pick(first, 10, 20, 30)
  b := pick(third, 10, 20, 30)
  c := total(three, 10, 20, 30)
  if a != 10 || b != 30 || c != 60 {
    ret 1
  }
  println("{} {} {}", a, b, c)
  ret 0
}
