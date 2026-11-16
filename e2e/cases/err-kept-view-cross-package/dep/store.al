// A dependency that stores the view it is handed.

pub fn store(mut v: &mut Vec<(str, i32)>, s: str) {
  v.push((s, 1))
}
