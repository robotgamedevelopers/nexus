# AGENTS.md

The rules for an AI working in this repository. Each one holds on every change.

## Code

- nexus is C++ built with CMake. Build with `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` and `cmake --build build -j4`.
- Zero warnings. Warnings are errors on every compiler; a change that adds a warning does not land.
- Prax is pinned and never forked or patched. When nexus needs something Prax lacks, work around it in nexus and record the gap as feedback for Prax's author.
- Errors never fail silently. Each error carries its cause and how to fix it.

## Tests

- A unit test uses no real time, no sockets, no network and no outside services. It takes time as an argument and fakes the transport. Anything that needs the real thing is a functional test.
- `ctest -L unit` runs the unit tests; `ctest -L model` runs the TLA+ models and their trace checks. CI runs both on every pull request.

## Models

- Every state machine has a TLA+ module beside it, written before or with the code.
- Each model has reversed witnesses: broken variants that TLC must reject.
- The code is bound to its model by trace checking in CI. A model the code is not checked against does not count.
- Client and server are separate state machines, apart from the transport between them, and each has its own TLA+ model.
- Where code does index arithmetic (rings, wrapping counters, reused slots), the model carries the same arithmetic.

## Layers

- nexus is built in layers, bottom up. Each layer is secured before the next one starts.
- The layer list lives in `docs/roadmap.sexp`, rendered to `docs/ROADMAP.md` by `nova-roadmap render`; run `nova-roadmap check` after any change to either.
- A tracer bullet is declared before it starts, runs end to end once, and then stops. What it taught is kept; its code is not the product.

## The repository

- The repository is private until it is released. Nothing about it is posted anywhere else.
- Future work and ideas go in `docs/roadmap.sexp`, never in GitHub issues.
- Commits use the shared agent identity, whose email is `305107455+rowan-claude@users.noreply.github.com`, set with repo-local `git config` only.
- No personal names anywhere in the repository: code, comments, docs or commit messages.
- License: GNU AGPL v3, permanently.
