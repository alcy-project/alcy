fn main() -> i32 {
  (a, b) := "k=v".split_once("=").or_panic_with_message("expected a value")
  if a != "k" {
    ret 1
  }
  if b != "v" {
    ret 2
  }
  if "k=v".split_once("x").is_some() {
    ret 3
  }
  mut n := 0
  mut last := ""
  for piece in "a,b,c".split(",") {
    n = n + 1
    last = piece
  }
  if n != 3 {
    ret 4
  }
  if last != "c" {
    ret 5
  }
  mut empty := 0
  for piece in "".split(",") {
    empty = empty + 1
  }
  if empty != 1 {
    ret 6
  }
  mut trail := 0
  for piece in "a,".split(",") {
    trail = trail + 1
  }
  if trail != 2 {
    ret 7
  }
  mut whole := 0
  for piece in "abc".split("") {
    whole = whole + 1
  }
  if whole != 1 {
    ret 8
  }
  (c, d) := "a=b=c".split_once("=").or_panic_with_message("expected a value")
  if c != "a" {
    ret 9
  }
  if d != "b=c" {
    ret 10
  }
  s := format("x;y")
  (e, f) := s.split_once(";").or_panic_with_message("expected a value")
  if e != "x" {
    ret 11
  }
  if f != "y" {
    ret 12
  }
  ret 0
}
