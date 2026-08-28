# Legacy design decisions (SRC-XX archive)

Archive of the design rationale and historical context that once lived as inline
`SRC-XX` comments in `src/`. Those comments were trimmed to short "what it does"
docs during a cleanup pass; the *why* and the traceability IDs are preserved here
so nothing is lost. Full change history is in `git log` (search the `SRC-` tags
in commit messages).

> This file is documentation only. It is not read by any build step.

## Correctness / concurrency (C-series)

### SRC-C01 — project-owned posix-qv port with start-up race fix
`src/bsp/qpc_port/qf_port.c` is a **project-owned copy** of the QP/C 8.1.4
posix-qv port (`src/qpc/ports/posix-qv/qf_port.c`). The vendored `src/qpc`
submodule stays pristine; this copy carries a local fix over upstream 8.1.4:

- Upstream had a **start-up race** and a **data race** on `l_isRunning`:
  `l_isRunning` was written `true` *after* the ticker thread was created, so if
  the ticker ran in that window it observed `false`, exited immediately, and QF
  never received clock ticks.
- Fix: publish `l_isRunning = true` **before** creating the ticker, with
  release/acquire atomic ordering; create the ticker **joinable** so `QF_run`
  can join it on shutdown before destroying the mutex/condvar it uses;
  release-store `false` in `QF_stop` so both the ticker and the event loop
  observe the stop.

### SRC-C02 — reference-counted shared MMIO platform ownership
The AXI-Lite regions are a single global resource shared by several services
(VideoAO owns dynclk/VTC/GPIO; SubtitleAO owns overlay/BRAM). `hw_platform`
(`src/bsp/platform/linux/hw_platform.c`) is acquired on each service init and
released on cleanup; the `/dev/mem` mapping is torn down only when the **last**
owner releases it. This stops one service's cleanup from pulling the mapping out
from under another. A failed init releases its reference so it never leaves a
dangling one.

### SRC-C03 — asynchronous worker shutdown; nothing blocks the QP/C thread
The runtime rests on one invariant: **no operation that can block runs on the
QP/C thread.** Three call sites broke it (ARCH-01,
`docs/arch_tickets/ARCH-01-concurrency-invariants.md`).

- **`USBAudioAO` joined its capture thread inside a state handler.** Combined with
  an unbounded ALSA recovery loop, a device stuck in permanent overrun could keep
  the worker alive forever; the join then blocked the cooperative thread, and
  `SystemAO`'s 16 s shutdown timeout could never fire because the blocked thread is
  the one that dispatches it. Fail-fast shutdown deadlocked.
  Fix: `usb_audio_stream` gained the request/complete/finish triple that
  `stt_ws_client` already had, and the AO gained a `stopping` state that polls for
  completion on its existing health timer and only then joins. Both AOs now
  implement the same contract.
- **The ALSA recovery loop was unbounded.** `usb_audio_capture_read_chunk` now caps
  recoveries per chunk (`USB_AUDIO_CAPTURE_MAX_RECOVERIES`) and observes an abort
  flag raised by `usb_audio_capture_abort`, returning `-ECANCELED` on a requested
  stop. The retry policy lives in `usb_audio_capture_recovery_decision`, kept
  separate from the ALSA calls so the termination argument is testable on the host
  where ALSA is compiled out entirely.
- **Logging blocked the cooperative thread per event.** `stdout` is line buffered
  at startup, `fflush` is now reserved for errors, and the per-transcript and
  per-render records moved to `DEBUG`. A bounded ring plus a writer thread was
  considered and deferred: line buffering removes the syscall-per-record cost, and
  the remaining exposure is one `fprintf` per event at `DEBUG`. Revisit in ARCH-04,
  which owns the rest of the logging facility.
