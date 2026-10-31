// A `str` parameter is a view, and rebuilding a local view from it -
// by assignment or by a slicing accessor - still names the caller's
// buffer. Handing that back is what the std's trimming and splitting
// accessors do, so it stays accepted beside the slot borrow's refusal.
fn drop_first(mut s: str) -> str {
  if str_len(s) == 0 {
    ret s
  }
  ret str_slice(s, 1, str_len(s))
}

fn main() -> i32 {
  r := drop_first("abc")
  if str_len(r) != 2 {
    ret 1
  }
  ret 0
}
