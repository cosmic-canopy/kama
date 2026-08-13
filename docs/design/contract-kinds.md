# The contract `for` clause — completing the kind gate (in-flight design)

*In-flight design doc. **Delete this file when the work ships**, once SPEC carries the record — see the
maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

> Found while closing the view model: `Viewable` and the iterator contracts cannot be spelled correctly
> under the clause as it stands. Companion briefs: [unsafe-seam.md](unsafe-seam.md) ·
> [view-model.md](view-model.md).

## The problem, measured

A contract must declare which kinds may implement it — `type contract C for value|resource|both { … }`,
mandatory, enforced at `src/kama.cemit.cpp:4564-4574`. GOALS §3c names **six** kinds. The clause knows
**two**, plus `both` for the pair.

So `view` and `intrinsic` have no way to be named, and they get in anyway: a `type view` maps to
`TypeKind::Value` (`kama.cemit.cpp:4914`), and an intrinsic conformance is simply admitted. The result
is a gate that reports something other than the truth:

| | |
|---|---|
| contracts declared in `lib/`+`prelude/` | **28** |
| of those, spelled `for both` | **15** |
| of those, whose clause **does not name a kind that actually implements it** | **15** |
| `for both` corpus-wide | **101**, across **144** contract declarations |

Fifteen of twenty-eight is not drift, it is the clause having stopped carrying information. The census
(regenerate it before trusting it — see *Verification*):

| contract | declared | actually implemented by |
|---|---|---|
| `Iterator` · `IteratorMut` | `for both` | value, **view** |
| `Iterable` · `IterableMut` | `for both` | resource, **view** |
| `Hashable` · `Equatable` | `for both` | value, **intrinsic**, **enum** |
| `Comparable` | `for both` | value, resource, **intrinsic** |
| `Serialize` · `Deserialize` | `for both` | resource, **intrinsic** |
| `Format` · `FromStr` · `FromStrRadix` | `for both` | **intrinsic** (`FromStr*`: intrinsic only) |
| `Error` | `for both` | **enum** |
| `Real` · `FixedBacking` | `for value` | value, **intrinsic** |

`Real<T is This> for value` implemented by `type intrinsic <float64>` is the clearest single case: the
clause says *value*, the implementer is an intrinsic, and it compiles.

## The model

1. **The clause takes any combination of the five implementable kinds** — `value`, `resource`, `view`,
   `enum`, `intrinsic`.

   `contract` is not among them. A contract implementing a contract is **refinement**, a different
   axis, already spelled by `implements` on the contract itself (`ReliableStream … implements Reader,
   Writer`) and already handled by `linkContracts`.

2. **The spelling stays the comma list. No grammar change.** `for_kinds_opt` is already
   `FOR kind_name_list` with comma separation (`kama.y:737-744`), and comma already means *any of
   these* — the existing emitter comment even says "`both` / listing both = either". The entire
   restriction is the name gate at `kama.cemit.cpp:4564-4574` plus a two-flag `InterfaceInfo`
   (`allowsValue`/`allowsResource`, `kama.cemit.h`). This is emitter work.

   `+` was considered, for visual parity with a generic bound (`<K: Hashable + Equatable>`), and
   rejected: `+` means **conjunction** in a bound (satisfy all) and would mean **disjunction** here
   (any one may implement). One symbol, opposite senses, and it costs a grammar change where comma
   costs none.

3. **`both` is retired.** It named an arbitrary pair the moment there were more than two kinds, and
   101 uses show it is being reached for as a default rather than chosen. Every contract states its
   real kind set. This is the source-breaking part, which is why the work lands pre-1.0.

4. **The gate keys on `ci.isBorrow` (`kama.cemit.cpp:4933`), not on `TypeKind`.** `view →
   TypeKind::Value` at `:4914` stays exactly as it is — a view genuinely *copies* like a value, and
   that mapping governs the copy/pass path. Only the conformance gate learns to tell them apart. Keep
   this separation: it is what stops a kind-gate change from touching how views are passed.

**Migration is forced, not optional.** The moment `view` is its own kind, `for both` stops admitting
the 18 `type view` iterators, so every contract they implement must be restated whether or not `both`
survives. Retiring `both` only widens an unavoidable pass.

