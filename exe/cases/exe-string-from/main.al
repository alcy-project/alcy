// `String::from` copies a view and `byte` reads one position.
fn main() -> i32 {
  s := String::from("abc")
  if s.as_str() != "abc" {
    ret 1
  }
  if s.byte(0) != 0x61u8 {
    ret 2
  }
  if s.byte(2) != 0x63u8 {
    ret 3
  }
  e := String::from("")
  if e.len() != 0 {
    ret 4
  }
  ret 0
}