- **`QF_stop()` mutated `QF_readySet_` outside the critical section** (upstream
  behaviour), racing the ticker thread. Now inside it — the port's second local
  divergence, recorded in the `qf_port.c` banner alongside SRC-C01. The hardcoded
  priority `1` is retained but explained: any set bit breaks the `QPSet_isEmpty`
  predicate that parks the event loop, and the bit is never acted upon because the
  loop re-tests `l_isRunning` first.

## High-severity (H-series)

### SRC-H02 — single-buffer video passthrough (deliberate over triple buffering)
`src/svc/video_pipeline/`. Capture (S2MM) and display (MM2S) are both pinned to
**one** framebuffer, so there is no producer/consumer swap and no extra buffers
are mapped. This was chosen deliberately over triple buffering: with no
frame-boundary swap, simultaneous capture/display of the same frame is
acceptable for a passthrough overlay and avoids the buffering complexity and
latency a swap chain would add.

### SRC-H03 — bounded wait for input timing detector
`src/svc/video_pipeline/video_pipeline.c`. Real HDMI sources deliver timing
within tens of ms. The pipeline waits a generous ceiling (2000 ms) for the input
timing detector to report a mode; if it stalls past that, the detector is
restarted so an absent/flaky source cannot strand `ACQUIRING_TIMING` forever.

### SRC-H05 — subtitle mask vs BRAM window build-time guard
`src/hal/subtitle_bram/subtitle_bram.c`. A gnu99-safe negative-array-size
build-time assert fails the compile if the subtitle mask is ever larger than the
AXI BRAM controller's address window — otherwise `subtitle_bram_clear()` and the
renderer would write past the mapped BRAM. Reference HW mask is 256x64 / 2 KiB.

### SRC-H07 — coordinated fail-fast STOP/STOPPED shutdown
On any component failure, `system_ao_t` broadcasts `SYSTEM_STOP` to every worker
AO (VideoAO, USBAudioAO, SubtitleAO, SttAO) using a single immutable static
event (so the broadcast can never fail on pool exhaustion), waits for each to
acknowledge with `SYSTEM_STOPPED` (bounded by a ~500 ms timeout), then terminates
the QF event loop so the process exits with hardware/threads/sockets already
quiesced. Each AO's `quiesce()` is idempotent and shared by its error path and
the STOP handler.

## Medium-severity (M-series)

- **SRC-M01** (`SttAO.c`): NULL-check `me`/`e` before reading any field. The
  earlier code read `e->is_final` to compute the pool margin before this guard
  existed.
- **SRC-M02** (`SubtitleAO.c`): the startup "DONE" marker is a temporary
  diagnostic; the inactivity clear timer is armed at startup so the marker is
  removed after the normal timeout even if no STT transcript ever arrives.
- **SRC-M03** (`SubtitleAO.c`, `subtitle_pipeline.c`): surface (do not silently
  discard) failures to blank the overlay — the logical text state is reset
  regardless, and a failed blank may leave stale content on screen. On a failed
  init, leave the overlay explicitly disabled rather than partially configured.
- **SRC-M04** (`SystemAO.c`): request subtitle init only once, and only when both
  video and usb-audio are ready. Guarding on `subtitle_init_requested` makes a
  duplicate video-ready event idempotent.
- **SRC-M05** (`video_dma.c`): never advertise more frames than the kernel
  actually exposes. Mapping `i % info.frame_count` would silently alias
  framebuffers and claim buffers a later `*_SELECT`/config would reject.
- **SRC-M07** (`stt_event_rx.*`): NDJSON wire-protocol contract with the PC-side
  sender (UTF-8 literal bytes, one JSON object per line). A byte-boundary
  truncation can split a multi-byte UTF-8 code point, so any incomplete trailing
  sequence is trimmed to end on a whole code point.
- **SRC-M08** (`video_dynclk.c`): sanity bounds for the requested pixel clock —
  ceiling above every supported mode (148.5 MHz @ 1080p60) and an error tolerance
  below `clk_find_params`' "no match" sentinel but above real synthesis error, so
  an unsupported frequency fails instead of programming a wildly wrong clock.
  Non-finite (NaN/Inf) and out-of-range requests are rejected before any
  float-to-int conversion.
