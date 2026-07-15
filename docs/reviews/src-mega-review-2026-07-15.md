# Mega review de `src/`

**Repositorio:** `subtitle_overlay_fw`  
**Fecha:** 2026-07-15  
**Alcance:** todo `src/`, con foco profundo en el código que entra al firmware Linux/QP/C. `scripts/`, CI, deployment y herramientas auxiliares quedan para una revisión posterior.  
**Resultado:** **no recomendaría considerar este firmware robusto todavía**. La base es razonable y está bastante testeada, pero hay 2 riesgos críticos, varios defectos de arquitectura/concurrencia y una cantidad apreciable de scaffolding que parece terminado sin estarlo.

## 1. Resumen ejecutivo

La revisión encontró:

| Severidad | Cantidad | Interpretación |
|---|---:|---|
| Crítica | 2 | Puede romper todos los timers o dejar referencias MMIO inválidas |
| Alta | 7 | Puede congelar QV, degradar video, colgar el startup o violar el mapa de hardware |
| Media | 12 | Defectos reales o contratos frágiles que deben corregirse antes de endurecer el sistema |
| Baja / deuda | 6 | APIs, estado, templates y vendor footprint sin consumidor o desalineados |

Los cuatro problemas que atacaría primero son:

1. La carrera de arranque del ticker en el port POSIX-QV puede dejar **todos los `QTimeEvt` sin ticks**.
2. `VideoAO` puede desmontar MMIO global que `SubtitleAO` continúa usando.
3. `USBAudioAO` ejecuta inicialización y `pthread_join()` bloqueantes dentro del único hilo cooperativo QV.
4. Los tres framebuffers de video existen, pero captura y display trabajan siempre sobre el mismo índice; no hay triple buffering real y existe riesgo de tearing.

No encontré un repositorio “todo roto”. Hay varias decisiones buenas: eventos tipados, `Q_EVT_CAST`, colas y pools acotados, sockets no bloqueantes durante el polling, ownership bastante claro, límites de buffers mayormente cuidados y 172 tests que pasan. El problema es que los defectos más serios están en los bordes de integración que los mocks no ejercitan.

## 2. Qué se revisó y cómo

### 2.1 Inventario

| Categoría | Tamaño observado | Tratamiento de la review |
|---|---:|---|
| Código propio y compatibilidad | 62 archivos C/H, 10.806 líneas | Revisión manual módulo por módulo |
| Driver VTC generado por Xilinx | 7 archivos C/H, 4.854 líneas | Revisión de integración, wrappers y código que se enlaza |
| Distribución QP/C completa | 221 MiB, 5.245 archivos; 2.724 C/H y 278.163 líneas | Inventario completo; revisión profunda del subconjunto compilado y validación de checksums |

No tiene sentido atribuir como deuda propia cada ejemplo, BSP y third-party de la distribución upstream de QP/C. Por eso la conclusión sobre “todo `src/`” distingue:

- código alcanzable por el binario;
- código generado/vendorizado;
- material upstream no compilado que sólo agrega superficie de mantenimiento.

El subconjunto QP/C usado (`qep_hsm`, `qf_act`, `qf_actq`, `qf_dyn`, `qf_mem`, `qf_qact`, `qf_qeq`, `qf_time`, headers y port POSIX-QV) coincide con los SHA-1 de QP/C 8.1.4. Esto confirma que el defecto del ticker descrito abajo está en la copia upstream limpia, no en una modificación local.

### 2.2 Validaciones ejecutadas

| Validación | Resultado | Lectura correcta |
|---|---|---|
| `make test` | 172 tests, 0 fallos, 0 ignorados | Pasa con loopback habilitado |
| Cobertura | Generada con sockets loopback habilitados | Buena en servicios puros; muy baja o inexistente en threads/ALSA/MMIO real |
| `make clang-tidy` | Exit 0 | Sólo reporta ruido de casts MMIO; no detectó varios problemas manuales |
| `cppcheck 2.7` | Detectó un posible NULL dereference real en `SttAO.c` | Útil como segunda opinión; también produjo falsos positivos por macros QP/C |
| Build local `video-port-check`, ALSA off | 30 objetos, link correcto, sin warnings | No reemplaza el cross-build ARM con ALSA |
| Checksums QP/C usados | Todos OK contra 8.1.4 | Vendor no modificado en el subconjunto compilado |

