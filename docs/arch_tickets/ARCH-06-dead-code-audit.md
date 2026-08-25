# ARCH-06 — Dead code, unreachable paths, and write-only state

**Findings:** F17b, F18, F20, plus a fresh sweep (items **D1–D7** below)
**Size:** S–M · **Depends on:** nothing · **Do early** — it shrinks what ARCH-03/04/05
have to refactor

---

## Why this ticket exists

This was the concern raised directly, and the sweep found more than the review had. The
problem is not size — it is that **dead code is indistinguishable from intended design
when someone reads the tree cold.** An examiner opening `subtitle_pipeline.h` cannot tell
that five of its twelve functions have no caller, so they read the interface as the
design. Every unreachable branch is a claim about behaviour that isn't true.

There is a second, sharper reason. Two of the findings elsewhere in this backlog exist
*because* dead code hid a real problem: the unreachable fast path in ARCH-03 looked like
an optimisation that was working, and the vestigial chunk struct in ARCH-04 hid the fact
that the audio queue had moved.

**The rule this ticket establishes:** every exported symbol has a production caller, or
it does not exist. Test-only visibility is a build-time concern, not a public header.

---

## Decision rule (apply per item, don't blanket-delete)

For each item below, pick exactly one and record it:

- **USE** — there is a real need; wire it into the product now. (Only if the need is
  concrete and in this backlog, not "might be handy".)
- **DELETE** — remove the symbol, its declaration, and its tests. Git remembers it.
- **EXPOSE-FOR-TEST** — genuinely needed by tests but not by the product: make it
  internal (`static`) and reach it from the test via a `test/support` accessor or by
  including the `.c`, rather than exporting it from the public header.

Do **not** leave anything as "keep, unused" without a one-line reason in
`docs/legacy-llm/decisions.md`. A documented deliberate keep is fine; an undocumented
one is what got us here.

---

## Scope

### F17b — Exported functions with no production caller

Counted as *references across all non-vendored `src/`*, where 2 = prototype +
definition, i.e. **zero call sites**:

