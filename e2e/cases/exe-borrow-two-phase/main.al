// A call's exclusive receiver is a reservation: the arguments evaluate
// while the place is still readable, and the call activates the
// borrow, so the self-feeding forms resolve. The sanitized run is what
// checks the frees.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(0)
  v.push(1)
  // Fibonacci reads two elements and grows with the answer.
  mut i: usize := 2
  while i < 10 {
    v.push(v[i - 1] + v[i - 2])
    i = i + 1
  }
  if v.len() != 10 {
    ret 1
  }
  if v[9] != 34 {
    ret 2
  }
  // The receiver's own length is an argument.
  v.push(v.len() as i32)
  if v[10] != 10 {
    ret 3
  }
  // An element feeds the growth.
  v.push(v[0])
  if v[11] != 0 {
    ret 4
  }
  // Appending at the end reads the length in place.
  v.insert(v.len(), 99)
  if v.len() != 13 {
    ret 5
  }
  if v[12] != 99 {
    ret 6
  }
  ret 0
}
