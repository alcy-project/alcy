// str_byte with a runtime index is evaluated by the comp interpreter:
// fold each byte of a UTF-8 string through a while loop and compare the
// sum against the same expression at runtime.
fn sum_bytes(comp s: str) -> usize {
  ret comp {
    mut total := 0usize
    mut i := 0usize
    while i < str_len(s) {
      total += str_byte(s, i) as usize
      i += 1
    }
    total
  }
}

fn main() -> i32 {
  const t := sum_bytes("aéb")
  // 'a' + 0xC3 + 0xA9 + 'b' = 97 + 195 + 169 + 98 = 559
  if t != 559usize {
    ret 1
  }
  ret 0
}
