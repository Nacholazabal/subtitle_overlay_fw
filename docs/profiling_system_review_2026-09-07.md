# Review del sistema de profiling

Fecha: 2026-09-07

## Veredicto

La base actual alcanza para demostrar que el tramo local del firmware es corto,
pero todavía no alcanza para presentar un breakdown end-to-end confiable. No
hace falta integrar el SDK nativo de Perfetto ni construir un profiler de kernel.
El formato Chrome JSON actual es suficiente: Perfetto soporta slices, instants,
counters, metadata y flows en ese formato.

El trabajo necesario es, en este orden:

1. corregir el significado y la integridad de los eventos existentes;
2. instrumentar el camino directo de producción en Colab;
3. alinear los relojes de placa y servidor;
4. agregar unos pocos counters de salud;
5. automatizar la recolección y el resumen.

`ftrace`, `perf`, cache misses y profiling CUDA por operador deben quedar como
herramientas opcionales para investigar un problema concreto, no como requisito
del profiler normal de la tesis.

## Qué ya funciona

- `./scripts/build.sh -p` y `./scripts/run.sh -p` activan el tracing solamente
  en el build de profiling. El build normal no crea el trace.
- El firmware usa `CLOCK_MONOTONIC`, apropiado para medir intervalos locales.
- Los eventos se pueden convertir a Chrome JSON y abrir en Perfetto.
- La captura real `20260907-0055` tuvo 12.912 eventos válidos durante 116,741 s.
- Hubo 5.838 chunks capturados y 5.838 eventos de enqueue, sin saltos en sus
  secuencias.
- Los 253 pares begin/end de render estuvieron balanceados.
- La cadencia de captura fue estable: 20,000 ms p50, 20,005 ms p90 y
  21,270 ms máximo.
- El tramo frame de texto recibido → commit lógico del overlay fue corto:
  8,787 ms p50, 11,811 ms p90 y 14,245 ms máximo.

La última medición permite afirmar que el firmware no parece ser el componente
dominante de la latencia de 1,5 s. No permite atribuir todavía el resto entre
red, buffering de Nemotron e inferencia.

## Hallazgos del código actual

### P0 — Corregir antes de usar un trace combinado

#### 1. Los relojes de firmware y servidor no están alineados

`trace.c` y `unified_trace.py` restan un `base_time` local y comienzan cerca de
`t=0`. `merge_traces.py` concatena ambos archivos sin transformar timestamps.
Dos procesos que comenzaron en momentos diferentes aparecen falsamente
sincronizados.

Consecuencia: hoy un archivo llamado `unified_trace.json` no puede usarse para
medir placa → Colab → placa.

Corrección mínima:

- al iniciar cada tracer, capturar juntos `CLOCK_MONOTONIC` y
  `CLOCK_REALTIME`/Unix time;
- emitir ambos anchors como metadata;
- conservar monotonic para duraciones;
- convertir cada evento a una timeline UTC común durante el merge;
- remapear los PID por fuente, porque placa y Colab pueden tener el mismo PID;
- advertir o rechazar un merge end-to-end si falta un clock anchor.

NTP da una alineación suficiente para este objetivo, pero el reporte debe
identificarla como `ntp_aligned` y no prometer precisión submilisegundo.

#### 2. Los nombres actuales no coinciden con lo medido

- `audio_chunk_capture` sucede después de que termina el read ALSA, no cuando
  comienza a entrar el audio.
- `audio_ws_send` sucede antes de `stt_ws_client_submit_audio()`: mide un intento
  de enqueue desde el thread de captura, no el envío WebSocket.
- El envío real ocurre luego en `send_audio_chunk()` dentro del worker STT y no
  tiene evento.
- `transcript_ws_rx` se emite para todo frame de texto antes de conocer su tipo;
  incluye mensajes de control además de transcripts.
- `transcript_parsed` se emite en `SttAO`, después del ring y del polling, pero
  antes de saber si el post a `SubtitleAO` fue aceptado.
- `subtitle_display` se emite después del write del bit enable. Es un commit
  lógico al overlay, no evidencia de un píxel visible en HDMI.

Consecuencia: algunas cifras de la captura son válidas como intervalos, pero no
con el nombre causal que hoy muestran.

Corrección: renombrar y mover los eventos según la tabla objetivo de este
documento. No conservar aliases engañosos.

#### 3. El tracer Python puede pisar y filtrar archivos

