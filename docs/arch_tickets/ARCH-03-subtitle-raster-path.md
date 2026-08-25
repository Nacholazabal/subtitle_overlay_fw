# ARCH-03 — Subtitle pipeline: raster cost, frame sync, renderer contract

**Findings:** F2 (high), F17a, F16, F27, F23, F24
**Size:** M–L · **Depends on:** ARCH-06 lightly (it retires the neighbouring dead
exports; do that first so you refactor less code)

---

## Why this ticket exists

This is the biggest measurable latency win in the firmware, and the one place where the
"nothing blocks the cooperative kernel" invariant is broken not by a call to a blocking
API but by sheer volume of work: **~186,000 uncached AXI-Lite accesses per caption
update, inside one run-to-completion handler, for every partial transcript.**

It also holds the most interesting single fact in the review: the optimised path that
would fix most of it already exists, is unit-tested, and **cannot be reached in
production.**

Get before/after numbers. This belongs in the thesis' latency chapter as a measurement,
not an estimate.

---

## Scope

### F2 — The raster path blocks the kernel; its fast path is dead code (HIGH)

Per `SUBTITLE_TEXT_SIG` (including every partial), `SubtitleAO.on_subtitle_text` →
`render_current_state` → `subtitle_pipeline_write_caption`
(`src/svc/subtitle_pipeline/subtitle_pipeline.c:311`) does:

1. `subtitle_text_renderer_render_caption` into a **32 KB stack buffer** (`:315`)
2. `subtitle_pipeline_set_box` → `subtitle_overlay_configure` (4 register writes)
3. `subtitle_bram_clear` — **8,192 word writes** over the whole 1024×256 mask
   (`src/hal/subtitle_bram/subtitle_bram.c:173`)
4. `subtitle_bram_write_bitmap` — the **per-pixel** path
   (`src/hal/subtitle_bram/subtitle_bram.c:306`–`341`)

Step 4 is the problem. Each pixel is a read-modify-write of a `volatile uint32_t` over
AXI-Lite — one uncached read plus one write — repeated 32 times for every 32-pixel word,
and it writes zero bits as well as one bits so step 3's clear is partly redundant.

For a representative two-line caption (~936 × 95 px): ~88,900 iterations ≈ 178,000
accesses, plus the 8,192-write clear. **~186,000 device accesses per update.**

Now the sharp bit. `subtitle_bram_write_full_bitmap()`
(`src/hal/subtitle_bram/subtitle_bram.c:126`) does the same job word-at-a-time in 8,192
writes with no reads. It is guarded at `:300` by:

```c
if ((x == 0) && (y == 0) && (width == SUBTITLE_BRAM_MASK_WIDTH)
    && (height == SUBTITLE_BRAM_MASK_HEIGHT))
```

The renderer always returns a compact box — height is capped at
`3 × line_height(41) + 2 × RENDER_PADDING_Y(10) = 143`, well under 256
(`src/svc/subtitle_pipeline/subtitle_text_renderer.c:592`–`597`). **The guard can never
be true in production.** The fast path is reachable only from tests.

**Do, in two stages, measuring between them:**

*Stage 1 — make the word path reachable (~20×).* Have the renderer emit at a
32-px-aligned x-origin with a word-multiple width, and relax the HAL guard from "the
whole mask" to "a word-aligned rect": `x % 32 == 0 && width % 32 == 0`, then pack whole
words per row for `height` rows. That is `(width/32) × height` writes and **zero reads** —
for the example box, ~2,800 writes instead of ~178,000 accesses. Keep the general
per-pixel path for the unaligned case (tests use it) but make sure production never
takes it.

*Stage 2 — shadow-diff (another ~5–10× on typical updates).* Keep a 32 KB DRAM shadow
of the mask in `subtitle_bram_t`. On write, compare word-by-word and issue MMIO writes
only for changed words. Partial-to-partial updates change a few hundred words at most,
because most of the caption text is unchanged between partials. This also makes step 3's
full clear unnecessary — clearing becomes "diff against an all-zero shadow".

