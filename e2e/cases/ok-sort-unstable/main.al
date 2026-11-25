// Both sort implementations are public: stable is the default and
// unstable is named by path.

fn main() -> i32 {
  mut a := [3i32, 1, 2, 5, 4]
  sort::stable::sort(a.as_mut_slice())
  if a[0] != 1 || a[4] != 5 {
    ret 1
  }
  mut b := [3i32, 1, 2, 5, 4]
  sort::unstable::sort(b.as_mut_slice())
  if b[0] != 1 || b[4] != 5 {
    ret 2
  }
  mut c := [3i32, 1, 2, 5, 4]
  sort::unstable::sort_by(c.as_mut_slice(), (x: &i32, y: &i32) -> *x > *y)
  if c[0] != 5 || c[4] != 1 {
    ret 3
  }
  ret 0
}
