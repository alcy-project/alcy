// A `str` local is a view, so a function may hand the view back: the
// two words name a buffer someone else owns. The address of the slot
// that holds those words is not the view, though - the frame keeps it -
// and a borrow of the slot is the escape every other type already
// refuses.
fn leak() -> &str {
  u := "abc"
  ret &u
}
