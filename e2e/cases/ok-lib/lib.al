// A library builds to an object with no entry: no `main` is
// required, nothing links, and the items stay available to a future
// importer.
pub fn double(x: i32) -> i32 {
  ret x + x
}
