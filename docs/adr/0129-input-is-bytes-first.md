# ADR-0129: Input is bytes first

- Subject: the language
- Status: Accepted
- Date: 2026-10-30

## Context

`lib/std` has no way to read the process's standard input. `io`
reads whole files into a `String` and writes to stdout through the
compiler-expanded `print` family; a program that wants a token from
stdin has nowhere to name it. The target programs (a command-line
tool reading its arguments, a competitive-style program reading
tokens) and the language's constraints shape the design:

- `str` is bytes, and a count is total (ADR-0091). There is no UTF-8
  validity to report at this layer, so a line reader must not invent
  one.
- There are no exceptions and `?` does not convert (ADR-0093); the
  error vocabulary is explicit.
- An abort is named (ADR-0088), so the ergonomic idiom on failure is
  `or_panic_with_message`, not a shorthand.
- There are no macros (ADR-0097). A declaration site that carries
  types, lengths, and bindings (proconio's `input! { n: usize, a:
  [i32; n] }`) is a token-carrying construct, which the language does
  not have and does not plan to add for input.
- The floor is `sys` (ADR-0128): faithful calls on a descriptor.

The two existing shapes to weigh are Rust's (`stdin` plus
`BufRead::lines` plus `parse`: general, compositional, and verbose)
and proconio's macro (declarative, typed, and impossible here). The
middle ground has a name in other languages: a token cursor over a
buffered reader.

## Decision

**Input is two layers over `sys`: a buffered byte reader, and a token
scanner over it. Bytes first; text conveniences come later.**

- `io::BufReader` owns its buffer and not its descriptor. `stdin()`
  wraps `sys::STDIN` and never closes it.
  - `fill()` performs one underlying read into the buffer and
    answers the bytes gained (a short read is itself; zero is the
    end).
  - `read_byte()` answers `Option<u8>` (`None` at the end),
    `read_to_end()` the remainder, and `read_line()` one line with
    one trailing `\n` (and one `\r` before it) dropped. No UTF-8
    validation: the bytes are the text.
  - `EINTR` is retried at this layer and the retry is documented;
    the raw layer keeps it visible (ADR-0128).
- `io::Scanner` is the token domain. ASCII space, tab, CR, and LF
  separate tokens; a token is the byte run between them, so tokens
  cross lines the way the token domain expects.
  - `next_token()` is the token; the typed reads are
    spec-dispatched as `next::<T>()` over one `Parse` capability,
    not a name per type. The capability's shape (an associated
    function - the spec system has receiver methods only) is a
    prerequisite whose own record lands with its implementation
    (ADR-0132).
  - The failure taxonomy is three-way: the end of input, a token
    that does not parse, and the operating-system error underneath.
    The exact payloads land with the implementation; the
    distinction is the decision.
  - A vector read takes its count from the caller, because the
    language cannot put a runtime length in a type - the same fact
    that keeps proconio's `[i32; n]` out of reach.
- The general base is Rust-shaped: open capabilities for byte sources
  and sinks (a `Read`/`Write` pair, with the buffered types as the
  standard implementations), because composition over sources is what
  makes the design more than a scanner. The scanner is the token
  domain's sugar over that base, not a second abstraction.
- `BufWriter`, when it lands, flushes explicitly (`flush`, `finish`)
  for checking, and its `Drop` attempts a best-effort flush and
  ignores the failure: a closed pipe must not abort a program at
  scope exit.
- **proconio-class input is never an input-specific language
  feature.** Its blockers are named, and it may exist only as a
  library after they are solved in general:
  - The declaration form needs a token-carrying construct (macros),
    which ADR-0097 defers with a floor; `comp` evaluates values and
    cannot introduce bindings in a caller.
  - The struct flavor needs read-only compile-time reflection (its
    own record, when designed; ADR-0130).
  - The length coupling (`[T; n]`) cannot live in a struct's field
    types, because `n` is a runtime value; without a general
    attribute system (deferred for `repr(C)`) or a convention, the
    no-syntax interim is a wrapper type that carries its own length
    (a length-prefixed chunk), which covers the common shape and not
    a length shared by two fields. This record refuses a grammar
    extension for input alone.
- The wasm backend reads through WASI's `fd_read`; the playground
  answers the end of input when it has none. The exe runner gains an
  input field in `expect.toml` so cases can cover the path.

## Consequences

- A program reads tokens with plumbing it can see: bytes, a buffer, a
  cursor, and explicit errors; the ergonomic read is
  `sc.next::<T>().or_panic_with_message("name")`, which names its
  abort (ADR-0088).
- Input cannot fail on UTF-8 where the language has no UTF-8:
  `read_line` is total over bytes, and a validating reader, when
  wanted, is a `unicode` convenience rather than a mode of the base.
- The scanner is cheap for the target domain (no per-line
  allocation, tokens across lines) and is not the only way to read:
  the byte reader and the source/sink capabilities compose (a file,
  a future socket).
- The cost is vocabulary: a scanner is one more type to learn, and
  the Rust comparison - `Read`/`BufRead`/`FromStr` - remains the base
  underneath it.

## Alternatives considered

- **Rust-style only (no scanner).** Rejected for the domain:
  cross-line whitespace, the parse-failure/end-of-input distinction,
  and no per-line allocation are what the target programs want; the
  open capabilities stay the base.
- **A schema string expanded by the compiler
  (`io::input("(usize, [i32; 0])")`).** Rejected: it is a second
  syntax for types, it cannot bind the caller's names, and it makes
  `comp` a schema language rather than the language's own code
  (ADR-0097's spirit).
- **A compiler-recognized input form.** Rejected: an input-specific
  grammar is what this record refuses; `print`'s expansion is a
  typed conversion of values, not a declaration language.
- **Reflection now, for the struct flavor.** Not taken: it is a
  large facility whose record belongs with its own justification
  (ADR-0130); input alone does not carry it, and it still would not
  express a runtime length.

## Deferred

- Validating (UTF-8-aware) line and token readers in `unicode`.
- A global stdin lock and the thread story that needs it (the
  `thread` package is empty).
- `open`, `close`, and `seek` (ADR-0128's deferred list).
- The exact error payloads and the vector-read spellings.
