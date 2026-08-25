# ARCH-02 — Startup & lifecycle orchestration

**Findings:** F6 (high), F7, F8, F4, F25
**Size:** S–M · **Depends on:** nothing

---

## Why this ticket exists

`SystemAO` implements a genuinely good coordinated *shutdown*: an immutable static STOP
event that cannot fail on pool exhaustion, per-component acknowledgement by bitmask,
bounded by a timeout. Startup got none of that care. It is a straight line where the
real dependency graph is a fork, it has no timeout, and it latches so hard that a
resolution change never propagates.

The user-visible symptom, worth fixing for bring-up alone: **with no HDMI source
connected, the firmware produces no output at all and never starts speech-to-text.**

---

## Scope

### F6 — Speech-to-text is gated on HDMI lock (HIGH)

`SystemAO` sequences: Video → (Video ready **and** USB audio ready) → Subtitle → STT.

- `src/svc/system/SystemAO.c:143` (`on_init`, requests Video + USB audio)
- `src/svc/system/SystemAO.c:170`–`284` (`on_component_ready`, the chain)
- `src/svc/video_pipeline/VideoAO.c:174` — Video only posts ready on
  `POLL_STREAMING_STARTED`, i.e. after the input is locked *and* passthrough is running

But only `SubtitleAO` needs the video geometry (it sizes the caption bar from
`e->width`/`e->height`, `src/svc/subtitle_pipeline/SubtitleAO.c:152`). STT has no video
dependency whatsoever.

Consequence chain with no source connected: the ALSA thread runs and captures;
`stt_ws_client_shared()` is constructed lazily *by that thread*
(`src/svc/usb_audio/usb_audio_stream.c:152`); but `stt_ws_client_start()` is never
called, so `submit_audio` returns `-EAGAIN` at
`src/svc/stt/stt_ws_client.c:1217` and every chunk is counted dropped, indefinitely.
No session, no transcripts, no network diagnosis.

**Do:** fork the graph in `on_init` — request `COMPONENT_INIT` for Video, USB audio
**and** STT immediately. Gate only the `AO_Subtitle` init on the video-ready
dimensions. Update the readiness bookkeeping accordingly: the transition to
`system_ao_run` currently happens when `on_component_ready` returns 0, which only
`COMPONENT_STT` does (`SystemAO.c:274`) — replace that with an explicit
"all expected components ready" bitmask, mirroring how `stopped_mask` /
`SYSTEM_STOP_EXPECTED_MASK` already work at `SystemAO.c:30` and `:450`. The symmetry is
the point: startup and shutdown should use the same mechanism.

### F7 — There is a shutdown timeout but no startup timeout

`system_ao_stopping` arms `SYSTEM_SHUTDOWN_TIMEOUT_TICKS` on entry
(`src/svc/system/SystemAO.c:439`). `system_ao_init` arms nothing
(`src/svc/system/SystemAO.c:339`). If any component never posts ready — no source, an
unsupported resolution reaching `VIDEO_PIPELINE_UNSUPPORTED_INPUT`, a stalled detector —
the state machine waits forever. Meanwhile `video_pipeline_poll` returns
`POLL_UNCHANGED` silently (`src/svc/video_pipeline/video_pipeline.c:179`), so nothing
is logged either.

**Do:** arm a periodic time event on entry to `system_ao_init`. On each expiry, log
which components are still outstanding **by name** — `component_id_to_str` already
exists at `SystemAO.c:86` — and keep waiting (video genuinely may have no source yet;
this is a diagnostic, not a fault). Consider escalating to an error after N periods if
you want fail-fast parity, but a repeating "still waiting for: video" line is the part
that pays for itself on the bench.

Also worth surfacing: `VIDEO_PIPELINE_POLL_UNSUPPORTED_INPUT` and
`POLL_TIMING_TIMEOUT` are returned by the pipeline but `VideoAO`'s `on_video_poll`
ignores every result except `ERROR` and `STREAMING_STARTED`
(`src/svc/video_pipeline/VideoAO.c:159`–`196`). An unsupported resolution is currently
indistinguishable from no cable. Log them.

### F8 — A resolution change after startup never reaches the subtitle pipeline

Three latches, all needed for the bug:

1. `VideoAO.ready_posted` is set once and cleared only by `quiesce()`
   (`src/svc/video_pipeline/VideoAO.c:186`, `:213`).
2. `video_pipeline_poll` in `STREAMING` returns `POLL_UNCHANGED` unconditionally — it
   re-checks only the GPIO lock bit, never re-reads detector timing
   (`src/svc/video_pipeline/video_pipeline.c:193`).
3. `SubtitleAO` computes bar geometry once, in `on_component_init`
   (`src/svc/subtitle_pipeline/SubtitleAO.c:152` → `subtitle_pipeline_init` →
   `default_config`, `src/svc/subtitle_pipeline/subtitle_pipeline.c:55`).

