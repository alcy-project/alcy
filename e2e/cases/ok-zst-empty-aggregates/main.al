// Empty aggregates are zero-sized, and a container of them keeps its
// elements without reserving storage for any of them.
struct Empty {}

enum Nothing {}

fn main() -> i32 {
  if size_of::<Empty>() != 0 {
    ret 1
  }
  if size_of::<Nothing>() != 0 {
    ret 2
  }
  if size_of::<[Empty; 8]>() != 0 {
    ret 3
  }
  mut v := Vec::<()>::new()
  v.push(())
  v.push(())
  if v.len() != 2 {
    ret 4
  }
  e := Empty {}
  a := &e
  b := &e
  if a != b {
    ret 5
  }
  ret 0
}
