// `split_whitespace` skips runs of spaces and yields the words.
fn main() -> i32 {
  text := "  let   x  "
  mut count := 0
  for word in text.split_whitespace() {
    if count == 0 && word != "let" {
      ret 1
    }
    if count == 1 && word != "x" {
      ret 2
    }
    count = count + 1
  }
  if count != 2 {
    ret 3
  }
  ret 0
}
