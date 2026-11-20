// A loop body's fixed-size slots belong to the frame, not to the block
// that names them: an aggregate built inside a loop otherwise takes a
// fresh slot every turn, and a long enough loop walks the stack into
// the heap. The loop is sized past that point for a default build.
fn main() -> i32 {
  mut i := 0
  mut t := 0
  while i < 200000 {
    s := format("x{}", i)
    t = t + str_len(s.as_str()) as i32
    i = i + 1
  }
  // "x" plus the digits of 0..199999: 200000 + 1088890.
  if t != 1288890 {
    ret 1
  }
  ret 0
}
