// A multi-segment path that names no variant is still refused: only a
// recorded unit variant joins the constants a comp argument accepts.
fn double(comp n: i32) -> i32 {
  ret n * 2
}

fn main() {
  _ := double(helper::C)
}
