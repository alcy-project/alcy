// Whitespace-separated tokens: repeats and ends are skipped.
fn main() -> i32 {
  mut n := 0
  for w in "  a  bb ccc ".split_whitespace() { n = n + 1 }
  if n != 3 { ret 1 }
  mut first := ""
  for w in "  a  bb ".split_whitespace() { if str_len(first) == 0 { first = w } }
  if first != "a" { ret 2 }
  mut none := 0
  for w in "   ".split_whitespace() { none = none + 1 }
  if none != 0 { ret 3 }
  mut last := ""
  for w in "a b".split_whitespace() { last = w }
  if last != "b" { ret 4 }
  s := format("x\ty\nz")
  mut m := 0
  for w in s.split_whitespace() { m = m + 1 }
  if m != 3 { ret 5 }
  ret 0
}
