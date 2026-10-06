# symbol

Symbol names: the table a run interns them in, and the mangling of
linker-visible ones.

Every symbol the compiler defines is encoded deterministically
(`_A` + version + kind + path + generics), so a source name can
never collide with a C library entry point or with another
package's symbol. Foreign names pass through untouched.

Names are held by `SymbolTable` as views of the bytes that already
spell them, so a name parsed out of a source is never copied; only a
name whose bytes do not outlive the call is.

## Entry points

- `mangle(signature, types, strings)` -> encoded name. Inputs always
  come from lowering; the encoding is total over well-formed
  signatures.
- `demangle(text)` ->
  `base::Result<Demangled, DemangleError>`: `NotAlcySymbol` for a
  missing prefix, `UnknownVersion` for a version mismatch,
  `Malformed` for truncated, undecodable, or trailing-byte bodies.
  Never partially decodes.
- `display(type)` renders a decoded type in source spelling for
  diagnostics.
- `SymbolTable::try_intern(name)` -> the handle for a name whose bytes
  live longer than the table; `intern_copied` for one whose bytes do
  not; `get(id)` reads a name back.

## Input requirements

- Mangled text from untrusted sources (object files, user input)
  must go through `demangle`, never a partial hand-parse: version
  and shape disagreements are errors, not best-effort output.
