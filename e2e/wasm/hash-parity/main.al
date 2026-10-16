// XXH3-64 over every length class, against the values the vendored
// C library answers for the same bytes. The direct-wasm backend walks
// the same 64-bit shifts under the checked-arithmetic overlay, where a
// valid count once read as out of range and aborted before the check
// was fixed: this case is what pins the fix under wasm32.

fn main() -> i32 {
  if xxh3_64("") != 0x2d06800538d394c2u64 {
    ret 1
  }
  if xxh3_64("a") != 0xe6c632b61e964e1fu64 {
    ret 2
  }
  if xxh3_64("ab") != 0xa873719c24d5735cu64 {
    ret 3
  }
  if xxh3_64("abc") != 0x78af5f94892f3950u64 {
    ret 4
  }
  if xxh3_64("abcd") != 0x6497a96f53a89890u64 {
    ret 5
  }
  if xxh3_64("abcde") != 0x55c65158ee9e652du64 {
    ret 6
  }
  if xxh3_64("abcdefgh") != 0x6f45a76842a96483u64 {
    ret 7
  }
  if xxh3_64("abcdefghi") != 0xe0dde4fc174590a0u64 {
    ret 8
  }
  if xxh3_64("abcdefghijklmno") != 0xa8edaf6dc2724d85u64 {
    ret 9
  }
  if xxh3_64("abcdefghijklmnop") != 0x3d3ccac9af14d8a8u64 {
    ret 10
  }
  if xxh3_64("abcdefghijklmnopq") != 0xca7f3571df47cacfu64 {
    ret 11
  }
  if xxh3_64("abcdefghijklmnopqrstuvwxyz01234") != 0xa8126711d036c549u64 {
    ret 12
  }
  if xxh3_64("abcdefghijklmnopqrstuvwxyz012345") != 0x5a4ee6ee5f6dadcfu64 {
    ret 13
  }
  if xxh3_64("abcdefghijklmnopqrstuvwxyz0123456") != 0xb8787ddc6103ba40u64 {
    ret 14
  }
  if xxh3_64("abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz01") != 0x4b66243173645742u64 {
    ret 15
  }
  if xxh3_64(
    "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz01"
  ) != 0x931500acdd1e6ce5u64 {
    ret 16
  }
  if xxh3_64(
    "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrs"
  ) != 0x63e976ff6d3e6722u64 {
    ret 17
  }
  if xxh3_64(
    "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrst"
  ) != 0x30d769616650b99du64 {
    ret 18
  }
  if xxh3_64(
    "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstu"
  ) != 0x978bbc0f2c4d07f9u64 {
    ret 19
  }
  if xxh3_64(
    "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrst"
  ) != 0x71218393cf88fcc5u64 {
    ret 20
  }
  if xxh3_64(
    "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvw"
  ) != 0x56449f406b039e4eu64 {
    ret 21
  }
  if xxh3_64(
    "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwx"
  ) != 0x43f8e58f86e097f2u64 {
    ret 22
  }
  if xxh3_64(
    "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxy"
  ) != 0x2c15fe9d5dd02598u64 {
    ret 23
  }
  if xxh3_64(
    "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijkl"
  ) != 0x5ae2ab600866bdd2u64 {
    ret 24
  }
  if xxh3_64(
    "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnop"
  ) != 0x3250be577471081cu64 {
    ret 25
  }
  if xxh3_64(
    "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwx"
  ) != 0xd0f155cbdb88fcd4u64 {
    ret 26
  }
  ret 0
}
