# ADR-0088: A name spells its unit and its abort

- Subject: the language
- Status: Accepted
- Date: 2026-10-23

## Context

The std's surface is settling — `Option`, `Result`, `str`, `String`,
slices, ranges, iterators — and a name is the one decision a language
cannot revise after code is written against it. Documentation does not
fix a misleading name: the reader at a call site sees the name, not
the page.

The regrets are concrete. `unwrap` and `expect` say nothing about
aborting. `str::len` reads like a character count and is a byte count.
`to_owned` and `to_string` are two spellings of one conversion, and
`len_chars`-shaped names would repeat the first mistake in the other
direction by hiding a linear walk behind the same prefix as an O(1)
length. `target` names a compilation target, a build product, and a
command's path argument in overlapping tools, and alcy already carries
all three meanings.

Three decisions made alongside this one give names something to agree
with: ADR-0083 (a value is sized), ADR-0084 (the string is text, so
`String` sits above `core` and `unicode`), and the `char` type that
the text work reserves. The string API needs one spelling that says
what it counts, and the compiler's vocabulary needs one word per
meaning.

## Decision

**A name spells the consequence its signature hides — an abort, a
unit, a cost, and a word's meaning — and adding syllables where the
reading is already unambiguous is not.**

The rules:

1. **An abort is named.** A method that can panic says so:
   `or_panic_with_message(str)` and `or_panic_with(fn)`. No new
   shorthand hides an abort, and no existing one survives the rename
   below. Exceptions where the syntax is the name: the `Index`
   operator, `panic` itself, and `assert`/`debug_assert`.
2. **A unit is named when the type does not pin one.**
   `<quantity>_<unit>`: `len_bytes`, `timeout_millis`. `len` stays
   where the element type makes the count unambiguous (`[T]`,
   `Vec<T>`), and `is_empty` stays because every unit agrees on it.
3. **A linear cost is not hidden.** `len_*` is O(1) only; a walk is an
   iterator (`chars()`) or a verb (`count_chars`).
4. **One word, one meaning.** A glossary fixes overloaded words across
   the std and the compiler; `target` is split below.
5. **Conventional clarity beats added syllables.** Where the
   convention cannot be misread, do not lengthen.

The applications decided with the rules:

- **The option family.** `Option` and `Result` share four spellings:
  `or(default: T)`, `or_else(fn() -> T)`,
  `or_panic_with_message(str)`, and `or_panic_with(fn)`, where
  `Option`'s closure takes nothing and `Result`'s receives the error
  (`fn(E) -> str`), because the error is what the message needs. The
  message form is verbatim; an error is not appended behind the
  author's back. `unwrap`, `expect`, and `unwrap_or` retire;
  `Result::or`'s current meaning (the value substitution) is the
  family's, and `unwrap` is not reintroduced.
- **The string API.** `str` and `String` both offer `len_bytes`,
  `count_chars`, and `count_graphemes` (ADR-0084 for where each comes
  from), `is_empty` unchanged; `slice` becomes `byte_slice`, `pop`
  becomes `pop_byte` with `pop_char` reserved, and the linear counts
  keep the verb so the walk is visible. `find` and `rfind` keep their
  names and return byte offsets: a search API's offset is its
  subject's own unit.
- **The text scalar is `char`**, spelled like the other builtin
  scalars (`str`, `bool`, `i32`), and it means a Unicode scalar value:
  not a byte, not a grapheme. `rune` is not chosen (`char` keeps the
  literal's name in `grammar.md`, and the invariant carries the
  meaning); `codepoint` is verbose, and `scalar` is arithmetic
  vocabulary. `Ascii` stays the deferred byte-side nominal.
- **`target` is split.** `--target` and `codegen::Target` keep the
  compilation target, the word's one meaning in every other toolchain.
  A build product becomes `Artifact` (`pipeline::Artifact`,
  `ArtifactKind::{Bin, Lib}`, `ArtifactScope`), and a command's path
  argument becomes `input` (`config.input`, usage `<PATH>`). The
  spec's "`[[bin]]` target" prose becomes "artifact". LLVM's own
  `TargetMachine` and triple stay inside the LLVM adapter, and
  `platform_*` is reserved for a future `cfg`.
- **Reserved names** so a later decision cannot collide: `chars`,
  `graphemes`, `pop_char`, `or_option`, `platform_*`.

## Consequences

- The renames land as mechanical commits over the std and the CLI;
  they change no semantics, and the preludes keep every user-visible
  path stable (ADR-0084).
- A call site with a panic reads as one; a call site with a unit
  reads as one; a call site with a walk reads as one.
- The string's three counts line up with the layers: `len_bytes` and
  `count_chars` answer from `core`'s `str`, and `count_graphemes`
  from `unicode`.
- New APIs are reviewed against the rules, which is where the rules
  earn their keep: they decide the next name, not only the listed
  ones.

## Alternatives considered

- **Keep `unwrap`/`expect` and document them.** Rejected: the abort
  is not in the name, and the call site is where the reader is. This
  is the regret the first rule exists for.
- **`or_value`/`or_fallback` for the value substitution.** Rejected:
  `or` already reads as "this, or that" and is already `Result`'s;
  `value` is a word that says nothing, and `fallback` has no lazy
  sibling while `or_else` is the established pair.
- **`--triple` or `--platform` for the compilation target.** Not
  chosen: `--target` is the cross-compiler convention, and the
  overload worth removing is the build product and the path argument,
  which the split removes. `--triple` remains the alternative if the
  word itself must go.
- **`len_chars`/`len_graphemes`.** Rejected: they put a linear walk
  under the prefix that means O(1) length, which is the same mistake
  as `str::len` with the sign flipped.
- **`rune` for the text scalar.** Not chosen: the builtin scalars are
  lowercase, the parser already names the literal a character
  literal, and `char` with a stated invariant is enough. `rune` is
  the alternative if the name should warn against reading it as a
  grapheme.
- **Lengthen every name (`has_zero_elements`).** Rejected: it adds
  syllables where the reading is already unambiguous, and precision
  is only worth paying for where ambiguity is real.
