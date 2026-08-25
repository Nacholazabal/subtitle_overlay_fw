# ARCH-04 — Layering contracts: dependency inversion, error model, config, logging

**Findings:** F9 (high), F11, F13, F14, F12, F15
**Size:** M–L · **Depends on:** nothing (ARCH-05 wants the config half of this done first)

---

## Why this ticket exists

The `svc/` ↔ `hal/` ↔ `bsp/` split is real, not nominal — which is what makes the six
places that violate it stand out. Each one is a *cross-cutting concern with two
contracts instead of one*: two ways to report an error, two ways to log, configuration
read at four layers, and one service reaching sideways into another's globals.

Individually these are small. Together they are what makes the tree read as
"accumulated" rather than "designed", and that is the difference the thesis is being
judged on.

**Invariant this ticket defends:** dependencies point inward and downward. `svc/` may
use `hal/`; `hal/` may use `bsp/`; siblings inside `svc/` talk through events or
injected interfaces, never through each other's globals. Board identity lives in `bsp/`.
Configuration enters once, at the top.

---

## Scope

### F9 — The audio producer reaches into the STT service's singleton (HIGH)

`src/svc/usb_audio/usb_audio_stream.c` includes `stt_ws_client.h` (`:25`) and calls
`stt_ws_client_shared()` from its capture thread (`:152`), then
`stt_ws_client_submit_audio()` per chunk (`:216`). One `svc/` module reaches sideways
into another `svc/` module's private singleton, and the real-time data path bypasses the
active-object model entirely.

Three concrete costs:

- **Testability.** `usb_audio_stream` cannot be unit-tested without the whole WebSocket
  client. `test/svc/usb_audio/test_usb_audio_stream.c` exists but is constrained by this.
- **Implicit ordering.** Whichever thread calls `shared()` first constructs the client via
  `pthread_once` (`stt_ws_client.c:912`). That is *why* the "all audio before STT init is
  silently dropped" behaviour (ARCH-02, F6) is invisible when reading either module.
- **Inverted direction.** The producer knows its consumer's identity, so a second sink —
  a local PCM recorder for the thesis' evaluation runs, say — means editing the producer.

The vestigial evidence: `usb_audio_stream_chunk_t`
(`src/svc/usb_audio/usb_audio_stream.h:48`) still carries `payload`, `sequence` and
`timestamp_ns` from when this module owned the queue. It is now a 1.9 KB stack scratch
buffer with a struct's name. `usb_audio_stream_t.next_sequence` and `total_dropped`
survive for the same reason.

**Do:** invert with a one-struct sink interface, owned by a new
`src/svc/usb_audio/audio_sink.h` (or in `app/` if you prefer it as an app-level contract):

```c
typedef struct {
    void *ctx;
    int (*submit)(void *ctx, const void *pcm, size_t size,
                  uint64_t timestamp_ns, uint32_t dropped);
} audio_sink_t;
```

Pass it to `usb_audio_stream_start()`. `SttAO` (or `app.c` at wiring time) supplies the
WS client as the sink — note that `SttAO` already holds the client pointer
(`src/svc/stt/SttAO.c:44`), so it is the natural provider. Then:

- delete `#include "stt_ws_client.h"` from `usb_audio_stream.c`
- collapse `usb_audio_stream_chunk_t` to what is actually used
- the `pthread_once` singleton leaves the real-time path (ARCH-05 can then remove it
  entirely)

### F11 — Two error vocabularies meet with no conversion layer

The video stack returns Xilinx `XST_*` all the way up: `video_dma` → `video_vtc` /
`video_gpio` / `video_dynclk` → `video_io` → `video_pipeline`. Everything else — audio,
STT, subtitle, `net_tls` — returns negative errno plus the `APP_ESTATE` extension
(`src/app/errorno.h:29`).

They meet in `VideoAO`, which tests `== 0` — correct only because `XST_SUCCESS` happens
to be 0 — and then **discards the real code**, substituting a flat `-EIO`:

- `src/svc/video_pipeline/VideoAO.c:132`–`151` (init: `status = -EIO` regardless)
- `src/svc/video_pipeline/VideoAO.c:171` (poll: `enter_error(me, -EIO)`)
- `src/svc/video_pipeline/video_pipeline.c:97` (returns `XST_INVALID_PARAM` / `XST_FAILURE`)

So every video fault reaching `SystemAO` and the log is the same value, whether the cause
was an unmapped MMIO region, a PLL lock timeout, or a rejected ioctl.

