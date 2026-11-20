// A range bound that is not comp-known is still refused at the head:
// the error names the iterable the source wrote, not the desugared
// cursor declaration.
fn main() {
  bound := 4
  const x := comp {
    mut s := 0
    for i in 0..<bound {
      s += i
    }
    s
  }
  _ := x
}
