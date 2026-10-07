struct Grid {
  cell: i32,
}

impl Index<usize, i32> for Grid {
  fn index(self: &Self, i: usize) -> &i32 {
    ret panic("unreachable")
  }
}

fn main() -> i32 {
  ret 0
}
