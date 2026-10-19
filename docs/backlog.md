# Backlog

Peripheral work, unordered. When every item is checked the list is
replaced. The foundations are in `roadmap.md`.

## Open

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
  is only mis-typed when the index expression is a method call's
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
- [ ] `size_of::<T>()` is not comp-evaluable: `const N: usize =
  size_of::<()>()` reports `Construct 'intrinsic is not comp-evaluable'
  is not supported`. The answer is a property of the layout the lowering
  already holds, and an array length written with it would fold. It wants
  a decision on which layout queries the comp engine may answer.
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
- [ ] A UTF-8 byte order mark at the start of a source is an `Invalid
  character` at 1:1: the lexer has no rule for the sequence, so an
  editor that saves one makes the whole file unreadable. Other compilers
  skip it; the spec says nothing, so this wants a decision before the
  lexer learns the three bytes.
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
- [ ] An associated function's call infers nothing: the member is
  resolved before its call's arguments are read, so
  `Holder::<i32>::map_to(...)` still spells what a method call now
  infers; a spelled turbofish also counts the synthetic `fn()`
  parameters a declaration carries.
- [ ] Negative comp-arg cases for the loosened predicates: a runtime
  value inside a match arm or a block (`double(match runtime_val …)`,
  `double({ y := runtime; y })`) must still be refused, the guard
  check a match now performs wants its own pin, and a generic caller's
  other-instantiation variant record wants one case proving the
  predicate's miss stays a compile error. The spelling and the
  evaluation rules are in `docs/spec/comp.md`; the predicate lives in
  `compiler/analyzer/comp_evaluator.cc` (the parameter checks around
  `:867`) with the interface in `comp_evaluator.h`.
- [ ] `size_of::<T>()` is not comp-evaluable: `const N: usize =
  size_of::<()>()` reports `Construct 'intrinsic is not comp-evaluable'
  is not supported`. The answer is a property of the layout the lowering
  already holds, and an array length written with it would fold. It wants
  a decision on which layout queries the comp engine may answer.
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
- [ ] A deeply nested pattern hangs the cover walk: a `match` arm of
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
  new std module's layout shift exposed). The analyzer's readers had
  the same shape throughout; they now take a copy - ten in
  `types_expr.cc`, ten in `types.cc`, and four in `comp_evaluator.cc`
  - so no live reference spans a check that can intern.
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
- [ ] The symbol a summary call names: `lower_summary_call` declares an
  external qualified by the package (`dep::double`) under the C calling
  convention, which no artifact defines, so a dependent's build stops at
  the linker where its check resolved. Multi-object linking wants the
  mangled symbol the dependency's own build writes, and the external
  linkage its runtime needs.
- [ ] A summary carries no struct field names: a signature over a
  nominal is left out of the surface a dependent spells, because a
  literal or a field read would need names the `SUMMARY` section does
  not carry. A names table beside the rows is what lets nominals cross
  the boundary, and with them the associated functions and methods.
- [ ] One nominal as several type nodes: the checker can intern a
  second type node over a declaration's own struct or enum shape (a
  `String` node beside the declaration's), and `types_equal` compares
  origins, so two nodes over one declaration read as different types
  while only the drop lookup was taught to see through them. What
  interns the second node - and whether the nodes should collapse to
  one - wants finding.
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
  `MAX_FN_ENTRIES = 8192` turns a package past it into an internal error
  rather than a diagnostic.
- [ ] Split `Lowerer`: comp evaluation, format expansion, and AST-to-IR
  lowering share one class with a large public surface.
- [ ] Borrow-pass scratch sized by the program, not the function: the
  arrays are allocated at package size and each function clears only the
  entries it names. The final pass's per-worker memory cost is why
  spreading it lost by measurement (ADR-0047), so the sizing waits for a
  reason to spread it.

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
- [ ] A UTF-8 byte order mark at the start of a source is an `Invalid
  character` at 1:1: the lexer has no rule for the sequence, so an
  editor that saves one makes the whole file unreadable. Other compilers
  skip it; the spec says nothing, so this wants a decision before the
  lexer learns the three bytes.
- [x] Qualified generic calls: `helper::id::<i32>(x)` answered
  "Unresolved value 'id'" because neither module-qualified resolution
  branch reached the generic-function lookup. Both do now, with the
  reachability check a qualified name owes (`ok-qualified-generic-call`,
  `err-qualified-generic-private`).
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