So 1080p → 720p without dropping lock is never noticed. Drop-and-relock at a new
resolution restarts passthrough correctly, but `ready_posted` is still 1, so no second
ready event is posted and the caption box keeps the old display's centring and bottom
margin — off-centre, possibly off-screen.

**Do:**
- Cheap half: clear `ready_posted` on `POLL_SIGNAL_LOST` so a relock re-announces.
- Full fix: while `STREAMING`, re-read detector timing periodically and compare against
  `active_mode`; on a change, restart passthrough for the new mode and emit the
  geometry event. Either re-use `COMPONENT_READY_SIG` or add a `VIDEO_MODE_CHANGED_SIG`
  — the latter is clearer, and `app.h`'s signal enum has room.
- Give `SubtitleAO` a re-configure path that recomputes `default_config` and calls
  `subtitle_overlay_configure` without a full teardown. `subtitle_pipeline_set_box`
  (`subtitle_pipeline.c:255`) already does most of this; it just needs the new
  display dimensions stored.

### F4 — AO priority ordering is inverted relative to deadline tightness

`src/app/app.c:36`–`40`: SystemAO 1, VideoAO 2, USBAudioAO 3, SttAO 4, SubtitleAO 5.

In the QV kernel priority decides which ready AO's next event is dispatched, so
`SubtitleAO` — whose handler is the heaviest in the system (see ARCH-03) — is dispatched
ahead of the 10 ms STT poll and the 100 ms video poll whenever several are queued.
Rate-monotonic reasoning inverts that: STT polls every 10 ms, video every 100 ms, and
caption rendering is event-driven with the loosest deadline of the three (a caption
30 ms late is invisible to a viewer). No comment explains the current order, which
suggests declaration order rather than a decision.

**Do:** either reorder so `SubtitleAO` sits lowest, **or** keep it and write the
rationale next to the constants. Do not leave it unexplained — the periods are known,
so the justification is two lines either way, and it is exactly what a thesis examiner
asks about. Record the choice in `docs/legacy-llm/decisions.md`.

### F25 — `app_init`'s construction order is load-bearing and unstated

`QActive_start` runs the top-most initial transition immediately
(`QASM_INIT`, `src/bsp/qpc_port/qf_port.c:416`), and `system_ao_init`'s `Q_ENTRY_SIG`
posts `COMPONENT_INIT` to the other AOs (`SystemAO.c:345`). So `SystemAO` **must** be
started last (`src/app/app.c:151`); moving it earlier posts to unregistered active
objects and trips a QP/C assert.

**Do:** one comment at `app.c:151` stating the constraint and why. Zero-risk, and it
prevents a genuinely confusing failure. While there, the `TODO` at `app.c:163` for
ButtonsAO/LEDAO and the `bsp_init_placeholder()` at `:65` are the two other
startup-shaped loose ends — decide whether they are still planned and note it.

---

## Out of scope

- What the raster path *does* once `SUBTITLE_TEXT_SIG` arrives: **ARCH-03**.
- The `COMPONENT_ERROR` payload carrying a flattened `-EIO`: **ARCH-04** (F11).

---

## Acceptance criteria

- [ ] STT and USB audio initialise independently of video lock; only `SubtitleAO` waits
      for dimensions. With no HDMI cable, the log shows an STT session attempt and its
      link state.
- [ ] Startup readiness uses an explicit expected-components bitmask, symmetric with
      `SYSTEM_STOP_EXPECTED_MASK`.
- [ ] `system_ao_init` logs outstanding components periodically by name; the firmware
      is never silent while waiting.
- [ ] `VideoAO` logs `UNSUPPORTED_INPUT` and `TIMING_TIMEOUT` distinctly from "no signal".
- [ ] A source resolution change (or unplug → replug at a different resolution)
      re-centres the caption box without a restart.
- [ ] AO priorities are either reordered or justified in a comment **and** in
      `decisions.md`.
- [ ] `app_init`'s "SystemAO last" constraint is documented at the call site.
- [ ] `make test` green, `make clang-tidy` clean, `./scripts/build.sh` succeeds.

## Verification

- **Unit:** extend `test/integration/qpc/test_system_ao_startup.c` — assert STT receives
  `COMPONENT_INIT` without any video-ready event; assert the run transition requires
  the full mask; assert the init timeout fires and names the missing component.
- **Unit:** extend `test/svc/video_pipeline/test_video_pipeline.c` — drive
  `STREAMING` → timing change → assert a mode-change result is returned.
- **On-board (Nacho):** boot with no HDMI input and confirm STT connects and logs link
  state; then connect a source and confirm captions appear centred. Then switch the
  source resolution and confirm the box re-centres.
