// `&&` and `||` evaluate the right side only when the left does not
// decide, so a diverging right side is never reached.

fn boom() -> bool {
  panic("the right side ran")
}

fn main() -> i32 {
  a := false && boom()
  if a { ret 1 }
  b := true || boom()
  if !b { ret 2 }
  if !(true && true) { ret 3 }
  if false && true { ret 4 }
  if !(true || false) { ret 5 }
  if false || false { ret 6 }
  mut total := 0
  mut i := 0
  while i < 3 && total < 3 {
    total = total + 1
    i = i + 1
  }
  if total != 3 { ret 7 }
  ret 0
}
