// ASCII is every byte below 0x80.
fn main() -> i32 {
  if !"hello".is_ascii() { ret 1 }
  if !"".is_ascii() { ret 2 }
  if !"a\tb".is_ascii() { ret 3 }
  s := String::new()
  s.push(0xC3u8)
  s.push(0xA9u8)
  if s.is_ascii() { ret 4 }
  t := format("ok")
  if !t.is_ascii() { ret 5 }
  ret 0
}
