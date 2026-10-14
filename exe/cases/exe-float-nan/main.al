// `==` is false for NaN and `!=` is true - the IEEE unordered pair -
// and every ordering is false.
fn main() -> i32 {
  z := 0.0f64
  nan := z / z
  if nan == nan { ret 1 }
  if !(nan != nan) { ret 2 }
  if nan < z { ret 3 }
  if nan > z { ret 4 }
  if nan <= z { ret 5 }
  if nan >= z { ret 6 }
  inf := 1.0f64 / z
  if inf != inf { ret 7 }
  if !(inf > z) { ret 8 }
  ret 0
}
