# Architecture Review — src/

## Scope and intent

This review looks at the *architecture* of the author-owned code under src/: the
QP/C active-object topology, layer boundaries (app / svc / hal / utils),
module cohesion, dependency direction, and places where the code is more tangled or
more verbose than it needs to be.

It is deliberately complementary to c_code_quality_review.md. That report covers
C-language quality (CERT-C, the errorno.h macro collision, truncation, doxygen,
etc.) and explicitly excludes topology. This one is the opposite: it is about whether
the active-object design and the layering are clean, and where the AI-written code
grew spaghetti or boilerplate.

What was reviewed (read in full): src/app/, the five AO modules under src/svc/,
their service support modules, all of src/hal/, and all of src/utils/. Build
files, src/bsp/, src/qpc/ (vendored), and tests were intentionally ignored, as
requested.

*No code was changed. This is analysis only.*

---

## Executive assessment

The architecture is, overall, in good shape and clearly designed on purpose — much
better than typical "the AI just kept adding files" output. The layer split is real
and mostly respected, the AO modules follow a consistent template, event contracts
are explicit, timer lifecycles are correct, and SystemAO is a genuine orchestrator
that does not own or construct the other AOs. The error contract (0 / negative
-Exxx) is consistent across nearly every module.

The things worth fixing fall into three buckets:

1. *Two oversized service files* (usb_audio_stream.c at 849 lines and
   stt_event_rx.c at 677 lines) that each mix three or four responsibilities in one
   translation unit. These are the real "spaghetti" hotspots and the best return on
   refactoring effort.
2. *Business/policy logic that has leaked down into the HAL layer*
   (video_dynclk.c clock-solver tables, usb_audio_capture.c ALSA negotiation/AGC).
   The HAL is supposed to be a thin register/ioctl adapter; in two modules it is
   making decisions that belong in a service.
3. *Small, real layering and consistency papercuts* — a svc → hal include for
   geometry constants, synchronous one-time device setup performed inside an AO init
   handler, and per-event MMIO writes inside the subtitle handler.

Importantly, a claim that is easy to make but is *not* true here: the AOs do *not*
do continuous blocking I/O inside their state handlers. The blocking work (ALSA
capture loop, TCP send loop) lives in worker threads, and the polling paths
(*_poll()) are non-blocking. The explicitly-dangerous subtitle_pipeline_commit()
(documented as "must not be called from QP/C AO state handlers") is *not* called
from any handler. So the core real-time discipline is intact; the remaining concerns
are smaller and localized.

---

## Layer map and dependency direction

app  ─────▶ svc ─────▶ hal ─────▶ bsp / Xilinx libs
  │           │          │
  └───────────┴──────────┴────────▶ utils   (leaf, no upward deps)
              └────────────────────▶ qpc     (framework)

| Layer  | Includes from                         | Verdict |
|--------|---------------------------------------|---------|
| app  | svc, utils, qpc                 | :white_check_mark: correct — top layer wires everything |
| svc  | app (contracts), peer svc, utils, qpc | :white_check_mark: correct, *one exception* (below) |
| hal  | bsp, Xilinx libs, utils (log)   | :white_check_mark: correct — no upward includes into svc/app |
| utils| stdlib only                           | :white_check_mark: clean leaf layer, dependency-free |

The dependency graph points the right way almost everywhere. utils is a proper leaf
(verified: log, ring_buffer, timestamp, delay, logarithm include only
stdlib). No HAL module reaches up into svc or app.

