// A function value's type records no opt-in, so an unsafe function
// cannot become one until unsafe function types carry it.
unsafe fn poke() {}

fn main() {
  f := poke
  _ := f
}
