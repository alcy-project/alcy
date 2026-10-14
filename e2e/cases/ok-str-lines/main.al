// Lines split on `\n`, a trailing `\r` comes off, and a final empty
// line is not reported.
fn main() -> i32 {
  mut n := 0
  for line in "a\nb\nc".lines() { n = n + 1 }
  if n != 3 { ret 1 }
  mut first := ""
  for line in "a\nb".lines() { if str_len(first) == 0 { first = line } }
  if first != "a" { ret 2 }
  mut empty := 0
  for line in "".lines() { empty = empty + 1 }
  if empty != 0 { ret 3 }
  mut trail := 0
  for line in "a\n".lines() { trail = trail + 1 }
  if trail != 1 { ret 4 }
  mut crlf := 0
  for line in "a\r\nb\r\n".lines() { crlf = crlf + 1 }
  if crlf != 2 { ret 5 }
  s := format("x\ny")
  mut m := 0
  for line in s.lines() { m = m + 1 }
  if m != 2 { ret 6 }
  ret 0
}