**Do:** pick negative errno as the single vocabulary above the HAL. Convert **once**, at
the HAL boundary, inside each `video_*` adapter — those adapters become the only modules
that mention `XST_*` (they must, they call the imported Xilinx driver). Then `video_io`,
`video_pipeline` and `VideoAO` speak errno like everything else, and the real code
reaches `app_error_evt_t.code`. Map at minimum: `XST_SUCCESS`→0,
`XST_INVALID_PARAM`→`-EINVAL`, `XST_DEVICE_NOT_FOUND`→`-ENODEV`, `XST_NO_DATA`→`-ENODATA`,
`XST_FAILURE`→`-EIO`.

`XST_NO_DATA` is load-bearing control flow, not an error — `video_pipeline_poll` uses it
to mean "detector not ready yet" (`video_pipeline.c:200`). Keep that distinction visible
in the new vocabulary (`-ENODATA` is fine, but comment it).

### F13 — Board identity leaks upward into the service layer

`src/svc/video_pipeline/video_io.c` includes `xparameters.h` (`:17`) and hardcodes
`XPAR_V_TC_1_DEVICE_ID` for the input VTC (`:55`) and `XPAR_V_TC_0_DEVICE_ID` for the
output (`:263`). A service module therefore knows which numbered instance of a hardware
block plays which role in this bitstream — the one fact the HAL exists to hide. It also
means "input is VTC 1" is discoverable only by reading a service file.

**Do:** let the HAL name roles, not instances — `video_vtc_init_detector(video_vtc_t*)`
and `video_vtc_init_generator(video_vtc_t*)`, each resolving its device ID internally
(they already translate through `hw_platform_translate`, so the knowledge belongs there).
Alternative if you prefer explicitness: pass a small board descriptor down from `bsp/`.
Either way, `xparameters.h` must not be reachable from `svc/` — that is the check.

Related cleanup in the same header: `src/bsp/platform/xparameters_linux.h` defines both
`XPAR_V_TC_0_*` and unused `XPAR_VTC_0_*` aliases. ARCH-06 owns deleting the dead ones;
don't add more.

### F14 — Configuration has no layer: 22 env vars read at four sites

Twenty-two `SUBTITLE_*` / `USB_AUDIO_*` variables, read by `getenv` in four modules —
one of them a **HAL driver**:

- `src/svc/stt/stt_ws_client.c:1028`–`1054` (16 vars, via `env_string` / `env_u32`)
- `src/svc/usb_audio/usb_audio_stream.c:329`, `:360`, `:376` (3)
- `src/hal/usb_audio/usb_audio_capture.c:207`–`209` (3 — mixer control, card, volume %)
- `src/svc/subtitle_pipeline/SubtitleAO.c:243` (2, via `resolve_timeout_ticks`)

No schema, no single place that lists them, no one-shot dump of the effective
configuration at boot. `scripts/run.sh` documents 8 of the 22.

For the thesis this is a **reproducibility** problem as much as a design one: a recorded
run cannot be reconstructed from its log, because the log never states the full
configuration it ran under.

**Do:** add `src/app/app_config.{c,h}` that:

- reads and validates every variable **once**, at startup, before any AO starts
- logs the complete resolved set as one block (this is the reproducibility payoff)
- hands typed structs down through the existing `*_config_t` parameters —
  `usb_audio_capture_config_t`, `usb_audio_stream_config_t`, `stt_ws_client_config_t`
  are already shaped for this, so mostly it is moving the `getenv` calls up
- keeps `number_parse_u32` as the validation primitive (already good, already tested)

HAL modules then take configuration only as arguments. The three mixer settings in
`usb_audio_capture.c` are the only real outliers to move — extend
`usb_audio_capture_config_t` with the mixer fields.

Then document all 22 in one place: either a table in the config header or
`docs/configuration.md`. Extend `scripts/run.sh` to pass through the ones that matter for
a run.

### F12 — Two logging paths, and the log line is 128 bytes

Three modules write with `fprintf(stderr, "[module] …")` instead of `log.h`:

- `src/hal/video_dma/video_dma.c:64`, `:106`, `:135`, `:194`, `:205`, `:212`, `:222`, `:243`
- `src/hal/video_dynclk/video_dynclk.c:326`, `:380`
- `src/bsp/platform/linux/hw_platform.c:59`, `:107`

Only `log.h` carries a severity level, so the video HAL's failures cannot be filtered,
thresholded, or redirected with the rest — and they are exactly the failures that matter
during bring-up.

Separately, `LOG_MAX_MESSAGE_LENGTH` is **128 bytes** (`src/utils/log/log.h:30`) and
`log_message` truncates silently (`src/utils/log/log.c:108`). Lines already over it:

