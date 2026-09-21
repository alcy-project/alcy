fn main() {
  mut a := 1
  r := &mut a
  a = 10
  _ := r
}
