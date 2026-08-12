# Derived view-escape check

*In-flight campaign doc. **Delete this file when the work ships**, once SPEC carries the record — see the
maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

This is what is left of a design review held during M2a (2026-08-04) that started as "is retro-impl
dangerous?" and ended with a model for how *every* kind declares conformance. It scheduled four campaigns.
Three are closed: the **contract model** shipped (`e039e5d`), **const generics** shipped with
`std::num::Fixed<B, const F>` — what the language now *is* lives in [SPEC.md](../SPEC.md) — and **full
generic specialization** is a declared non-goal, recorded in [ROADMAP §2](../ROADMAP.md). This one is not
started.

## The gap

Nothing verifies that a `view` can satisfy the contract it implements — every `isBorrow` use is at escape
sites, destructibility or isolate prep, none at the `implements` site.

## The check

Reject at the `implements` site, over the methods **actually injected** — the marker-contract pattern puts
the factory in the impl rather than the contract, so reading the contract alone would miss it. What to
reject:

- a static/ctor factory returning `This`
- methods handing back `Owned<This>` / `Shared<This>`

`View.slice() -> This` stays legal, because it borrows the receiver.

## Shape of the work

Purely additive — no syntax, no contract re-declarations. Necessarily **partial**: boxing-idiom contracts
like `Error` have perfectly borrow-safe signatures (`fn string message()`) and stay caught later, at the
boxing site.

A negative claim needs a `tests/xfail/` fixture in the same commit, per the house rule in
[AGENTS.md](../../AGENTS.md).
