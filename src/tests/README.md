# tests

Unit tests (doctest). One directory per module, wired explicitly in
`BUILD.gn`.

Conventions the suite relies on:

- The build uses `-fno-exceptions`: use `CHECK`, never `REQUIRE`
  (which needs exceptions), and return early from the test case on
  fatal setup failures.
- `base::Result` is move-only with `&&`-qualified `unwrap()`:
  always `std::move(result).unwrap()`.
- Public APIs are checked-only: tests feed both well-formed input
  and directly-constructed malformed data (dangling arena indices,
  unterminated token streams, null module trees, unverifiable IR)
  and assert structured `Result` failures, never crashes.
- Fixtures reserve arena space up front (`arena.reserve(1u << 20)`)
  and gate each setup step with `CHECK` + early return.
