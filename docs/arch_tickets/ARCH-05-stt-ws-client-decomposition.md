# ARCH-05 — Decompose `stt_ws_client`

**Findings:** F10, F21, F22, F19
**Size:** L · **Depends on:** ARCH-04 (do the config layer and F15 first — they remove
two of the five concerns before you start cutting) · **Do last** — no behavioural urgency

---

## Why this ticket exists

`stt_ws_client` is the largest module in the firmware and the only real god object:
**1,607 lines of implementation behind a 330-line header, with a 45-field struct.**

It matters more than its runtime behaviour suggests, for two reasons. First, it sits
directly beside the three best-abstracted modules in the tree — `stt_ws_frame`,
`stt_json`, `stt_session_json` are pure, I/O-free, take their entropy and clock as
parameters, and document their wire contract. The client is what they all drain into, and
the contrast is stark. Second, it is the module a thesis examiner is most likely to open,
because it is where the network protocol lives.

There is no bug to fix here. This is about the architecture reading as designed.

---

## The five concerns currently in one struct

`src/svc/stt/stt_ws_client.h:142`–`186`:

| Concern | Fields | Natural home |
| --- | --- | --- |
| Configuration + URL parsing | `config` (24 sub-fields) | `stt_ws_config` |
| Connection lifecycle state machine | `conn`, `state`, `backoff_ms`, `next_attempt_ms`, `last_rx_ms`, `last_tx_ms`, `session_started_ms`, `session_generation` | `stt_link` |
| Audio TX queue | `audio_queue[16]`, `audio_head`, `audio_count`, `audio_dropped_total`, `audio_seq` | `stt_audio_txq` |
| Transcript RX ring + sequence dedup | `ring[8]`, `ring_head`, `ring_count`, `last_event_seq`, `have_last_event_seq`, `last_event_generation` | `stt_event_ring` |
| WebSocket message reassembly | `rx[8256]`, `rx_used`, `msg[1280]`, `msg_used`, `msg_active`, `msg_overflow` | `stt_link` |
| Worker lifecycle + 19 counters | `lock`, `worker_cond`, `worker_thread`, `worker_started`, `worker_done`, `stop_requested`, `stats` | `stt_link` + `stt_ws_stats` |

## Symptoms, so the split has targets

- **Locking is split by convention, not by structure.** Some fields are guarded by
  `client->lock`; others (`conn`, `rx`, `rx_used`, `msg*`, `last_tx_ms`,
  `next_attempt_ms`, `session_started_ms`) are worker-owned and unguarded. Nothing in the
  struct distinguishes the two groups, so every future edit has to re-derive which is
  which. `enter_backoff` even reads `client->backoff_ms` after unlocking, for a log line
  (`stt_ws_client.c:327`).
- **Lock churn in the drain path.** `stt_ws_client_poll_events` takes and releases the
  mutex once per event, including a lock-release-log-continue dance
  (`stt_ws_client.c:1436`–`1467`).
- **A "legacy" API that half-works.** `stt_ws_client_cleanup` early-returns leaving the
  object initialised, and says so in a comment (`:1569`–`1586`). It has no production
  caller (see ARCH-06).
- **Hidden side effect.** `stt_ws_client_send_audio` calls `stt_ws_client_service()` as
  its first action (`:1366`) — which is F21 below.
- **Public surface driven by tests.** 15 exported functions, several documented as
  existing "for host probes": `_service`, `_send_audio`, `_cleanup`.

---

## Scope

### F10 — Split the module along the seams above

Suggested target shape (names negotiable; the boundaries are the point):

- **`stt_ws_config.{c,h}`** — `stt_ws_client_default_config` and
  `stt_ws_client_parse_url` move here essentially unchanged. Note that ARCH-04's
  `app_config` will already have taken the `getenv` calls, so this becomes pure
  defaults + URL parsing + validation, and is trivially testable. `parse_url` already has
  good tests to carry over.
- **`stt_audio_txq.{c,h}`** — a bounded ring with drop-oldest-on-full semantics.
  Extracted from `submit_audio` (`:1202`), `audio_pop` (`:274`),
  `audio_discard_pending` (`:292`). Owns its own mutex/condvar. ~120 lines, fully
  testable without a socket.
- **`stt_event_ring.{c,h}`** — a bounded ring with **shed-oldest-partial-first**
  semantics (the interesting policy, currently buried at `:626`–`661`) plus the
  per-session generation and sequence dedup from `poll_events` (`:1437`–`1467`). Also
  ~150 lines, fully testable. This is the piece most worth having as a standalone unit —
  the shedding policy is a real design decision and deserves to be visible and tested as
  one.
- **`stt_link.{c,h}`** — the state machine, the transport, reassembly, and the worker
  thread. Retains `net_tls` ownership and the "worker owns the socket, no lock needed"
  rule — which becomes *true by construction* once the two rings own their own locks,
  because there is then nothing shared left to guard by convention.
- **`stt_ws_client`** — either disappears, or becomes a thin façade holding the four and
  exposing the five functions `SttAO` actually calls (`_shared`, `_start`,
  `_request_stop`, `_stop_complete`, `_finish_stop`, `_poll_events`, `_stats`, `_state`,
  `_state_name`, `_report_delivery`). A façade is fine; a façade that also contains logic
  is what we are leaving behind.

