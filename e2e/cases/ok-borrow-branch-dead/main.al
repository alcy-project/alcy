// A loan's extent is a region, so a loan that is dead on the branch
// taken does not block the return that branch makes. The `ret` ends `v`
// while `held` is out of scope on this path, and nothing is read
// through it after, so there is no conflict to report.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  v.push(2i32)
  held := v.at(0)
  if v.len() != 2 {
    ret 1
  }
  ret *held.unwrap() - 1
}