Cada `NemotronPipelineStream` crea un `UnifiedTracer` sobre
`logs/server_trace.jsonl` usando modo `"w"`. Una nueva sesión trunca la anterior.
Además, `NemotronPipelineStream.close()` cierra el pipeline pero no el tracer.
El bridge legacy y Nemotron también comparten el mismo path por defecto.

Corrección mínima:

- un tracer propiedad de la aplicación FastAPI, no uno por stream;
- un archivo único por `run_id`;
- cierre en el shutdown de la aplicación;
- sesiones marcadas con `session_id`, sin truncar el archivo;
- path explícito en Drive desde la notebook;
- el bridge legacy debe usar otro source/path y no intervenir en producción.

### P1 — Integridad y legibilidad del trace

#### 4. Todos los threads aparecen como `tid=1`

El firmware tiene como mínimo el thread QP/C, el capture thread y el STT network
worker. El tracer guarda `tid=1` en el contexto global. Python repite la misma
suposición aunque `session.push_pcm()` corre mediante `asyncio.to_thread()`.

Corrección:

- obtener el TID Linux real al emitir cada evento;
- registrar metadata con nombres estables: `qpc-main`, `usb-capture` y
  `stt-network`;
- en Python usar el identificador real del thread y nombres `fastapi-loop` e
  `inference-worker`;
- mantener begin/end de una slice en el mismo track.

Perfetto define un track como una secuencia de ejecución independiente; usar
tracks distintos es lo que permite ver concurrencia real.

#### 5. El writer C no construye una línea de forma atómica

Un evento se arma con varios `fprintf()` compartiendo un `FILE*` global y luego
se hace `fflush()`. Las llamadas individuales de stdio pueden estar protegidas,
pero la secuencia completa puede intercalarse con otro thread.

Corrección suficiente, sin agregar un nuevo AO ni un writer thread:

- construir el evento completo en un buffer local fijo;
- hacer una sola escritura por línea;
- contabilizar eventos descartados o demasiado grandes;
- bufferizar y hacer flush periódico, al cerrar y ante una solicitud de snapshot;
- ningún error de profiling puede detener el firmware.

Un thread dedicado de tracing sería una optimización futura, no un requisito.

#### 6. Los argumentos C no son JSON seguros

Los call sites construyen fragmentos JSON con printf. Los textos no escapan
comillas, barras ni controles, y `%.40s` corta por bytes. La captura real partió
ocho caracteres UTF-8; el conversor pudo recuperarlos reemplazando el byte final,
pero eso es recuperación, no una solución de origen.

Corrección recomendada: no guardar texto del transcript. Para profiling alcanza
con `seq`, `is_final`, `text_bytes`, `visible_chars` y, si se necesita correlación,
un hash corto. Los argumentos numéricos deben serializarse mediante helpers
tipados, no mediante JSON crudo.

#### 7. `transcript_ws_rx.bytes` siempre vale cero

`client->msg_used` se resetea antes del trace. Hay que preservar
`completed_bytes`, procesar el mensaje y emitir el tamaño real. El evento
posterior al decode debe indicar también `message_type`.

#### 8. El trace crece sin límite

La captura creció aproximadamente 12,7 KB/s: alrededor de 45,6 MB por hora en
NDJSON. Un build `-p` queda instalado como servicio y puede continuar escribiendo
durante horas.

Corrección:

- límite fijo de tamaño para el archivo de profiling;
- al alcanzar el límite, emitir/contabilizar `trace_full` y dejar de registrar;
- no intentar rotación compleja en esta etapa;
- flush periódico para limitar pérdida ante crash;
- mostrar claramente en el log cuando tracing se deshabilita por capacidad.

### P1 — Completar el camino de producción

#### 9. El server directo casi no está instrumentado

`server/runtime/app.py`, que es el camino placa → Colab real, no emite eventos
para recepción, buffering ni envío. Parte de la instrumentación existente está
en `bridge.py`, que es legacy/evaluación y no participa en producción.

El slice actual llamado `gpu_inference` mide el tiempo wall-clock de
`pipeline.transcribe_step()`. Es una medición útil del paso Nemotron, pero no
demuestra por sí sola tiempo puro de kernels CUDA. Debe llamarse
`nemotron_step`. PyTorch Profiler puede separar CPU/CUDA si alguna vez hace
falta investigar el modelo, pero no es necesario en cada corrida.

#### 10. Faltan IDs para seguir una unidad de trabajo

El protocolo ya transporta `audio_seq`, timestamp de captura y contador de
drops. Hay que reutilizarlos, no inventar otro protocolo de profiling.

- Para audio: correlacionar por `audio_seq` y `capture_timestamp_ns`.
- Para resultados: correlacionar por `transcript_seq` y `session_id`.
- El merge puede crear flows visuales a partir de esos IDs.