Note the interaction with step 2: `set_box` reconfigures overlay geometry *before* the
mask is rewritten, so for one frame the hardware can composite new geometry over old
mask content. That is what F17a below is for.

### F17a — The frame-sync primitives exist, are unused, and are marked unusable

`subtitle_pipeline_commit` / `_clear_sof` / `_poll_sof`
(`src/svc/subtitle_pipeline/subtitle_pipeline.c:363`, `:387`, `:403`) wrap the overlay's
sticky start-of-frame flag. None has a production caller. `_commit`'s own doc comment
says it "can spin through many MMIO reads" and "must not be called from QP/C AO state
handlers" — and it spins up to `SUBTITLE_PIPELINE_SOF_TIMEOUT_READS` = **5,000,000**
register reads (`:23`).

So today the overlay update has **no frame synchronisation at all** and can tear
mid-frame, while the API that would fix it sits unused and self-documented as unusable
from the only place that needs it.

**Do:** wire it in properly using the non-blocking primitive, not the spinning one.
`_poll_sof` is already single-read and nonblocking — that is the usable half.
Sketch:

- On new caption text: render + `set_box` into the shadow, then `clear_sof`, then arm a
  short one-shot time event.
- On that time event: `poll_sof`; if set, flush the changed words to BRAM; if not,
  re-arm (bounded retries, then flush anyway rather than dropping the caption).

This composes with Stage 2 above: the shadow-diff is what makes the flush short enough
to fit inside a blanking interval. Note the deliberate single-buffer passthrough
trade-off already recorded as `SRC-H02` in `docs/legacy-llm/decisions.md` — this is the
same tearing concern on the overlay side, so record the outcome next to it.

If you decide frame sync is out of budget for now, **say so in the decision record and
delete the three functions** rather than leaving them as unreachable API. That is the
ARCH-06 rule: no public function without a production caller.

### F16 — The renderer's output contract contradicts itself

`subtitle_text_renderer_render_caption`
(`src/svc/subtitle_pipeline/subtitle_text_renderer.c:566`):

- rejects any `dst_size < RENDER_BITMAP_SIZE` (32 KB, `:580`)
- `memset`s all `dst_size` bytes (`:599`)
- then writes a **compact-stride** image — `stride = (width + 7) / 8` where `width` is
  the returned box width (`:510`)

So it demands a full-mask-sized buffer, clears all of it, and fills a small
differently-strided region at the front, leaving the caller to re-derive the stride from
a returned value. `subtitle_pipeline_write_caption` then passes `sizeof(bitmap)` as
`src_size` to the HAL, which computes its *own* `src_stride` from `width`
(`src/hal/subtitle_bram/subtitle_bram.c:293`) — the two agree only by coincidence of
identical arithmetic in two modules.

**Do:** make the stride explicit in the interface. Either take
`(dst, dst_stride, max_w, max_h)` and have the caller own the layout, or return a small
descriptor (`{ uint8_t *bits; uint32_t stride, width, height; }`). Then the 32 KB
`dst_size` check becomes a real bound (`stride × height ≤ dst_size`) instead of a proxy,
and the `memset` only clears what is used. This is a prerequisite for Stage 1 above,
since the aligned-width change lives in the same contract.

### F27 — Stack budget in a cooperative handler: ~40 KB per caption

One render is 32 KB (mask, `subtitle_pipeline.c:315`) + ~4.6 KB (two `render_layout_t`,
each `512 × 4` codepoints + spans, `subtitle_text_renderer.c:573`–`574`) + 2 KB (nested
`RENDER_UTF8_MAX` buffers in `build_layout`/`sanitize_slice`, `:304` and `:133`).

Fine on Linux's 8 MB main thread. Fatal if any of this is ever reused on bare metal or
an RTOS with per-task stacks — which is a live possibility for a thesis that discusses
hardware/software partitioning.

**Do:** move the mask buffer to a file-scope static in `subtitle_pipeline.c` (safe:
single-threaded by the QV invariant — state that in the comment), or into
`subtitle_pipeline_t`. Same for the two `render_layout_t` if you keep them. Then write
the resulting worst-case stack figure into the decision record as a stated assumption
rather than an accident.

