// A string literal pattern is deferred (docs/spec/deferred.md): the
// view is two words, so the arm's test cannot be the one machine
// compare the other literal kinds lower to. The refusal names the
// comparison to write instead.
fn main() -> i32 {
  s := "hello"
  match s {
    "hello" => { ret 0 }
    _ => { ret 1 }
  }
}