No hace falta incluir PCM ni texto en el trace.

### P2 — Salud del sistema, sin sobredimensionar

#### 11. Faltan counters que expliquen un pico

El firmware ya mantiene la mayoría de los contadores necesarios en
`stt_ws_client_stats_t`. Deben emitirse como counters una vez por segundo, no
como instants por cada operación:

- profundidad y high-water mark de la cola PCM;
- chunks enviados y descartados;
- profundidad y drops del transcript ring;
- drops por event pool y por cola de `SubtitleAO`;
- sesiones, reconexiones, errores TLS y de protocolo;
- transcripts parciales/finales y entregas aceptadas;
- recuperaciones ALSA por xrun/suspend;
- opcional: `getrusage()` del proceso para user CPU, system CPU, max RSS y
  context switches.

Estos datos son suficientes para responder si la CPU, la red o las colas no
acompañan el tiempo real. Cache misses no son necesarios salvo que aparezca un
bottleneck local no explicado.

#### 12. El evento final debe llamarse commit lógico

El evento después de `subtitle_overlay_enable()` debe llamarse
`overlay_commit`, no `subtitle_display`. Si se quiere confirmar que la PL
procesó al menos un frame, puede leerse el sticky SOF de forma no bloqueante en
un polling posterior y emitir `overlay_sof_seen`. No se debe llamar al
`subtitle_overlay_wait_sof()` bloqueante desde un handler QP/C.

Incluso con SOF, el reporte debe aclarar que no es una captura física del HDMI.

## Conjunto mínimo de eventos objetivo

### Firmware

| Track | Evento | Tipo | Campos mínimos | Significado |
| --- | --- | --- | --- | --- |
| `usb-capture` | `alsa_read` | Slice | bytes, status | Tiempo bloqueado esperando un chunk |
| `usb-capture` | `audio_chunk_ready` | Instant | capture timestamp, bytes, dropped | PCM medido/AGC listo |
| `usb-capture` | `audio_enqueue` | Instant | capture timestamp, status | Resultado real del handoff a la cola |
| `stt-network` | `audio_ws_send` | Slice/instant | audio seq, capture timestamp, bytes, status | Escritura WebSocket real |
| `stt-network` | `ws_state` | Instant | old/new state, reason | Conexión, backoff y reconexión |
| `stt-network` | `transcript_decoded` | Instant | session, transcript seq, final, bytes | Transcript JSON válido recibido |
| `qpc-main` | `transcript_dispatch` | Instant | seq, final, accepted/drop reason | Post real hacia `SubtitleAO` |
| `qpc-main` | `subtitle_render` | Slice | seq, final, chars | Render, geometría y BRAM completos |
| `qpc-main` | `overlay_commit` | Instant | seq, status | Enable/config escrito en la PL |
| counters | `pipeline_health` | Counter, 1 Hz | colas, drops, reconnects, errors | Salud y backpressure |

No es imprescindible separar `subtitle_render` en renderer, BRAM y overlay en
la primera versión. Si vuelve a superar aproximadamente 10–15 ms, entonces sí
conviene agregar `bitmap_render` y `bram_update` como slices anidadas.

### Server directo

| Track | Evento | Tipo | Campos mínimos | Significado |
| --- | --- | --- | --- | --- |
| `fastapi-loop` | `session_open/close` | Instant | session, config/hash | Límite de sesión |
| `fastapi-loop` | `audio_frame_rx` | Instant | audio seq, board timestamp, bytes, drops | Chunk recibido desde la placa |
| `inference-worker` | `session_push_pcm` | Slice | audio seq, buffered samples | Conversión/buffering del runtime |
| `inference-worker` | `nemotron_step` | Slice | samples, first/last | Llamada completa al pipeline NeMo |
| `fastapi-loop` | `transcript_emit` | Instant | transcript seq, final, end sec, reason | Resultado enviado a la placa |
| counters | `server_health` | Counter, 1 Hz | buffered audio, sessions/errors | Salud del server |

## Metadata necesaria por archivo

- `run_id` y `source` (`board` o `colab`);
- commit/build identifier;
- build mode (`profiling`);
- PID real y nombres de threads;
- monotonic/realtime clock anchors;
- sample rate, chunk size y formato;
- configuración efectiva Nemotron y modelo;
- sesión y motivo de cierre;
- video mode/frame rate cuando esté disponible.

## Cambios deliberadamente fuera de alcance

