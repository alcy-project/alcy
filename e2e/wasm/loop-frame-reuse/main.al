// A loop body's binding must not walk the shadow stack up: the frame
// reserves the slot once, so an iteration count no longer sizes the
// frame. Before that reservation, twenty thousand iterations pushed the
// frame past the stack reserve and into the heap, and the loop's own
// state replaced the vector's data.
fn main() -> i32 {
  mut v := Vec::<u64>::with_capacity(8)
  v.push(111)
  v.push(222)
  mut total: u64 := 0
  for i in 0..<20000u64 {
    total += i
  }
  if total != 199990000u64 { ret 1 }
  match v.at(0) {
    Option::Some(x) => {
      if *x != 111 { ret 2 }
    }
    Option::None => { ret 3 }
  }
  match v.at(1) {
    Option::Some(x) => {
      if *x != 222 { ret 4 }
    }
    Option::None => { ret 5 }
  }
  ret 0
}
