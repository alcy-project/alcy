// A typed variadic constrains every collected argument to its element
// type; an untyped one keeps accepting a mixed shape.
comp fn count<T>(args: T..) -> usize {
  ret args.len()
}

const EMPTY: usize := count::<i32>()

const THREE: usize := count(1, 2, 3)

comp fn mixed(args: ..) -> usize {
  ret args.len()
}

const MIXED: usize := mixed(1, "two", true)

fn main() -> i32 {
  if EMPTY != 0 {
    ret 1
  }
  if THREE != 3 {
    ret 2
  }
  if MIXED != 3 {
    ret 3
  }
  ret 0
}
