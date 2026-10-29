# ADR-0116: A closure implements the Fn spec its signature names

- Subject: the language
- Status: Accepted
- Date: 2026-10-29

## Context

ADR-0075 made each `fn()` spelling a fresh anonymous type parameter with
an automatic bound, monomorphized per witness: a declaration that names
`fn() -> T` is generic over the closure its caller passes, and the
concrete closure type - the witness - is the caller's, per call site.
That is monomorphization's own shape, and in parameter position it costs
nothing: the drop of the caller's closure is the substituted type's own
glue.

The record named the bound "structural until specs can express it", and
the machinery that grew around the bound is the transitional shape that
promise was making room for. A signature-side `fn()` spelling carries a
hand-rolled bound (`OwnFnBound`) checked by hand-rolled code
(`check_own_fn_witness`) against a witness stand-in interned beside the
synthetic parameter; a return-position spelling is a slot the body's
first `ret` answers, recorded per instantiation after a per-declaration
key shipped a live bug - two instantiations of one generic shared the
first instantiation's closure type, and a program that instantiated
twice was refused with the other instantiation's type.

The language has the machinery this bound asks for. The spec system
carries parameterized specs with the shape the bound needs
(`spec Index<I, O> { fn index(self: &Self, i: I) -> &O; }`), the
dispatch resolves a call through a bound statically per concrete type,
and the compiler already owns the operator-spec table that verifies
declarations against itself (ADR-0053). A closure's call is the same
question one level up: the type can be called with the signature the
spelling names, and the impl is the body the lowering registered.

## Decision

**The `fn(P) -> R` spelling names the `Fn` spec. Every closure nominal
implements it, and the impl is the code the lowering registers.**

- `core` declares the spec the operator specs' way, parameterized by
  the signature: `spec Fn<P, R> { fn call(self, P) -> R; }`, with `P`
  the parameters as the tuple the signature spells. It is
  compiler-owned like the operator table: a closure implements it by
  construction, the impl is generated beside the call function the
  lowering registers (ADR-0064's `closure_fn_index`), and no
  user-written impl exists, so the seal has nothing to gate.
- `fn(P) -> R` in a signature is sugar for an anonymous type parameter
  bounded by `Fn<P, R>` - in parameter position, where the caller's
  closure binds it, and in return position, where the body's literal
  binds it. The desugaring is ADR-0075's; the bound is the spec's, and
  the checking of a call through the parameter rides the dispatch the
  spec system already resolves statically per concrete type (ADR-0080's
  shape, one level up).
- **The closure nominal is parameterizable.** A closure literal in a
  generic body interns its captures with the declaration's type
  parameters where the captured values' types sit, so the literal
  implements the bound once, parametrically, and the body checks
  against it once. Each instantiation substitutes the parameters, and
  the substituted nominal is the concrete impl the instantiation's
  calls dispatch through.
- **The hand-rolled machinery retires with the spec's landing:**
  `OwnFnBound`, `check_own_fn_witness`, the stand-in a return slot
  resolved with until the body answered, and the witness record the
  per-instantiation key corrected. The bound checking is the spec
  system's; the impl lookup is the spec dispatch's.

## Alternatives considered

- **Keep the hand-rolled bound as the permanent form.** Rejected: it is
  a parallel system answering a question the spec system owns - bounds,
  dispatch, and impl lookup are the specs', and a closure bound outside
  them is machinery that must eventually meet the spec system anyway.
  ADR-0075 named this the transitional form; building it out further is
  the expedience the language's own records keep rejecting.
- **Signature-typed closures: one structural type per signature, the
  captures riding an environment cell that carries its own drop.**
  Rejected. A signature-typed closure cannot name the captures, so the
  drop of an owning value goes through a header the environment cell
  carries - an indirect call at every drop of an owning closure, and a
  header per environment. The capture-typed nominal drops statically
  through its substituted glue, which is the zero-cost shape the
  language's monomorphization already pays for everywhere else.
- **Key the witness record per instantiation and stop.** That is the
  fix the merge carries, and it is correct as far as it goes - but it
  keeps the hand-rolled bound, the stand-in, and the per-instantiation
  re-check, which is the work this record retires.

## Consequences

A generic whose signature names an owning function type is checked
once: the literal's nominal is parametric, the return slot's bound is
the spec's, and the specialization derives the concrete impl.
ADR-0079's caveat closes, and such a generic crosses a package boundary
with the rest.

The closure becomes a first-class citizen of the spec system: a bound
composes with the others, the dispatch handles a call through an
abstract closure statically per concrete type, and the tooling that
reads specs reads the closure's.

The witness slot stays what the caller sees: the return type the
signature spells - `fn() -> T` - not the closure nominal behind it. A
caller that stores the returned value holds the substituted nominal,
and its captures are the value's fields, the way a closure literal's
are.

The value's representation is unchanged by this record: the captures
nominal, the `Func` view beside it, the environment tuple the value
owns (ADR-0064). The view's pointer naming the value's own address is
the representation decision the backlog carries separately (the
closure's environment is the closure's own address); this record
decides the bound and the impl, and both work with either
representation.

The drop glue of a parametric closure nominal is the substituted
nominal's: captures may own values of the type parameters, and the
specialization derives the glue from the substituted fields, the way it
derives the rest of the body.
