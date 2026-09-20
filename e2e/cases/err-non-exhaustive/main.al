enum Choice { Yes, No(i32) }

fn f(c: Choice) -> i32 {
  ret match c {
    Yes => 1,
  }
}