Durante una ejecución dentro del sandbox fallaron 6 tests de STT porque el entorno prohibía `socket(AF_INET)`. Al repetir fuera de esa restricción, los 23 tests de `stt_event_rx` pasaron. No es un fallo del código. Sí se observó que Ceedling terminó con exit code 0 aun cuando Unity había informado fallos; eso pertenece a la futura review de tooling/CI.

## 3. Hallazgos críticos

### SRC-C01 — Carrera de arranque y data race en el ticker POSIX-QV

**Evidencia:** `src/qpc/ports/posix-qv/qf_port.c:58`, `:104-138`, `:199-253`, `:304-309`.

`l_isRunning` comienza en `false`. `QF_run()` crea el thread del ticker en `:220-231`, pero recién escribe `true` en `:248`. El thread entra directamente a `while (l_isRunning)` en `:121`. Si el ticker corre en la ventana entre su creación y la asignación, termina para siempre. El main sigue ejecutando normalmente, pero ningún `QTimeEvt` vuelve a vencer.

Además, `l_isRunning` se comparte entre threads como `bool` ordinario, sin atomic ni acceso consistente bajo el mismo mutex. `QF_stop()` también escribe sin lock. Formalmente es una data race de C. El thread es detached y `QF_run()` destruye mutex/condition variables sin hacer join.

**Impacto:** los polls de video, USB audio, STT y los timers de subtítulos pueden quedar congelados de forma no determinista. Es un fallo sistémico.

**Recomendación:** mantener un patch local documentado o actualizar a una versión upstream donde esté resuelto. Como mínimo, establecer el estado de ejecución antes de crear el ticker, usar sincronización definida y hacer el lifecycle joinable. Agregar un test de integración que ejecute `QF_run()` repetidamente y verifique ticks reales. El checksum limpio no vuelve seguro el port; sólo identifica su procedencia.

### SRC-C02 — `VideoAO` invalida MMIO que `SubtitleAO` sigue usando

**Evidencia:** `src/bsp/platform/linux/hw_platform.c:23-39`, `:63-110`; `src/svc/video_pipeline/video_pipeline.c:97-108`, `:134-150`; `src/svc/video_pipeline/VideoAO.c:199-215`; `src/svc/subtitle_pipeline/subtitle_pipeline.c:102-150`.

`hw_platform` mantiene un único conjunto global de mapeos: dynclk, VTC, overlay, BRAM y GPIO. El video es quien llama `hw_platform_init()`. Subtitle toma punteros a overlay/BRAM, pero no adquiere ownership ni referencia. Ante un error de video, `VideoAO` llama `video_pipeline_cleanup()`, que ejecuta `hw_platform_cleanup()` y hace `munmap()` de todas las regiones. `SubtitleAO` permanece activo con bases virtuales antiguas.

El error terminal de `SystemAO` tampoco detiene coordinadamente al resto de los AOs. Un texto o timer posterior puede escribir mediante un puntero MMIO desmontado.

**Impacto:** `SIGSEGV`/`SIGBUS`, escritura a una región incorrecta si la VA se reutiliza, o hardware en estado incoherente.

**Recomendación:** mover el ownership de plataforma al lifecycle de aplicación/system, o implementar acquire/release con reference count. Ningún servicio consumidor debe desmontar recursos globales. Definir una secuencia global de quiesce/stop antes de liberar MMIO.

## 4. Hallazgos de severidad alta

### SRC-H01 — Operaciones bloqueantes dentro del scheduler cooperativo QV

**Evidencia:** `src/svc/usb_audio/USBAudioAO.c:118-145`, `:171-180`; `src/svc/usb_audio/usb_audio_stream.h:99-107`; `src/svc/usb_audio/usb_audio_stream.c:890-1000`, `:1027-1046`; `src/hal/usb_audio/usb_audio_capture.c:314`.

`on_component_init()` llama directamente `usb_audio_stream_start()`. Su propio contrato admite que puede bloquear durante setup; abre/configura ALSA y crea threads. En error, el AO llama `usb_audio_stream_stop()`, que hace dos `pthread_join()`.

