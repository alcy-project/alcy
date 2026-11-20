// The explicit markers name work the compiler performs, so they belong
// in a comp context; a runtime branch must spell itself as a plain if.
fn main() -> i32 {
  comp if true {
    ret 0
  }
  ret 1
}
