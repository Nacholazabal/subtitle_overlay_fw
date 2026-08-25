# Architecture hardening tickets

Six self-contained tickets derived from [`docs/architecture_review_2026-08-25.md`](../architecture_review_2026-08-25.md).
Each is scoped to be worked in **one chat / one branch** without needing the others
open, and each carries its own file:line evidence, acceptance criteria and
verification steps.

Finding IDs (`F1`…`F27`) refer back to the review. Every finding is assigned to
exactly one ticket; where a finding touches two tickets, the owning ticket is named
and the other cross-references it rather than duplicating the fix.

## The tickets

| ID | Title | Findings | Size | Depends on |
| --- | --- | --- | --- | --- |
| [ARCH-01](ARCH-01-concurrency-invariants.md) | Concurrency: nothing blocks a thread that must not block | F1, F3, F5, F26 | S–M | — |
| [ARCH-02](ARCH-02-startup-lifecycle.md) | Startup & lifecycle orchestration | F4, F6, F7, F8, F25 | S–M | — |
| [ARCH-03](ARCH-03-subtitle-raster-path.md) | Subtitle pipeline: raster cost, frame sync, renderer contract | F2, F16, F17a, F23, F24, F27 | M–L | ARCH-06 (light) |
| [ARCH-04](ARCH-04-layering-contracts.md) | Layering contracts: inversion, error model, config, logging | F9, F11, F12, F13, F14, F15 | M–L | — |
| [ARCH-05](ARCH-05-stt-ws-client-decomposition.md) | Decompose `stt_ws_client` | F10, F19, F21, F22 | L | ARCH-04 (config half) |
| [ARCH-06](ARCH-06-dead-code-audit.md) | Dead code, unreachable paths, write-only state | F17b, F18, F20, + fresh sweep | S–M | — |

## Suggested order

1. **ARCH-01** — the only ticket that fixes something that can hang the firmware.
2. **ARCH-06** — cheap, mechanical, and it shrinks the surface every later ticket
   has to reason about. Do it early so ARCH-03/04/05 aren't refactoring code that
   should have been deleted.
3. **ARCH-02** — small, and it makes bring-up self-explaining (no more silent board).
4. **ARCH-03** — the biggest measurable latency win; wants ILA numbers on both sides.
5. **ARCH-04** — cross-cutting contracts; touches many files shallowly.
6. **ARCH-05** — largest diff, no behavioural urgency, best done once the config
   layer from ARCH-04 exists to absorb the config half of the split.

## Ground rules for every ticket

- **Build check:** `./scripts/build.sh` (or the `build-firmware` skill) after each
  ticket. The firmware cannot be built in WSL — ARM builds go through the PetaLinux VM.
- **Tests:** `make test` (or the `run-tests` skill) must stay green; add tests for
  any new module boundary the ticket creates.
- **Lint:** `make clang-tidy` (or the `run-lint` skill).
- **Do not deploy.** Nacho deploys and tests on the board.
- **Record decisions.** Anything deliberately left as-is, or any trade-off taken,
  goes in `docs/legacy-llm/decisions.md` under a new `SRC-` tag, matching the existing
  format. Several of these tickets exist because a good decision was never written down.
- **One invariant per commit** where practical, so a regression bisects cleanly.

## The two invariants these tickets protect

Most findings are a single call site breaking one of these. Stating them here so each
ticket can point at the one it defends:

1. **No operation that can block runs on the QP/C thread.** ALSA reads, DNS, TLS,
   socket I/O, and long MMIO loops belong to worker threads or are split across
   time events.
2. **Dependencies point inward and downward.** `svc/` may use `hal/`; `hal/` may use
   `bsp/`; siblings inside `svc/` talk through events or injected interfaces, never
   through each other's globals. Board identity lives in `bsp/`.