- No integrar el SDK C++ nativo de Perfetto en PetaLinux 2018.3.
- No migrar inmediatamente de Chrome JSON a protobuf `.pftrace`.
- No agregar un AO de profiling.
- No instrumentar cada función, frame de video o acceso MMIO.
- No ejecutar PyTorch Profiler continuamente en Colab.
- No exigir `perf`, PMU, cache counters o ftrace para aceptar el profiler.
- No guardar audio ni frases completas en el trace.

Chrome JSON es una elección razonable para este proyecto y Perfetto lo importa
de forma nativa. Slices, counters, metadata y flows ya cubren el caso de uso.

## Orden de implementación recomendado

### Entrega A — Trace firmware confiable

1. Corregir nombres y ubicación de los eventos.
2. TID real y metadata de los tres threads.
3. Una línea JSON segura por write, sin texto crudo.
4. Flush periódico, cierre correcto y límite de archivo.
5. Counters 1 Hz reutilizando stats existentes.
6. Tests C del writer, concurrencia, UTF-8/args, límite y trace disabled.

### Entrega B — Trace de producción en Colab

1. Mover la propiedad del tracer a la aplicación FastAPI.
2. Instrumentar `app.py` y renombrar `gpu_inference` a `nemotron_step`.
3. Archivo por run en Google Drive, sin truncar por sesión.
4. Cierre en shutdown y tests con modelo/sesión fake.

### Entrega C — Merge y operación

1. Alinear por clock anchors y remapear PID/TID por source.
2. Validar schema, pares de slices, monotonicidad y correlación de secuencias.
3. Crear flows audio y transcript en el export, no necesariamente en firmware.
4. Agregar un collector que descubra la placa, copie el snapshot, reciba el
   server trace, genere `perfetto.json` y un resumen Markdown.
5. Mantener el exportador tolerante para traces históricos, pero reportar toda
   reparación o evento descartado.

### Entrega D — Solo si queda una duda concreta

- Comprobar soporte de `ftrace`/tracefs para scheduling.
- Usar `perf stat` si está disponible para ciclos/cache misses.
- Usar una captura corta de PyTorch Profiler para separar CPU/CUDA.

La documentación oficial de Linux recomienda empezar por herramientas y
métricas simples y pasar a `perf`/ftrace cuando ya existe un problema que
localizar. `ftrace` además requiere soporte específico del kernel.

## Aceptación

Una captura de 2–5 minutos se acepta si:

- no hay líneas JSON inválidas ni UTF-8 reparado;
- aparecen tracks separados para QP/C, captura, red, FastAPI e inferencia;
- todos los bytes de audio son no cero y los nombres reflejan la operación real;
- `captured = enqueued + enqueue_drops`;
- `enqueued = sent + tx_drops + pending`, considerando límites de la captura;
- transcript emit/receive/dispatch/commit reconcilian por sesión y secuencia;
- toda slice begin tiene su end en el mismo track;
- el merge informa el método de sincronización y no mezcla clocks sin anchor;
- el resumen entrega p50/p90/p95/máxima por etapa y todos los drops;
- el archivo respeta el límite y el trace reporta su propio drop/error count;
- un build sin `-p` no crea archivos ni ejecuta el path de tracing;
- ninguna falla del profiler afecta audio, STT, QP/C o HDMI.

## Archivos que probablemente cambien

- `src/utils/trace/trace.c` y `trace.h`;
- `src/svc/usb_audio/usb_audio_stream.c`;
- `src/svc/stt/stt_ws_client.c` y stats/colas relacionadas;
- `src/svc/stt/SttAO.c`;
- `src/svc/subtitle_pipeline/SubtitleAO.c`;
- `server/runtime/unified_trace.py`;
- `server/runtime/app.py`;
- `server/runtime/nemotron.py`;
- `scripts/merge_traces.py`;
- tests C y `server/tests/` correspondientes;
- notebook de servidor para path/export del trace.

## Referencias primarias

- [Perfetto: formatos externos y soporte de Chrome JSON](https://perfetto.dev/docs/getting-started/other-formats)
- [Perfetto: track events, slices, counters, flows y clocks](https://perfetto.dev/docs/instrumentation/track-events)
- [Perfetto: conversión de datos timestamped y flows](https://perfetto.dev/docs/getting-started/converting)
- [Linux kernel: guía de debugging userspace, ftrace y perf](https://docs.kernel.org/process/debugging/userspace_debugging_guide.html)
- [PyTorch Profiler: actividades CPU/CUDA](https://docs.pytorch.org/tutorials/recipes/recipes/profiler_recipe.html)
