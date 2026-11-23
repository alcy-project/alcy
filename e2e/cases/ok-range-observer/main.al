// A range is interval data: its bounds can be read and membership
// tested without walking the cursor.

fn main() -> i32 {
  r := 1..<3
  if !r.start.is_bounded() {
    ret 1
  }
  if !r.end.is_bounded() {
    ret 2
  }
  match r.end.value() {
    Option::Some(v) => {
      if v != 3 {
        ret 3
      }
    }
    Option::None => {
      ret 4
    }
  }
  if !r.contains(1) {
    ret 5
  }
  if !r.contains(2) {
    ret 6
  }
  if r.contains(3) {
    ret 7
  }
  if r.is_empty() {
    ret 8
  }
  if !(3..<3).is_empty() {
    ret 9
  }
  if !(5..<2).is_empty() {
    ret 10
  }
  open := 5..
  if open.end.is_bounded() {
    ret 11
  }
  match open.end.value() {
    Option::Some(_) => {
      ret 12
    }
    Option::None => {}
  }
  if open.is_empty() {
    ret 13
  }
  if !open.contains(100) {
    ret 14
  }
  ret 0
}
