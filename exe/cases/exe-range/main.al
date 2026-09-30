// A range expression is `Range<T>` data: the endpoints keep their
// inclusion, and an absent side is `Unbounded`. Matching on the
// endpoints reads every spelling back out.
fn endpoint(b: Bound<i32>) -> i32 {
  match b {
    Bound::Included(v) => v,
    Bound::Excluded(v) => 1000 + v,
    Bound::Unbounded => -1,
  }
}

fn main() -> i32 {
  mut bad := 0
  r := 1..3
  if endpoint(r.start) != 1 {
    bad = 1
  }
  if endpoint(r.end) != 1003 {
    bad = 2
  }
  s := 1..=3
  if endpoint(s.start) != 1 {
    bad = 3
  }
  if endpoint(s.end) != 3 {
    bad = 4
  }
  t := 1..<3
  if endpoint(t.end) != 1003 {
    bad = 5
  }
  u := 2..
  if endpoint(u.start) != 2 {
    bad = 6
  }
  if endpoint(u.end) != -1 {
    bad = 7
  }
  w := ..4
  if endpoint(w.start) != -1 {
    bad = 8
  }
  if endpoint(w.end) != 1004 {
    bad = 9
  }
  v := ..
  if endpoint(v.start) != -1 {
    bad = 10
  }
  if endpoint(v.end) != -1 {
    bad = 11
  }
  ret bad
}
