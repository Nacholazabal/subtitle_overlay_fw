# ARCH-01 — Concurrency: nothing blocks a thread that must not block

**Findings:** F1 (high), F3, F5, F26
**Size:** S–M · **Depends on:** nothing · **Do first** — F1 is the only finding that
can hang the firmware.

---

## Why this ticket exists

The runtime rests on one invariant: *no operation that can block runs on the QP/C
thread.* That is what lets a dead network link, a stalled ALSA device, or a slow
console coexist with real-time video on a single cooperative kernel. `SttAO` honours
it carefully. Three other call sites do not.

Read [the review's runtime section](../architecture_review_2026-08-25.md#the-runtime-as-actually-built)
for the thread map if you need the picture.

---

## Scope

### F1 — `USBAudioAO` joins a worker thread inside a state handler (HIGH)

`SYSTEM_STOP` → `usb_audio_ao_top` → `quiesce()` → `usb_audio_stream_stop()`, which
calls `snd_pcm_drop()` and then **`pthread_join()`** — synchronously, on the
cooperative kernel thread.

- `src/svc/usb_audio/USBAudioAO.c:176` (`quiesce`), `:235` (`SYSTEM_STOP_SIG`)
- `src/svc/usb_audio/usb_audio_stream.c:448` (`usb_audio_stream_stop`, the join at `:458`)

`SttAO` already solves exactly this, and is the pattern to copy:

- `src/svc/stt/SttAO.c:302` `begin_stop()` — request only, never blocks
- `src/svc/stt/SttAO.c:312` `complete_stop()` — joins only after `stop_complete` proved the worker exited
- `src/svc/stt/SttAO.c:474` `stt_ao_stopping` — a state that polls for completion

**The compounding half.** `usb_audio_capture_read_chunk()` retries recoverable ALSA
errors in an unbounded loop with no stop check and no retry ceiling
(`src/hal/usb_audio/usb_audio_capture.c:376`–`404`): `-EPIPE` → `snd_pcm_prepare()` →
try again, forever. Normally `snd_pcm_drop()` makes the next `readi` return `-EBADFD`,
which `recover_pcm` maps to `-EIO` and the loop exits. But a device that keeps
xrunning stays in the loop — the join never returns, and **`SystemAO`'s 16 s shutdown
timeout cannot rescue it, because the timeout event is dispatched by the very thread
blocked in the join.** Fail-fast shutdown deadlocks.

**Do:**
1. Give `usb_audio_stream` a request/complete/finish triple mirroring
   `stt_ws_client_request_stop` / `_stop_complete` / `_finish_stop`
   (`src/svc/stt/stt_ws_client.c:1244`–`1298`). The capture thread sets a `worker_done`
   flag under the existing `state_mutex` before returning.
2. Add a `usb_audio_ao_stopping` state to `USBAudioAO` that re-uses the existing
   `USB_AUDIO_POLL_SIG` time event to poll for completion, then transitions to
   `stopped` and posts `SYSTEM_STOPPED`. Mirror `stt_ao_stopping` including its
   "already stopping, ignore repeated STOP" case.
3. Bound the recovery loop in `read_chunk`: a retry ceiling (a handful is plenty —
   past that the device is gone, not glitching) **and** a stop-requested check so no
   worker can outlive its stop request. This needs a way for the HAL to observe the
   stop; simplest is an `abort_requested` flag on `usb_audio_capture_t` set by
   `usb_audio_capture_abort()` and tested in the loop.

Note the error-path caller too: `enter_error()` → `quiesce()` on the same path
(`USBAudioAO.c:203`), so the async stop must work from both the error and the
coordinated-shutdown entry.

### F3 — Logging blocks the cooperative thread, per event

`app_log_output` does `fprintf` then an unconditional `fflush`
(`src/app/app.c:74`–`78`). Two callers make that per-partial:

- `src/svc/stt/SttAO.c:274` — INFO per transcript forwarded
- `src/svc/subtitle_pipeline/SubtitleAO.c:345` — INFO with the full caption text per render

On a serial console each line is a blocking write of milliseconds; on a pipe with a
slow reader it blocks as long as the reader takes.

**Do (cheap, in this ticket):** demote both per-event lines to `LOG_DEBUG`, and drop
the per-line `fflush` in favour of line buffering (`setvbuf(stdout, NULL, _IOLBF, …)`
once at startup). Keep `fflush` on `LOG_LEVEL_ERROR` so a crash doesn't lose the
last error.

**Do (proper, optional here):** a bounded lock-free ring plus one writer thread, so a
log call from any of the four threads is a bounded copy. If you defer it, say so in
the decision record — ARCH-04 owns the rest of the logging facility (two paths, 128-byte
cap) and can pick it up.

### F5 — The network worker waits on the wall clock

`worker_wait()` builds its deadline from `CLOCK_REALTIME` and the condvar is
initialised with default attributes, so `pthread_cond_timedwait` uses the wall clock —
while `now_ms()` and every other timer in the codebase use `CLOCK_MONOTONIC`.

- `src/svc/stt/stt_ws_client.c:254`–`272` (the wait), `:1091` (`pthread_cond_init`)

This firmware *deliberately boots with a wrong clock and waits for NTP*
(`STT_WS_STATE_WAIT_CLOCK`, `stt_ws_client.c:556`), so a step is a normal event, not a
hypothetical. A forward step returns early (harmless); a **backward** step turns a
100 ms wait into an arbitrarily long stall while the 16-slot audio queue overflows and
drops.

**Do:** `pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)` at init and build the
deadline from `CLOCK_MONOTONIC`. Handle the `pthread_condattr_*` failure path the same
way the existing init failures are handled.

### F26 — Port note: `QF_stop()` mutates shared state outside the critical section

Inherited from upstream `posix-qv`, but you already own a forked copy with a documented
divergence, so it is in scope to either fix or annotate.

- `src/bsp/qpc_port/qf_port.c:337`–`344`

Two issues: `QPSet_insert(&QF_readySet_, …)` and `pthread_cond_signal` run **outside**
`QF_CRIT_ENTRY/EXIT`, racing the ticker thread; and the loop is unblocked by inserting
the hardcoded priority `1`, which is correct today only because `SYSTEM_AO_PRIO == 1`
(`src/app/app.c:36`).

**Do:** wrap the mutation in the critical section, and replace the magic `1` with a
comment explaining the mechanism (it only needs *some* set bit to break the
`QPSet_isEmpty` wait). Extend the existing port-divergence banner at
`qf_port.c:56`–`68` with this second local fix so a future re-sync re-applies both.

---

## Out of scope

- The rest of the logging facility — two logging paths, the 128-byte message cap,
  timestamps: **ARCH-04**.
- `SystemAO`'s missing *startup* timeout: **ARCH-02**.
- Long MMIO loops on the cooperative thread (the raster path): **ARCH-03**. Same
  invariant, different mechanism and much larger diff.

---

## Acceptance criteria

- [ ] No `pthread_join`, `snd_pcm_*` blocking call, or unbounded loop is reachable from
      any QP/C state handler. Grep `USBAudioAO.c` and `SttAO.c` for symmetry: both AOs
      have `stopping` states and neither calls a join directly.
- [ ] `usb_audio_capture_read_chunk` cannot loop forever: it returns an error after a
      bounded number of recovery attempts, and returns promptly once abort is requested.
- [ ] A SIGTERM with the USB capture device forcibly removed mid-run still reaches
      `system: all components stopped; terminating` (or the timeout path) rather than
      hanging.
- [ ] `stt_ws_client`'s condvar is monotonic; no `CLOCK_REALTIME` remains in
      `src/svc/stt/`.
- [ ] Per-transcript and per-render logs are at DEBUG; INFO output is bounded per
      second regardless of transcript rate.
- [ ] `QF_stop()` mutates `QF_readySet_` under the critical section; the port banner
      documents both local divergences.
- [ ] `make test` green, `make clang-tidy` clean, `./scripts/build.sh` succeeds.

## Verification

- **New unit test:** `test/svc/usb_audio/test_usb_audio_stream.c` — assert that
  `usb_audio_stream_stop`-equivalent request returns without joining, that
  `stop_complete` is false while the worker is live and true after, and that
  `finish_stop` returns `-EAGAIN` while live. Mirror the existing WS-client stop tests.
- **New unit test:** `test/hal/usb_audio/test_usb_audio_capture.c` — mock a PCM that
  returns `-EPIPE` indefinitely and assert `read_chunk` returns an error rather than
  spinning.
- **Integration:** `test/integration/qpc/` already has a harness; add a case that
  broadcasts `SYSTEM_STOP` and asserts all four `SYSTEM_STOPPED` acks arrive without
  the harness blocking.
- **On-board (Nacho):** run, then `kill -TERM`; confirm clean shutdown in the log. Then
  repeat with the USB audio dongle unplugged mid-run.