| Symbol | src refs | test refs | Suggested |
| --- | --- | --- | --- |
| `subtitle_pipeline_write_text` | 2 | 1 | DELETE — a one-line wrapper for `write_caption(…, 1U)` (`subtitle_pipeline.c:299`) |
| `subtitle_pipeline_write_bitmap` | 2 | 3 | DELETE or EXPOSE-FOR-TEST |
| `subtitle_pipeline_commit` | 2 | 3 | **ARCH-03 owns this** (F17a) — USE via `poll_sof`, or DELETE all three SOF functions |
| `subtitle_pipeline_clear_sof` | 2 | 3 | ↑ same decision |
| `subtitle_pipeline_poll_sof` | 2 | 6 | ↑ same decision |
| `subtitle_bram_set_pixel` | 2 | 9 | EXPOSE-FOR-TEST — a reasonable HAL primitive, but exporting it with no caller is what makes the header misleading |
| `subtitle_bram_clear_pixel` | 2 | 2 | DELETE — not even a plausible future need once bitmaps are word-written |
| `subtitle_text_renderer_render` | 2 | 13 | DELETE — wrapper for `render_caption(…, 1U)` (`subtitle_text_renderer.c:542`); migrate the 13 tests to `render_caption` |
| `video_dma_status` | 2 | 8 | Decide: USE (a health check in `VideoAO`'s poll would be genuinely useful, and would give `POLL_ERROR` a real cause) or DELETE |
| `video_gpio_set_hpd` | 2 | 0 | DELETE — zero references anywhere including tests; `video_gpio_init` already asserts HPD (`video_gpio.c:57`) |
| `video_modes_default` | 2 | 1 | DELETE — the pipeline only ever matches by resolution (`video_modes_find`) |
| `video_modes_all` | 2 | 3 | DELETE or EXPOSE-FOR-TEST |
| `video_pipeline_get_state` | 2 | 20 | EXPOSE-FOR-TEST, or USE in `VideoAO`'s error logging (it would make the log say *which* state failed) |
| `log_unsubscribe` | 2 | 3 | Keep — a subscribe/unsubscribe pair is a coherent API even with one production subscriber. Record the reason. |
| `stt_ws_client_cleanup` | 2 | 2 | DELETE — self-documented as "legacy", early-returns leaving the object initialised (`stt_ws_client.c:1569`). **ARCH-05** replaces it. |

Also in the same family, from the review: `stt_ws_client_service` and
`stt_ws_client_send_audio` are exported and documented as existing "for host probes"
(`stt_ws_client.h:250`, `:274`). They *are* called internally, so they aren't dead — but
they should not be public. Fold into **ARCH-05**.

### F18 — Dead files shipped in `src/`

Neither is referenced by `Makefile` or `project.yml` — nothing compiles them:

- `src/utils/template/template.c` + `.h`
- `src/utils/template_qpc_AO/template_qpc_AO.c` + `.h`

They are code *templates*, not product code. `src/` should contain only what ships.

**Do:** move to `tools/templates/` (or `docs/`), and reference them from the
`multi-file-workflows` skill so the "adding a new AO" checklist points at them. Keeping
them is right; keeping them in `src/` is not.

### F20 — Unreachable branch and a log-spam loop

`src/svc/usb_audio/usb_audio_stream.c:180` handles `-EAGAIN` from
`usb_audio_capture_read_chunk`, which **never returns it** — the HAL returns 0,
`-EINVAL`, or `-EIO` only (`usb_audio_capture.c:355`–`415`).

Two lines up, `first_read_pending` gates a `LOG_INFO("waiting for first ALSA chunk")` at
`:170` that re-fires on **every** loop iteration until the first *successful submit*,
because the flag is cleared only at `:242`.

**Do:** delete the `-EAGAIN` branch (or make the HAL actually return it if a nonblocking
mode is planned — but nothing suggests it is), and log the "waiting" line once. Note that
ARCH-01 also edits this loop for the retry ceiling; whoever lands second rebases.

### D1 — Write-only struct fields (fresh sweep)

Seven fields are assigned and never read. Each is either a missing feature or a leftover;
both are worth resolving because a field that is written implies to a reader that
something depends on it.

| Field | Written at | Never read |
| --- | --- | --- |
| `video_pipeline_t.frames[]` | `video_pipeline.c:110` (filled by `video_dma_init`) | **see D2 — this one is significant** |
| `video_pipeline_t.input_timing` | `video_pipeline.c:223` | Detected timing is stored and never consulted; `video_modes_find` uses the local `timing` |
| `video_input_t.timing` | `video_io.c:161` | A second copy of the same thing, also unread |
| `video_input_t.frame_index` | `video_io.c:207` | — |
| `video_output_t.frame_index` | `video_io.c:333` | — |
| `video_dynclk_t.actual_frequency_mhz` | `video_dynclk.c:432` | The *actual* synthesised clock is computed, stored, and never reported. Arguably should be **logged** — it is a real measurement (the mode table asks for 74.25 MHz; what did the MMCM give?) |
| `video_vtc_t.device_id` | `video_vtc.c:63` | — |
| `subtitle_pipeline_t.enabled` | `subtitle_pipeline.c:174`, `:446` | Overlay enable state is tracked and never queried |

**Do:** delete, or use. Two are worth *using* rather than deleting:
`actual_frequency_mhz` in a `LOG_INFO` at mode start (thesis-relevant: it quantifies pixel
clock error), and `input_timing` if ARCH-02's mode-change detection needs a previous
value to compare against — check with that ticket before deleting it.

### D2 — The framebuffer is mapped and never touched

`video_dma_init` mmaps `frame_count` framebuffers and hands the pointers back
(`video_dma.c:235`–`250`); `video_pipeline` stores them in `pipeline->frames`
(`video_pipeline.c:110`) and **nothing ever dereferences them.** With
`VIDEO_PIPELINE_FRAME_COUNT == 1` and 1920×1080×3 stride, that is a ~6 MB mmap of
device memory established at every init for no current consumer — the passthrough path
is entirely VDMA-to-VDMA and never involves the CPU.

This is almost certainly scaffolding for a future software compositor, and it may well be
correct to keep. But it should be a *stated* intent, not an unexplained mmap.

**Do:** decide and record. If the CPU will never touch pixels (the overlay is done in
PL), stop mapping them and drop the `frames` parameter from `video_dma_init` — the
`uint8_t* frames[VIDEO_DMA_MAX_FRAMES]` out-parameter is the only reason that awkward
signature exists. If a software path is planned, say so in `decisions.md` with a
`SRC-` tag.

### D3 — Duplicate fields in `video_dma_t`

`dma->frame_size` and `dma->mmap_size` are both assigned `info.frame_size` on adjacent
lines (`video_dma.c:232`–`233`) and used interchangeably afterwards (`:237`, `:239` use
one; `:275`, `:281` use the other).

**Do:** keep one. If they can ever legitimately differ, the code doesn't express how.

### D4 — Unused macros

- `STT_WS_READ_SLICE` (`src/svc/stt/stt_ws_client.c:30`) — zero uses. The read slice is
  actually `sizeof(client->rx) - rx_used` (`:860`).
- `VIDEO_PIPELINE_MAX_HEIGHT` (`src/svc/video_pipeline/video_pipeline.h:37`) — zero uses;
  only `MAX_WIDTH` feeds `VIDEO_PIPELINE_STRIDE`. Either use it to bound the mode table /
  frame size, or delete it. Note it's a *plausible* missing guard: nothing currently
  checks a detected mode against the buffer's max height.
- In `src/bsp/platform/xparameters_linux.h`: `XPAR_VTC_0_*` / `XPAR_VTC_1_*` are unused
  duplicate aliases of the `XPAR_V_TC_*` names actually used, and
  `XPAR_AXI_GPIO_VIDEO_DEVICE_ID` / `_IS_DUAL` / `_INTERRUPT_PRESENT` /
  several `_HIGHADDR`s have no references. Two names for one address is exactly how a
  wrong-address bug gets written.

**Do:** delete the aliases and unused entries. Keep only what the code references, since
this header is hand-maintained against the bitstream rather than generated.

### D5 — Vestigial type: `usb_audio_stream_chunk_t`

`src/svc/usb_audio/usb_audio_stream.h:48` still declares `payload` / `timestamp_ns` /
`sequence` / `bytes_used` from when this module owned the audio queue. It is now a 1.9 KB
stack scratch buffer. `usb_audio_stream_t.next_sequence` and `total_dropped` are the
matching leftovers (still used, but only to populate fields the consumer re-derives).

**Owned by ARCH-04 (F9)** — the sink inversion is what makes the correct shape obvious.
Listed here for completeness; don't fix it twice.

### D6 — The unreachable fast path

`subtitle_bram_write_full_bitmap` cannot be reached in production
(`subtitle_bram.c:126`, guard at `:300`). **Owned by ARCH-03 (F2)** — the fix is to make
it reachable, not to delete it. Listed here because it is the most consequential
"dead code" item in the tree and belongs in this ticket's final inventory.

### D7 — Stale identifiers

`stt_event_rx_delivery_status_t` and the `stt_event_rx_parse_line()` doc reference.
**Owned by ARCH-05 (F19).** Same reason as above.

---

## Add a guard so this does not come back

The sweep that produced D1–D4 was ad-hoc. Make it repeatable — this is the part that
makes "flawless architecture" a property the repo maintains rather than a state it was
briefly in.

**Do:**

1. Add `-Wunused-function -Wunused-but-set-variable -Wunused-macros` to the host
   `video-port-check` target's flags in the `Makefile`. The first two are free; the third
   is noisy on the BSP shims, so scope it to `src/svc` and `src/hal` if needed.
   `video-port-check` already compiles the full non-vendored tree with host gcc, so it is
   the right place — no VM needed.
2. Add `scripts/dead_symbols.sh` reproducing the exported-symbol sweep: for every function
   declared in a `src/**/*.h`, count references in non-vendored `src/`, and fail if any
   has fewer than 3 (prototype + definition + ≥1 call). Allow an explicit ignore list with
   a required reason string per entry — that file becomes the documented "deliberate keep"
   record.
3. Wire both into `.github/workflows/` alongside the existing checks, or at minimum into
   `make clang-tidy`'s script so it runs with the rest of the static analysis.

The ignore-list-with-reasons is the important half. A check that can only be satisfied by
deleting things gets disabled; a check that can be satisfied by *explaining* things gets
kept.

---

## Out of scope

- Fixing the *behaviour* behind D6 (raster fast path): **ARCH-03**.
- Fixing D5 and the config `getenv` sprawl: **ARCH-04**.
- Fixing D7 and the STT public surface: **ARCH-05**.

This ticket removes and documents; the other tickets change behaviour. Keeping that line
is what makes this one safe to land early and review quickly.

---

## Acceptance criteria

- [ ] Every exported function in non-vendored `src/` has ≥1 production call site, **or**
      an entry in the ignore list with a stated reason, **or** is gone.
- [ ] `src/utils/template*` no longer exists under `src/`; the templates live under
      `tools/` and the `multi-file-workflows` skill points at their new location.
- [ ] No write-only struct fields remain in `src/hal/` or `src/svc/`, except any recorded
      as deliberate in `decisions.md`.
- [ ] The framebuffer mmap is either removed or documented with a `SRC-` decision entry.
- [ ] `video_dma_t` has one size field.
- [ ] `xparameters_linux.h` contains only referenced definitions; no duplicate address
      aliases.
- [ ] The `-EAGAIN` branch is gone and the "waiting for first ALSA chunk" line logs once.
- [ ] `make video-port-check` compiles with `-Wunused-function
      -Wunused-but-set-variable` and produces **no** warnings.
- [ ] `scripts/dead_symbols.sh` exists, passes, and runs in CI (or from
      `scripts/clang_tidy.sh`).
- [ ] `make test` green — note that deleting exports means deleting or migrating their
      tests; coverage percentage may *rise* as untested-but-exported code disappears.
- [ ] `./scripts/build.sh` succeeds.

## Verification

- `make video-port-check` clean with the new warning flags — this is the strongest single
  signal, since it is a real compile of the real tree.
- `scripts/dead_symbols.sh` exits 0.
- `make coverage` before/after: record both numbers. If coverage drops, a live symbol was
  deleted by mistake.
- Diff review: every deletion should be either a wrapper, a leftover, or accompanied by an
  ignore-list reason. Nothing in this ticket should change runtime behaviour — if the
  board behaves differently after it, something live was removed.