- **SRC-M09** (`xil_assert.h`): the userspace Xilinx-assert shim returns early
  from the function on a failed precondition (matching genuine non-fatal Xilinx
  behavior), instead of the previous no-op that let the generated driver keep
  going and dereference an invalid pointer.

## Low-severity (L-series)

- **SRC-L06** (`log.h`): a disabled `LOG(...)` expands to `do { } while (0)` so it
  stays a single statement — safe after a brace-less `if`, before an `else`, and
  when followed by a semicolon.

## Dead-code audit (D-series)

From ARCH-06 (`docs/arch_tickets/ARCH-06-dead-code-audit.md`). The rule the
ticket establishes: **every exported symbol has a production caller, or it does
not exist.** These are the entries that were deliberately *not* deleted, plus the
two judgement calls the ticket asked to be recorded rather than decided silently.
`scripts/dead_symbols.sh` enforces the rule; `scripts/dead_symbols.ignore` is the
machine-readable half of this section.

### SRC-D01 — `log_unsubscribe` kept without a production caller
`src/utils/log/log.h`. Subscribe and unsubscribe are a coherent pair, and the
asymmetry of shipping only half of it is worse than the unused half. There is one
production subscriber today and it never detaches. Kept deliberately, listed in
`scripts/dead_symbols.ignore`.

### SRC-D02 — no CPU mapping of the framebuffer; the compositor stays in PL
`src/hal/video_dma/video_dma.c`, `src/svc/video_pipeline/video_pipeline.h`.
`video_dma_init` used to `mmap` every framebuffer slot and hand the pointers back
through a `uint8_t* frames[]` out-parameter, which `video_pipeline` stored and
**nothing ever dereferenced** — roughly 6 MB of coherent DMA memory mapped into
userspace at every init for no consumer.

Decided: **the CPU never touches pixels.** Passthrough is VDMA-to-VDMA and
subtitle composition happens in the PL (`axis_video_overlay` reads the mask from
BRAM), so there is no software compositor planned and no reason to keep
scaffolding for one. The kernel client `dma_alloc_coherent`s the buffers at probe
and `mmap` is only a userspace view of them, so dropping it changes nothing on
the wire; if a software path is ever added, the mapping comes back with it.

Removing the mapping removed the awkward out-parameter (`video_dma_init` is now
`(dma, frame_count)`) and both duplicate size fields with it. The ticket asked
for `video_dma_t` to end up with *one* size field; it ends up with none, because
`frame_size`/`mmap_size` existed only to serve the mapping. `frame_count` stays —
it bounds `frame_index` in configure/select.

### SRC-D03 — `video_dma_status` deleted rather than wired into the poll loop
`src/hal/video_dma/video_dma.c`. The ticket offered a health check in `VideoAO`'s
poll as the alternative to deleting it. Deleted, because giving `POLL_ERROR` a
transport cause changes the pipeline's error model, and the error model belongs
to **ARCH-02**. ARCH-06 removes and documents; it does not change behaviour. If
ARCH-02 wants the check, the kernel's `HDMI_VDMA_*_STATUS` ioctls are still
there and `git log` has the wrapper.

### SRC-D04 — the frame-sync trio is ARCH-03's to resolve
`subtitle_pipeline_commit` / `_clear_sof` / `_poll_sof` have no production caller
and are the subject of ARCH-03's F17a: wire `_poll_sof` into the caption flush or
delete all three. Kept unchanged here so the two tickets do not collide, listed
in `scripts/dead_symbols.ignore` with that reason.