## Target spellings

The floor for each contract is its measured implementer set; widening beyond that is a policy call,
and two are made deliberately here:

```kama
type contract Iterator<T>    for value, view;              // a BORROWING iterator is a view;
type contract IteratorMut<T> for value, view;              //   a GENERATING one is a value
type contract Iterable<T>    for resource, view;
type contract IterableMut<T> for resource, view;
type contract Hashable       for value, resource, enum, intrinsic;
type contract Equatable<T is This> for value, resource, enum, intrinsic;
type contract Comparable<T is This> for value, resource, intrinsic;
type contract Error          for value, resource, enum;    // WIDER than measured (enum only) —
                                                           //   the declared intent, kept
type contract Real<T is This> for value, intrinsic;
type contract Viewable<T>    for value, resource, view, intrinsic;   // WIDER than today's hosts
```

- **`Iterator` is `for value, view`, not `for view`.** "An iterator is a view" holds only for a
  *borrowing* iterator. `type value Args implements Iterator<string>` (`prelude/global.kama:379`) and
  `IntRange` across five test fixtures generate their values and borrow nothing — they are correctly
  values. Note also that "borrowing ⇒ must be `type view`" is **not** compiler-enforced and cannot
  easily be, since nothing distinguishes a borrowed `UnsafePtr` field from an owned one. That is
  finding ② in [ROADMAP.md](../ROADMAP.md) §2, and it stays a known limit rather than a claim.
- **`Iterable` does not need `intrinsic`.** `string` reaches `foreach` through the *intrinsic
  collection* path (`kama.cemit.cpp:3705-3820`), not the iterator protocol; `s.chars()` hands back a
  `type view` that implements `Iterator`. Measured, after an earlier draft asserted otherwise.
- **`Viewable` is `for value, resource, view, intrinsic`** — anything with storage may opt in, even
  though today's hosts are only resources plus `View<T>` itself. `enum` is excluded: a tagged union's
  payload is not a contiguous run. Rationale in [view-model.md](view-model.md).

The remaining contracts are settled the same way during migration: **run the census, do not infer a
kind list.**

## Milestones

**M1 — widen the gate.** Accept the five kind words at `kama.cemit.cpp:4564-4574`; replace
`allowsValue`/`allowsResource` with a kind set; check it at the `implements` site against
`ci.isBorrow` / the declaration's kind word. Reject `both` with a diagnostic that names the kinds to
write instead.

**M2 — migrate.** 101 `for both` sites in `.kama` plus the `for value`/`for resource` contracts the census
flags. Mechanical once M1's diagnostic tells each site what it needs.

**The prose migrates too, and it is easy to miss** — `for both` appears in `docs/SPEC.md` (11),
`docs/TYPE_MODEL.md` (2), `docs/tour.md` (2) and `docs/KEYWORDS.md` (1, inside the `type contract` row's
refinement example `type contract A for both implements B`). None of these is reached by a fixture, so
nothing fails if they are left stale.

**M3 — fixtures.** Every new rejection needs an `xfail` in the same commit:
`for both` as an unknown kind word; a `type view` implementing a contract that does not list `view`;
a `type intrinsic` implementing one that does not list `intrinsic`; a `type enum` likewise. Note the
existing *"must declare which kinds may implement it"* diagnostic has **no fixture today either** —
add it here.

## Definition of done

SPEC's contract section states the five implementable kinds and the comma list, with `both` gone. Every
negative claim has an `xfail`. `./dev matrix` green. Then this file is deleted.

## Verification

Regenerate the census rather than trusting the table above — this repo's rule is that a doc is not
evidence:

```sh
grep -rh '^type contract' --include='*.kama' lib prelude | wc -l      # 28
grep -rh '^type contract' --include='*.kama' lib prelude | grep -c 'for both'   # 15
grep -rho 'for both' --include='*.kama' lib prelude tests examples bench | wc -l  # 101
```

For the per-contract implementer sets, scan every `type <kind> … implements …` declaration across the
corpus and group by contract name. The two corrections that pass caught — `Iterable` not needing
`intrinsic`, and `Copyable` appearing as a false positive because `when [T: Copyable<T>]` is a *bound*
and not a conformance — are the reason to run it rather than read it.
