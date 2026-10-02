// The library half: no entry of its own, items a binary can reach once
// packages cross a boundary.
pub fn double(x: i32) -> i32 {
  ret x + x
}
