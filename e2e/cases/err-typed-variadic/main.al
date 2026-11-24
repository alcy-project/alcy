comp fn count(args: i32..) -> usize {
  ret args.len()
}

const N: usize := count(1, "two", 3)

fn main() -> i32 {
  ret 0
}
