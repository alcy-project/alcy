comp fn pick(args: ..) -> usize {
  _ := args[5]
  ret args.len()
}

fn main() -> i32 {
  const n := pick(1, 2)
  if n != 2 {
    ret 1
  }
  ret 0
}