En POSIX-QV todos los AOs comparten un hilo cooperativo. Mientras estas llamadas bloquean, no corre ningún otro AO y tampoco se procesan eventos ya encolados. `stt_event_rx_init()` también ejecuta `getaddrinfo()` desde `SttAO`; con host configurado por nombre puede bloquear. Las esperas busy-loop de dynclk agregan hasta decenas de milisegundos al mismo problema.

**Recomendación:** convertir init/stop de recursos bloqueantes en workers con evento de completion, o completar el setup antes de entrar a `QF_run()`. Los handlers QV deben ser pasos cortos y acotados.

### SRC-H02 — Triple buffering declarado pero no implementado

**Evidencia:** `src/svc/video_pipeline/video_pipeline.c:52-75`, `:93-105`; `src/svc/video_pipeline/video_pipeline.h` (`VIDEO_PIPELINE_FRAME_COUNT` y `active_frame`).

Se mapean tres frames, pero `active_frame` se fija a cero y nunca cambia. MM2S y S2MM se inician sobre el mismo framebuffer. No existe política de producer/consumer, frame-ready ni swap en frontera vertical.

**Impacto:** captura y display pueden acceder simultáneamente al mismo buffer, causando tearing. Los otros dos buffers son scaffolding muerto y dan una falsa impresión de robustez.

**Recomendación:** decidir explícitamente entre single-buffer passthrough o triple buffering real. Si se implementa swap, respetar la frontera de frame/SOF; el ioctl `*_SELECT` por sí solo no garantiza una conmutación segura.

### SRC-H03 — Adquisición de timing y modo no soportado pueden quedar colgados indefinidamente

**Evidencia:** `src/svc/video_pipeline/video_io.c:79-106`; `src/svc/video_pipeline/video_pipeline.c:159-235`.

`detector_started_ms` se guarda “para una futura política de timeout” y nunca se lee. `XST_NO_DATA` deja el pipeline en `ACQUIRING_TIMING` para siempre. `UNSUPPORTED_INPUT` también queda inmóvil mientras el GPIO siga locked; si cambia la resolución sin una transición de lock visible entre polls, no reintenta.

**Impacto:** startup sin conclusión, o pipeline incapaz de recuperarse de un cambio de fuente. El test actual codifica esta inmovilidad en lugar de detectar recuperación.

**Recomendación:** timeout explícito, backoff/restart del detector y revalidación periódica del timing aun con lock alto.

### SRC-H04 — Eventos ready/error pueden perderse y dejar al sistema esperando para siempre

**Evidencia:** `post_ready()`/`post_error()` en `SttAO.c:72-117`, `USBAudioAO.c:64-110`, `VideoAO.c:67-116`, `SubtitleAO.c:113-157`; márgenes en `src/app/app.h:29-30`.

Cada helper retorna `void`. Si falla `Q_NEW_X` o `QACTIVE_POST_X`, sólo loguea. El AO puede quedar funcionando, pero `SystemAO` nunca recibe ready; o un fallo real nunca llega al coordinador. Los márgenes reservan capacidad, no garantizan entrega. Incluso los eventos finales STT se descartan explícitamente cuando pool/queue llegan al margen 1.

**Recomendación:** tratar control/error como flujo garantizado: pool/cola dedicados, evento estático cuando aplique, retry acotado o transición local a una condición observable. La documentación no debe prometer “always dispatched” con la implementación actual.

### SRC-H05 — Conflicto serio en el tamaño de la BRAM de subtítulos

**Evidencia:** `src/bsp/platform/xparameters_linux.h:39-40` define 32 KiB; `src/hal/subtitle_bram/subtitle_bram.h:26-30` usa máscara 1024×256 = 32 KiB. La documentación de entorno del repositorio describe 8 KiB/256×64.

Código y `xparameters_linux.h` son internamente consistentes, pero contradicen la documentación marcada como hardware validado. No se puede decidir sólo desde C cuál fuente está stale.

**Impacto si el hardware real es 8 KiB:** `subtitle_bram_clear()` y el renderer escriben cuatro veces el rango válido. Si el hardware es 32 KiB, la wiki está induciendo a futuras regresiones.