Sequencing that keeps the tree green throughout: extract `stt_audio_txq` first (fewest
dependents), then `stt_event_ring`, then `stt_ws_config`, and only then reshape what
remains into `stt_link`. Each step is independently committable with tests.

Also fold in while you are here: the singleton. With ARCH-04's sink inversion done,
`stt_ws_client_shared()` (`:912`) has exactly one caller (`SttAO.on_component_init`,
`SttAO.c:142`), so the `pthread_once` and the `shared_ready` flag can go and the instance
can be owned by `SttAO` or constructed in `app.c` and injected. That removes the last
piece of lazy global state from the real-time path.

### F21 — The worker services the connection twice per audio chunk

`worker_main` calls `stt_ws_client_service()` (`:1132`), then pops a chunk, then calls
`stt_ws_client_send_audio()` — which calls `service()` again (`:1366`). Each 20 ms chunk
therefore drives two full passes of `ensure_connected` + `pump_rx` + idle-timeout check +
`maybe_ping`: 100 receive pumps per second where 50 would do.

Not hot in absolute terms; it is a symptom of the hidden side effect. `send_audio` doing
connection management is what makes the double call invisible at the call site.

**Do:** make the send path pure transmission — assume a ready session, return
`-ENOTCONN` otherwise — leaving `stt_link`'s worker loop as the single place that
services the link. This falls out naturally from the split.

### F22 — The TLS context and CA store are rebuilt on every reconnect

`net_tls_open` → `build_context` per connection
(`src/hal/net_tls/net_tls.c:440` → `:238`), which does `SSL_CTX_new` then
`SSL_CTX_load_verify_locations` on `/etc/ssl/certs/ca-certificates.crt` (`:268`) —
re-parsing the whole bundle on each retry. With the backoff schedule that is once every
0.5 s at the start of an outage, on a Cortex-A9.

The fix pattern is already in the same function: the library-init guard at `:240` is a
one-shot static.

**Do:** build the `SSL_CTX` once, keyed on the CA configuration, and reuse it across
connections; only the `SSL` object is per-connection. Keep it inside `net_tls` — this is
a HAL-internal change, no interface movement, and `net_tls`'s opacity is what makes it
safe. Measure reconnect time before/after; it is a number worth reporting for recovery
behaviour.

*(This finding lives in `hal/net_tls`, not in `stt_ws_client` — it is here because it is
the same reconnect path and the same chat will already have it loaded. Split it out if
you'd rather keep the ticket single-module.)*

### F19 — Stale names from a module that no longer exists

- `stt_event_rx_delivery_status_t` is still the parameter type of a **public** function,
  `stt_ws_client_report_delivery` (`stt_ws_client.h:308`, defined
  `stt_transcript_parse.h:56`)
- `stt_session_json.h:20` still points readers at `stt_event_rx_parse_line()`, which does
  not exist — the function is `stt_transcript_parse_line`

**Do:** rename the type to match its home (`stt_transcript_delivery_status_t` or
`stt_delivery_status_t`) and fix the doc reference. Mechanical, and it removes a false
trail for anyone reading the STT layer for the first time.

---

## Out of scope

- The `getenv` calls currently in `default_config`: **ARCH-04** takes those.
- `strstr`-based finality detection (F15): **ARCH-04**, deliberately, so this ticket
  inherits a clean ingress path.
- Deleting `stt_ws_client_cleanup` / `_service` / `_send_audio` from the public header:
  **ARCH-06** decides their fate. If ARCH-06 lands first, this split gets simpler.

---

## Acceptance criteria

- [ ] No single translation unit in `src/svc/stt/` exceeds ~600 lines.
- [ ] No struct in `src/svc/stt/` mixes fields guarded by a mutex with fields guarded by
      thread-ownership convention. Where ownership is by convention, the header says so
      per field group.
- [ ] `stt_audio_txq` and `stt_event_ring` are separately unit-tested with no socket, no
      TLS, and no threads — including the shed-oldest-partial-first policy and the
      per-session sequence reset.
- [ ] `send_audio` performs no connection management; exactly one call site services the
      link per loop iteration.
- [ ] `SSL_CTX` is created once per process (or once per CA configuration), not per
      connection; reconnect time measured before/after.
- [ ] No identifier or comment in `src/svc/stt/` references `stt_event_rx`.
- [ ] The public header exposes only what `SttAO` calls.
- [ ] `make test` green with equal or better coverage than before, `make clang-tidy`
      clean, `./scripts/build.sh` succeeds.

## Verification

- **Unit:** new `test/svc/stt/test_stt_audio_txq.c` and `test_stt_event_ring.c`. The ring
  tests are the valuable ones: fill with partials and assert the oldest partial is shed;
  fill with finals only and assert oldest is shed; assert a generation bump invalidates
  copied entries and lets `seq 0` through again.
- **Unit:** the existing `test/svc/stt/test_stt_ws_client.c` should keep passing against
  the façade — treat any test you must rewrite as a signal the boundary moved something it
  shouldn't have.
- **Coverage:** compare `make coverage` before and after; the split should raise it,
  since the rings become directly reachable.
- **On-board (Nacho):** a run with a deliberate mid-session network drop, confirming
  reconnect, session restart, and that transcripts resume with no stale-sequence warnings.
