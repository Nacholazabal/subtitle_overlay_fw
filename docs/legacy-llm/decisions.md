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
