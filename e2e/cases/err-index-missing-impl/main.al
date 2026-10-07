struct Grid {
  cell: i32,
}

fn main() -> i32 {
  g := Grid { cell: 1 }
  ret g[0]
}
