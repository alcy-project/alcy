// A reference is not a type a comp-known value can have
// (docs/spec/comp.md), so a comp block cannot leave one behind. The
// emitter has no constant of that type, and without the refusal the
// value reached an emitter check.
fn main() {
  const x := comp { &7 }
  _ := x
}
