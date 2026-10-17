// The UTF-8 encoder's branch points and the ends of the scalar range:
// a \u{...} escape names a Unicode scalar value, so the widths here are
// one, two, three, and four bytes either side of every boundary.
fn main() -> i32 {
  if "\u{0}".len() != 1 { ret 1 }
  if "\u{7F}".len() != 1 { ret 2 }
  if "\u{80}".len() != 2 { ret 3 }
  if "\u{7FF}".len() != 2 { ret 4 }
  if "\u{800}".len() != 3 { ret 5 }
  if "\u{D7FF}".len() != 3 { ret 6 }
  if "\u{E000}".len() != 3 { ret 7 }
  if "\u{FFFF}".len() != 3 { ret 8 }
  if "\u{10000}".len() != 4 { ret 9 }
  if "\u{10FFFF}".len() != 4 { ret 10 }
  s := "h\u{e9}llo"
  if s.len() != 6 { ret 11 }
  t := "x\u{1F600}y"
  if t.len() != 6 { ret 12 }
  ret 0
}
