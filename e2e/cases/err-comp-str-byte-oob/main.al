fn at(comp s: str, comp i: usize) -> u8 {
  ret comp { str_byte(s, i) }
}

fn main() {
  const x := at("abc", 9usize)
  _ := x
}
