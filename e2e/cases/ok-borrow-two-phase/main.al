// A call's exclusive receiver is a reservation the call activates, so
// the arguments may read the place in between (ADR-0012).
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(v.len() as i32)
  v.push(v[0])
  v.insert(v.len(), 9)
  if v.len() != 4 {
    ret 1
  }
  ret 0
}