**Recomendación:** verificar contra el `.xsa`/Address Editor y la configuración real del BRAM controller. Generar este contrato desde una única fuente o agregar asserts de build y una prueba de smoke en board.

### SRC-H06 — Geometría del overlay puede exceder tanto la pantalla como la máscara

**Evidencia:** `src/svc/subtitle_pipeline/subtitle_pipeline.c:54-80`; máscara fija en `subtitle_bram.h:26-30`.

El bar toma al menos 1024×256. Por lo tanto, en 640×480 y 800×600 el overlay es más ancho que el display. Para 1920×1080, el ancho del 80% es 1536, mayor que la máscara de 1024. No hay validación contra display ni contrato visible de clipping/scaling del IP.

**Recomendación:** definir formalmente si el IP recorta, repite o escala la máscara. Clampear la geometría a capacidades físicas y display, o parametrizar renderer/BRAM. Validarlo en los tres modos soportados.

### SRC-H07 — No existe una estrategia global de shutdown/error

**Evidencia:** `src/app/app.c:132-164`; `src/svc/system/SystemAO.c:275-287`, estado terminal alrededor de `:395`; cleanup local en cada AO.

`QF_onCleanup()` está vacío. SIGINT en el port QP/C llama cleanup y `exit()` directamente desde el signal handler; `exit()` y una limpieza general no son operaciones async-signal-safe. Ante error de un componente, System entra a un estado terminal pero no detiene USB threads, sockets, DMA, VTC, overlay ni los otros AOs.

**Recomendación:** diseñar señales `STOP/STOPPED`, orden de quiesce y timeout de shutdown. System debería coordinar la parada; el callback final sólo libera lo que ya fue detenido de forma segura.

## 5. Hallazgos de severidad media

### SRC-M01 — Dereference antes del check de NULL en `SttAO`

**Evidencia:** `src/svc/stt/SttAO.c:188-199`.

`e->is_final` se lee al calcular `margin` y recién después se comprueba `e == NULL`. Cppcheck también lo detectó. El caller actual pasa elementos válidos de un array, por lo que no parece alcanzable hoy, pero la función contiene un contrato falso y es una regresión esperando ocurrir.

**Recomendación:** validar `me`/`e` antes de cualquier lectura y agregar test directo o extraer el helper a una unidad testeable.

### SRC-M02 — El marcador “temporal” `DONE` puede permanecer para siempre

**Evidencia:** `src/svc/subtitle_pipeline/SubtitleAO.c:160-240`, `:408-410`.

El init dibuja y habilita `DONE`, pero no arma un timer para borrarlo. El timer de clear se rearma sólo después de recibir texto. Si nunca llega STT, el marcador queda indefinidamente.

**Recomendación:** armar un timeout específico al terminar init o no publicar un marcador de diagnóstico en el flujo de producción.

### SRC-M03 — Rollback incompleto durante init de subtitle

**Evidencia:** `src/svc/subtitle_pipeline/subtitle_pipeline.c:102-170`.

`initialized` se marca sólo al final. Si configure, clear o disable fallan después de escrituras parciales, cleanup no deshabilita overlay porque exige `initialized != 0`. Puede quedar hardware parcialmente configurado.

También `SubtitleAO::clear_subtitle()` descarta los errores de clear/disable (`SubtitleAO.c:267-273`): el estado lógico se borra aunque el overlay físico pueda seguir mostrando contenido viejo.

**Recomendación:** usar fases/flags de adquisición y un único bloque de rollback que deshaga cada etapa completada.

### SRC-M04 — Invariantes débiles y duplicados en la secuencia de startup

**Evidencia:** `src/svc/system/SystemAO.c:156-266`.

La rama `COMPONENT_VIDEO` no consulta `subtitle_init_requested`; un ready duplicado puede enviar un segundo init. `COMPONENT_STT` lleva a éxito sin validar explícitamente que video/USB/subtitle estén listos. `last_ready_component` sólo se asigna y nunca se consume.

**Recomendación:** modelar readiness como bitmask/invariantes idempotentes y rechazar eventos fuera de orden. Agregar tests de duplicados y permutations adversas.

### SRC-M05 — `video_dma_init()` puede anunciar frames que el kernel no tiene

**Evidencia:** `src/hal/video_dma/video_dma.c:175-243`.

