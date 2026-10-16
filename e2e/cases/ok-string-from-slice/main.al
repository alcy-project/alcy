// A scanner builds an owned word from the bytes it accepted.
fn main() -> i32 {
  bytes := [108u8, 101u8, 116u8]
  word := String::from_slice(bytes.as_slice())
  if word.as_str() != "let" {
    ret 1
  }
  if word.len() != 3 {
    ret 2
  }
  empty := Vec::<u8>::new()
  if String::from_slice(empty.as_slice()).len() != 0 {
    ret 3
  }
  ret 0
}