*The one inversion:* [src/svc/subtitle_pipeline/subtitle_text_renderer.c](src/svc/subtitle_pipeline/subtitle_text_renderer.c#L20)
includes the HAL header subtitle_bram.h to borrow the mask geometry constants
(SUBTITLE_BRAM_MASK_WIDTH/HEIGHT) and derive its bitmap stride/size from them. A
service module pulling a hardware header for compile-time geometry is a soft layering
break. (Note subtitle_pipeline.h also includes subtitle_bram.h, so the coupling is
already established in the pipeline header.) This is low-severity but worth tidying —
see recommendations.

---

## Active-object topology

Five AOs, started in [src/app/app.c](src/app/app.c#L82): Video, USBAudio,
Subtitle, Stt, and System (started last, so it can immediately fan out
COMPONENT_INIT_SIG). Priorities are sensible (orchestrator highest, leaf sinks
lowest). main() is minimal and app_init() privately owns pools, queue storage,
priorities, and QActive_start() — exactly the intended split.

Event flow:

                 COMPONENT_INIT_SIG (directed)
   SystemAO ───────────────────────────────────▶ Video / USBAudio / Subtitle / Stt
       ▲   COMPONENT_READY_SIG / COMPONENT_ERROR_SIG (directed back)
       └───────────────────────────────────────────┘

   SttAO ───── SUBTITLE_TEXT_SIG (directed) ─────▶ SubtitleAO

This is a clean, directed command/response orchestration. A few observations:

- *SystemAO is a true orchestrator.* It does not contain, construct, or start the
  other AOs; it coordinates them only through opaque AO_* handles and events. State
  shape (init → run → terminal error) is clean and the handlers are readable.
  :white_check_mark: This is the part most likely to rot in an AI-grown codebase, and it has held up.

- *Communication is entirely directed posts, no publish/subscribe.* That is a
  legitimate choice for this fan-out/fan-in topology and the repo's own QP/C notes say
  not to add pub-sub ceremony unless needed. Worth a conscious decision point: as soon
  as more than one AO needs to react to the same fact (e.g. several consumers of "video
  dimensions known"), switch those specific signals to QACTIVE_PUBLISH. Today,
  directed posts are fine.

- *Timer lifecycles are correct.* QTimeEvt objects are constructed in the ctor,
  armed in the init handler (VideoAO, SttAO) or rearmed on activity
  (SubtitleAO's inactivity clear), and disarmed on error entry. No lazy
  construction, no leaks. :white_check_mark:

- *Single shared event pool* ([app.c](src/app/app.c#L86), 64 slots, one union of all
  payload types). This is simple and currently fine. The only caveat: it is a global
  resource shared by a 100 Hz system with a high-rate partial-transcript path. The
  per-signal Q_NEW_X margins (e.g. STT reserving more budget for partials than
  finals) are a thoughtful mitigation, but there is no explicit worst-case sizing
  note. Recommend a one-line comment documenting the peak-load assumption next to the
  pool definition so future AOs don't silently overcommit it.

### Nuance: one-time synchronous setup inside init handlers

USBAudioAO, VideoAO, SttAO, and SubtitleAO each perform their device/socket
bring-up synchronously inside the idle-state init handler (e.g.
[USBAudioAO.c](src/svc/usb_audio/USBAudioAO.c#L125) → usb_audio_stream_start(),
which does usb_audio_capture_init() and then spawns the two worker threads at
[usb_audio_stream.c](src/svc/usb_audio/usb_audio_stream.c#L794)). The continuous
blocking work is correctly off-loaded to those threads, so this is *not* the classic
"AO blocks forever in a handler" anti-pattern. The residual concern is only that the
one-time open/bind/connect runs on the AO thread during startup, briefly stalling that
AO's event processing while the chain initializes sequentially. For a startup path this
is acceptable; flag it only if init latency or init-time failures (e.g. TCP connect
retries) ever need to stay responsive to other events. If so, the fix is an async
"start requested → READY/ERROR posted back" handshake rather than a synchronous return.

### Nuance: per-event MMIO inside the subtitle handler

SubtitleAO's SUBTITLE_TEXT_SIG handler calls subtitle_pipeline_clear() →
write_text() → enable() ([SubtitleAO.c](src/svc/subtitle_pipeline/SubtitleAO.c#L359-L373)),
which are bounded BRAM/MMIO writes. These are recurring per-subtitle-event hardware
writes on the AO thread. They are bounded and fast (and crucially they do *not* call
the blocking subtitle_pipeline_commit()), so this is acceptable today. Keep an eye on
it only if subtitle update rate climbs; the bitmap render + BRAM blit per event is the
heaviest thing any handler does.

---
[3:17 PM]## Spaghetti / oversized hotspots (ranked)

### 1. usb_audio_stream.c — 849 lines — *biggest target*

[src/svc/usb_audio/usb_audio_stream.c](src/svc/usb_audio/usb_audio_stream.c) is one
translation unit doing four jobs:

- TCP transport: connect/retry, non-blocking send, FD lifecycle, stream header framing.
- Concurrency: two worker threads (capture + sender) and a mutex/cond producer-consumer
  queue.
- A hand-rolled chunk *queue* (queue_init / queue_push_drop_oldest /
  queue_pop_wait / queue_cleanup).
- Orchestration + config + AGC integration + env-var parsing.

Symptoms: a large cluster of tiny mutex-wrapper accessors (stream_request_stop,
stream_stop_requested, stream_add_dropped, …) that each repeat lock/read/unlock,
nested connect-retry conditionals, and repeated metrics logging.

*Suggested split (description only, no edits):*
- usb_audio_tcp_sender.c — connect/send/header/sender thread.
- usb_audio_chunk_queue.c — the producer-consumer queue (and consider whether
  src/utils/ring_buffer can back it instead of a bespoke queue).
- usb_audio_stream.c (trimmed) — capture thread, AGC merge, config, lifecycle.

This is the single highest-value refactor: it isolates the TCP layer and the queue so
each is independently testable, and it shrinks the file to something a human can hold
in their head.

### 2. stt_event_rx.c — 677 lines — *second target*

[src/svc/stt/stt_event_rx.c](src/svc/stt/stt_event_rx.c) mixes a non-blocking TCP
server (bind/listen/accept/recv, line buffering) with a *hand-rolled JSON parser*
(json_value, json_get_u32/bool/double/string). Each parser repeats the same
"find key → check null → type-parse → return 0/-EINVAL" shape, and process_byte()
folds line buffering, \r\n handling, and event parsing into one ~60-line function.

*Suggested split:*
- json_simple.c — the flat-NDJSON parser, made reusable (table-driven would collapse
  the six near-identical functions). Other modules parse JSON-ish input too, so this
  has reuse value.
- stt_event_rx_net.c — server socket + line accumulation.
- stt_event_rx.c (trimmed) — poll orchestration + building subtitle_text_evt_t.

### 3. video_dynclk.c — policy in the HAL

[src/hal/video_dynclk/video_dynclk.c](src/hal/video_dynclk/video_dynclk.c) carries
large hardcoded MMCM lock/filter lookup tables plus a floating-point frequency solver
(clk_find_params / clk_find_reg). That is *algorithm/policy*, not a register
adapter. A HAL module should set registers and poll the lock bit; deciding which
register values realize a requested frequency belongs above it.

*Suggested split:* move the solver + tables into a svc-level clock-config module
(e.g. video_clock_config) that produces a register set; leave video_dynclk as
set_regs(...) + poll_lock(...). Magic encodings like 0x1041 and the 48-bit
binary literals should also get datasheet-referenced names.

### 4. usb_audio_capture.c — ALSA policy in the HAL

[src/hal/usb_audio/usb_audio_capture.c](src/hal/usb_audio/usb_audio_capture.c) does
PCM parameter negotiation (configure_pcm), overrun/suspend recovery (recover_pcm),
and mixer-based capture-gain control (set_capture_gain). Buffer-strategy and
recovery-threshold decisions are service policy. Consider keeping HAL at
open/read/close and lifting the negotiation/recovery/gain policy into the USB-audio
service. (Lower priority than the dynclk split because this one is at least
self-contained.)

Everything else in svc/ and hal/ is well-factored: subtitle_text_sanitize.c
(clean UTF-8 state machine), subtitle_pipeline.c, video_pipeline.c, video_io.c,
video_modes.c, and the small HAL adapters (subtitle_bram, subtitle_overlay,
video_gpio, video_vtc, video_dma) are all cohesive, with no dead code and
consistent error returns.

---

## Consistency notes

- *Templates are being followed.* Real modules match the section ordering / guards in
  src/utils/template and src/utils/template_qpc_AO. This is a meaningful win for a
  largely AI-authored tree — the structure has not drifted.
- *Error contract is consistent:* 0 on success, negative -Exxx on failure, almost
  everywhere. The main exception is Xilinx-wrapped HAL functions that surface
  XST_* semantics; consider normalizing those to -Exxx at the HAL boundary so
  callers see one convention. (The errorno.h enum-vs-<errno.h> collision itself is
  a C-quality issue already captured in the other report.)
- *Logging:* services and AOs use the log utility consistently; a few HAL modules
  fall back to fprintf for errors. Routing those through the same LOG_* macros (or
  returning the code and letting the caller log) would keep logging module-scoped.

---

## Prioritized recommendations

| # | Action | Layer | Severity | Why |
|---|--------|-------|----------|-----|
| 1 | Split usb_audio_stream.c into transport / queue / orchestration | svc | High (tidiness) | 849-line multi-responsibility file; best refactor ROI |
| 2 | Split stt_event_rx.c; extract reusable JSON parser | svc | Medium | 677 lines, hand-rolled parser repetition |
| 3 | Lift clock solver + tables out of video_dynclk.c into a svc clock-config module | hal→svc | Medium | Policy leaked into HAL |
| 4 | Remove the svc → hal include in subtitle_text_renderer.c; pass geometry in or define render geometry independently | svc | Low | Layering inversion |
| 5 | Move ALSA negotiation/recovery/gain policy out of usb_audio_capture.c | hal→svc | Low | Policy in HAL (self-contained, lower urgency) |
| 6 | Document the shared event-pool peak-load assumption near its definition | app | Low | Future AOs could silently overcommit 64 slots |
| 7 | Normalize HAL error returns (XST_* → -Exxx) and route HAL errors through LOG_* | hal | Low | One error/logging convention |
| 8 | (Optional) Convert init-state setup to an async start/READY handshake if init responsiveness ever matters | svc | Optional | Only if startup latency/retries become a problem |

### What is already good (keep doing it)

- SystemAO as a pure orchestrator with opaque handles and directed events.
- main()/app_init() split; pools, queue storage, priorities owned in app.c.
- Correct QTimeEvt lifecycle in every AO that uses a timer.
- Worker-thread offload of continuous blocking I/O (this is the part people usually get
  wrong, and it is right here).
- Consistent 0 / -Exxx error contract and template-conformant module structure.
- utils kept as a true dependency-free leaf layer.

---

## Closing note

There is no large-scale architectural rework needed. The active-object framework usage
is correct and the layering is sound. The cleanup work is concentrated in two oversized
service files and two HAL modules that absorbed policy they should have delegated.
Doing items 1–4 would remove essentially all of the "30 lines where 5 would do /
one file doing four jobs" feeling without disturbing the topology you have already
established.[3:17 PM]# C Code Quality Review

## Scope

This review compares the author-owned C code in subtitle_overlay_fw with the C quality practices used in ~/source/emu-mds/mpnpu-core. It intentionally does *not* review whether QP/C active objects are the right topology, and it does *not* treat Zephyr-vs-embedded-Linux differences as defects by themselves.

Reviewed areas:

- src/app/
- src/hal/
- src/svc/
- src/utils/
- linux/hdmi_vdma_client/
- representative unit tests under test/

Excluded from the main review:

- generated/vendor-style BSP code such as src/bsp/vtc_v7_2/
- third-party framework code
- topology-level QP/C design choices unless they create local C quality problems

## mpnpu-core baseline used for comparison

The useful mpnpu-core standards are language- and firmware-quality standards, not topology standards:

- SEI CERT-C mindset: avoid undefined behavior, unchecked truncation, sign-changing implicit conversions, unchecked pointer arithmetic, uninitialized data, and unchecked array bounds.
- No heap allocation in firmware-style code; use static or stack memory. Linux/POSIX boundary code may need OS APIs, but those boundaries should be explicit and isolated.
- Use fixed-width integer types for registers, protocol fields, wire formats, and hardware-facing state. Use size_t for object sizes and uintptr_t for raw addresses.
- Use explicit error contracts: 0 on success, negative errno-style values on failure, and do not silently convert failures into success.
- Keep module APIs small, cohesive, and documented.
- Public interfaces need useful Doxygen-style contracts: inputs, outputs, return values, blocking behavior, ownership, and initialization requirements.
- Hardware-facing structures and wire formats should have compile-time layout checks where layout matters.
- Logging should be module-scoped and consistent, not ad hoc printf/fprintf scattered through library code.
- Unit tests should cover error paths and boundary cases, not just happy paths.

## Executive assessment

The codebase already has some good C instincts: most HAL/service functions validate null pointers, there is a clear attempt to separate HAL, service, and app layers, the subtitle BRAM code uses uintptr_t/volatile appropriately for MMIO-like memory, and the repository has real unit tests.

The weak spots are the typical AI-generated ones: many files have template boilerplate, public headers expose implementation details, some modules rely on production assert(), parsing code misses CERT-C checks, OS-specific Linux/POSIX concerns bleed into generic service structures, and some error contracts are fragile or ambiguous. The highest-priority issue is not aesthetic: src/app/errorno.h defines enum members named EIO, EAGAIN, and EINVAL, which collide with standard <errno.h> macros depending on include order.

## Priority findings

### P0: Project-local error names collide with <errno.h>

*Examples*

- src/app/errorno.h defines EIO, EAGAIN, EINVAL, and ESTATE.
- src/svc/stt/stt_event_rx.c includes <errno.h> before "errorno.h".
- src/svc/usb_audio/usb_audio_stream.c includes "errorno.h" and later <errno.h>, making correctness include-order dependent.

This is a C correctness problem, not a style problem. Standard errno names are commonly macros. If <errno.h> is included before errorno.h, the enum declaration can be preprocessed into invalid C.

*Recommendation*

Rename the project-local enum values to a project namespace and keep return values negative:

c
typedef enum
{
    APP_ERR_IO     = 5,
    APP_ERR_AGAIN  = 11,
    APP_ERR_INVAL  = 22,
    APP_ERR_STATE  = 32,
} app_errorno_e;

Then return -APP_ERR_INVAL, or better, use the platform <errno.h> values directly for POSIX/Linux code and reserve project-specific names only for project-specific failures.

### P1: Numeric parsing is not CERT-C quality

*Examples*

- src/svc/stt/stt_event_rx.c parses JSON integers with strtoul() and only checks end == cursor.
- src/svc/stt/stt_event_rx.c converts unsigned long to uint32_t without checking range.
- src/svc/stt/stt_event_rx.c and src/svc/usb_audio/usb_audio_stream.c parse environment ports with strtoul(env_port, NULL, 10), losing conversion diagnostics.

This is exactly the kind of thing mpnpu-core tries to avoid: unchecked range conversion and success-shaped defaults from invalid input.

*Recommendation*

Create one shared helper for decimal uint32_t parsing:

- set errno = 0 before strtoul
- require end != input
- reject trailing non-space/non-terminator characters
- reject errno == ERANGE
- reject values greater than UINT32_MAX
- reject zero where the caller needs a nonzero port

Then reuse it in STT JSON parsing and environment config parsing.

### P1: ring_buffer uses assert() as its runtime contract

*Examples*

- src/utils/ring_buffer/ring_buffer.c uses assert() for null pointers and capacity validation.
- src/utils/ring_buffer/ring_buffer.h says capacity should be a power of two, but ring_buffer_init() does not enforce it.
- Several APIs return -1 instead of the project error contract.
- There are API/documentation quality issues such as lenght misspellings and stale comments.

Assertions disappear under NDEBUG, so invalid inputs become unchecked undefined behavior in production builds. In mpnpu-core style, public utility APIs should validate inputs and return explicit errors.

*Recommendation*

Refactor the ring buffer as a normal firmware utility:

- return int from init/reset/write APIs that can fail
- use -APP_ERR_INVAL/-EINVAL style values instead of -1
- enforce capacity assumptions or remove the power-of-two claim
- make read-only inputs uint8_t const *
- use consistent spelling (length)
- test invalid arguments, empty reads, full overwrite behavior, and boundary lengths

### P1: Linux/POSIX implementation details are exposed through public service structs

*Examples*

- src/svc/usb_audio/usb_audio_stream.h exposes pthread_mutex_t, pthread_cond_t, pthread_t, and int sender_fd.
- src/svc/stt/stt_event_rx.h exposes int server_fd and int client_fd.
- src/hal/video_dma/video_dma.h exposes the Linux file descriptor and mapped framebuffer internals.

For embedded Linux code, POSIX file descriptors and pthreads are appropriate implementation details. The quality issue is that public headers make these details part of the service ABI and blur whether the module is firmware-generic, Linux-only, or test-only.

*Recommendation*

Make Linux boundaries explicit:

- either document these modules as Linux-only boundary modules in the header and directory structure, or hide OS details behind opaque structs
- keep public config/event structs portable and stable
- keep file descriptors, pthread objects, and socket state private to .c files where possible
- name modules/directories so firmware-generic code cannot accidentally depend on POSIX internals

### P1: Thread and resource API failures are often unchecked

*Examples*

- queue_init() in src/svc/usb_audio/usb_audio_stream.c ignores pthread_mutex_init() and pthread_cond_init() return values.
- now_ns() ignores clock_gettime() failure.
- cleanup paths intentionally ignore ioctl(), munmap(), and close() results in src/hal/video_dma/video_dma.c, but the policy is not documented.

Cleanup functions sometimes must continue after failures, but initialization must not proceed with partially initialized synchronization primitives. mpnpu-core code tends to make failure policy explicit and analyzable.

*Recommendation*

- Return int from init helpers that call fallible OS APIs.
- Roll back partially initialized resources in reverse order.
- For cleanup, document when ignored return values are intentional and log important failures where useful.
- Consider small helpers for "best-effort cleanup" so ignored failures are centralized instead of scattered.

### P2: Wire-format and hardware-layout code needs stronger compile-time checks

*Examples*

- src/svc/usb_audio/usb_audio_stream.c builds stream headers from local arrays without static assertions tying constants to actual array sizes.
- send_stream_header() copies USB_AUDIO_STREAM_HEADER_MAGIC using strlen() even though the destination size is a fixed protocol field.
- HAL register packing macros in src/hal/subtitle_overlay/subtitle_overlay.c validate 16-bit bounds at runtime but do not pair the register layout with named masks/shifts in a way that is easy to audit.

mpnpu-core uses _Static_assert, fixed-width fields, alignment checks, and explicit layout assumptions where hardware or wire format matters.

*Recommendation*

- Use _Static_assert(sizeof(USB_AUDIO_STREAM_HEADER_MAGIC) <= USB_AUDIO_STREAM_HEADER_MAGIC_BYTES, ...).
- Copy a bounded compile-time count rather than strlen() for fixed protocol fields.
- Prefer named SHIFT, MASK, and MAX macros for packed registers.
- Add compile-time checks for protocol header word counts and payload sizing.

### P2: The STT JSON parser is a fragile ad hoc parser

*Examples*

- json_value() searches for "key" anywhere in the line, then finds the next colon.
- Numeric parsing does not validate token terminators.
- String parsing handles only a small subset of escapes and intentionally converts unsupported escapes to spaces.

This may be acceptable for a tightly controlled NDJSON test interface, but the contract needs to say that. Without that contract, it reads like a general JSON parser while only implementing a narrow subset.

*Recommendation*

Either:

- document it as a narrow, trusted, top-level NDJSON parser and harden its token validation, or
- replace it with a small explicit tokenizer/parser if the input is not fully controlled.

Add tests for:

- integer overflow
- trailing garbage after numbers
- duplicate keys
- keys appearing inside string values
- malformed escapes
- oversized lines and recovery after discard

### P2: Public API contracts are inconsistent

*Examples*

- Many headers expose bare prototypes without useful contract documentation, e.g. src/hal/video_dma/video_dma.h, src/svc/stt/stt_event_rx.h, and src/svc/usb_audio/usb_audio_stream.h.
- AO globals and constructor functions do not consistently document ownership, event-only access rules, blocking behavior, and initialization order.
- Some implementation comments use @param None, which is not useful Doxygen style.

The .c files often have Doxygen blocks, which is good, but consumers read headers first. Header contracts should state the rules that callers must obey.

*Recommendation*

For every public function or public object, document:

- whether it may block
- required initialization state
- ownership and lifetime of buffers
- whether pointers may be null
- whether the function is safe from an AO event handler
- precise success/failure return values

### P2: Error handling is not consistently "must check"
[3:17 PM]*Examples*

- Most functions return int, but there is no warn_unused_result/__must_check equivalent for critical APIs.
- Return-value ignoring is done with (void) casts in several places, but there is no distinction between "safe to ignore" and "must handle".

mpnpu-core uses __must_check in important headers. That is useful because C APIs otherwise make it easy to accidentally ignore failures.

*Recommendation*

Add a small portability macro, for example:

c
#if defined(__GNUC__) || defined(__clang__)
#define APP_MUST_CHECK __attribute__((warn_unused_result))
#else
#define APP_MUST_CHECK
#endif

Apply it to init/configure/write/parse APIs where ignored failures are almost always bugs.

### P3: Boilerplate and generated-looking comments reduce trust

*Examples*

- Many files contain Some fancy copyright message here (if needed).
- src/utils/ring_buffer/ still has <Your Name> in the copyright block.
- Files contain large empty section banners such as "Private variable declarations" when there are no declarations.
- Comments sometimes narrate obvious code rather than explaining constraints.

This does not usually break code, but it makes the code look unreviewed and AI-generated. It also hides the comments that actually matter.

*Recommendation*

- Replace placeholder copyright text with real project-standard headers.
- Remove empty banner sections or keep only useful sectioning.
- Keep comments focused on contracts, hardware assumptions, wire formats, and non-obvious behavior.
- Prefer concise Doxygen over decorative headers.

### P3: TODO placeholders are still in runtime paths

*Examples*

- src/app/app.c has a bsp_init_placeholder() TODO.
- src/app/app.c has TODOs for future ButtonsAO and LEDAO.
- src/svc/system/SystemAO.c has TODOs in on_run() and on_error().

TODOs are fine during prototyping, but production firmware-style code should make the intended behavior explicit. A TODO in an error path is especially risky because it can become silent failure.

*Recommendation*

Replace TODOs with one of:

- implemented behavior
- a tracked issue ID and explicit temporary behavior
- a log message and safe fallback
- a compile-time feature flag if the subsystem is intentionally absent

## Positive patterns worth keeping

- src/hal/subtitle_bram/subtitle_bram.c uses uintptr_t and volatile uint32_t * for hardware-facing memory access.
- subtitle_bram_write_bitmap() checks source buffer size against computed stride and height before reading.
- HAL/service code generally validates null pointers and initialization state.
- The codebase has a real unit-test structure under test/, including STT parsing tests and HAL/service tests.
- Module boundaries are generally understandable: app wiring, HAL adapters, services, and utilities are separate directories.

These are good foundations. The cleanup should preserve them while tightening contracts and removing AI-generated rough edges.

## Suggested cleanup roadmap

1. Fix the errorno.h namespace collision first. It can create compile failures based solely on include order.
2. Add shared checked parsing helpers and replace direct strtoul()/strtod() usage where values are stored in fixed-width fields.
3. Refactor ring_buffer into a production-safe utility with explicit error returns and boundary tests.
4. Decide which modules are Linux-only and make that explicit through opaque types, directory naming, or header comments.
5. Add APP_MUST_CHECK and apply it to critical APIs.
6. Add _Static_assert checks around protocol and hardware layout assumptions.
7. Clean placeholder comments, stale TODOs, typos, and decorative empty sections.
8. Expand tests around error paths: invalid config, conversion overflow, malformed STT lines, thread init failures where mockable, ring-buffer invalid inputs, and boundary sizes.

## Review checklist for future C changes

Use this checklist during future reviews:

- Does every public function have a clear contract?
- Are all hardware/protocol fields fixed-width?
- Are all size calculations checked for overflow before indexing/copying?
- Are string-to-number conversions fully checked?
- Are OS-specific handles hidden or explicitly documented as boundary types?
- Are fallible init steps checked and rolled back?
- Are ignored cleanup errors intentional and documented?
- Are wire/register layouts protected by masks, shifts, and static assertions?
- Are tests covering failure paths and boundaries?
- Is the code free of placeholders, decorative boilerplate, and comments that only restate the code?