- the `stt-ws: target=… ca=… connect=… handshake=… idle=… ping=… backoff=…` banner
  (`stt_ws_client.c:1104`)
- the nine-counter STT metrics line (`SttAO.c:217`) — described in its own comment as
  "the only place the run's counters are recorded", and being cut in half
- `subtitle: rendering … text="…"` with a 128-char caption (`SubtitleAO.c:345`)

**Do:** route all three modules through `log.h`, and raise `LOG_MAX_MESSAGE_LENGTH` to
256 (it is `#ifndef`-guarded, so a `-D` in the Makefile is enough — but prefer changing
the default so tests see the same value). Also: `log.h`'s header comment claims the
facility "adds a timestamp" (`log.h:9`–`11`) and `app_log_output` does not
(`app.c:76`) — add one (monotonic ms since start is enough and is what makes recorded
runs analysable) or fix the doc.

ARCH-01 owns the *blocking* aspect of logging (per-event INFO, `fflush`). If ARCH-01
deferred the ring-buffer version, this is the ticket to reconsider it in — you are
already in `log.c`.

### F15 — Protocol knowledge duplicated in the transport layer

`push_event` decides finality with
`strstr(line, "\"is_final\":true")` (`src/svc/stt/stt_ws_client.c:615`) while the real
parser — `stt_transcript_parse_line`, which handles both the `is_final` and `type`
dialects, validates their agreement, and is thoroughly tested — is one layer away and is
called on the very same line minutes later in `poll_events` (`:1431`).

The substring match is whitespace-sensitive: a server emitting `"is_final": true` would
silently classify **every final as a partial**, which changes which events get shed when
the ring is full (`:626`–`661`) — so a protocol formatting change degrades caption
quality with no error anywhere.

**Do:** stop duplicating the knowledge. Either parse once at ingress and store the
parsed `is_final` on the ring entry (better: it also removes the double parse), or, if
the ring must stay a raw-line buffer, call `stt_transcript_parse_line` for the finality
bit rather than `strstr`. ARCH-05 will move this code; doing it here first is fine and
makes that split smaller.

---

## Out of scope

- Splitting `stt_ws_client` itself: **ARCH-05**. This ticket only fixes the *contracts*
  it participates in (F15, and the config half of F14).
- Deleting the dead `XPAR_VTC_*` aliases and other dead symbols: **ARCH-06**.
- The blocking/`fflush` behaviour of logging: **ARCH-01**.

---

## Acceptance criteria

- [ ] `grep -rn 'stt_ws_client' src/svc/usb_audio/` returns nothing; the capture path
      submits through an injected `audio_sink_t`.
- [ ] `usb_audio_stream` has a unit test that runs against a counting fake sink, with no
      WebSocket client linked.
- [ ] `grep -rn 'XST_' src/svc/` returns nothing; `grep -rln 'xparameters' src/svc/`
      returns nothing.
- [ ] `app_error_evt_t.code` carries the real failure cause for video faults; distinct
      causes produce distinct codes in the log.
- [ ] `grep -rn 'getenv' src/` shows call sites only in `src/app/app_config.c`.
- [ ] Startup logs the complete resolved configuration as one block; all 22 variables are
      documented in one place.
- [ ] `grep -rn 'fprintf(std' src/` returns nothing outside `app.c`'s log sink.
- [ ] `LOG_MAX_MESSAGE_LENGTH` ≥ 256; the STT metrics line and the stt-ws banner appear
      complete in a captured log.
- [ ] Finality is decided by the parser, not by `strstr`; a test with
      `"is_final": true` (with a space) classifies correctly.
- [ ] `make test` green, `make clang-tidy` clean, `./scripts/build.sh` succeeds.

## Verification

- **Unit:** new `test/svc/usb_audio/` case driving `usb_audio_stream` against a fake sink,
  asserting submit count and dropped accounting.
- **Unit:** extend `test/svc/video_pipeline/test_video_pipeline.c` and `test_video_io.c`
  to assert errno-style returns; extend `test/hal/video_dma/test_video_dma.c` for the
  conversion mapping.
- **Unit:** new `test/app/test_app_config.c` — valid, invalid, and absent values for a
  representative sample of variables, asserting defaults are retained and warnings emitted.
- **Unit:** extend `test/svc/stt/test_stt_ws_client.c` with the spaced-JSON finality case.
- **On-board (Nacho):** confirm the config block appears at boot, and that a deliberate
  video fault (e.g. wrong bitstream) now logs a specific cause rather than `-EIO`.
