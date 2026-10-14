// A binary with no free `main` is diagnosed while checking, before a
// linker would name the missing entry.
fn helper() -> i32 {
  ret 0
}
