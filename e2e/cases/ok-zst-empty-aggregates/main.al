// Empty aggregates are zero-sized, and a container of them keeps its
// elements without reserving storage for any of them.
struct Empty {}

enum Nothing {}

enum OneValue {
  Only,
}

fn main() -> i32 {
  if size_of::<Empty>() != 0 {
    ret 1
  }
  if size_of::<Nothing>() != 0 {
    ret 2
  }
  if size_of::<OneValue>() != 0 {
    ret 8
  }
  if align_of::<[u16; 0]>() != 2 {
    ret 9
  }
  if size_of::<[Empty; 8]>() != 0 {
    ret 3
  }
  // A zero-sized element has no stride, so a runtime index still names
  // the same address as a constant one.
  zsta := [(); 4]
  at := 3 as usize
  if &zsta[0] != &zsta[at] {
    ret 7
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
