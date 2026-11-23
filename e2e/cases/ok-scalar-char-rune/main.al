// The two scalar types, their literals, checked construction, a rune
// cursor, and character range patterns all agree on one program.
fn main() -> i32 {
  ascii := 'a'
  wide := '日'
  widened: rune := 'z'
  if ascii != 'a' {
    ret 1
  }
  if wide != '日' {
    ret 2
  }
  if widened != 'z' {
    ret 3
  }
  match ascii {
    'a'..='z' => {}
    _ => ret 4,
  }
  match widened {
    'a'..<'{' => {}
    _ => ret 5,
  }
  mut count := 0 as usize
  for r in "日本語abc".runes() {
    count = count + 1
  }
  if count != 6 {
    ret 6
  }
  match char_from_u8(65u8) {
    Option::Some(c) => {
      if c != 'A' {
        ret 7
      }
    }
    Option::None => ret 8,
  }
  match rune_from_u32(0x110000u32) {
    Option::None => {}
    Option::Some(r) => ret 9,
  }
  if format("{}", 'a').as_str() != "a" {
    ret 10
  }
  if format("{}", '日').as_str() != "日" {
    ret 11
  }
  if format("{}", '\n').as_str() != "\n" {
    ret 12
  }
  ret 0
}
