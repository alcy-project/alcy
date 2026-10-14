// A binary needs a free `main`: without one the program reaches the
// linker, whose failure names the tool rather than the declaration.
fn helper() -> i32 {
  ret 1
}