Si el driver informa menos frames que los solicitados, el init no falla: mapea usando `i % info.frame_count` y deja `dma->frame_count` con el número solicitado. Eso puede duplicar mappings y hacer que un select/config posterior use un índice rechazado por kernel.

**Recomendación:** exigir `info.frame_count >= requested`, o exponer exactamente el count real y adaptar la política superior.

### SRC-M06 — Recuperación ALSA y parada de workers pueden bloquear indefinidamente

**Evidencia:** `src/hal/usb_audio/usb_audio_capture.c:144-151`; `src/svc/usb_audio/usb_audio_stream.c:1027-1046`.

Ante `-ESTRPIPE`, `snd_pcm_resume()` se repite mientras devuelve `-EAGAIN`, sin sleep ni consulta de stop. Un worker puede hacer busy-spin y el `pthread_join()` del AO queda esperando. La condition variable usa espera temporal pero `CLOCK_REALTIME`; saltos de reloj afectan latencia. El broadcast se hace sin el mutex de queue.

El sender extrae el chunk antes de enviarlo; si `send_chunk()` falla, ese audio se pierde pero no incrementa `total_dropped`. Además, AO y sender comparten la responsabilidad de cerrar `sender_fd`, lo que merece un protocolo de ownership más estricto para evitar carreras de close/reuse. Los overrides string de environment se truncan silenciosamente con `snprintf()`.

**Recomendación:** retry acotado con sleep/cancel, condición monotónica y protocolo de stop bajo el mismo mutex que protege el predicado.

### SRC-M07 — Riesgos de interoperabilidad JSON/UTF-8 en STT

**Evidencia:** parser en `src/svc/stt/stt_event_rx.c`, especialmente `json_parse_string()` y truncado de `text`.

El parser rechaza `\uXXXX`. Un emisor Python con `json.dumps()` por defecto escapa caracteres españoles y sería rechazado, salvo que use `ensure_ascii=False`. El truncado de texto puede cortar un code point UTF-8; el sanitizer lo reemplaza después, perdiendo caracteres.

La secuencia se marca como consumida al parsear, antes de saber si el evento llegó a Subtitle. Por diseño, un evento descartado por pool/queue no puede reenviarse con el mismo `seq` durante esa sesión: será `rejected_old_seq`. Es válido sólo si el protocolo de ACK documenta que esos drops son definitivos. La lectura byte a byte también puede costar hasta 512 syscalls por poll; está acotada, pero es innecesariamente cara.

**Recomendación:** fijar el protocolo en una spec y testear ambos lados. O implementar escapes Unicode correctamente, o exigir UTF-8 literal y validarlo al recibir. Truncar en frontera de code point.

### SRC-M08 — Dynclk acepta valores no finitos y hace busy-wait en QV

**Evidencia:** `src/hal/video_dynclk/video_dynclk.c`, búsqueda/configuración y waits alrededor de `:302-324`, `:351-379`, asignación en `:414`.

Se valida frecuencia `> 0`, pero no `isfinite()` ni rango antes de conversiones a enteros. NaN puede atravesar comparaciones y conducir a conversión indefinida. No se impone tolerancia máxima al error de frecuencia. Stop/start realizan loops activos de hasta ~10 ms cada uno dentro del handler cooperativo.

**Recomendación:** validar finitud/rango/tolerancia y convertir el lock wait en polling por timer o worker.

### SRC-M09 — Los asserts de compatibilidad Xilinx no hacen assert

**Evidencia:** `src/bsp/bsp_compat/xil_assert.h:11-14` y uso extendido en el driver VTC generado.

Las macros sólo evalúan la expresión y continúan. El driver generado confía en `Xil_AssertVoid`/`Xil_AssertNonvoid` antes de dereferenciar. Los wrappers propios validan varios caminos comunes, pero la API pública vendorizada queda con una protección engañosa.

**Recomendación:** implementar compatibilidad que falle de forma definida o encapsular/ocultar por completo la API Xilinx detrás de `video_vtc`.

### SRC-M10 — Convenciones de error incompatibles entre módulos

Audio/subtitle retornan `0` o errno negativo; video mezcla `XST_*` positivos, `hw_platform` retorna `-1` y `VideoAO` colapsa causas a `-EIO`. La convención del proyecto indica errno negativo para HAL.

