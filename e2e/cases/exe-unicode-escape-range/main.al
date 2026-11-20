// The UTF-8 encoder's branch points and the ends of the scalar range:
// a \u{...} escape names a Unicode scalar value, so the widths here are
// one, two, three, and four bytes either side of every boundary.
fn main() -> i32 {
  if "\u{0}".len_bytes() != 1 {
    ret 1
  }
  if "\u{7F}".len_bytes() != 1 {
    ret 2
  }
  if "\u{80}".len_bytes() != 2 {
    ret 3
  }
  if "\u{7FF}".len_bytes() != 2 {
    ret 4
  }
  if "\u{800}".len_bytes() != 3 {
    ret 5
  }
  if "\u{D7FF}".len_bytes() != 3 {
    ret 6
  }
  if "\u{E000}".len_bytes() != 3 {
    ret 7
  }
  if "\u{FFFF}".len_bytes() != 3 {
    ret 8
  }
  if "\u{10000}".len_bytes() != 4 {
    ret 9
  }
  if "\u{10FFFF}".len_bytes() != 4 {
    ret 10
  }
  s := "h\u{e9}llo"
  if s.len_bytes() != 6 {
    ret 11
  }
  t := "x\u{1F600}y"
  if t.len_bytes() != 6 {
    ret 12
  }
  ret 0
}