### SRC-D05 — detected timing is bounded by the framebuffer geometry
`src/svc/video_pipeline/video_pipeline.c`. `VIDEO_PIPELINE_MAX_HEIGHT` had no
uses; only `MAX_WIDTH` fed `VIDEO_PIPELINE_STRIDE`. Detected timing is now
checked against both before the mode table is consulted, so the buffer bound is
stated where it matters. No behaviour change today: every mode in the table fits,
and a timing past the bound had no matching mode either.

### SRC-D06 — two write-only fields became measurements instead of deletions
- `video_dynclk_t.actual_frequency_mhz` is the clock the MMCM actually
  synthesised. It is now logged at mode start next to the frequency the mode
  table requested (`video_io.c`), which quantifies the pixel clock error — a
  number the thesis wants, and one that was previously computed and thrown away.
- `video_pipeline_t.input_timing` is now read to name the resolution in the
  `unsupported input timing` warning. `active_mode` is NULL in that state, so the
  detected timing is the only thing that can say *which* resolution was rejected;
  ARCH-02 asks for exactly that log line.

Both readers are `LOG_*` calls, which compile out when `CONFIG_LOG_ENABLED` is
undefined — that is the unit-test build only. Both shipping builds (`app` and
`video-port-check`) define it.

### SRC-D07 — code templates moved out of `src/`
`tools/templates/` now holds `template.{c,h}` and `template_qpc_AO.{c,h}`.
Neither was referenced by the `Makefile` or `project.yml`, so nothing compiled
them; `src/` should contain only what ships. The `multi-file-workflows` skill
points at their new location.

### SRC-D08 — where the dead-code guard runs
`make video-port-check` is a **host-gcc** target. Its final step partial-links the
tree with `$(CC) -r`, which the ARM cross toolchain cannot do (`cannot find
-lgcc_s`), so the guard runs on the CI runner and locally, not in the VM. ALSA and
OpenSSL live in the PetaLinux sysroot, so CI compiles with
`USB_AUDIO_ENABLE_ALSA=0 USB_AUDIO_ENABLE_TLS=0`.

That leaves a gap: code inside `#ifdef CONFIG_USB_AUDIO_ALSA` is not covered by
the warning flags in CI. It was checked by hand in the VM when this landed — all
36 translation units compile warning-free with ALSA and TLS enabled — and
`make app` (the shipping build) is warning-free there too. If that `#ifdef` grows,
re-check it in the VM rather than trusting the CI job alone.

### ARCH-02-F4 — AO priorities follow rate-monotonic scheduling

`src/app/app.c` assigns QP/C priorities to active objects based on their polling
periods, following rate-monotonic scheduling principles: shorter period = higher
priority (tighter deadline), since the QV kernel dispatches the highest-priority
ready AO's next event.

- **SttAO** (priority 5, highest): 10 ms poll period — tightest deadline, must
  process STT transcript input promptly to avoid dropping chunks or delaying
  subtitle updates.
- **USBAudioAO** (priority 4): 100 ms poll — monitors the ALSA capture thread's
  liveness; same period as VideoAO but less time-critical.
- **VideoAO** (priority 3): 100 ms poll — checks HDMI input lock state and
  detector timing; a missed poll can delay mode acquisition but does not drop data.
- **SubtitleAO** (priority 2, event-driven): No periodic poll; responds to
  `SUBTITLE_TEXT_SIG` and timeout events. Its handler is the heaviest in the
  system (renders text, writes BRAM, configures overlay MMIO) but has the loosest
  deadline — a caption 30 ms late is invisible to a viewer, whereas STT poll skew
  directly affects transcription quality.
- **SystemAO** (priority 1, lowest): Orchestration only; handles startup/shutdown
  coordination and error reports, all of which tolerate scheduling latency.

Rationale documented here per ARCH-02 F4 to prevent unexplained priority inversions.
The original declaration order (SystemAO 1, VideoAO 2, USBAudioAO 3, SttAO 4,
SubtitleAO 5) placed the heaviest handler ahead of the tightest-deadline poller;
the corrected order ensures STT's 10 ms deadline is met when multiple AOs have
queued events.