**Impacto:** diagnósticos pobres y comparaciones fáciles de escribir al revés.

**Recomendación:** traducir `XST_*` una sola vez en el wrapper y exponer un contrato uniforme hacia servicios/AOs.

### SRC-M11 — Los tests fuertes terminan justo antes del hardware y los threads reales

`video_io` y `video_pipeline` muestran 100% de líneas, pero `video_dma`, `video_dynclk`, `video_gpio`, `video_vtc`, `hw_platform`, `VideoAO` y `USBAudioAO` no forman parte real de la cobertura de producción; se mockean. `usb_audio_stream.c` tiene sólo 18,83% de líneas y `usb_audio_capture.c` se prueba casi exclusivamente con ALSA deshabilitado. `SystemAO` está en 76,92%.

**Recomendación:** tests de wrapper ioctl/MMIO con backend inyectable, tests reales de lifecycle de workers y un test end-to-end QF con ticker real. No usar el 100% de pipeline como proxy de cobertura de video.

### SRC-M12 — Cambios de modo con lock alto no se detectan y falta validar timing VTC

Mientras está `STREAMING`, el pipeline sólo mira el GPIO lock y no vuelve a leer timing. Un cambio de resolución que mantenga lock conserva stride/VTC/modo viejo. El wrapper VTC tampoco valida relaciones como sync/porches/total antes de restas unsigned; hoy los modos estáticos son válidos, pero la API acepta estructuras arbitrarias.

El output se arranca antes que la captura, por lo que puede mostrar inicialmente memoria vieja. Los helpers `video_input_stop()`/`video_output_stop()` descartan fallos de stop del HAL y retornan éxito, ocultando cleanup incompleto.

**Recomendación:** revalidación periódica o evento de cambio y validadores completos antes de programar registros.

## 6. Hallazgos de baja severidad y código stale

### SRC-L01 — Estado sin efecto, duplicado o sólo de observabilidad

Se encontraron, entre otros:

- `SystemAO.last_ready_component`;
- `video_input_t.detector_started_ms`;
- `video_input_t.frame_index` y `video_output_t.frame_index`;
- `SubtitleAO.running`, escrito pero no consultado (los demás AOs sí usan su flag en cleanup/poll);
- `stt_event_rx_t.client_connected`, actualizado pero no usado para gobernar el código de producción;
- `usb_audio_queue_t.dropped` duplicando `total_dropped`;
- `video_dynclk_t.actual_frequency_mhz`;
- `video_pipeline_t.active_frame`, que se consulta pero nunca cambia de cero.

Algunos pueden ser observabilidad futura, pero hoy inflan el modelo mental y simulan funcionalidades inexistentes. Borrarlos o convertirlos en invariantes realmente consultadas.

### SRC-L02 — APIs públicas consumidas sólo por tests o por nadie

| API | Uso de producción observado |
|---|---|
| `video_dma_status` | Ninguno |
| `video_gpio_set_hpd` | Ninguno |
| `video_modes_default`, `video_modes_all` | Sólo tests; producción usa `video_modes_find` |
| `subtitle_bram_set_pixel`, `clear_pixel` | Sólo tests |
| `subtitle_pipeline_write_text`, `commit`, `clear_sof`, `poll_sof` | Sólo tests |
| `subtitle_text_renderer_render` | Sólo tests; producción usa `render_caption` |
| `video_pipeline_get_state` | Sólo tests |
| `log_unsubscribe` | Sólo tests |

No toda API de test es basura, pero debería clasificarse como debug/test, hacerse privada o eliminarse. La exposición actual hace más difícil detectar qué contrato es realmente estable.

### SRC-L03 — Se enlazan partes VTC que la aplicación no usa

El Makefile compila `xvtc_intr.c` (597 bytes de text en build x86) y `xvtc_selftest.c` (144 bytes), aunque el diseño Linux userspace no usa interrupciones VTC ni invoca self-test. `xvtc.c` aporta ~12,7 KiB de text y se enlaza como objeto completo. Sin `-ffunction-sections`/`--gc-sections`, gran parte de la API vendor queda en el binario.

**Recomendación:** compilar sólo fuentes necesarias o habilitar garbage collection de secciones verificando el map file ARM.

