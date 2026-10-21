# Backlog

Peripheral work, unordered. When every item is checked the list is
replaced. The foundations are in `roadmap.md`.

## Open

- [x] A nested enum payload laid its inner type out twice per level.
  `enum_payload_area` measured each field for the payload's alignment
  and then called `fields_layout`, which measured every field again, so
  `alcy check` doubled per `Option` level: a chain nested twenty-four
  deep cost 13.9B instructions against 102M for an empty `main`, and
  the check is linear now. `fields_layout` already returns the
  strictest alignment beside the size, so one call per variant is
  enough. Regression case: `e2e/cases/ok-nested-enum-layout/`.
- [x] A struct field and an enum variant could repeat a name.
  The declaration site scans its members before a generic form interns
  per use, so `enum E<T>` is covered, and reports the second
  declaration. Cases: `e2e/cases/err-duplicate-field/` and
  `e2e/cases/err-duplicate-variant/`.
- [x] A deeply nested generic type costs explosive compile time. A
  Resolved in 29933019: a generic call resolution is cached per call
  expression and instance (`Checker::call_resolutions_`,
  `compiler/analyzer/checker.h`), so resolving one level no longer
  re-resolves every level below it. The nest is linear now: at depths
  12/16/20 the debug build went from 231/1,082/15,900 ms to 20/27/91 ms
  and the release build from 85/1,202 ms to 29/94 ms. Regression case:
  `exe/cases/exe-nested-generic-depth/`. The analysis below is kept for
  the record.
  value of a type like `Option<Option<...<E>...>>` nested twenty
  levels deep is checked in about a second and twenty-eight levels
  past a minute, where `alcy check` had to be killed. Nothing in the
  source needs a pattern: a single wildcard arm and a bound value
  reach it, an explicit binding type does not help, and the value is
  a couple hundred bytes. The cost is therefore in building or
  canonicalizing the nested generic instances, not in the cover walk
  and not in the or-pattern expansion. The type itself is not the
  cost: a function whose parameter names the same nesting forty deep
  checks in a third of a second, so the type syntax and the instance
  are built cheaply. What explodes is checking the nested value -
  the `Some(Some(...))` expression - and an explicit binding type
  still spends several seconds at twenty-four levels. Generic
  instantiation is what the cost needs: nested tuples, which have no
  instantiation, check in a fifth of a second sixty deep, while a
  nested generic function call `wrap(wrap(...))` takes twenty-nine
  seconds at twenty levels. The generic call path is far worse than
  the enum constructor path at the same depth, so the instance work
  is where to look. The growth is exponential in the depth, not
  quadratic: `wrap(wrap(...))` costs 1.2 s at sixteen levels and 29 s
  at twenty, a factor of two per level. Something re-expands the
  whole type per instantiation instead of sharing the instance the
  nesting already built. The reuse lookup is `instantiate_generic`
  (`types.cc:2351`): it first compares argument indices, then falls
  back to a structural `types_equal` walk because, as its own comment
  says, compound arguments are re-minted on each resolution and so
  carry fresh indices. Re-minting a nested argument while checking it
  is what can double the work per level; the fix is to canonicalize
  the argument before the lookup so the index fast path hits.
  Instrumenting the lookup shows the exponent is in `types_equal`
  itself, not in how often it is called: at twelve and sixteen levels
  the call, scan, and structural-comparison counts are nearly the same
  (about 1080 scans and 330 comparisons per 128 calls) while the time
  grows fivefold, so one comparison of a nested type is what explodes.
  Avoiding the structural fallback by canonicalizing the arguments
  removes those comparisons. The comparison's own walk is linear and
  cycle-safe, and it normalizes every step through `type_origin`
  (`types_equal_inner`, `types.cc:4010`), so the expensive call is
  likely `type_origin` on a nested compound type. That call is only a
  map chase (`types.cc:2221`), so it is cheap. The counts above came
  from `instantiate_generic`, which mints type instances, while the
  nested `wrap` calls resolve through the function instantiation path
  (`instantiate_generic_method`, `types.cc:5344`); a few hundred type
  level calls cannot account for seconds. Neither path is hot: the
  type path and `instantiate_generic_method` both stay under a few
  hundred and a few dozen calls. `wrap` is a free generic function,
  so its calls resolve through the free-function instantiation path.
  That path is `instantiate_fn` (`types.cc:6153`), and it is the hot
  one: a counter there reads 4,352 calls at twelve levels and 65,792
  at sixteen, a factor of about two per level that matches the wall
  clock. The same instantiation is requested exponentially often even
  though the function already scans `fn_instances` for a match
  (`:6157`), so the reuse keeps missing. The fix is to make that
  lookup hit. The reuse loop itself is correct, though: it compares
  module, item, arity, and every argument by index or structure
  (`:6163`), so it is not a missed match. The same `(item, args)` is
  asked for again and again and the loop hands back the same
  signature, which means the caller re-checks the body of the generic
  function once per request - caching the instance stops duplicate
  instances, not duplicate body checks. Memoizing the check, or the
  check of a body already in progress, is where the fix belongs. The
  check is `precheck_called_fn` (`types.cc:4819`), and the call that
  repeats is from `check_const_dependencies` (`:4707`): that walk
  follows a call into the callee's body to find the values a constant
  expression depends on, and the body of a generic function is walked
  once per instantiation it is reached through. The walk runs from a
  pass over each function's body (`:4620`) that learns which values a
  constant expression can reach, and `precheck_called_fn` is what
  follows a call into the callee's body. The precheck already keeps a
  per-item set (`prechecked_fns_`) and deliberately skips generic
  bodies (`types.cc:4840`), so the descent stops at the first generic
  call. What repeats instead is `lookup_function` inside
  `precheck_called_fn` (`:4828`), which instantiates the generic once
  per walk. A counter on the walk itself reads under 256 calls at
  both depths, so the walk is not invoked exponentially either; the
  instantiations come from resolving the nested call expressions each
  time they are reached, not from the walk repeating. A counter on
  `check_expr` settles it: it runs more than 12,288 times at twelve
  levels and more than 135,168 at sixteen, a factor of about two per
  level, so the checker re-types the same expression tree
  exponentially often and the instantiations are a consequence. The
  recorded type table holds one entry per check but nothing reads it
  back, and a memo must respect that a bare integer literal is
  re-checked against a sibling's type. A factor of exactly two per
  level points at a regular double check: each level's argument is
  typed once on its own and once against the type the enclosing
  generic resolution expects, and both walks descend. A trace confirms
  it: at four levels the same low expression indices appear again and
  again, first plain and then with an expected type, and every re-type
  walks the subtree beneath it, so the work doubles per level.
  Finding that second check and reusing the first answer is the fix.
  A memo of completed checks was tried: keyed on module, expression,
  expected type, and instance and storing successes only, it cuts the
  wrapper from 15.9 s to 0.12 s at twenty levels, but it fails
  `ok-method-infer` and `ok-own-fn-param`, so an inference-dependent
  expression's answer is not a function of that key. The cache has to
  exclude those checks, or the inference has to become memoizable. A
  trace of `ok-method-infer` shows why: one expression (kind 0, the
  kind to look up in `ast::ExprKind`) is checked twice under exactly
  the same module, expression, expected type, and instance, and the
  inference that settles in between makes the second answer the right
  one. The key has to carry whatever makes the answer final, or those
  expressions have to stay out of the memo. Read the pair as a
  speculative check the caller throws away followed by the real one
  the settled inference allows: the first answer is never meant to be
  reused, so the fix is to find the caller that asks early and
  resolve once, rather than to remember the first answer. The trace
  shows the pair repeating one level up as well (`expr=20` is checked
  with and without an expected type just before each check of
  `expr=16`), so the early pass is not local: it comes from the top of
  the chain, most likely a method or generic resolution that checks
  its operands once to infer the type arguments and checks them again
  with the arguments it inferred instead of reusing the first pass. It
  is not a visible pair though: the operand checks in `types_expr.cc`
  and `types.cc` are each single, so the second walk starts inside
  `check_expr_inner`'s own `Call` and `MethodCall` cases - read those
  with the trace's kinds in hand. The kinds say the unsound repeat is a
  `Literal` (0) under a `Cast` (10) inside a `Range` (24): the cast is
  checked with and without an expected type and each walk re-adopts the
  literal, which is the re-check the comment on `check_expr` describes.
  The exponential case is different - it repeats calls - so a memo that
  leaves literal adoption alone (or handles it separately) may still be
  sound for the call-driven nesting. It is not: with literals kept out
  of the table the two cases still fail, because a literal that adopts
  a context type changes the answer of every expression above it, and
  their entries are already stored. A sound table therefore has to keep
  out any expression whose subtree holds a literal, or carry the
  inference generation that makes an answer final. The subtree walk
  has a model to copy: `verify_expr_children` (`compiler/ast/verify.cc`
  `:163`) switches on the node kind and names every child, so the
  predicate is a small mirror of it plus a per-expression cache. Three
  variants were tried and all fail the same two cases: the full table,
  the table without literals, and the table that stores only checks
  made with no expected type. The answer therefore depends on the
  inference state in every variant, so no key built from the module,
  expression, expected type, and instance can stand in for it. The
  fix is the subtree predicate (a literal below can always move an
  answer) or an explicit inference generation. The predicate does not
  need the child switch: the checks run bottom up, so a member flag
  set where a literal is handled, compared around the inner call
  (`before != after`), tells whether this expression's subtree held
  one - and a subtree that held one is never stored, so a cache hit
  cannot hide it. It does not help either: with a literal anywhere in the
  subtree kept out, the same two cases still fail, so the stale entry
  is not literal-driven - any expression can be re-typed once the
  inference around it settles. Four variants have now been measured
  (full, no literals, no expected type, no literal subtree) and all
  fail the same way, so the fix is an explicit inference generation or
  the removal of the duplicate request, not a better exclusion. The
  caller trace names the mechanism: every repeat is a parent checking
  the child it already checked - `Cast` (20) asks for `Literal` (16)
  again, `Range` (40) asks for `Deref` (39) again - so the outermost
  expression of the body runs twice and each level below re-walks its
  children. Find what checks the outer expression twice (the
  comp-dependency pass that calls `precheck_called_fn` is the first
  suspect) - and that suspect is out: every check of the doubled body
  runs with `comp_depth == 0` and no precheck in progress, and the
  first chain ends at the body's outermost expression before the
  second begins. So the ordinary path checks a body twice: one pass
  completes a body and the main pass checks it again, and each
  instantiation of a nested generic repeats that pair at the level
  below. The first pass is most likely the return-position witness
  walk: it records its findings at `own_fn_return_witnesses_`
  (`types.cc:6095`) by checking the body's return values, and the
  recorded witness is consumed later while a signature is resolved
  (`:5578`, `:6487`, `:6716`, `:6897`). Read those two ends to see
  whether the witness pass and the ordinary body pass both walk the
  body, and keep one of them from re-walking. The first pass is
  `try_bind_own_fn_return` (`types.cc:6074`): it answers a return
  position's stand-in by checking the value (`:6092`) before the
  ordinary body pass reaches the same `ret`, so every expression the
  value names is walked twice and a nested instantiation doubles it
  again. The recorded answer is already in the type table, so the
  ordinary pass can read it back instead of walking the value. The
  `Return` case already guards that: when `try_bind_own_fn_return`
  answers, the ordinary check is skipped (`types_expr.cc:3984`), so
  the value is not walked twice there. The two complete chains in the
  trace must therefore be two runs of a body check itself - find the
  caller that checks one body twice rather than a node that re-checks
  a child. The body check itself is `check_block(module, body, ...)`
  (`types.cc:7764`) inside the item handler, so the handler runs once
  per body and something calls it twice for the same item. Find its
  caller - a declaration pass and a body pass, or a per-reference
  walk - and let the second call see the type already recorded. The
  handler is `Checker::check_fn` (`types.cc:7717`) and it has both:
  the precheck path calls it (`:4887`, `:4936`) and the ordinary path
  calls it again (`:6472`, `:6883`, `:7036`), so a body runs twice.
  Read the precheck callers first: `precheck_fn_body` skips a generic
  declaration, so the instantiated body of a generic function reaches
  `check_fn` from both ends and doubles at every level. The precheck
  end is a full body check: it saves the per-body state, sets
  `cur_inst` to the instance (`types.cc:4881`), raises
  `prechecking_fn_`, and calls `check_fn` (`:4887`) just for the typed
  expressions, then restores everything. The ordinary end calls it
  again for the same instance. Open question to settle first: the
  precheck returns early for a generic declaration, so check which end
  actually doubles a generic function's instantiated body. A counter
  on `check_fn` rules that out: it runs 253 times at twelve levels and
  257 at sixteen, so bodies are not re-checked. The exponential lives
  inside one body's expression walk, and the two chains the earlier
  trace showed are two groups of expressions, not two runs of a body.
  Look again at what checks the outermost expression twice within one
  body - the comp-known predicate walk is the next candidate, since
  it runs with `comp_depth == 0` and reads the same tree. Read the
  trace again and the two problems separate: the doubled walks that
  make the exponent ask the same expression with different expected
  types (`expr=20` plain, then with `exp=169`), so no key dedupes them
  and the fix has to stop the second request; the same-key repeats
  that broke the four memo variants are another, rarer shape (a
  literal under a cast) and they are what any table must leave alone. The
  argument path shows the shape: `check_call_args` (`types_expr.cc:673`)
  types each argument once against its parameter and then walks the
  same argument again through `expr_comp_known` for a comp parameter
  (`:696`). A generic call infers its type arguments by checking them
  once and then reaches `check_call_args` with the resolved
  parameters, so the tree is walked twice per level - read `check_call`
  (`:1488`) for the inference pass and let it hand the resolved
  arguments over instead of re-checking. The inference sits in the
  path-callee branch: `check_call` (`:1488`) handles a closure or
  value callee first, and the branch that resolves a named generic
  function continues past `:1531`, where the type arguments are worked
  out. Read from there. The pair is right there: the named branch
  calls `resolve_generic_fn(module, item, call.args, call.type_args,
  span)` (`types_expr.cc:1549`) to work out the type arguments from the
  arguments, and then hands the same `call.args` to `check_call_args`
  (`:1560`), which types every one of them again. `resolve_generic_fn`
  already knows the types it inferred from; carrying them back (or
  letting it record them where `check_call_args` reads) removes the
  second walk and the exponent with it. The inference function is
  `Checker::resolve_generic_fn` (`types.cc:6607`); it first fills the
  explicitly spelled parameters and then works the rest out from the
  arguments, below `:6640`. Read from there: whatever it computes per
  argument is what `check_call_args` recomputes, so threading one
  vector of argument types from the inference to the argument check
  is the whole fix. It can be done without touching
  `resolve_generic_fn` at all: the inference's check already recorded
  each argument's type under the current instance, so `check_call_args`
  can read that entry, unify it with the parameter, and skip the
  second walk. Keep literals out of the shortcut (the member flag from
  the memo work says when a subtree holds one) because adopting the
  parameter type is exactly what the second walk is for. Check the
  assumption first: `resolve_generic_fn` does not check the arguments
  itself. Its one inference call is `infer_bound_arguments(module,
  params, bound, span)` (`types.cc:6670`), which takes no argument
  list, so the argument types reach it another way (a member holding
  the pending call, or a helper). Read it before writing the reuse,
  since a wrong assumption here means no reuse to make. It is wrong:
  `infer_bound_arguments` (`types.cc:1020`) reads only the generic
  parameters' annotations and bounds, never the call arguments, so the
  argument-driven binding happens in `check_call_args`'s own
  `unify` (`types_expr.cc:691`). There is no separate argument walk to
  remove, and the second walk that makes the exponent is somewhere
  else - the trace's parent/child repeats are the place to look, with
  a parent asking the same child once plainly and once with an
  expected type. Check the block walk first: the body block is checked
  by `check_block` (compiler/analyzer/types_expr.cc:4025), and if it visits
  statements or the trailing value twice, that is the root every
  parent/child repeat below it is a copy of - and it does: the value
  is checked (`types_expr.cc:4045`) and then handed to
  `expr_comp_known` (`:4050`), a second structural walk of the same
  tree, when `verify_comp_known` is set. `check_call_args` does the
  same for a comp argument (`:695`). `expr_comp_known` is pure - it
  answers whether a tree is comp-known, not what a context infers - so
  the sound fix is to memoize it per expression and instance, unlike
  the type table. That should collapse the exponent. Shape of the
  fix: the predicate reads `Checker::expr_comp_known` (compiler/analyzer/types_expr.cc:371)
  and recurses through its children by name, so renaming it to
  `expr_comp_known_inner` and adding a small `expr_comp_known` wrapper
  (lookup, call, store) keyed on module, expression, and instance
  routes every recursive call through the memo. A block predicate has
  the same shape and wants the same wrapper. Before writing it, note
  the predicate's real signature: `expr_comp_known(u32 module,
  ast::ExprIdx expr, std::span<const std::string_view> bound) const`
  (`types_expr.cc:371`). Its answer depends on the names `bound`
  holds, so a memo keyed on module, expression, and instance alone is
  wrong unless the bound set is the same on every walk of an
  expression. Establish that first - either it is fixed per expression
  and the simple key holds, or the key has to carry the bound set. Both
  happen: the recursion carries one `bound` unchanged
  (`types_expr.cc:415`-`:499`), but an outside entry uses another -
  `check_call_args` calls it with no bound at all (`:696`), while an
  inner walk of the same argument carries the enclosing names. So the
  simple key is unsound, and the memo has to key on the bound set (or
  the two entry kinds have to agree on one). A counter there settles
  it: `expr_comp_known` is called fewer than a hundred times at
  twelve and sixteen levels, so it is not the amplifier either. The
  hot functions found so far are `check_expr` (12,288 then 135,168)
  and `instantiate_fn` (4,352 then 65,792), and their ratio to the
  program size is the thing to explain; `check_fn` (253, 257) and the
  comp-known predicate are both linear. Look for what sits between a
  `check_expr` call and an `instantiate_fn` call and repeats. It is
  `resolve_generic_fn` itself: a counter on it reads about 4,200 calls
  at twelve levels and 65,800 at sixteen, which matches
  `instantiate_fn` almost exactly, so the generic call resolution is
  the amplifier and it runs once per check of a call expression. The
  resolution should be remembered per item, arguments, and instance -
  the signature it returns does not change between two checks of the
  same call - so memoizing it there is the fix; a per-candidate
  re-check inside it is the other thing to look for. A per-expression
  counter names the hottest instead: a `Path` (kind 1) reaches 4,000
  checks and a whole chain of `Cast`s (kind 10, one per level) reach
  1,000 to 4,000 each, so one level's cast is re-typed as many times
  as its depth below the top. The `Cast` case itself checks its operand
  once (`types_expr.cc:2214`), so the doubling starts with whatever
  checks the topmost cast of the chain twice - read the case that owns
  it (the trace put a `Question`, `Deref`, and `Call` above it). Read
  the counts once more: they are per AST node across the whole run, and
  a node is checked once per instantiation, so the same cast reaching
  4,000 means the instantiation it lives in was made thousands of
  times, not that one body was re-typed. The exponent is in the number
  of instantiations - the `resolve_generic_fn` and `instantiate_fn`
  counts - so the question is why one instantiation is built again and
  again. That is the reuse loop from the earlier notes
  (`instances_by_nominal_`, `types_equal`), and the structural
  comparison there is what to re-examine. The mint itself is
  `fn_instances.push_back(FnInstance{...})` (`types.cc:6458`), which is
  where a miss counter goes; `types.cc:6153` is no longer the
  function's head, so re-locate `Checker::instantiate_fn` before
  instrumenting. Count calls against mints: if mints stays flat while
  calls grow, the reuse works and the requests themselves multiply; if
  mints grows with calls, the reuse is missing. Measured: reuse is
  fine. `instantiate_fn` takes 4,351 calls and mints 56 instances at
  twelve levels, and 65,791 calls with 60 mints at sixteen, so the
  distinct instantiations grow linearly and the requests explode. The
  fix is therefore on the request side: something asks for the same
  instantiation again and again, and since the body checker is linear
  (`check_fn` 253 to 257) the asking happens inside one body walk. Find
  the loop that walks a call expression more than once per body. It
  asks for one instantiation over and over: the first thirty calls at
  eight levels are all `item=1`, `inst=0xffffffff`, `nargs=1` - the same
  instantiation, requested again and again with no other call between.
  So a small cache in `instantiate_fn` keyed on module, item, argument
  indices, and instance removes them, and the structural reuse behind
  it already handles arguments re-minted with new indices. The cause is
  on the requesting side, but this is the safe place to cut it. The
  mint block is `types.cc:6455`-`6463` and the calls into it are cheap
  work, which is the contradiction left: every counter measured is
  linear (bodies, comp-known, instances) or a count of cheap requests,
  and none of them accounts for the seconds. Before changing anything,
  time the work itself - a sampling profile of `alcy check` on the
  sixteen-level wrapper, or a cumulative timer around each phase of
  the analyzer - so the next change is aimed at the seconds and not at
  a count. `perf record -g` on the sixteen-level wrapper works and the
  profile is flat: the hottest single address is 4.25 percent and the
  rest fall away quickly, with no analyzer function on top. The build
  is the debug one, and it carries AddressSanitizer, so much of that
  time is the sanitizer's bookkeeping and the picture is not the
  release one. Measure the release build's wrapper before treating the
  seconds as an algorithmic cost; if it is still super-linear there,
  profile that build instead. Measured: the release build keeps the
  growth. Twelve, sixteen, and twenty levels take 26 ms, 85 ms, and
  1,202 ms, so the factor of two per level is still there and only its
  constant is about ten times smaller than the sanitized build's. The
  exponent is real work, not sanitizer bookkeeping, so the search
  stands. Note that `out/build/alcy` is now the release binary - the
  debug one has to be rebuilt before any instrumented run. The release
  binary carries no debug symbols, so `addr2line` answers `??` for its
  addresses; a symbolized profile needs a `-g` release or the debug
  build. What the unsymbolized one does show is grouping: three tight
  clusters in the binary and a steady share in `libc` at addresses
  that look like the memcpy/malloc family, so allocation and copying
  inside the repeated calls is the leading hypothesis - each request
  that walks a call may build argument and parameter vectors just to
  throw them away. Count allocations (`mallinfo2` before and after, or
  a counter in the allocator) at twelve and sixteen levels before
  choosing a fix. The call chain is short: `instantiate_fn` has exactly
  one caller, `resolve_generic_fn` (`types.cc:6763`), and that has one
  caller of its own, the named branch of `check_call`
  (`types_expr.cc:1549`). So every count above is just the number of
  times one call expression is checked, and the twenty-six identical
  requests in a row mean one call expression is checked twenty-six
  times back to back. The multiply is therefore entirely in the
  expression walk: a parent asks `check_expr` for the same call child
  again and again. Read the cases the caller trace named - `Cast`,
  `Range`, `Deref`, `Binary`, `Call` - for a second `check_expr` of one
  child, which is now the single remaining question. The cases are
  mostly single checks: `Cast` types its operand once (`:2214`),
  `Binary` checks logical operands once each and otherwise hands both
  to `check_binary_operands` (`:3756`), which takes `expected` and is
  the next read (`types_expr.cc:619`) - a helper that learns the
  left operand's type and then re-types it against the unified one
  would be exactly the doubling. It has one, but a small one: when the
  two operands do not match and one is a bare integer literal, that
  operand is checked once more against its sibling (`types_expr.cc:650`
  onward), which is the adoption the comment above it describes and a
  leaf-sized walk. So the binary helper is out for a call-bearing
  operand, and the doubling has to be in the remaining cases with a
  nested child: `Range` (its two endpoints), `Deref`, and the call
  path itself. `Range` is the same shape as `Binary`: the case
  delegates to `check_range` (`types_expr.cc:4017`), which types one
  endpoint once and re-types a bare integer literal endpoint against
  its sibling (the adaptation at `:2587`), again a leaf-sized walk. So
  the two remaining places with a nested child are `Deref` and the call
  path, and if both check once, then no single node re-asks a child and
  the twenty-six checks of one call come from repeated entries higher
  up rather than from one parent. `Deref` closes the audit: it checks
  its operand once (`types_expr.cc:3716`) and only unifies the pointee
  (`:3732`). Every case the caller trace named - `Cast`, `Binary`,
  `Range`, `Deref` - checks each child once, with the only repeats
  being the leaf-sized literal adoption. So no single node re-asks a
  child, and one call expression reached twenty-six entries from
  somewhere above the expression: a statement, block, or loop body
  walked twice inside one function body. Measure that next - count
  `check_stmt` and `check_block` entries per body and instance - rather
  than reading more expression cases. Statements are clean too: a
  counter on `check_stmt` (`types_expr.cc:4229`) never reaches two
  hundred entries at sixteen levels, so no statement is re-walked.
  With the node audit closed and statements flat, the twenty-six
  entries for one call happen inside the checking of a single
  statement's expression - yet no case re-asks a child. The next
  measurement has to show the path, not the count: record, for the
  hottest expression, the kind of the expression that was being
  checked above it (a small parent stack in the checker), and read off
  how the same node is reached twenty-six times. It shows a loop:
  before every hot `Cast` (six, eight, ten, and on) the expression
  entered just before was the same `Path` (expression four), so one
  iteration checks that path and then one cast of the chain, over and
  over - four thousand iterations of a Path-then-Cast pair. That is a
  call resolution re-reading its callee path and then one argument, so
  the loop is around candidate or method resolution: `check_method_call`
  (`types_expr.cc:1708`) and `check_call`'s named branch are where a
  per-candidate pass would sit. Read the method path first, since a
  method chain on a generic is what the wrapper spells. It is not a
  candidate loop inside the case: `check_method_call` types the
  receiver once (`types_expr.cc:1712`) and then works with that type.
  So one pass of the observed pair is one whole method-call check -
  receiver then argument - and the method call itself is being entered
  thousands of times. The remaining question is who re-enters it: no
  parent case, statement, or body does, so look for a loop that walks
  expressions from the top - a `for` desugar, a comp unroll, or a
  retry around a failed resolution that starts again from the callee.
  Each of those is already read away: `check_call` does not retry a
  failed generic resolution (it checks the arguments and reports,
  `types_expr.cc:1551`), the wrapper's body is not comp, and it has no
  `for`. That leaves one measurement that decides between the two
  remaining stories - the same expression checked within one
  instantiation, or once per instantiation across many. Print `cur_inst`
  with each of the consecutive entries of one hot expression: if the
  instance changes on a short cycle the call is re-entered by the
  instantiation machinery; if it never changes, one body walk checks it
  over and over. Measured, and the two stories separate cleanly: the
  hot `Path` and `Cast` (`expr=4` and `6`) are always checked with
  `cur_inst` at the top level, `0xffffffff`, while the instantiated
  bodies' own nodes (`Block`, `Cast`, `Path` at `expr=3`, `2`, `0`)
  carry instances 14, 16, 18, 20 and on. So the outermost call is
  re-entered thousands of times inside one walk of one top-level
  statement, and the node audit missed the loop because it lives in a
  helper rather than a case. The special-cased call paths are the
  natural fit: `check_fmt_format` and `check_fmt_write`, reached from
  `check_call` (`types_expr.cc:1569`-`:1573`), which walk a call's
  arguments once per format placeholder. Their heads are
  `check_fmt_write` (`types_expr.cc:981`) and `check_fmt_format`
  (`:1065`); the write one types the format string (`:1004`) and the
  buffer (`:1013`) once each and then handles the value arguments below
  `:1022`, which is the code to read for a per-placeholder pass. Note
  the recognizers: `is_core_fmt` (`:715`) and `is_io_print` (`:724`)
  cover `write`/`format` and `print`/`println`, so a `println` of the
  wrapper reaches this path. They are clean too: `check_fmt_write`
  types each value argument once, in one loop over the arguments
  (`types_expr.cc:1038`), and `verify_fmt_literal` (`:1045`) reads the
  types it collected rather than re-checking the arguments. Fifteen
  candidates are now read away down to the statement level, so the
  remaining instrument has to show the exact nest: keep a real parent
  stack in the checker (push the expression at the head of
  `check_expr`, pop on the way out) and print it when one hot expression
  is entered the second and third time. The guess-and-read loop has run
  its course; only the true stack will name the caller. It does. The
  second entry of the hot `Path` (expression four) arrives under a
  chain of sixteen `Cast`s (`36, 34, 32, ..., 8, 6`, all kind 10), so
  the top-level expression is one sixteen-link cast chain and the path
  is its base; every level of the chain re-walks the level below it.
  The chain's own root is checked thousands of times while the
  statement that owns it is not, so one statement check walks its
  value expression ten or more times. That is the loop left to find:
  read `check_stmt` (`types_expr.cc:4229`) and its statement-value
  helper for a path that checks the statement's expression more than
  once. The declaration case does it: the initializer is checked
  (`types_expr.cc:4245`) and then handed to `expr_comp_known`
  (`:4251`) when the declaration is const or `verify_comp_known` is
  set - the third twin of the same pattern, after `check_block:4050`
  and `check_call_args:695`. That is the loop: one checked value, then
  one comp-known walk of the same tree, and the comp-known walk
  recurses into nested calls whose arguments are checked and walked
  again. The counter in the earlier note says the predicate itself runs
  under a hundred times, so settle the flag first: print
  `verify_comp_known` at the wrapper's declaration and see whether this
  path is even taken. It is not: at every declaration of the wrapper
  `verify_comp_known` is zero and the declaration is not const, so the
  twin walk never runs and the three comp-known sites are all gated
  off. That also confirms the earlier sub-hundred count. So the loop
  is not a comp-known twin either, and the arithmetic still does not
  close: the chain is entered thousands of times while the statement
  that owns it is under two hundred. That gap means the owning
  statement is not a declaration - it has to be a container that is
  itself checked repeatedly, a loop body or condition, or a comp
  region. Print the statement kind (and its statement index) at
  `check_stmt` when the initializer chain is present. Reading the
  reproducer rather than guessing its shape settles it: the body is
  `o := wrap(wrap(...))` with sixteen nested calls, so the hot `kind`
  10 nodes are `Call`s, not casts, and the chain is a call chain over
  the base `E::A`. The mechanism is then plain: checking the outer
  call resolves the generic and instantiates `wrap`, and to work out
  its type argument it checks its own argument - the next call in,
  which resolves and instantiates in turn, and so on down the sixteen.
  The cost of one resolution is one resolution per level below it, so
  T(k) = 1 + sum of T(i) for i below k, which doubles per level and
  matches the measured `resolve_generic_fn` counts exactly. The fix is
  to remember a call's resolution per expression and instance - the
  same fix the counts pointed to - because resolving it again returns
  the same signature. The key must carry
  the expected type as well as the expression and the instance: a
  memo that ignores it skips the unification the expected type asks
  for and would accept a mismatch, and one that caches only the
  untyped checks only halves the exponent. On the rebased main
  the wrapper costs 15.9 s at twenty levels where it cost 29 s before,
  so the constant improved but the growth did not: twenty-six levels
  still has to be killed. Reproduce with a generated file.
- [x] A method call whose receiver is a range index aborted the
  emitter: `n := s[1..<3].len()` (and `first`, and the same chain on a
  `Vec`) reports `Unsupported cast` from the LLVM backend
  (`llvm_ir_emitter_compute.cc:327`) and `Construct 'struct' is not
  supported` from the direct wasm one, while `x := s[1..<3]` followed
  by `x.len()` lowers correctly. Parenthesizing the receiver
  (`(s[1..<3]).len()`) does not help, so it is not the parse. The IR
  for the failing form casts the `Range` value to `u64` and indexes
  the slice with it, so the lowered run value reaches the call as an
  index operand: the receiver's type and the lowered operand disagree
  by the time the method call is built. The lowering's own range check
  reads the same wrong type - moving it ahead of the spec-call target
  changes nothing - so the analyzer recorded an integer for the range
  expression, and `check_index` then took the element path. The index
  is only mistyped when the index expression is a method call's
  receiver, so the next step is the receiver check in
  `check_method_call`, which looks up the method before the index
  expression is typed. The analyzer does record the call: `record_call`
  names the element `Index` impl for the index expression, so
  `check_index` itself took the spec path because the range expression
  it typed came back as an integer. Nothing after the receiver check
  re-types the receiver, so the wrong type is fixed at that first
  `check_expr` of the receiver. The type table is append-only and
  `Lowerer::expr_type` reads the last entry, so that last entry is what
  lowering sees: an integer for the index expression, while the same
  lowering materializes a `Range<i32>` for it. The two disagree, so the
  next step is to print the tag `expr_type(index.index)` holds in the
  index lowering and compare it with the expression the analyzer last
  typed. The likely mechanism is `range_element`: it asks
  `generic_find(type_origin(type))`, and a field or payload copies the
  type node without carrying the index the instance registered - the
  same gap the exhaustiveness check hit, fixed there by matching the
  shape a type points at. A copy reaching `range_element` answers
  false, and `check_index` then reads the range as an element index.
- [x] An inclusive range whose end is one below its start (`s[3..=2]`)
  yielded an empty run, while the exclusive spelling (`s[3..<2]`) aborted:
  the inversion check compares the decoded endpoints, and the included
  end decoded one past the element it named, so the two orders collapse.
  It is safe today (the width is zero) but the two spellings of one
  inverted interval disagree.
- [ ] A reference stored into a longer-lived place outlives its
  referent: `mut r: &i32 := &0; { n := 5; r = &n; } *r` compiles, and
  so does the same store through a struct field or an `if` arm, which
  leaves `*r` reading storage the scope already ended.
  `docs/spec/deferred.md` leaves the outlives constraints between
  regions to the region-annotated captures work, so the check wants that
  relation rather than a rule about one instruction; the case is here so
  that work starts from a reproducer.
- [x] The exhaustiveness check read one level, so `Some(Some(v))` and
  `None` covered `Some` and `None` and `Some(None)` kept no arm: the
  match compiled and the value fell through - a segfault on the host and
  a trap on wasm, with the same hole behind a tuple, a struct field, and
  a nested variant payload. A bare name that spells a fieldless variant
  counted as a binding for the same reason. `patterns_cover` specializes
  one column at a time now, and a row of wildcards ends the descent on a
  type that contains itself. `exe-nested-pattern` and
  `ok-nested-pattern` each held a match genuinely missing `E::C`.
- [x] `size_of::<T>()` is not comp-evaluable: `const N: usize =
  size_of::<()>()` reported `Construct 'intrinsic is not comp-evaluable'
  is not supported`. The comp engine answers `size_of` and `align_of`
  from the type layout now, reading the type argument from the
  instantiation table since a type-only intrinsic carries no value
  argument. `exe-sizeof-comp` holds the values.
- [x] An array repeat count was a decimal only: `[0u8; N]` was refused
  even though the array type `[u8; N]` and the spec's length rule take
  a value parameter or a path to a `const` integer. The literal takes
  the same grammar as the type now, resolving the path through the
  array length check, and a runtime local still reports
  (`ok-array-repeat-const`, `err-array-repeat-not-const`).
- [x] A generic struct literal resolved every turbofish argument as a
  type, so `ArrayVec::<i32, 3> { ... }` for `struct ArrayVec<T, N:
  usize>` reported "A value argument cannot be used as a type" - the
  declaration ADR-0054 names as the value parameter's acceptance case.
  The literal resolves a value parameter's argument as a value now, the
  same mixed rule the type path already used, so the turbofish, an
  inferred expected type, and a value-first parameter list all work.
  `exe-value-param-struct` holds the three.
- [x] A tuple variant's owner turbofish was ignored: for
  `Vec2::<i32, 2>::Items([5, 6])` the instance had to come from the
  payload, which cannot read a value parameter out of an array length,
  so the call reported "Cannot infer the generic type" despite the
  annotation. A named owner instantiates the variant's enum now, and
  the payload resolves under the instance's bindings.
  `exe-variant-value-args` holds it.
- [ ] A type mismatch between two closure literals reads `expected
  'struct', found 'struct'`: each closure literal is its own type, which
  is what the error is about, and the message names the kind both sides
  share instead. Spelling an anonymous closure by its signature, or
  numbering the environments, is what a reader needs - it wants a
  decision on what a closure type is called in a diagnostic.
- [x] The wasm call site pushed each argument's own words, and the
  operand a `()` argument carries is the lowering's placeholder, so the
  placeholder stayed on the stack and the next store read it as its
  address - the call ran, returned the right value, and wrote elsewhere.
  Arguments are matched to the callee's parameter types now, and a
  zero-sized one contributes nothing. A zero-sized load or store was
  also `Construct 'void' is not supported`, which kept `Vec<()>` from
  compiling at all; both are no-ops. `e2e/wasm/unit-arg` holds the
  shapes and `exe-unit-arg` runs them on the host.
- [x] The uninhabited type reached LLVM as `void` wherever it appeared,
  so the same data layout assertion that caught the unit type caught a
  `!` field, a `!` tuple element, a `!` parameter, and a `!` array
  element - the layout gives every one of those size zero. It builds as
  the empty struct now, and the `ret` guard accepts both zero-sized
  tags. `exe-never-shape` holds the shapes.
- [x] The unit type was LLVM's `void` wherever it appeared, so a `()`
  field, tuple element, or argument crashed the emitter on the data
  layout's assertion that a type have a size, while the wasm backend
  compiled the same shapes. It builds as the empty struct the layout
  describes now, a function's return position keeps `void`, an argument
  carries the empty struct its parameter names, and a store through a
  pointer to the unit type writes nothing - without that last part
  `h.a = ()` put eight placeholder bytes over `h.b`, which the layout
  places at the same offset. `exe-unit-field` holds the shapes.
- [ ] An unreachable match arm is not reported: two arms with the same
  literal pattern keep the first and the later one can never run, while
  the compiler does warn about a statement after `ret` (WD003). The
  complete check is pattern overlap - ranges, or-patterns, enum shapes -
  and wants a decision on whether the duplicate is a warning or an
  error; a guarded arm must not count as covering a later unguarded one.
- [x] A UTF-8 byte order mark at the start of a source is an `Invalid
  character` at 1:1: the lexer has no rule for the sequence, so an
  editor that saves one makes the whole file unreadable. Other compilers
  skip it; the spec says nothing, so this wants a decision before the
  lexer learns the three bytes.
  Resolved: the source view drops a leading `EF BB BF`, so the offsets
  every reader sees are the text's own; the grammar's lexical notes
  record the decision. A mark anywhere else stays an invalid character.
- [x] A struct kept two fields under one name and an enum two variants
  under one: nothing at the declaration checked, so a struct literal was
  asked for both fields and an access reached the first, while a second
  variant resolved to whichever entry a lookup reached. The declaration
  site reports the duplicate now, before the generic forms intern per
  use, and `err-duplicate-field` and `err-duplicate-variant` hold both.
- [x] A \u{...} escape whose value is not a Unicode scalar compiled and
  then vanished: the lexer validated the escape's shape only, so
  \u{110000}, a surrogate like \u{D800}, and a digit run past the width
  of the value all passed, and unescaping dropped what it could not
  encode - a string spelled with \u{110000} printed as if the escape
  were not there. The lexer reads the value now, reports it with the
  escape's own span, and the grammar states the range.
- [x] An unsuffixed integer literal in no typed position kept the `i32`
  default whatever it held: `x := 4294967296` compiled to `0`,
  `18446744073709551616` to `0`, and `0x0102030405060708 as u64` to the
  low half of the magnitude. The spec's rule that a literal must fit its
  type ran only for a suffix or an expected integer type, and the
  magnitude parse wrapped past `u64`. The default is checked now, a
  cast's target and the operand beside a literal name the type first, and
  `err-integer-literal-default` and `ok-integer-literal-range` hold the
  two sides.
- [x] The formatter's fixed-point digits were exact only to about
  sixteen places: `{:.Nf}` took each digit by multiplying the fraction
  by ten in `f64`, so the error compounded and the seventeenth place
  was wrong - `{:.17f}` of `0.1` printed `0.10000000000000000` where
  half-up rounding of the value is `0.10000000000000001`, `0.7` printed
  `0.70000000000000000` against `0.69999999999999996`, and `1/3`
  printed `0.33333333333333330` against `0.33333333333333331`. The
  fraction is the dyadic rational `M / 2^K`, and its digits and the
  half-up bit come from one exact product and a shift now, so every place
  is the value's; `exe-fmt-exact` and `e2e/wasm/fmt-exact` hold the
  shapes, and the spec's `N` cap of 17 stands.
- [ ] The exponent form's mantissa is reached by multiplying the
  magnitude by ten or a tenth per step, so its low places are the
  accumulated rounding, not the value's: `{:.17f}` of `f64::MAX` prints
  `1.79769313486234683e308` where the value rounds to
  `1.79769313486231571e308`, about thirteen good places out of the
  seventeen asked for. A single scaled multiplication per step would
  bound the error to one rounding, and the exact answer wants a power of
  ten table or the scaled long division the fraction digits use.
- [x] A cast from a float to an integer was undefined where the language
  says nothing is (ADR-0066): LLVM's `fptosi` left NaN and an
  out-of-range value poison, so a checked build, a release build, and a
  formatter that received the poisoned word each answered differently,
  and the direct-wasm backend trapped. Both backends saturate now
  (ADR-0077), the spec's numeric tower names the rule, and
  `exe-cast-saturate` and `e2e/wasm/cast-saturate` hold the edges of
  every destination.
- [x] Printing a signed type's minimum aborted in a checked build: the
  formatter takes the magnitude with a subtract from zero, which
  overflows for the one value whose magnitude the signed range cannot
  hold, so `println("{}", i32::MIN)` panicked and only a release build
  printed it. The magnitude comes from the wrapping form now, which the
  `MIN / -1` rewrite already uses, and `exe-fmt-min` prints the minimum
  of each signed width.
- [x] A constant comparison folded at the destination's type in the
  release passes: a comparison's destination is a bool, so `-1i8 < 0i8`
  ranked the raw words unsigned and folded to false - an all-constant
  branch in a release build took the wrong arm at every signed width,
  while the debug build, which does not fold, was right. The fold ranks
  the operands' type now, and `exe-fold-compare` holds the pairs.
- [x] A function whose registers pass the wasm local cap produced a
  module no engine parses: the emitter spends one local per register and
  the engines refuse more than 50000, which a function the release
  passes inline a great deal into can reach. The backend reports the
  construct now (ADR-0049), `tools/check_wasm.py` gained an `error`
  field for a case that expects a rejection, and `too-many-locals` uses
  it.
- [x] A frame that reached the direct-wasm stack reserve's end wrote
  itself over the heap: the shadow stack and the bump allocator grow up
  from addresses one reserve apart, so a deep enough recursion replaced
  a live buffer's bytes and the program kept running - a `Vec`
  allocated before the recursion read a frame's own value at its first
  element, and only a recursion that outran the memory trapped. Every
  function compares its frame's new pointer against an immutable global
  the layout publishes and reports "stack overflow" rather than crossing
  it; `e2e/wasm/stack-limit` recurses under the limit with a live vector
  and then past it.
- [x] A raw pointer aborted the direct-wasm emitter at an internal
  check: the element offset `ptr_offset` and `ptr_offset_mut` return
  has a raw pointer destination, and the emit case accepted references
  only. The same program's `(*r).y` asked for a projection through
  `u8`, because a raw pointer base fell to the byte-addressed fallback
  where the pointee is what the walk needs. Both sites dereference the
  pointee the way the LLVM emitter does now, and
  `e2e/wasm/raw-pointer` covers creation, read, write, offset, struct
  projection, and array indexing.
- [x] Narrow arithmetic under direct-wasm left a container that did not
  spell the type: the unchecked form of an `i8` wrap, a left shift, or
  `MIN / -1` kept a value past the type's range, so a compare in a
  release build answered apart from it (`exe-arith-defined` exited 9).
  The arithmetic path canonicalizes a narrow result the way the checked
  path already did, and `e2e/wasm/narrow-arith` runs with `--release`.
- [x] A match over a reference scrutinee wrote its payload binding at
  the pointee's width: the binding's store reused the place operand,
  whose recorded type is the payload's, and a backend that reads the
  operand's type emitted an `i64` store with a pointer word on the
  stack - an invalid module for any `&Enum` with a wide payload. The
  operand restamps itself as the reference now, and
  `e2e/wasm/match-ref-payload` covers the arm.
- [x] Doc-comment collection in the parser: `skip_insignificant` keeps
  the span of each `DocComment` it walks past, the item loop hands an
  item the ones written above it, and `ItemNode::docs` carries them as
  spans into the same bytes. The lexer already tokenized them, so the
  site generator is the only reader left to write.
- [x] The direct-wasm backend typed a field or element address by the
  register beside it, so a `GetElementPtr` into an `f64` declared an
  `F64` local and handed it an address: any program that formats a
  float emitted an invalid module. Addresses are words whatever they
  point at (`shape_of_definition`), `e2e/wasm/float-format` covers the
  spelling, and the guide's `{:.2f}` example is a runnable fence again.
- [x] Consuming `for x in a` over an array failed on direct-wasm: the
  backend's pointer lists named `Ptr`, `Ref`, `MutRef`, and `Function`
  but not the two raw pointers, so the `*T` a cursor keeps in
  `core/slice.al` was "not supported" (EK001). One predicate
  (`is_thin_pointer`) names every tag that rides as an address word
  now, and `e2e/wasm/for-array` walks an array and a `Vec` by value;
  the guide keeps `.iter()` because a by-reference walk is what the
  example wants, not because the other was refused.
- [x] Moving a non-`Copy` element out of an owned array is refused now
  (`lower_moved` sees an array element under the value and answers
  `MoveOutOfElement`), where it used to load a copy that the array's
  glue ended a second time - a double free in safe code. The refusal
  covers a field of an element too (`a[0].name`), and an intrinsic's
  by-value argument takes the same road.
- [x] A match that returned from inside an arm double-freed its
  scrutinee: the move a by-value match records landed at the join
  block, so an arm that returned before it never recorded the move,
  and the scrutinee's drop ran on the payload the arm's binding had
  already taken. The drop walk's event lands before the arms now,
  while the borrow marker stays at the join, after the payload reads.
  `exe-match-move-return` holds it.
- [x] A guarded arm that bound a non-`Copy` value by move took it
  before the guard ran, so a false guard fell through to the next arm
  with the value already gone - a double free in safe code, and
  before that an emitter dominance error from the binding's drop
  flag. A by-value non-`Copy` binding in a guarded arm is refused now,
  and the guard's false edge ends the arm's bindings so the sweep can
  place their drops. `match` on a reference still binds by reference
  (`exe-guard-ref-scrutinee`, `err-guard-moves-binding`). Borrowing
  bindings for a by-value scrutinee is the feature that lifts the
  refusal.
- [x] A pattern condition whose then-arm returned from inside did not
  record the move that takes the bound payload out of the scrutinee,
  so the scrutinee's drop ran on the payload the binding had already
  taken - a double free in safe code. The drop walk's move event lands
  before the then-arm's body now, while the borrow marker stays at the
  arm's tail. `exe-cond-arm-return` holds it.
- [x] A pattern condition marked its scrutinee moved on the else-arm
  even when the pattern did not match, so that value's destructor was
  skipped and leaked. The else-arm ends the scrutinee in place now -
  the end is the move the borrow walk sees - so the value is spent
  once whichever arm runs. `exe-cond-else-drop` holds it.
- [x] An arity message counted in the plural for one: `f(1, 2)` on a
  one-argument call read `expects 1 arguments`. The call, closure,
  variant-call, variant-field, and format-placeholder counts carry the
  singular suffix now. `err-call-arity-singular` holds the call case.
- [x] A constant expression that divided by zero reported `Construct
  'division by zero in comp evaluation' is not supported in this
  lowering slice`: the AST evaluator's generic failure category became
  the lowering's unsupported-construct message. Division by zero is its
  own failure now, with `Division by zero in constant evaluation`
  (ED015) the way overflow already had one. `err-comp-div-zero` holds
  it.
- [x] A type mismatch between two nominals, two instantiations, or two
  closure signatures printed only the kind twice (`expected 'struct',
  found 'struct'`). Diagnostics spell the type the way the source
  writes it now (`type_display`): nominal and arguments, reference and
  pointer pointees, array lengths, tuple elements, and closure
  signatures. `err-mismatch-nominal`, `err-mismatch-generic-args`, and
  `err-mismatch-closure` hold it.
- [x] An unsuffixed integer literal before `as` was typed as the
  destination, so `300 as u8` tripped the literal's range check
  instead of truncating to 44. The literal takes the target only when
  its magnitude needs it (a value past `i32`); otherwise it keeps its
  default and the cast converts. A magnitude past every type it could
  take is still refused. `exe-cast-literal` holds it.
- [x] The exponent form of a float lost mantissa digits: the
  scientific branch normalized to [1, 10) by multiplying by 0.1 once
  per decade, and the roundings accumulated, so `{:.16f}` of
  `f64::MAX` printed `1.7976931348623468e308` where the value's own
  digits are `1.7976931348623157e308`. The mantissa is rebuilt from
  the original magnitude and one correctly-rounded power of ten (a
  table entry and an exact product of tens), with the loop's own value
  standing in only for a subnormal, where the power is past the
  largest double. `exe-fmt-exponent` holds it.
- [x] A struct move-update (`S { a: new, ..base }`) with non-`Copy`
  fields was refused with "Use of moved" on a named base and leaked
  the overwritten field on a temporary one: the lowering marked the
  whole base moved before reading the surviving fields, and the drop
  walk did not track a field a `record_drop_in_place` had ended. Each
  base field now ends in its own place - the overwritten one is
  dropped where it stands, the rest move into the result - and the
  drop walk registers the place a drop ends.
  `exe-struct-update-drop` holds it.
- [x] An enum whose payload is an array of destructible values emitted
  an invalid GEP: the drop glue reached each field through a `u8`
  address and then projected an element from it, so the emitter built
  `getelementptr i8` with two indices and LLVM refused the function.
  The field address carries the field's own type now.
  `exe-enum-array-drop` holds it.
- [ ] Moving an element out of an owned array, which the refusal above
  stands in for: the drop walk's paths name fields and tuple slots, so
  the pass needs an element step that ends the elements the move left
  behind. A value moved out of a temporary array (`f().a[0]`) is the
  same shape one level out.
- [x] Matching over a borrowed value whose type carries a non-`Copy`
  payload reads it in place: the discriminant is read where it lies, a
  pattern's bindings are shared references, and moving a payload out
  stays refused (`err-borrowed-move-out`). A `Copy` payload binds as
  `&T` too, so one rule covers every arm.
- [x] `map` has a constructor: a call reads the closure argument's
  recorded function type, so the element type infers unspelled -
  `v.map((x: &T) -> U)` (`ok-vec-map`, `exe-vec-map`) - and the same
  reading fills a free function's return slot (`ok-closure-infer`).
- [x] An associated function's call infers nothing: the member is
  resolved before its call's arguments are read, so
  `Holder::<i32>::map_to(...)` still spells what a method call now
  infers; a spelled turbofish also counts the synthetic `fn()`
  parameters a declaration carries.
  Resolved: a call's arguments reach the path resolution and bind the
  member's own parameters, so `Pair::id(7)` and
  `Holder::<i32>::map_to(7)` infer; the turbofish count already skips a
  synthetic `fn()` parameter. Case: `ok-assoc-fn-infer`.
- [ ] Negative comp-arg cases for the loosened predicates: a runtime
  value inside a match arm or a block (`double(match runtime_val …)`,
  `double({ y := runtime; y })`) must still be refused, the guard
  check a match now performs wants its own pin, and a generic caller's
  other-instantiation variant record wants one case proving the
  predicate's miss stays a compile error. The spelling and the
  evaluation rules are in `docs/spec/comp.md`; the predicate lives in
  `compiler/analyzer/comp_evaluator.cc` (the parameter checks around
  `:867`) with the interface in `comp_evaluator.h`.
- [x] `size_of::<T>()` is not comp-evaluable: `const N: usize =
  size_of::<()>()` reported `Construct 'intrinsic is not comp-evaluable'
  is not supported`. The comp engine answers `size_of` and `align_of`
  from the type layout now, reading the type argument from the
  instantiation table since a type-only intrinsic carries no value
  argument. `exe-sizeof-comp` holds the values.
- [x] An array repeat count was a decimal only: `[0u8; N]` was refused
  even though the array type `[u8; N]` and the spec's length rule take
  a value parameter or a path to a `const` integer. The literal takes
  the same grammar as the type now, resolving the path through the
  array length check, and a runtime local still reports
  (`ok-array-repeat-const`, `err-array-repeat-not-const`).
- [x] A generic struct literal resolved every turbofish argument as a
  type, so `ArrayVec::<i32, 3> { ... }` for `struct ArrayVec<T, N:
  usize>` reported "A value argument cannot be used as a type" - the
  declaration ADR-0054 names as the value parameter's acceptance case.
  The literal resolves a value parameter's argument as a value now, the
  same mixed rule the type path already used, so the turbofish, an
  inferred expected type, and a value-first parameter list all work.
  `exe-value-param-struct` holds the three.
- [x] A tuple variant's owner turbofish was ignored: for
  `Vec2::<i32, 2>::Items([5, 6])` the instance had to come from the
  payload, which cannot read a value parameter out of an array length,
  so the call reported "Cannot infer the generic type" despite the
  annotation. A named owner instantiates the variant's enum now, and
  the payload resolves under the instance's bindings.
  `exe-variant-value-args` holds it.
- [ ] A type mismatch between two closure literals reads `expected
  'struct', found 'struct'`: each closure literal is its own type, which
  is what the error is about, and the message names the kind both sides
  share instead. Spelling an anonymous closure by its signature, or
  numbering the environments, is what a reader needs - it wants a
  decision on what a closure type is called in a diagnostic.
- [ ] The compiler's stack on a small-stack host: compiling to an
  object needs most of 64 KiB even natively, so the wasm test binary
  runs with a 1 MiB stack while programs it builds keep the default.
  Any standard library growth showed up as a gate failure rather than
  a number; find the deep path, or budget the stack, before the
  headroom runs out the same way.
- [x] A deeply nested pattern hangs the cover walk: a `match` arm of
  (Resolved: the nested enum type was laid out twice per level, which is
  the whole cost, and the cover walk is linear; both shapes below check in
  245M instructions at sixty-four levels. The analysis is kept for the
  record.)
  the form `Option::Some(Option::Some(... (E::A | E::B) ...))` with a
  wildcard arm costs about a second at twenty levels of nesting and
  more than a hundred at twenty-eight, where `alcy check` had to be
  killed. The input is a few hundred bytes, so the cost is in the
  cover walk, not in reading it, and the growth is steep enough to
  read as exponential in the nesting depth. A plain constructor chain
  with no `|` anywhere - `Some(Some(... E::A ...))` and a wildcard
  arm - hangs the same way, so the cost is not the or-pattern
  expansion. Reproduce with `alcy check` on a generated file.
  Confirmed on the release build with `Option::Some(...(E::A | E::B)...)`
  over `Option::Some(...(E::A)...)` and a wildcard arm: a 657-byte file
  at twenty levels takes 160 ms and a 769-byte one at twenty-four takes
  1,901 ms, so the factor per level is about 1.85 in a small input - the
  cost is the cover walk, not the parse. Start from the same shape.
  A counter on `patterns_cover` (`types_expr.cc:3053`) shows the walk
  itself is called fewer than ten thousand times at both depths, so the
  hundred milliseconds to seconds are per call, not from call count:
  the matrix is large and every call works over a copy of it. Its
  signature takes both `types` and `rows` by value (the recursive calls
  at `:3167`, `:3221`, `:3282` move what they can), so specialize the
  matrix in place or share it, and measure the per-call work before
  adding a cache. The matrix is not it either: the cells summed over
  every call (rows times columns) stay under a million at both depths,
  so the walk holds a small matrix and spends its time per cell. Read
  the body (`types_expr.cc:3053` to `:3300`) for the call each cell
  makes - the specialization probably asks the type layer a question
  (`types_equal`, `type_origin`, or a nominal lookup) that is not
  constant time on a deeply nested type, and that is where a nest turns
  into seconds. The head of the body splits or-patterns into one row
  per alternative (`types_expr.cc:3061`-`:3083`) and then scans every
  cell with `pattern_is_wildcard(types[i], row[i])` (`:3093`) on each
  call, which is the first per-cell type question; the specialization
  that follows below `:3100` is the rest. Both are candidates. Time a
  single `pattern_is_wildcard` on a nesting of four, eight, and
  sixteen to see whether one cell costs depth-many steps, because that
  would make the whole walk quadratic before any repeat. It does not:
  `pattern_is_wildcard` (`types_expr.cc:2949`) looks only at the
  pattern's kind, with one scan of the enum's variants for a bare
  identifier (`:2960`), and never recurses into the type. So the scan
  is cheap and the seconds are in the specialization below `:3100` -
  the next read - or in the or-split copy (`:3077`), which duplicates a
  row per alternative. The specialization takes the head type
  (`types_expr.cc:3135`), copies `rest` (`:3136`), and branches on the
  head's tag (`:3138`): `I1` recurses once per value (`:3143`) and a
  nominal presumably once per variant, each with its payload columns
  appended. That branching is the nesting: one `Option<...>` level
  turns into as many recursive calls as the type has variants, so the
  cost follows the type structure, and the backlog's note that a chain
  without `|` hangs the same way fits. The call count stays under ten
  thousand only because wildcard rows return early. Read the nominal
  case past `:3145` next. The enum case is the doubling:
  `case ir::TypeTag::Enum` (`types_expr.cc:3173`) walks every variant
  (`:3181`), appends that variant's payload columns, specializes the
  rows for it, and calls itself once per variant (`:3221`). An
  `Option<...>` level has two variants, so every level turns one call
  into two and the walk doubles per level of the type; the wildcard-row
  early return only trims the branches that cannot matter, which is why
  the raw call count stays under ten thousand while the work still
  grows steeply. `Never`, enum, and `I1` together with the wildcard
  rule are the standard matrix algorithm, and the fix is to stop
  re-deriving a specialization both a Some arm and a wildcard arm
  already agree on - either compare the specialized row sets against a
  cached answer keyed on the type list and the row matrix, or rule out
  the redundant variant first. The construction is written out at
  `:3189`: a wildcard row is copied into every variant as `arity`
  wildcards (`:3192`), a row naming another variant is dropped
  (`:3201`), and a matching row contributes its fields. In the
  reproducer that means the wildcard arm sits in both variants, and a
  variant whose only row is the wildcard returns on the all-wildcard
  scan (`:3090`) without recursing. That reads as linear, so the
  doubling has to be rows that are not all-wildcards accumulating as
  the levels go down. Measure `rows.size()` and the number of cells at
  each level of the nest rather than per call, to see whether the row
  count grows with depth. It does not do so the way the shape
  suggested: a counter that buckets the calls by column count saw only
  columns one and zero, each small on its first entry and neither
  reaching a thousand calls, so the specialization consumes the payload
  column rather than adding one per level - the `Some(x)` at the bottom
  closes the pattern. The recursion is therefore over one column that
  is peeled a level at a time, which is linear in the type, and the
  seconds are not in the row count either. That points back at the
  work inside each call on a deeply nested type: `enum_variants`
  (`:3175`), the `EnumType` copy (`:3176`), and `type_origin` or a
  nominal lookup per call, each of which may walk the whole nesting.
  The or-split is out as well: the same nest without any `|`
  (`Option::Some(...(E::A)...)`) takes 109 ms and 1,513 ms at twenty
  and twenty-four levels against 143 ms and 1,916 ms with it, so
  expanding alternatives is not what costs. Both depths stay under ten
  thousand calls yet the time differs thirteenfold, so the work per
  call grows with the nesting. Time the calls next, not count them:
  accumulate the interval between consecutive entries with a steady
  clock, bucketed by column count, and the bucket that eats the time
  names the operation. The calls are few: no column count reaches five
  hundred entries, so the walk makes only tens of calls over the whole
  nest while the run takes a tenth of a second and up - each call
  costs milliseconds. The entry signature explains the failures:
  `interpret_function` takes a `u64& quota` and a `u32& depth`
  (`compiler/comp/ir_interpreter.h:75`), so the interpreter is bounded on
  purpose and `Interpreter::fail` is the guard firing. A nested chain
  that hits the depth guard fails, that failure propagates one level up,
  and the caller tries the next level and fails again - the doubling is
  that retry ladder, with `is_float_type` on the way. The fix is to try
  the deep evaluation once (or cache the failure per node), not once per
  level of the chain. Read the depth and quota handling around
  `Interpreter::fail` and the call site that retries before changing it.
  costs milliseconds. The symbols are resolved at last, and they are not
  in the type checker: the hot frames are `ir::is_float_type(ir::TypeTag)`
  at 16 percent, `std::optional<comp::IrRunError>::operator=` at 11, and
  `comp::Interpreter::fail` at 8, all in the comp interpreter
  (`compiler/comp/ir_interpreter.cc`, `compiler/ir/type_util.h`). The
  nested constructor chain is being **evaluated** at compile time, and
  `Interpreter::fail` being that hot says the evaluation is failing all
  the way down, level by level. The doubling is therefore comp-time
  evaluation of the chain, not typing, and the fix is on that side: stop
  re-evaluating each level, or memoise the interpreter's per-node result.
  The recipe that made this readable: build with
  `--build-subdir=opt --gn-arg=perf_symbols=true`, profile the kept
  unstripped binary at `out/opt/exe.unstripped/alcy`
  (`build/toolchains/gcc_clang.gni:308`), and resolve the addresses with
  `addr2line` against that same binary.
  costs milliseconds. The documented symbol recipe is
  `--build-subdir=opt --gn-arg=perf_symbols=true` (`compiler/docs/build.md:26`,
  `performance.md:72`), and `out/opt/args.gn` does record
  `perf_symbols = true` - but `nm -C out/opt/alcy` still lists nothing, so a
  strip comes from somewhere other than the `perf_symbols` branch in
  `build/config/compiler/BUILD.gn:251`. Grep the link configuration for
  every `--strip` site and make the build keep the table, then profile
  `nm20` to name the frame; that is still the shortest path to the
  doubling.
  costs milliseconds. Handoff for the tuple-variant branch
  (`types_expr.cc` around the `PathValue::Kind::TupleVariant` case, which
  moved with the rebase - grep it, do not trust a remembered line): the
  per-level work is `nominal_index`, `generic_find`, the inference,
  `push_generic_scope`, `variant_payloads`, and the argument's `unify`.
  All the call counts are flat, so measure iterations inside them, not
  calls: `variant_payloads` over the variant's fields and whatever
  `push_generic_scope` or `type_origin` walks. A symbolized profile
  remains the fastest way to name it - the `perf_symbols` GN argument
  exists for exactly this - so wiring that up is worth more than another
  counter.
  costs milliseconds. The cache was written and it changed nothing: with
  the inference remembered per call expression and instantiation the
  counts are 1,112,963,399, 4,108,665,978, and 16,078,486,237 against
  1,113,192,992, 4,108,422,641, and 16,078,515,028 - a change of two
  hundredths of a percent, so the second walk of the argument is not the
  cost even though it is real. The change was reverted. The doubling is
  therefore in one walk of the nested argument itself, and the next step
  is to count what a single `check_expr` of the argument does per level
  rather than how often it is called - the constructor branch's own body,
  not its repetition.
  costs milliseconds. Confirmed at `types.cc:2335`: inside
  `infer_from_payload_args` the argument is typed with
  `check_expr(module, args[i], nullptr)` just to learn its type, and then
  the caller types the same argument again against the payload
  (`types_expr.cc:1665`). That is two walks per level and the doubling.
  The sound fix is to cache the inference, not the argument's type:
  `infer_from_payload_args`'s inputs are the call expression and the
  instance, both fixed for a given request, so remembering its returned
  instance per call expression and instantiation is safe - the same
  shape as `Checker::call_resolutions_`. Do that, then measure with
  `perf stat -e instructions` and expect the twenty-level file near a
  hundred and fifty million.
  costs milliseconds. The code is `types_expr.cc:1622` onward: a
  generic enum's constructor infers its instance from the argument
  (`infer_from_payload_args`, `:1635`), and then types the same argument
  again against the payload at `:1665`. Two walks of one argument per
  level is the doubling - the same shape as the resolution cost that was
  fixed, one branch over. The fix is the same: remember the inference
  per call expression and instance (or let the later walk start from what
  the inference already computed) so the argument is typed once per
  level. Read `infer_from_payload_args` to confirm it is the walk that
  costs, then cache it.
  costs milliseconds. The last test removes the match entirely: the same
  nested `Option::Some(...)` written as a declaration and nothing else
  takes 1,113,192,992, 4,108,422,641, and 16,078,515,028 instructions at
  twenty, twenty-two, and twenty-four levels - the same numbers to
  within a tenth of a percent as the shallow-pattern file. So there is
  no pattern, no cover walk, and no match in the loop at all: typing a
  nested constructor-call chain is itself exponential. That is the same
  family as the resolution cost already fixed, on the constructor path,
  which does not reach `resolve_generic_fn`. Read `check_call`'s named
  branch for how a variant constructor is resolved per level - the
  `PathValue` kind that is not `GenericFn` and not `Function` - and
  find what it re-derives for the argument's nested type.
  costs milliseconds. The tables do not explode: peak memory is 58.5,
  59.0, and 58.8 MiB at twenty, twenty-two, and twenty-four levels on
  the shallow-pattern file, flat while the time goes 119 ms to 1.15 s,
  and the deep-pattern file is flat at 58.0 MiB too. So the growth
  allocates nothing and touches no growing table, which leaves one shape:
  a loop of constant size inside a function whose own count is flat,
  run far more times than the program has nodes. Count loop iterations
  rather than calls next - `unify`'s per-level recursion and the
  constructor path's element loop are the first two - with a counter
  that adds no behaviour.
  costs milliseconds. The instantiation is flat too: `instantiate_generic`
  (`types.cc:2357`) reaches a hundred and not two hundred at both
  depths. Every operation now has a flat count while the instructions
  double, so the growth has to be in the size of the structures those
  operations touch. Print the type table's size (`builder.types().size()`)
  and the instance and function table sizes at each depth: if they grow
  like `2^depth`, flat-count operations over them explode and the fix is
  to stop minting the intermediate types - the same shape as the
  resolution cache, one level down in the type builder.
  costs milliseconds. The decisive test is a shallow pattern against the
  same deep type - `match Option::Some(...(E::A)...) { _ => 1 }` - and it
  is exponential by itself: 1,113,833,814, 4,108,012,025, and
  16,078,527,379 instructions at twenty, twenty-two, and twenty-four
  levels, a factor of about 1.95 per level. So the growth is in typing
  the `Option::Some(...)` constructor chain, not in the pattern walk at
  all, and `bind_pattern`'s apparent role was the scopes-empty behaviour
  change already noted. This is the same family as the resolution cost
  that was fixed (`Checker::call_resolutions_`) on a different path: the
  constructor chain does not reach `resolve_generic_fn` (its count stayed
  under four hundred), so the cache does not cover it. Find what the
  constructor path re-derives per level and cache that the same way.
  costs milliseconds. The scope scans are real and they are fifteen
  mutation sites: `scopes.emplace_back()` and `scopes.pop_back()` at
  `types.cc:4640/4644`, `7735/7782`, and `7927/7939`, and at
  `types_expr.cc:1313/1341`, `2724/2769`, `3399/3441`, `3939`, and
  `4047/4080`. Replace `lookup_local` and `scope_of` with a map from
  name to the innermost binding, kept in step with those sites: a scope
  records what it shadowed so a pop restores it, which is the usual
  stack-of-overwritten-entries shape. Then measure with
  `perf stat -e instructions`; a quadratic term should drop while the
  doubling stays, confirming the two are separate.
  costs milliseconds. The scope scans are real: `lookup_local`
  (`types.cc:4227`) and `scope_of` (`:4238`) each walk every scope and
  every local in it, so a name lookup costs the whole binding stack, and
  a nest that binds one name per level pays that at every node. That is
  quadratic in the nesting rather than exponential, so it alone does not
  account for the doubling, but it is a genuine cost and a clear fix: a
  map from name to the innermost binding, kept as scopes are pushed and
  popped. Note it and then keep looking for the exponential - the two
  are separate findings and the map can be done first, with
  `perf stat -e instructions` to show what it buys.
  costs milliseconds. The top level is flat too: `check_stmt`
  (`types_expr.cc:4242`) reaches five hundred and not a thousand at both
  depths. So every count is flat - the statements, the expressions, the
  pattern binds, the resolution, the instances - and the instructions
  still double per level. That pins the growth to a loop inside one of
  those functions whose trip count follows the nesting rather than the
  input: something that scans a structure that grows with depth. The
  scopes stack is the first candidate (a scope per nesting level, scanned
  per node), and the payload lists are the second. Count loop iterations
  that touch `scopes` per depth, harmlessly, before reading further.
  costs milliseconds. The count is really flat: a threshold list shows
  seven hundred reached and eight hundred never, at twenty and at
  twenty-four alike, so `bind_pattern` cannot itself be the doubling
  and the earlier bisect conclusion was too quick. Returning false at
  its head also skips every binding, so the scopes stay empty and
  everything after - the cover walk, the rows, any comp-known walk -
  runs on different data. The linear count seen in that experiment is
  most likely that behaviour change, not the function. Redo the bisect
  with a change that does not alter what is bound, or stop bisecting and
  measure instructions per phase: put a counter on the top-level calls
  (`check_stmt`, `check_block`, `check_match`) and compare per depth,
  which needs no behaviour change at all.
  costs milliseconds. `resolve_variant_path` (`types.cc:7539`) is flat
  too - fewer than a hundred calls at twenty levels, a little over at
  twenty-four - so every call inside the tuple case is now flat or
  excluded and the exponential has to be `bind_pattern`'s own loop or
  scope bookkeeping. But the counts themselves do not add up: a
  recursive walk over a twenty-deep pattern and a twenty-four-deep one
  cannot both make exactly seven hundred calls, and `bind_pattern` did.
  Re-measure the call count before drawing anything from it - a total
  printed once at the end rather than a rolling multiple of a hundred,
  or a per-depth tally - because a wrong count has been steering the
  search. If the real count grows with depth, the doubling is the
  recursion itself and the fix is to walk the nest once.
  costs milliseconds. The hypothesis is disproved by its own count:
  `generic_instance_for` (`types.cc:2204`) is called fewer than a
  hundred times at twenty and at twenty-four levels, so the
  instantiation machinery is not what the pattern phase spends. Every
  call inside the tuple case is now flat or excluded - `unify` at five
  thousand, `generic_instance_for` under a hundred, `variant_payloads`
  worse when removed - except `resolve_variant_path`, still unmeasured.
  If that too is flat, the exponential is not a call at all and has to
  be `bind_pattern`'s own loop or its scope bookkeeping growing with the
  nesting; count the elements walked and the scope size per level next.
  costs milliseconds. Strongest candidate, and it is the same family as
  the resolution cost already fixed: `generic_instance_for`
  (`types_expr.cc:151`), reached when the variant's type is generic, is
  where a nested type could be instantiated again per call. The generic
  call path got a cache (`Checker::call_resolutions_`); this one may
  need the same treatment, or the instantiation it asks for may be
  re-minted because a re-parameterised type does not compare equal. Test
  it by measuring the instantiation machinery's involvement in the
  pattern phase rather than by disabling anything: count
  `generic_instance_for` and any `instantiate_generic` it reaches per
  depth, with the signature included in the anchor.
  costs milliseconds. A caution for the next instrument: `resolve_variant_path`
  (`types.cc:7539`) begins with a line that appears three times in the
  file (`if (node.segments.empty())`), so a bare body line is not a
  unique anchor - take the signature and the first body line together,
  or the edit refuses and nothing is measured. `generic_instance_for`
  is still unmeasured. Both are the last two calls left in the tuple
  case, and if both turn out flat the exponential is a loop that grows
  with the nesting rather than a call.
  costs milliseconds. `bind_pattern` is called about seven hundred times at
  both twenty and twenty-four levels - a flat count - yet disabling it
  removes the doubling, so a single call must do work exponential in the
  nesting. `unify` is already counted flat at five thousand calls, so it
  cannot be the exponential part. What remains inside the tuple case is
  `resolve_variant_path`, `generic_instance_for`, and the recursion's
  own bookkeeping. Count those two per depth next; if both are flat, the
  exponential is not a call at all but a loop whose trip count grows with
  the nesting - the binding scope (`scopes`) or a payload list - and the
  fix is to stop rebuilding it per level.
  costs milliseconds. `variant_payloads` is out, and the experiment also
  shows why a careless bisect misleads: with it returning an empty vector
  the counts rise to 4,568,739,870, 13,379,543,278, and 48,601,115,830 -
  slower than the 1.86/7.09/27.98 billion baseline - because every payload
  arity now mismatches and the diagnostics and recovery paths run. The
  doubling is unchanged (about 1.8 times per level), so the helper is not
  the cost, and a target whose removal changes behaviour is a poor
  target. Prefer the ones whose removal is inert: `unify` (already
  counted at five thousand calls) and `generic_instance_for`, or measure
  them rather than disabling them.
  costs milliseconds. The first attempt at bisecting
  `variant_payloads` (`types.cc:7609`) inserted the early return on the
  signature line - a mistake repeated from the earlier attempt - so it
  did not compile and the printed counts were the previous binary's.
  The body's first line is `if (is_error(enum_type) || resolved.enom ==
  nullptr) {` at `:7612`; put the `return {};` before that, never on
  the parameter line. Then measure and revert. The same care applies to
  `generic_instance_for` and `resolve_variant_path`.
  costs milliseconds. No pattern node is revisited: a per-index counter
  at the head of `bind_pattern` never reaches a thousand for any node,
  so the seven hundred calls are spread over distinct nodes and the
  doubling is inside a single call, not from re-descending a node. What
  the tuple case calls on a deeply nested type is the suspect list:
  `resolve_variant_path` (`types_expr.cc:132`), `unify` (`:146`),
  `variant_payloads` (`:147` and `:162`), and `generic_instance_for`
  (`:151`). One of them must cost `2^depth` for one nested type. Count
  and time those four the way the walk was timed, or bisect them one at
  a time, before changing anything.
  costs milliseconds. Its body opens with one case per pattern kind
  (`types_expr.cc:113`: wildcard, identifier, literal, tuple) and the
  tuple case resolves its variant, unifies, asks for the payloads, and
  walks the elements - a single recursion per child, so the doubling is
  not visible from the shape alone. Count visits per pattern index the
  way the expression walk was counted earlier: put a per-index counter
  at the head of `bind_pattern` and print the first index that crosses
  a thousand, with its kind. That name is the node being re-descended,
  and the caller that reaches it twice per visit is the fix.
  costs milliseconds. The bisect ran and found it: with `bind_pattern`
  returning false at its first line (`types_expr.cc:107`, `return
  false;` before its `const bool comp` line) the counts are 147,936,572,
  151,308,289, and 154,600,025 against 1,861,195,838, 7,086,514,185, and
  27,978,134,924. That is ninety-two to ninety-nine percent of the cost
  gone and, more to the point, the growth gone: about two percent per
  two levels instead of ninety-five. `bind_pattern` is the doubling.
  Its call count was seven hundred, so one call must cost `2^depth`
  inside - find the loop or re-descent in its body (`:107` to about
  `:250`) that walks the nested pattern more than once per node and
  make it walk once.
  costs milliseconds. The bisect ran: with `patterns_cover` returning
  true at its first line the counts are 1,779,522,788, 6,768,480,108, and
  26,716,066,218 against 1,861,195,838, 7,086,514,185, and 27,978,134,924
  - a saving of four and a half percent with the doubling still in
  place. The cover walk is four percent of the cost, not the bug, which
  also explains why its seventy calls never added up. The growth is in
  the other ninety-five percent, the pattern typing that runs before
  coverage. Bisect that next the same way: make `bind_pattern` return
  its refutability without walking, and then the scrutinee's typing, and
  read the count after each.
  costs milliseconds. With the documented metric in hand the bisect is
  cheap: disable one suspected pass, rebuild, and read the instruction
  count. The first experiment is the cover walk - return true from
  `patterns_cover` (its signature spans `types_expr.cc:3053`-3054, so
  the early return goes after the opening brace, not on the parameter
  line) and measure. If the count falls to linear, the cover walk is
  the doubling despite its seventy calls; if it does not move, the walk
  is out for good and the next pass is disabled. Do it with the release
  build and `perf stat -e instructions`, and revert the hack either
  way.
  costs milliseconds. The documented metric settles the shape exactly:
  `perf stat -e instructions` on the 657-byte file reports 1,861,195,838
  retired instructions at twenty levels, 7,086,514,185 at twenty-two,
  and 27,978,134,924 at twenty-four - a factor of about 1.95 per level,
  the clean doubling the wall clock only suggested. Two billion
  instructions for a few hundred bytes is the whole bug in one number,
  and the metric is stable, so a fix can be shown with
  `tools/perf.py ab` instead of argued from timings.
  costs milliseconds. There is a documented loop for this and it was
  not being used: `compiler/docs/performance.md` describes
  `tools/perf.py` and says the deciding number is retired
  instructions (`perf stat -e instructions`), which is the same on
  every run, while wall clock only answers whether a change helped. It
  also records that the compiler already carries a `perf_symbols` GN
  argument for names. The next step is to stop timing with a steady
  clock and run the loop the guide gives: `tools/perf.py scale` on the
  nested pattern shape, watch instructions per level grow faster than
  the input, then `tools/perf.py ab` once a change is made.
  costs milliseconds. The profile does name a place, once sorted by
  symbol: three addresses within a few dozen bytes of each other,
  `0x5212ce9`, `0x5212f0d`, and `0x5212dec`, hold about 38 percent of
  the samples, and disassembling the first shows `movups %xmm0,(%rax)`
  and `movups %xmm1,0xc(%rax)` - two sixteen-byte stores - then
  `mov $0x1,%al` and a jump. That is an inlined small copy: the seconds
  are memory copies, and a copy that runs once per call while the
  matrix grows is exactly the shape left open earlier when the by-value
  `rows` argument was set aside. The binary is stripped - `addr2line`
  and `nm` answer nothing - so name the function by rebuilding with
  symbols, or by reading the ASan report's own frames, before changing
  the copy.
  costs milliseconds. The loader frames are an artifact: with
  `LD_BIND_NOW=1` the run takes 135 ms and 1,939 ms at twenty and
  twenty-four levels against 143 ms and 1,903 ms, and `LD_DEBUG=statistics`
  reports a startup of 75,033 cycles with 176 relocations, a few
  hundredths of a millisecond. Lazy binding and the loader are not the
  cost, and the `ld-linux` frames were the symbolizer failing to place
  the compiler's own addresses. The seconds are in the compiler and no
  instrumented function owns them, so the profile has to be made to name
  them: build the release with `-g` and repeat, or resolve the addresses
  against the binary's load base before reading anything into a frame.
  costs milliseconds. `perf script` on the twenty-level nest shows the
  samples landing in the dynamic linker, `ld-linux-x86-64.so.2`, frame
  after frame, with almost nothing in the compiler itself. That is either
  the real answer - the cost is in lazy symbol binding or a loader path
  that a deep input somehow repeats - or an artifact of profiling the
  sanitized debug build, whose runtime makes loader-like calls of its
  own. Separate the two before chasing it: build the release with debug
  symbols and profile that, then check whether the loader still owns the
  samples. The release already shows the same growth (26 ms at twelve
  levels to 1,202 ms at twenty), so if it is an artifact the release
  profile will name a compiler frame instead.
  costs milliseconds. The row builder is a lambda, not a method
  (`unguarded_rows` at `types_expr.cc:2800`, called at `:2885`,
  `:2914`, and `:2934`), and it runs fewer than fifty times - at most
  three per match - over a two-arm match, so it is not the cost either.
  With the expression walk, the cover walk, `bind_pattern`,
  `instantiate_fn`, the resolution, and the row builder all measured
  at milliseconds or less against a 136 ms run, the one-at-a-time
  instrumenting has reached its limit. Build a release with debug
  symbols (`-g`) and profile that directly, so the frame that owns the
  seconds is named instead of guessed.
  costs milliseconds. `instantiate_fn` is next and is also innocent:
  its cumulative interval is about 1.3 ms over its two hundred calls at
  twenty levels and 1.1 ms at twenty-four, so the instances and their
  bodies cost milliseconds. The list of timed-and-cheap functions now
  covers the expression walk, the cover walk, `bind_pattern`,
  `instantiate_fn`, and the resolution. What has never been timed in
  the match path is `unguarded_rows()`, called at `:2885`, `:2914`,
  and `:2934` to build the row matrix, and the outer body of
  `check_match` itself; time those two next.
  costs milliseconds. The pattern machinery is innocent too, and that
  is a correction: timing `bind_pattern` from the inside totals about
  1.1 ms over its seven hundred calls at twenty levels - the 80 us seen
  earlier was the worst single interval, not the average - and the cover
  walk's seventy calls at 481 us worst are likewise well under a
  millisecond together. Every function timed so far costs a millisecond
  or so against a 136 ms run, so the seconds are in code never timed:
  `check_stmt`, `check_block`, `check_fn`, `instantiate_fn`,
  `resolve_type`, or lowering. Time `check_stmt` next, whose count was
  measured but whose interval never was.
  costs milliseconds. The walk is innocent: the whole run enters
  `check_expr` about five thousand times - the print stops before six -
  and the summed interval over every entry is under a millisecond, so
  the nested calls, the paths, and the typing all cost nothing against a
  136 ms run. What is left is the pattern side, and its two functions
  add up: the cover walk's worst interval (481 us) times its seventy
  calls is about 34 ms, and `bind_pattern`'s (80 us) times its seven
  hundred is about 56 ms, together near the run's own time. So the cost
  is per call in the pattern machinery, not in how often it is called.
  Time inside `bind_pattern` per pattern kind next.
  costs milliseconds. A bucket of the intervals by expression kind makes
  the shape of it clear: the first five thousand entries cost under a
  millisecond in total, with `Path` (kind 1) and the nested call (kind
  10) holding almost all of it, while the run takes a hundred
  milliseconds and up - so the cost is in the last few thousand entries,
  not spread evenly. The next print should come every thousand entries
  and report the top kind bucket so the tail is visible, and the kind
  that grows at the end is the case to open.
  costs milliseconds - about seventy microseconds per entry, since the
  whole run enters `check_expr` fewer than ten thousand times at every
  depth. Every count measured (resolution under four hundred, cover walk
  seventy, `bind_pattern` seven hundred, `unify` five thousand,
  `check_expr` under ten thousand) is flat while the time grows 1.85
  times per level, so the per-call cost inside `check_expr_inner` grows
  with the nesting. Bucket the interval between entries by
  `ast.exprs[expr].kind` next and open the kind whose bucket grows.
  costs milliseconds. One call on a deeply nested head type must be
  doing work proportional to the nesting. The candidates inside the
  enum case are `enum_variants(head)` (`types_expr.cc:3175`) and the
  `EnumType` lookup (`:3176`); `tag_of` and `as_enum` are constant time,
  so a lookup that resolves or instantiates the head is what is left -
  the same shape as the call-resolution cost already fixed, and the
  same remedy applies if so. The lookup chain is short:
  `enum_variants` (`types_expr.cc:3037`) asks `nominal_decl(type,
  ast::ItemKind::Enum)` (`:3038`) and returns the declaration's
  variants, so `nominal_decl` (types_expr.cc:2983) is the
  last place a per-call deep walk can hide. It is the same helper the
  exhaustiveness work leaned on, so read it with the nesting in mind:
  if it resolves the type through its origin or a structural
  comparison each time, a nest pays for it once per variant per level.
  It does not: `nominal_decl` (`types_expr.cc:2983`) is a map lookup in
  `nominal_by_type_` (`:2989`), then `generic_find` (`:2997`), then a
  scan comparing `as_enum` shapes (`:3009`), each constant time per
  candidate and none of them walking the nesting. With this helper
  clean, every read of the walk is cheap and no operation walks the
  type, so the remaining explanation is that the walk is entered more
  often than the coarse buckets showed. Count it exactly and time it:
  print every tenth entry with a running total, and keep the largest
  interval between entries with the pattern row that produced it. Done,
  and the answer is that the cover walk is not where the time is:
  `patterns_cover` is entered only about seventy times on the
  twenty-level nest and no interval between entries exceeds 481 us, so
  the whole walk costs a few milliseconds against a 143 ms run. The
  backlog title blames the cover walk and that is wrong. The rest of
  the time is in the pattern work around it - typing or resolving the
  deeply nested pattern before coverage is asked - so instrument the
  passes that build and type a match arm's pattern, not the cover walk.
  That pass is `check_match` (`types_expr.cc:3382`): it types the
  scrutinee once (`:3388`, with the comp-known walk gated at `:3390`)
  and then loops over the arms, typing each pattern below `:3411`.
  The scrutinee here is the nested call chain itself, which the
  resolution cache already made linear, so the seconds have to be in
  the loop that types the arm's nested pattern. Read from `:3411` and
  count the pattern-typing calls per arm the way the cover walk was
  counted. The loop calls `bind_pattern(module, arm.pattern,
  pattern_ty, pattern_by_ref)` (`types_expr.cc:3421`) once per arm, and
  `bind_pattern` (`:107`) is the recursive pattern walk itself - it is
  what decides refutability and binds names - so that is the function
  to count and time next. It is a good suspect for per-node work that
  grows with the type, such as enumerating a variant's fields or asking
  for nominal declarations again at every step, and the fix, when the
  hot spot is named, is the same as the resolution cache: remember the
  answer per pattern and type instead of deriving it twice. It is not:
  `bind_pattern` (`types_expr.cc:107`) is entered just over seven
  hundred times on the twenty-level nest with no interval above 80 us,
  so the whole pattern walk costs milliseconds against a 143 ms run.
  Both pattern-side functions are cheap now, which leaves the
  exhaustiveness driver around `patterns_cover` - `unguarded_rows()`
  and the row building at `:2885`, `:2914`, and `:2934` - and the
  `unify` calls that meet a nested type, as the next things to time.
  `unify` (`types.cc:4164`) is called over five thousand times on the
  twenty-level nest - about two hundred and fifty per level - and no
  interval between calls exceeds 30 us, so it accounts for tens of
  milliseconds of the 143 ms run. That count is the one to pin down:
  measure it at twenty, twenty-two, and twenty-four levels. If it grows
  by the same factor as the run, the pattern typing is calling unify
  once per pair of nested nodes and that is the bug; if it stays flat,
  the remaining seconds are elsewhere and the driver around the cover
  walk is next. It stays flat: `unify` (`types.cc:4164`) is called
  between five and six thousand times at twenty, twenty-two, and
  twenty-four levels alike, so it is a constant cost from the standard
  library's own bodies and not the growth. Every instrumented function
  so far is flat - the cover walk at seventy calls, `bind_pattern` at
  seven hundred, `unify` at five thousand - while the run grows about
  1.85 times per level, so the growth is in a region not yet timed:
  most likely the scrutinee's own typing, the nested constructor chain
  that the resolution cache was supposed to make linear. Time that
  directly next - the `check_expr` on the match scrutinee and the
  entries it makes - because a flat `unify` there means the cache is
  not hitting for this shape. The cache does hit: the scrutinee's
  constructor chain calls `resolve_generic_fn` fewer than four hundred
  times at twenty, twenty-two, and twenty-four levels alike, flat. So
  all four timed functions are flat - the resolution, the cover walk,
  `bind_pattern`, and `unify` - and the run still grows about 1.85
  times per level. The growth must be in a function none of those call:
  `check_expr`'s own entry, `type_origin`, `types_equal`, or
  `instantiate_generic`. Count `check_expr` per depth next; if its
  entry count explodes while every callee is flat, the cost is in the
  walk's dispatch or its type recording, not in the work. It does not
  explode either: `check_expr` is entered fewer than ten thousand times
  at twenty, twenty-two, and twenty-four levels alike. Every count in
  the run is flat while the time grows, and no single interval was ever
  more than tens of microseconds, so the growth is the per-call cost of
  `check_expr` itself accumulating - it is entered the same number of
  times but each entry costs more as the nesting deepens. The recording
  at the end of the wrapper (`modules[module].expr_types.push_back`,
  `types_expr.cc:3482`) is a candidate: a vector that grows with every
  call makes the run quadratic on its own. Time the wrapper's tail and
  the `modules[module]` lookup per depth before looking further.
- [ ] An unreachable match arm is not reported: two arms with the same
  literal pattern keep the first and the later one can never run, while
  the compiler does warn about a statement after `ret` (WD003). The
  complete check is pattern overlap - ranges, or-patterns, enum shapes -
  and wants a decision on whether the duplicate is a warning or an
  error; a guarded arm must not count as covering a later unguarded one.
- [x] References into the type tables held across checking: a struct
  literal's check kept a `StructType&` while a field value's check
  could intern a type and move the table (a heap-use-after-free the
  new std module's layout shift exposed). The audit closes with
  forty-five readers copying their entry - the struct, tuple, variant,
  pattern, element-index, run-index, array, intrinsic, and shape
  readers, the drop walk, the comp evaluator, and every other read of
  a builder table a call could move between. The rule is the file's
  now: a reference into a table the checker can grow is never held
  across a call. The few sites that read the frozen `Storage` keep
  their references, because nothing can move it.
- [x] A discarded temporary that owns memory ends where it stands:
  `_ := format("x")` and a bare `format("x")` are both ended by the
  discard path's `record_drop_in_place`, which the sanitized runs of
  `exe-discard-end` confirm. The `fmt` expansion still calls its
  collected tuple's glue directly, because the tuple is a scratch slot
  rather than a binding the pass could schedule.
- [x] A closure that owns its environment keeps its heap: the
  environment's capture type could resolve to a second type node for
  the same nominal (a `String` node beside the declaration's own), and
  the drop glue lookup answered only the type index a destructor was
  registered under, so the closure's nominal read as non-destructible
  and no drop was emitted. The lookup follows the nominal shape now, so
  a node interned through another path ends the way its declaration
  does: `exe-closure-capture-drop` walks a bound closure and a `fn()`
  parameter that both capture a heap string under the sanitizer, and
  the witness in `exe-method-infer` can hold a `String` again.
- [x] The symbol a summary call names, and the linkage that needs: each
  summary row carries the mangled symbol the library's own build writes
  (the emitter's encoding, computed where the artifact is written), the
  dependent's call declares it in the external table, and the target
  links the dependency's object. The runtime's entry points were already
  `linkonce_odr`; the two argument globals are discardable now too, so
  two objects in one image keep one copy. A package whose library has a
  summary resolves, links, and runs with the library's sources gone
  (`dependencies_test` builds both and runs the binary).
- [x] A summary carries no struct field names: the section writes the
  fields each struct row declares - name and visibility, in declaration
  order - and the surface spells a nominal row into its declaration (a
  struct from the runs, an enum from the variant names its table
  holds), so a dependent names the type, constructs a literal, and
  reads a field across the boundary. `dependencies_test` does all three
  with the library's sources gone. A signature that *names* a nominal
  is still left out, and the reason is the item below.
- [x] A summary call passes what the callee takes: the call handed the
  argument's place over where the callee takes its value, so a field
  read as an argument met the emitter's LLVM assertion. It crosses as
  the value now, the way a C entry point's does, and a nominal
  parameter, a nominal return, a nested nominal field, and a reference
  to a nominal all work - `dependencies_test` runs all four with the
  library's sources gone. Spelling a signature through a copy was
  refused while this was open; a row spells it now, because identical
  shapes lay out alike.
- [x] Associated functions and methods in a summary: a row names the
  nominal its item belongs to now (`owner`, the type the declaration
  names), and the surface groups the rows by it into the `impl` the
  declaration's module holds, after every nominal has been spelled. A
  method spells its receiver as `self` - `mut self` when the receiver
  takes an exclusive borrow, which its type says - and its call crosses
  as the value the callee takes with the receiver as the first
  parameter; a call names the callee's own item before its name, which
  two impls could share. A comp function gets no row, because its call
  belongs to the package's build rather than a dependent's.
  `dependencies_test` calls `Pair::new` and `p.total()` with the
  library's sources gone.
- [ ] A generic whose signature names an owning function type cannot be
  checked once, because the closure's witness is per instantiation
  (ADR-0075): the type the body returns is not the type the declaration
  names. Making the closure's type carry its own parameters is what
  would let such a generic be checked once, and cross a package
  boundary with the rest.
- [ ] The generic boundary in a summary: a `needs_body` row is
  omitted, so a dependent that names a generic item reads "Unresolved
  import" without learning why. The IR has no parameterized types (its
  tags are concrete), so a declaration's shape cannot be spelled; it
  exists only per instantiation. The decision is ADR-0079: a generic
  item crosses as its declaration and its checked body, and the
  dependent instantiates it. What remains is that work - a type
  parameter in the IR, the body checked once against its parameters, the
  specialization pass, the body in the summary, and the AST-level
  instantiation walk deleted.
- [ ] `core::string`'s `trim_start_matches` once reported `Returns
  reference to local 's'` for the `None` arm's `ret s` - the loop
  reassigns `mut s` from `s.strip_prefix(pat)` and the local holds a
  slice view, so the loan's place is the local while its pointee is the
  text. Six shapes faithful to it pass now (the free function with one
  and two parameters, the method form, the staged prelude reached from
  a program, and both reassignment orders), so the function alone does
  not produce the report; it came from a configuration that no longer
  exists (a summary call declared as a bodyless foreign function, which
  reached std code the extern path never reaches) and rebuilding that
  route is what would say whether the report was a false positive about
  a loan's place or a conservative rule worth keeping.
- [x] One nominal as several type nodes: the checker interned a
  second type node over a declaration's own shape (a `String` node
  beside the declaration's), and `types_equal` compares origins, so two
  nodes over one declaration read as different types while only the
  drop lookup was taught to see through them. The observable symptom
  was a two-node dump (`struct Pair(i32, i32)` at #19 and #158) that
  made a nominal-typed signature unspellable. It no longer reproduces:
  a library with a struct, a nested struct, an impl, and calls dumps
  one node per declaration now, and the dependent spells, links, and
  runs a nominal parameter, return, nested field, and reference. The
  diagnosis that stands is that the object a field or parameter holds
  is a storage copy - a reference the checker records an origin for,
  which lowering follows - so a consumer that meets one resolves it by
  shape; if the extra node returns, the reader audit's rule and
  `type_origins` are where to look.
- [x] Manifest lint: a pre-1.0 version range warns (WH007), since a
  minor may break under 1.0. The rule reads the parsed requirement -
  one `Equal` bound is a pin and stays silent - so `0.5.x`,
  `>=0.5.0, <1.0.0`, and a stray leading space all warn.
- [x] The requirement message named forms the grammar refuses: `1.2`
  and `1` are not terms - `version_req_test` pins what parses, a
  three-component version or one whose last component is `x` - while
  the message read as if they were. The message names the forms that
  parse (`1.2.3`, `1.2.x`, `1.x`) and `err-dep-version-form` pins it.
- [x] Format-string lint: the explicit `f` conversion (`{:.2f}` over
  `{:.2}`). The analyzer warns (WC045) and names the spelling to
  write; both spellings stay one piece, so the parser is unchanged.
- [x] The benchmark fixtures' own commands were outside the gate: smoke
  checked that every case was present and that a generated source still
  compiled, but not that a case's command succeeded, so three fixtures
  rotted silently when the private-access check landed
  (`build/executable`, `check/package`, and `check/wide-package`).
  Smoke runs each process case once with its `expected_exit` now, and
  its first run found the second and third.
- [x] Link-time measurement: the embedded linker's one-time startup
  probe and the `lldMain` call are their own trace rows
  (`lld-startup`, `lld-link`) beside the object write `emit-object`,
  so `alcy build --time-trace` reports where a link goes. The suite's
  `build/executable` case measures the wall time and asserts nothing
  about it, which is what the benchmarks README says.
- [ ] The embedded linker beyond this host: startup discovery hard-codes
  Linux paths, only the ELF driver is embedded, and ADR-0049's M3 (the
  wasm lld driver and the portable full compiler) is unstarted.
- [x] A binary with no free `main` is diagnosed before the linker
  sees it: `check_entry_point` runs per resolved target, so `check`
  and a `build` that would link both name the target (EI024). An
  associated `main` is not an entry point, and a library target needs
  none.
- [x] A non-regular input is refused rather than read as empty: a pipe
  or a device reports no size and cannot be mapped, so `check --file`
  on one would check a program out as nothing. `SourceManager::load`
  answers `NotRegular`, and `check` and `compile` name the path; the
  POSIX-guarded unit test uses `/dev/null`. A pipe still reaches the
  compiler through `compile --stdin`.
- [x] Container destructors end their elements. `Vec::drop` clears
  through `drop_in_place` once per live element, and `Map::drop` moves
  each live pair out of its slot into a binding the drop pass ends.
  The sanitized runs of `exe-vec-drop` and `exe-map-drop-element`
  report nothing left allocated; both walked a container of values
  whose destructors own a block.
- [ ] IR cache shared across profiles and machines. The lowered IR is
  one artifact per pointer width: a test lowers one source under two
  64-bit triples and compares the bytes (`pipeline/ir_emit_test.cc`),
  the release passes run after this boundary (ADR-0067), and the
  serialized form states the width in its header. The cache itself —
  a store keyed by package digest, dependency closure, format
  version, and pointer width, serving debug and release and every
  machine of a width — is the work. What it needs first is the
  summary-carrying artifact story (a library object is still opaque,
  `roadmap.md` item 1) and an answer for comp-folded `usize`
  constants, which currently bake the width into the immutables.
- [x] Inbounds GEPs and memcopy alignment. Lowering marks checked
  indexing, field projections, and aggregate construction inbounds,
  and the memcopy intrinsic carries the element alignment its two
  pointers share; serialization, the LLVM emitter, and the IR guide
  follow. The direct backend ignores both.
- [ ] Devirtualization of function values. A call through a value is
  an indirect call in both backends; LLVM sometimes folds it after
  inlining, and the direct backend never does. The adapters store
  their function in a struct field the caller fills, so the pass
  needs cross-function facts and lands once the pass stage grows a
  module pass.
- [ ] Bounds-check elision. Checking an index at every access costs a
  compare, a branch, and a panic block; a literal index the array's
  own length admits now skips the check entirely. What remains is the
  range analysis: `0..len` loops and repeated indices would remove
  the checks an earlier one dominates, and the `Switch` opcode stays
  unemitted for match chains.
- [ ] Index the side tables the proportionality work left behind. Most
  landed (ADR-0046); the tail is `expr_types` in both readers, the
  per-module closure, closure-fn, and indirect-call tables,
  `instantiate_fn`'s instance scan, `fn_args_for` and `closure_fn_index`,
  and the module statics. `Lowerer::expr_type` scans backward ("last
  entry wins") while `CompEvaluator::expr_type_in` scans forward ("first
  wins"), so the readers can disagree on a duplicate key, and
  `MAX_FN_ENTRIES = 8192` reports `ED013` at the call that spends the
  last slot now, rather than failing at an unspecified internal span;
  raising the budget itself waits for the indexing.
- [ ] Split `Lowerer`: comp evaluation, format expansion, and AST-to-IR
  lowering share one class with a large public surface.
- [ ] Borrow-pass scratch sized by the program, not the function: the
  arrays are allocated at package size and each function clears only the
  entries it names. The final pass's per-worker memory cost is why
  spreading it lost by measurement (ADR-0047), so the sizing waits for a
  reason to spread it.
- [x] A moved-from owning closure was still callable: `h := g` handed
  the environment to `h`, but `g()` still passed `g`'s own storage as
  the environment and read what `h`'s drop freed. The borrow pass
  refuses a store whose value is the address of a moved place, which is
  the shape a closure call writes its environment through;
  `err-closure-move-reuse` holds it. A place reached through a deref is
  exempt, because the value names the referent rather than the slot a
  move takes.
- [ ] Two handles over one exclusive loan: a closure that owns nothing
  is not moved (its captures are borrows, ADR-0064), so `h := g` on a
  closure holding a `&mut` capture gives two handles that share one
  loan, which `exe-closure-capture-mut` expects; whether both may write
  through it is the borrow checker's question rather than the move
  walk's.

### Language and library surface

- [ ] The formatter's fixed-point digits are exact only to about
  sixteen places: `{:.Nf}` takes each digit by multiplying the fraction
  by ten in `f64`, so the error compounds and the seventeenth place is
  wrong (`{:.17f}` of `0.1` prints `0.10000000000000000` where half-up
  rounding of the exact value is `0.10000000000000001`; `1/3` prints
  `0.33333333333333330` against `0.33333333333333331`). An exact
  conversion over the mantissa, a scaled long division, is the fix; the
  spec's cap of 17 places stands.
- [ ] The direct-wasm stack budget: a frame past the reserve (the
  shadow stack's base) panics with "stack overflow", which is a
  per-target limit native does not share and no spec states; a
  variable-length alloca still walks past it unchecked, and
  `too-many-locals` provokes the local-cap report through release
  register counts, so it wants rechecking when regalloc-adjacent work
  lands.
- [x] A moved-from owning closure was still callable: `h := g` handed
  the environment to `h`, but `g()` still passed `g`'s own storage as
  the environment and read what `h`'s drop freed - the byte it printed
  changed from run to run. The borrow pass reports a store whose value
  is the address of a moved place, and a closure that owns nothing
  stops being moved at all: its captures are borrows, so a second
  handle shares the one loan (ADR-0064). `err-closure-move-reuse`
  holds it.
- [ ] Owning function types (ADR-0075) beyond parameters and returns:
  a method's `fn(...)` parameter infers from its arguments, a turbofish
  spells only the parameters a declaration writes, and the return
  position takes its witness from the body's first `ret` - free
  functions and methods alike (`ok-own-fn-return`,
  `exe-own-fn-return`, `err-own-fn-return-mismatch`,
  `err-own-fn-return-missing`, `ok-own-fn-turbofish`). What remains: a
  spelling nested inside another type (`-> Option<fn(i32) -> i32>`, a
  struct field) still refuses (`err-own-func-type`); `fn()`-typed
  bindings are not yet move-only (reassignment must be refused); and
  named storage (`struct H { f: fn(...) }`) needs generics threaded by
  hand.
- [ ] A reference stored into a longer-lived place outlives its
  referent: `mut r: &i32 := &0; { n := 5; r = &n; } *r` compiles, and
  so does the same store through a struct field or an `if` arm, which
  leaves `*r` reading storage the scope already ended.
  `docs/spec/deferred.md` leaves the outlives constraints between
  regions to the region-annotated captures work, so the check wants that
  relation rather than a rule about one instruction; the case is here so
  that work starts from a reproducer.
- [x] Two-phase borrows: a call's exclusive receiver is a reservation
  taken before the arguments evaluate, and the call itself activates
  it, so `v.push(v.len())`, `v.push(v[0])`, and
  `v.insert(v.len(), x)` resolve. A read that is still live at the
  activation - a view handed in as an argument, a closure that captured
  the place - still reports (`ok-borrow-two-phase`,
  `exe-borrow-two-phase`, `err-borrow-two-phase-arg`,
  `err-borrow-two-phase-escape`).
- [x] A load of scalars owns its copy: the element load kept the
  receiver's loan alive in a container accessor's answer, so
  `d := v.pop()` on a `Vec<Point>` read as a borrow of `v` and
  refused the next `v.push`. The load now asks the same predicate an
  indirect call's result does, so a value that cannot carry a loan ends
  the borrow at the copy while one that holds references or views keeps
  the protection (`ok-borrow-owned-element`,
  `exe-borrow-owned-element`).
- [x] A method that takes its receiver by value moves a field receiver
  out of its owner: the call marks the field's place moved, so
  `s.b.unwrap()` with `b: Option<Vec<i32>>` retires the payload once
  (`ok-field-receiver`, `exe-field-receiver`).
- [x] A matched non-Copy field is moved out of its owner: the match
  marks the field's place moved, so `match s.b` with
  `b: Option<Vec<i32>>` retires the payload once
  (`ok-match-field-move`, `exe-match-field-move`).
- [x] A field hands back the type it declared: the reader returned the
  stored copy, and an enum field lost its variants with it — a match
  on `s.b` reported "Variant is not a member of the matched type" and
  `s.b.unwrap()` aborted, while a local of the same type was fine
  (`ok-enum-field`, `exe-enum-field`).
- [x] Nested type arguments close without a space: the type list stops
  when a `>` is banked, the guard the turbofish path already had, so
  `a: Vec<Vec<i32>>` parses (`ok-nested-generics`,
  `exe-nested-generics`).
- [x] The LLVM textual-IR emission under the wasm-host test suite: the
  runtime helpers' shapes were the 64-bit spelling of pointer-width
  values, and with the canonical `i64` shapes and the runtime's own
  widening, `emit_mode_test`'s "writes the module as textual IR" and
  the CLI's same case pass under the emscripten host.
- [ ] The tree-sitter grammar reads a condition that opens with `(` as
  a pattern, so `if (x) == (y) {}` and `if (x, 1) == (y, 1) {}` parse
  as an error there while the compiler accepts them. The
  structural-equality cases bind their tuple comparisons first to keep
  the differential check green; a condition grammar that forks on the
  token after the closing `)` is what would let the inline form
  through.
- [x] A UTF-8 byte order mark at the start of a source is an `Invalid
  character` at 1:1: the lexer has no rule for the sequence, so an
  editor that saves one makes the whole file unreadable. Other compilers
  skip it; the spec says nothing, so this wants a decision before the
  lexer learns the three bytes.
  Resolved: the source view drops a leading `EF BB BF`, so the offsets
  every reader sees are the text's own and a file with the mark and one
  without are the same program; the grammar's lexical notes record the
  decision. The mark stays an invalid character anywhere else.
- [x] A struct kept two fields under one name and an enum two variants
  under one: nothing at the declaration checked, so a struct literal was
  asked for both fields and an access reached the first, while a second
  variant was unreachable. Both declarations reject a repeated name now,
  naming it, and the surviving member is the first
  (`err-duplicate-field`, `err-duplicate-variant`).
- [x] Qualified generic calls: `helper::id::<i32>(x)` answered
  "Unresolved value 'id'" because neither module-qualified resolution
  branch reached the generic-function lookup. Both do now, with the
  reachability check a qualified name owes (`ok-qualified-generic-call`,
  `err-qualified-generic-private`).
- [x] An associated call bound the owner's turbofish to the member: the
  two-segment branch passed the whole path's arguments on, so
  `Holder::<i32>::map_to(true)` pinned the member's first generic to
  `i32` and refused the argument, and an owner with two parameters
  reported a mismatch against the member's one written slot. A call's
  arguments bind the member's own parameters now, the way a method
  call's do, and the owner's turbofish stays the owner's
  (`ok-assoc-fn-infer` pins the numeric and the differently-typed
  call).
- [x] An associated function's turbofish names its own arguments: the
  parser records which segment the turbofish followed and the resolver
  reads it, so `P::id::<i32>(x)` instantiates the member while
  `P::<T>::id(x)` names the owner's (`ok-assoc-fn-turbofish`).
- [x] Read-before-write through an operator: an assignment reads its
  value before it takes the place, so `v[i] = v[i] + 1`,
  `m[k] = m[k] + 1`, and the compound forms compile without a
  temporary. A call that both reads and writes resolves the same way:
  the receiver is a reservation the call activates, which is the
  two-phase rule above.
- [x] A returned error from `main() -> Result` is an exit status: the
  message goes to stderr and the process exits 1, matching the direct
  backend, where LLVM's entry used to call `alcy_panic` and abort with
  status 134.
- [x] Structural equality for tuples and arrays: `==` and `!=` walk the
  elements when every element is itself builtin comparable, which is the
  rule `docs/spec/types.md` already stated.
- [x] Float ranges: `for x in 0.5..<2.5` answers one diagnostic ("Float
  ranges do not iterate"). The arm of the desugared `Some` match binds
  its names against the error type now instead of being skipped, so the
  loop body no longer follows with an unresolved name (or a variant
  that "is not a member of the matched type"). `err-float-range` pins
  both halves.
- [x] A borrow of a temporary works (`a.eq(&2)`, `takes(&format("y"))`,
  `format("x").to_ascii_lowercase()`): the lowering gives the value a
  slot in the frame and loans that, and an owning one becomes an
  unnamed local the drop walk ends at the enclosing block, so no
  reference outlives it. `exe-borrow-temporary` and
  `exe-borrow-temporary-owning` walk the shapes under the sanitizer.
- [x] `Option::is_some`/`is_none`/`is_ok`/`is_err` took the receiver by
  value. They now read `self: &Self` (`lib/std/core/option.al:14`,
  `result.al:17`), so an owned option answers twice; the predicates are
  the place to pin that.
- [x] Standard dependency selection: the overlap diagnostic and the
  `--deps` help name the default `alcy/std/*` glob and the rule, so
  `--deps alcy/std/collections` explains itself: name a package or a
  member, not both.
- [x] `Map::get_or_insert(key, value)` returns the entry as `&mut V`,
  inserting first when the key is absent, and a missing key in `m[key]`
  panics with a map-specific message instead of the `Option::unwrap`
  wording.
- [ ] `Map` key ergonomics, the rest: an `Entry` once the borrow rules
  allow, generic keys, which wait for a `Hash` spec, and
  bounds; `keys()` traversal and `Vec` sorting moved to the roadmap's
  iteration and operator items.
- [x] Trace report nesting: the scan kept only the interval on top of
  the open stack, so an interval a later event spilled out of was gone
  before a shorter event that still fitted inside it arrived, and two
  threads reshaped the tree. The parent is the smallest span among the
  open intervals containing the event now, which one thread cannot tell
  apart from the old rule.
- [x] `demangle` recursed per type constructor with no depth limit. The
  guard is `decode_type` in `compiler/symbol/mangle.cc` (`:381`),
  which spends the language's nesting budget and stops a hostile
  symbol before the stack; `mangle_test.cc` follows a type nested
  within it (`:319`) and refuses one past it (`:332`).

- [ ] A value's type is named in three places, and a place is not a
  value: a register's row, the operand that names it, and the
  lowering's own value each carry a type, and the verifier compares
  none of them. The direct-wasm backend reads the operand's, so a
  lowering site that disagrees with the register writes a word of the
  wrong width - the borrowed binding's fix restamped one operand to
  match, and the backend's `is_address` case and the `Val` constructions
  around `lowering_expr.cc:377` are the same gap. A place's address is a
  pointer value, but a register that holds one is declared with the
  pointee's type, so the IR cannot say what the register holds. One
  home for a value's type, and a place that is not a value, is the
  fix; the verifier's silence is what let it drift.
- [ ] The direct-wasm backend has no allocation model: every SSA
  register gets a wasm local of its own, against ADR-0049's slot model,
  so the local count is the register count and one inlined call crosses
  the engine's cap, allocas need a second frame pass, and the stack
  guard costs four or five instructions per call in functions whose
  frame cannot reach the limit. The opcodes are raw hex whose only name
  is a comment, so a wide shift was misencoded while the suite was
  green, and a narrow result is canonicalized per operation rather than
  at one destination rule.
- [ ] A closure's environment is the closure's own address, so the
  value is self-referential and cannot be moved: a store of the address
  of a moved place is refused, and a closure that owns
  nothing is declared not moved, which is not a property of its type -
  a `&mut` capture is an exclusive loan, and two handles over one
  loan are what `exe-closure-capture-mut` expects. The environment
  belongs outside the value, or beside the call as its own operand.
- [ ] ADR-0076 says the region inference is total, and the backlog
  holds a program that reads dead storage and compiles. Until the
  solver exists, that record is a claim the code does not make, and a
  checker that accepts incorrect code is worse than one that refuses
  correct code.
- [ ] A summary identifies a nominal by comparing two type graphs, so
  two declarations with the same shape are the same type: the matcher
  walks both graphs with a pair budget, and a declaration past it, or
  one that two rows match, is dropped without a word. A declaration
  identity in the artifact, and a comparison by it, would end the
  matcher, the name fallback, and the per-occurrence re-spelling.
- [ ] A summary carries no spec implementation, so a dependent cannot
  see that a library's type implements one, and the roadmap asks a
  summary to carry them.
- [ ] The cache is consulted after the frontend has parsed and
  resolved, so a hit cannot skip the work that dominates, and the
  compiler's version string does not identify the binary that wrote an
  entry: two builds of one dirty tree share every key.
- [ ] `alcy run` compiles without the cache, so a run pays the front
  end every time. The build's cache lookup and the run's lowering are
  two paths over the same work, and the run path is the one that has
  no key.

## Shipped

- [x] Noun-form audit: modules, structs, and classes read as nouns
  (stdlib and compiler alike). The rule is that the last word of a name
  is a noun, recorded in `CONTRIBUTING.md`; the audit found `lower`
  (now `lowering`), `Desugar` (now `Desugarer`), `FmtParse` (now
  `FmtTemplate`), and `VerifyError` (now `VerificationError`, which also
  retired a name two modules shared), plus three ways one concept was
  spelled. Stdlib needed nothing. `debug` keeps its name as the one
  recorded exception.
- [x] Verb split: `compile` takes a file, `build` a package, `check`
  neither emits. The three verbs each sniffed the target's extension, so
  the sniff lived in three places and `build` carried options for two
  jobs. Designed in `docs/adr/0018-compile-build-check-split.md`.
- [x] Per-goal configuration under `.alcy/`: `alcy.toml` declares what
  a package is, so build choices moved out of it rather than growing
  beside it. Designed in `docs/adr/0019-goal-config-in-dot-alcy.md`.
- [x] Version control option for `new`/`init`: scaffold the ignore
  file for the chosen VCS, never initialize a repository.
- [x] Command result envelope: one output path rendering as text or
  `--json`, with statistics in the text line and the time trace
  embedded alongside it. Designed in `docs/adr/0020-command-result-envelope.md`.
- [x] Result lines as a column: counts carry their nouns, sizes scale
  past a kibibyte, and the verb sits at a fixed width so a session's
  results read as a table. A run announces itself before the program
  starts rather than reporting below its output.
- [x] Optimized builds: wire the optimization level through to LLVM
  behind the existing `--release`. The flag reached the object file and
  not the textual IR, because the pass pipeline ran inside the emitter
  rather than on the module; every backend now sees one module.
- [x] Benchmark harness: an engine for the compiler's phases beside the
  process runner, with a local reproducibility check and a CI smoke
  check. Designed in `docs/adr/0021-benchmark-measurement.md`. Nine of the seventeen phases have
  cases; `load`, `prelude`, and `link` have none on purpose, since each
  is a read, a staging of the embedded suite, or a subprocess, and a
  number for those belongs to the process runner.
- [x] `grammar.ebnf` maintenance: every grammar change diffs the file
  in the same commit.
- [x] `build/scripts` → `tools/` rename, in one shot.
- [x] Samples: `fizzbuzz`, `fibonacci`, and `primes`, run through the
  `exe` harness by `--cases-root` so an example that stops compiling
  fails the build.
- [x] `--emit=llvm-bc`: the module as LLVM's bitcode — the same content
  `--emit=llvm-ir` prints, in the form another LLVM tool reads without
  parsing text, and the second mode that needs no target.
- [x] Link arguments: `--link-args` on `build`, `run`, and `compile`,
  plus `link-args` in `.alcy/toolchain.toml`. They are the driver's own
  arguments, so a library or `-fuse-ld=lld` reaches it unquoted, and a
  flag replaces the file's list the way `--linker` replaces the file's
  driver. Before it, no external library could be linked at all.
- [x] Runtime emitted in process: the `alcy_*` functions are defined in
  the module our own codegen already builds, so no compiler is spawned
  for them on every build, the link line loses an input, and the
  runtime is optimized with the program. A hello-world's link phase
  went 60.1 ms → 29.2 ms and its total 114 ms → 85.2 ms. The pinned
  LLVM ships no clang and a host-made `.bc` would couple every build to
  its version, so the functions are built as IR rather than linked in.
  Designed in `docs/adr/0023-program-runtime-in-process.md`.
- [x] Diagnostic code space: a letter per component, and each component
  counting its own ids from 1. The letters are `A` lexer, `B` parser,
  `C` analyzer, `D` lowering, `F` borrow, `G` ir, `H` pkg, `I` pipeline,
  `J` codegen_llvm, `K` codegen (native, reserved), with `E`/`N`/`W`
  held back for severity, so a code reads `error[EA001]`: severity,
  component, id. The reason is that the 1000-per-stage stride has no
  room for a second code-generating backend, that its sub-ranges are
  narrower than the stages that use them, and that a stage should not
  have to know the layout of the whole compiler to pick a number. The
  registry in `compiler/docs/diagnostics.md` stopped restating the
  numbers; the enums own them, and every id is spelled out.
  Designed in `docs/adr/0039-diagnostic-code-space.md`.
- [x] CI carries the gates: `lint`, the coverage ratchet, the sanitized
  exe run, `check.sh` itself, and the benchmark smoke are jobs in
  `ci.yaml`; `style.yaml` keeps what needs no compilation database. The
  smoke ran in all ten matrix entries, judging shape and not timing,
  which is ten answers to one question. Cost was not the obstacle for
  any of them - two documents called the coverage ratchet the slowest
  gate in `check.sh` and it is among the cheapest, which
  `tools/measure_gates.py` now reports rather than leaves to be believed.
  Designed in `docs/adr/0040-gates-live-beside-the-builds.md`. The
  `check.sh` job was later removed - it rebuilt what the matrix had just
  built and asked every gate a second time on one runner - and the union
  it stood for moved into `check.sh` itself, which now also runs the
  release-mode gates, the wasm acceptance, and the benchmark smoke; the
  ADR's amendments record the change.
- [x] `unsafe` as a gate on operations: `unsafe fn` and
  `unsafe { ... }` blocks, always lexical, with the block's value
  being its own. The precondition-carrying intrinsics are declared
  `unsafe` and the checker verifies the marker against its canonical
  set, the standard library wraps its uses, and the borrow checker's
  block handling was fixed so a move inside a block stays moved.
  Raw pointers followed: `*T` and `*mut T` are thin, Copy, and
  outside the region system, safe to create by cast (including
  `0 as *T`), and read, written, and offset behind the gate.
  `extern "C"` declarations followed: bodyless functions whose names
  are the linker's symbols, called behind the gate, over the ABI of
  scalars, raw pointers, and `()`.
  A freestanding link mode followed: `freestanding = true` in the
  toolchain file, the compiler's `_start`, a per-declaration runtime
  reaching the kernel through raw syscalls, and an `mmap`-backed
  allocator, so `print` and the heap containers run with no crt, no
  loader, and no libc.
  Designed in `docs/adr/0050-unsafe-is-a-gate-on-operations.md`,
  `docs/adr/0051-extern-c-for-a-minimal-abi.md`, and
  `docs/adr/0052-freestanding-is-a-link-mode.md`.
- [x] alcy IR format: define, serialize, deserialize; ahead of
  `--emit=ir` and any cache. The text form is write-only by decision
  (`docs/adr/0056-the-ir-has-a-text-and-a-binary-form.md`) and the
  binary form round-trips byte for byte; the cache is still to come.
