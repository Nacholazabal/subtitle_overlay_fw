# Tracker de remediación — Mega review de `src/`

**Review origen:** [`docs/reviews/src-mega-review-2026-07-15.md`](./src-mega-review-2026-07-15.md)
**Rama de trabajo:** `dev/review` (== `main` en contenido; confirmado con el usuario — el prompt mencionaba `dev/VAD-improvements`, ya mergeada vía PR #12).
**Inicio:** 2026-07-18

## Estados posibles
- `pendiente` — sin empezar
- `en progreso` — en trabajo activo
- `corregido` — cambio implementado + evidencia (archivos, tests, explicación)
- `no aplica` — verificado que el hallazgo ya no aplica / la corrección causaría regresión; con evidencia
- `bloqueado por verificación externa` — requiere inspección de HW/VM/board o decisión de producto

## Baseline (Fase 0)
| Validación | Resultado | Fecha |
|---|---|---|
| Rama | `dev/review` @ `62aaa04` (worktree limpio) | 2026-07-18 |
| `make test` | 172 tests, 0 failures, 0 ignored | 2026-07-18 |
| `make clang-tidy` | exit 0 (solo warnings suprimidos en non-user code) | 2026-07-18 |
| `./scripts/build.sh` (VM) | ✅ exit 0 — binario ARM linkea limpio con los cambios de C02+M04, sin warnings `-Wall -Wextra` (VM encendida). Reejecutar tras cada grupo con cambios en `src/`. | 2026-07-18 |

## Tracker de hallazgos

### Críticos
| ID | Título | Estado | Evidencia / Notas |
|---|---|---|---|
| SRC-C01 | Carrera de arranque + data race del ticker POSIX-QV | bloqueado por verificación externa | **El defecto está DENTRO del port vendorizado `src/qpc/ports/posix-qv/qf_port.c` (QP/C 8.1.4 upstream).** El usuario definió QP/C como módulo externo inmutable: no se modifica `src/qpc`. El patch está diseñado y probado (revertido del vendor), pero requiere decisión de dónde vivir. **Opciones a discutir:** (a) *owned port* — copiar el port posix-qv (2 archivos) a nuestro árbol (p.ej. `src/bsp/qpc_port/`) y apuntar build ahí, dejando `src/qpc` pristino (patrón sancionado por QP/C: los ports son adaptables por el proyecto); (b) actualizar el gitlink a un QP/C con el fix upstream; (c) aceptar como riesgo residual documentado (la carrera es teórica: en 15/15 corridas el ticker entrega ticks). **Test de regresión ya agregado en nuestro árbol:** `test/integration/qpc/test_posix_qv_ticker.c` arranca el ticker real y verifica ticks + shutdown repetido (pasa 15/15 contra el port pristino). Naturaleza del fix necesario: publicar `l_isRunning=true` antes de crear el ticker, acceso atómico (`__atomic_*`, gnu99-safe) y ticker joinable con `pthread_join` en shutdown. |
| SRC-C02 | VideoAO invalida MMIO que SubtitleAO sigue usando | corregido | Ownership por **reference-count** en `hw_platform` (`src/bsp/platform/linux/hw_platform.c/.h`): `hw_platform_init` mapea solo en el primer acquire y toma referencia después; `hw_platform_cleanup` libera y **solo el último release** hace `munmap`+`close` (release sin referencias = no-op idempotente; falla de init hace teardown crudo con refcount aún en 0). `subtitle_pipeline` ahora **adquiere** la plataforma en init (nuevo flag `platform_ready`) y la **libera** en cleanup, con release en toda ruta de falla (helper `configure_hardware`). Así el cleanup de VideoAO ante error deja `refcount=1` y NO desmapea el overlay/BRAM que usa SubtitleAO. Tests nuevos: `test/bsp/platform/test_hw_platform.c` (7 casos: acquire/segundo-acquire/release-con-refs/último-release/underflow/falla-open/falla-map-parcial, syscalls falsas por macro-redirect) y `test/svc/subtitle_pipeline/test_subtitle_pipeline.c` (acquire/release, falla de acquire, release en cleanup no-inicializado). 183 tests OK, clang-tidy exit 0. **Pendiente relacionado:** quiesce/stop global coordinado antes de liberar MMIO → SRC-H07 (Fase 2); rollback de overlay/BRAM parcial → SRC-M03 (Fase 4). **Build ARM pendiente** (VM caída). |

### Altos
| ID | Título | Estado | Evidencia / Notas |
|---|---|---|---|
| SRC-H01 | Operaciones bloqueantes dentro del scheduler QV | pendiente | — |
| SRC-H02 | Triple buffering declarado pero no implementado | pendiente | — |
| SRC-H03 | Adquisición de timing / modo no soportado sin timeout | corregido (parcial) | `video_pipeline_poll` (`src/svc/video_pipeline/video_pipeline.c`) ahora acota `ACQUIRING_TIMING`: si `XST_NO_DATA` persiste `>= VIDEO_PIPELINE_TIMING_TIMEOUT_MS` (2s), reinicia el detector y vuelve a `WAITING_FOR_SIGNAL` (nuevo resultado `VIDEO_PIPELINE_POLL_TIMING_TIMEOUT`, backoff acotado). Se agregó uso real de `detector_started_ms` vía `video_input_detector_elapsed_ms` + `video_input_reset_detector` (`video_io`). Tests: `test_video_pipeline` (timeout reinicia detector) + `test_video_io` (elapsed/reset, incl. rollover uint32). Build ARM OK. **Restante:** revalidación periódica de timing en `UNSUPPORTED_INPUT`/`STREAMING` con lock alto → se trata en SRC-M12. |
| SRC-H04 | Eventos ready/error pueden perderse | pendiente | — |
| SRC-H05 | Conflicto de tamaño de la BRAM de subtítulos | pendiente | — |
| SRC-H06 | Geometría del overlay puede exceder pantalla y máscara | pendiente | — |
| SRC-H07 | No existe estrategia global de shutdown/error | pendiente | — |

### Medios
| ID | Título | Estado | Evidencia / Notas |
|---|---|---|---|
| SRC-M01 | NULL dereference en SttAO | corregido | `on_transcript` (`src/svc/stt/SttAO.c`) ahora valida `me`/`e`/`e->text[0]` **antes** de leer `e->is_final` (que se usaba para el margin antes del check). Reordenado; sin cambio de comportamiento para entradas válidas. Build ARM OK. |
| SRC-M02 | Marcador DONE permanente | corregido | `on_component_init` (`src/svc/subtitle_pipeline/SubtitleAO.c`) ahora arma `clear_time_evt` al terminar init exitoso, así el marcador DONE se limpia tras el timeout de inactividad aunque nunca llegue STT. Test nuevo `test_subtitle_startup_marker_clears_after_inactivity_timeout` (avanza 100 ticks → dispara `SUBTITLE_CLEAR_SIG` → overlay disabled). Build ARM OK. |
| SRC-M03 | Rollback incompleto en init de subtitle | pendiente | — |
| SRC-M04 | Invariantes débiles / duplicados en startup | corregido (parcial) | La rama `COMPONENT_VIDEO` de `on_component_ready` (`src/svc/system/SystemAO.c`) ahora guarda el posteo de subtitle init con `subtitle_init_requested == 0U` (simétrico con la rama USB), volviendo idempotente un *video ready* duplicado. Test nuevo `test_system_ignores_duplicate_video_ready_after_subtitle_requested` en `test/integration/qpc/test_system_ao_startup.c`. **Restante menor:** `last_ready_component` solo se asigna y nunca se consume → se resuelve en SRC-L01 (Fase 5); COMPONENT_STT no valida explícitamente readiness previo (bajo riesgo, el flujo lo garantiza por orden). |
| SRC-M05 | video_dma_init anuncia frames que el kernel no tiene | corregido | `video_dma_init` (`src/hal/video_dma/video_dma.c`) ahora exige `info.frame_count >= frame_count` (falla si el kernel expone menos) y mapea con índice directo `i` en vez de `i % info.frame_count` (que aliaseaba buffers). Test nuevo `test/hal/video_dma/test_video_dma.c` (3 casos, syscalls falsas incl. `ioctl` variádico). Build ARM OK. |
| SRC-M06 | ALSA recovery y parada de workers pueden bloquear | pendiente | — |
| SRC-M07 | Riesgos JSON/UTF-8 en STT | pendiente | — |
| SRC-M08 | Dynclk acepta no-finitos y hace busy-wait | corregido (parcial) | `video_dynclk_configure` (`src/hal/video_dynclk/video_dynclk.c`) ahora rechaza `!isfinite` (NaN/Inf pasaban el `<=0`), fuera de rango (`> 300 MHz`), y aplica tolerancia máxima de error de síntesis (10 MHz, holgada para no rechazar modos reales). Test nuevo `test/hal/video_dynclk/test_video_dynclk.c` (validación pre-MMIO). Build ARM OK. **Restante:** el busy-wait de lock (≤10ms, ya con timeout acotado `CLK_TIMEOUT_NS`) sigue en el handler; convertirlo a polling por timer/worker es parte del refactor de blocking-en-QV (ligado a H01, diferido). |
| SRC-M09 | Asserts Xilinx que no hacen assert | corregido | `src/bsp/bsp_compat/xil_assert.h`: `Xil_AssertVoid`/`Xil_AssertNonvoid`/`*Always` ahora retornan temprano ante precondición falsa (como el Xilinx real no-fatal), en vez del no-op que dejaba al driver seguir y dereferenciar punteros inválidos. Los wrappers `video_vtc` ya validan antes, así que una llamada bien formada nunca los dispara. Test nuevo `test/bsp/bsp_compat/test_xil_assert.c`. Build ARM OK (driver VTC compila con los macros nuevos). |
| SRC-M10 | Convenciones de error incompatibles | pendiente | — |
| SRC-M11 | Gaps de tests en threads/MMIO/hardware boundary | pendiente | — |
| SRC-M12 | Cambios de modo con lock alto + validación VTC | pendiente | — |

### Bajos / deuda
| ID | Título | Estado | Evidencia / Notas |
|---|---|---|---|
| SRC-L01 | Estado muerto / sólo observabilidad | pendiente | — |
| SRC-L02 | APIs públicas sólo consumidas por tests o nadie | pendiente | — |
| SRC-L03 | Objetos VTC no usados enlazados | pendiente | — |
| SRC-L04 | Distribución QP/C completa como superficie no usada | pendiente | — |
| SRC-L05 | Config QP/C sobredimensionada / legacy | pendiente | — |
| SRC-L06 | Templates, comentarios y funciones largas desalineados | pendiente | — |

## Bitácora de cambios
_(se completa por grupo a medida que se avanza)_