### SRC-L04 — La distribución QP/C completa es superficie de repositorio no usada

`src/qpc` contiene 221 MiB: ports para múltiples RTOS/MCUs, examples y enormes third-party STM32/ThreadX/FreeRTOS que no participan del firmware. No afectan el binario actual, pero perjudican búsquedas, scanners, onboarding y supply-chain review.

**Recomendación:** vendoring mínimo reproducible, submodule/tag inmutable o script de import que conserve únicamente include/core/port usado y licencia. Si se mantiene el árbol completo, excluirlo explícitamente de análisis que pretenda medir código propio.

### SRC-L05 — Configuración QP/C sobredimensionada y legacy

`src/qpc/ports/config/qp_config.h` usa `QF_MAX_ACTIVE=32` para 5 AOs, `QF_MAX_EPOOL=3` para 1 pool y `QP_API_VERSION=0`, habilitando compatibilidad histórica máxima. No es un bug funcional, pero incrementa superficie y oculta dependencias legacy.

**Recomendación:** reducir a necesidades reales y usar una API version explícita después de verificar build/tests.

### SRC-L06 — Templates, comentarios y tamaño de funciones desalineados

- `src/app/app.c:60-67` contiene `bsp_init_placeholder()` vacío.
- Signals/componentes de buttons/LED y TODOs existen sin servicio real.
- `template_qpc_AO` recomienda colocar constructor/punteros en `app.h` y usa `template_ao_t`, contradiciendo las convenciones actuales y los módulos reales.
- `log.h` habla de UART aunque el backend actual es stdout.
- Macros de logging deshabilitado expanden a `{}` en vez de `do { } while (0)`.
- Varias funciones exceden el objetivo de 60 líneas: `stt_event_rx_parse_line` (~193), `usb_audio_stream_start` (~111), `subtitle_text_sanitize` (~98), workers de USB (~80), `SystemAO::on_component_ready` (~111), entre otras.
- Cada render reserva 32 KiB de bitmap en el stack del único hilo QV (`subtitle_pipeline.c:237`); es tolerable en Linux, pero innecesario y poco determinista.
- El renderer borra `dst_size` completo, no sólo el bitmap requerido, y la tabla de glyphs no cubre toda la puntuación ASCII que el sanitizer permite. Signos españoles como `¿`/`¡` se degradan a espacios.

Los templates stale son especialmente peligrosos porque reproducen deuda en cada módulo nuevo.

## 7. Revisión específica de QP/C

### Bien implementado

- Enum de señales plano y eventos tipados en `app.h`.
- Uso consistente de `Q_EVT_CAST` para eventos derivados.
- `QTimeEvt` construidos en los ctors, no en runtime tardío.
- Colas estáticas, pool estático y límites explícitos.
- Los AOs target se arrancan antes que `SystemAO`, evitando posts a AOs aún no registrados.
- Prioridades únicas y coherentes con el flujo de datos.
- No se usa `QActive_setAttr()`, que el port POSIX-QV prohíbe.
- El polling de STT limita bytes y eventos por ciclo, una buena práctica cooperativa.

### Incorrecto o frágil

- Carrera/data race del ticker del port.
- Handlers con ALSA, DNS, busy-wait y joins bloqueantes.
- Eventos de control no garantizados pese a ser esenciales para la state machine.
- Error global sin coordinación de parada.
- Tests QP/C usan harness y ejercitan dispatch, pero no arrancan el ticker/thread real; por eso no detectan SRC-C01.
- `QF_onStartup()` configura el tick rate después de crear el ticker. Hoy coincide con el default de 100 Hz, pero el orden es frágil y la prioridad configurada llega tarde.

## 8. Observaciones por subsistema

| Subsistema | Evaluación | Riesgo principal |
|---|---|---|
| App/System | Regular | Startup no plenamente idempotente y sin shutdown global |
| QP/C integration | Deficiente hasta corregir C01/H01 | Ticker y bloqueo cooperativo |
| STT receiver | Bueno con reservas | Parser custom, Unicode y error delivery |
| USB audio | Regular | Threads/lifecycle poco cubiertos y joins en AO |
| Subtitle | Bueno en bounds, regular en integración | MMIO lifetime, geometría y rollback |
| Video services | Incompleto | Triple buffers sin uso y recovery sin timeout |
| Video HAL | Riesgoso por falta de tests reales | ioctls/MMIO, frame count y dynclk |
| BSP/Xilinx | Aceptable como vendor, wrapper mejorable | Asserts vacíos y código no usado enlazado |
| Utils | Bueno | Logging/docs/macros menores |

