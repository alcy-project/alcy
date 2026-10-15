// A tokenizer-shaped program: byte classification, a nested scan, and
// an owned vector of the pieces.
fn main() -> i32 {
  src := "let x = 1 + 2"
  mut idents := Vec::<String>::new()
  mut i := 0 as usize
  while i < src.len() {
    b := src.byte(i)
    if b >= 0x61u8 && b <= 0x7au8 {
      mut text := String::new()
      while i < src.len() {
        c := src.byte(i)
        if c < 0x61u8 || c > 0x7au8 {
          break
        }
        text.push(c)
        i = i + 1
      }
      idents.push(text)
      continue
    }
    i = i + 1
  }
  if idents.len() != 2 { ret 1 }
  if idents.at(0).unwrap().as_str() != "let" { ret 2 }
  if idents.at(1).unwrap().as_str() != "x" { ret 3 }
  ret 0
}
