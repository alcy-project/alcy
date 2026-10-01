// `for` iterates only through `into_iter`; no other method stands in.
struct Thing {
  x: i32,
}

fn main() {
  t := Thing { x: 1 }
  for v in t {}
}