## 9. Aspectos positivos que conviene preservar

- Las longitudes de texto y colas están acotadas; no se observó heap dinámico indiscriminado en el flujo QP/C.
- `stt_event_rx_poll()` es no bloqueante y limita trabajo por dispatch.
- El parser comprueba duplicados, rangos numéricos, secuencia y trailing garbage.
- `subtitle_bram_write_bitmap()` valida overflow y tamaño de fuente y clipea coordenadas.
- El renderer y sanitizer tienen tests útiles para truncado y UTF-8.
- Audio separa captura/sender en workers y mantiene el AO como supervisor, aunque el init/stop aún deba salir del hilo QV.
- Los tests de `video_io` exploran muchas rutas de rollback y errores, aun cuando los HAL estén mockeados.
- Los sources QP/C usados conservan integridad upstream verificable.

## 10. Roadmap recomendado

### P0 — Antes de seguir agregando features

1. Corregir/actualizar el port POSIX-QV y agregar test con ticker real.
2. Reubicar ownership de `hw_platform` y definir shutdown coordinado.
3. Sacar init/stop bloqueante del hilo cooperativo.
4. Confirmar en Vivado el tamaño BRAM y el contrato geométrico del overlay.
5. Elegir e implementar una política de framebuffer real.

### P1 — Robustez funcional

1. Agregar timeout/recovery de video y detección de cambios de modo.
2. Garantizar delivery de ready/error/control.
3. Corregir NULL dereference, marcador temporal y rollback parcial.
4. Endurecer worker stop/ALSA resume y fd ownership.
5. Formalizar y testear el protocolo NDJSON/UTF-8 con el emisor real.
6. Unificar códigos de error.

### P2 — Limpieza de vibecode/deuda

1. Eliminar o clasificar campos y APIs sin consumidor.
2. Podar objetos VTC no usados y revisar map file.
3. Reducir/configurar QP/C de acuerdo con el producto.
4. Actualizar templates y comentarios stale.
5. Partir funciones largas por responsabilidad.
6. Separar métricas de cobertura de mocks/vendor/código propio.

## 11. Criterio de cierre sugerido

La review debería considerarse resuelta cuando:

- un test repetido demuestra que el ticker siempre arranca y se detiene limpiamente;
- ningún AO ejecuta operaciones potencialmente bloqueantes;
- el ownership MMIO está centralizado y hay shutdown ordenado;
- BRAM/overlay tienen contrato confirmado contra hardware;
- video se recupera de no-signal, timing tardío, modo no soportado y cambio de resolución;
- ready/error no pueden perderse silenciosamente;
- los HAL críticos tienen pruebas sin mocks o backends falsos realistas;
- el inventario de APIs/estado stale fue eliminado o documentado como intencional.

## 12. Limitaciones de esta review

- No se desplegó ni ejecutó en la Arty Z7-20, porque no fue solicitado.
- No se hizo el cross-build ARM/PetaLinux: no hubo cambios de firmware y el build remoto modifica/usa otro entorno. El port-check local no valida ALSA ni ABI ARM.
- No se inspeccionaron Vivado block design, kernel driver VDMA ni el emisor STT; son necesarios para cerrar los puntos marcados como contrato de hardware/protocolo.
- Los 278k LOC upstream de QP/C no se reauditaron línea por línea. Se inventariaron y se revisó exhaustivamente el subconjunto que compila, con checksums de procedencia.

---

**Conclusión:** el repo tiene una arquitectura recuperable y bastante buen trabajo en validación de datos, pero todavía mezcla “implementado” con “scaffolding que parece implementado”. El riesgo no está tanto en buffer overflows obvios, sino en lifecycle, concurrencia cooperativa, ownership de hardware y estados que no progresan. Corregir P0 antes de nuevas features dará mucho más valor que aumentar cobertura superficial sobre módulos mockeados.
