// A loan read only on one side of a branch is still live on the other
// side's path to the join, so the conflict has to be reported there.
// This is the counterpart of `ok-borrow-branch-dead`: the loan is
// genuinely read after the write, on the fall-through path.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  held := v.at(0)
  if v.len() == 1 {
    v.push(2i32)
  }
  ret *held.unwrap() - 1
}
