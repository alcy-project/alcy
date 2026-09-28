// Thirteen modules against one root: the shape a package grows to as
// it splits, rather than a chain. Sibling-to-sibling references do not
// resolve yet, so the root calls each one directly.
fn main() {
  _ := m0::step(1)
  _ := m1::step(1)
  _ := m2::step(1)
  _ := m3::step(1)
  _ := m4::step(1)
  _ := m5::step(1)
  _ := m6::step(1)
  _ := m7::step(1)
  _ := m8::step(1)
  _ := m9::step(1)
  _ := m10::step(1)
  _ := m11::step(1)
}