### F23 — Unguarded signed overflow on an ink-free line

`measure_line` returns `{INT_MAX, INT_MIN, INT_MAX, INT_MIN}` when no glyph in the span
has a bitmap (`src/svc/subtitle_pipeline/subtitle_text_renderer.c:367`), and
`measure_visible` then computes `max_x - min_x + 1` on it (`:408`) — signed overflow,
undefined behaviour.

Currently unreachable because `decode_text` rejects all-whitespace input
(`:192`), but the invariant is held by a *different module's* early return rather than by
a check at the point of use.

**Do:** guard the empty-bounds case explicitly (treat ink width as 0 and skip the line),
and add a unit test that feeds a span of glyphs with `bitmap == NULL`.

### F24 — 3-byte UTF-8 is mis-decoded in the renderer

`decode_codepoint` handles only 1- and 2-byte sequences
(`src/svc/subtitle_pipeline/subtitle_text_renderer.c:145`–`160`): a 3-byte lead (0xE2…)
is treated as 2-byte, consuming 2 bytes and leaving a stray continuation byte that is
then decoded as a codepoint of its own — desynchronising the rest of the string.

Safe today only because `subtitle_text_sanitize` always runs first and converts or
strips every 3- and 4-byte sequence (`src/svc/subtitle_pipeline/subtitle_text_sanitize.c:147`).
That is an undocumented coupling between two modules that should each be independently
correct.

**Do:** the cheap correct fix is one comment plus one assertion of the precondition
("input must be sanitised; only 1- and 2-byte sequences are representable in the
font"), and a test asserting a raw 3-byte input either renders as the fallback glyph or
is rejected — not silently mangled. Full 3-byte decoding is not needed; the font has no
glyphs for it.

---

## Out of scope

- Retiring the *other* test-only exports around these files
  (`subtitle_pipeline_write_text`, `_write_bitmap`, `subtitle_bram_set_pixel` /
  `_clear_pixel`, `subtitle_text_renderer_render`): **ARCH-06**.
- The per-render INFO log on the cooperative thread: **ARCH-01** (F3).

---

## Acceptance criteria

- [ ] Production caption updates take a word-aligned path with **zero** per-pixel MMIO
      reads. Verified by inspection *and* by a test asserting the aligned path is chosen
      for renderer-shaped geometry.
- [ ] Measured MMIO access count (or wall-clock) per caption update recorded
      before and after, in the ticket's closing note and in `decisions.md`. Use the ILA
      `capture` skill or a `clock_gettime` bracket around
      `subtitle_pipeline_write_caption`.
- [ ] Overlay geometry and mask content are never visibly inconsistent: either
      frame-synced via `poll_sof`, or the decision to defer is recorded **and** the
      three SOF functions are deleted.
- [ ] The renderer's output contract states its stride explicitly; no module re-derives
      another's stride.
- [ ] No caption render allocates more than ~8 KB of stack; the worst-case figure is
      documented.
- [ ] Ink-free line and raw-3-byte-UTF-8 inputs have tests and cannot invoke UB.
- [ ] `make test` green, `make clang-tidy` clean, `./scripts/build.sh` succeeds.

## Verification

- **Unit:** `test/hal/subtitle_bram/test_subtitle_bram.c` — assert the aligned-rect path
  is taken for `(x=0, w=960, h=96)`; count writes via the existing fake MMIO region and
  assert no reads occur on that path.
- **Unit:** `test/svc/subtitle_pipeline/test_subtitle_text_renderer.c` — assert returned
  width is a multiple of 32; add the ink-free-line and 3-byte-UTF-8 cases.
- **Bench:** bracket `subtitle_pipeline_write_caption` with `CLOCK_MONOTONIC` and log the
  duration at DEBUG for a run; compare before/after. Cheaper than ILA and enough for the
  thesis figure — use ILA if you want the AXI-level picture.
- **On-board (Nacho):** confirm captions no longer tear on update, and that fast partials
  keep up with speech.
