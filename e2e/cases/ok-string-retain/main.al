// A byte-level string filter: only the accepted bytes are copied.
fn main() -> i32 {
  src := String::from("hello world")
  bytes := src.as_bytes()
  mut out := Vec::<u8>::with_capacity(bytes.len())
  mut i := 0 as usize
  while i < bytes.len() {
    if bytes[i] != 0x20u8 {
      out.push(bytes[i])
    }
    i = i + 1
  }
  s := String::from_slice(out.as_slice()).or_panic_with_message("the copy is ASCII")
  if s.as_str() != "helloworld" {
    ret 1
  }
  nothing := Vec::<u8>::new()
  t := String::from_slice(nothing.as_slice()).or_panic_with_message("empty decodes")
  if t.len_bytes() != 0 {
    ret 2
  }
  ret 0
}
