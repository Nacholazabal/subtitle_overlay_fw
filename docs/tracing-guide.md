# Guía de tracing (profiling unificado)

Sistema de profiling del proyecto. Captura eventos del **firmware (C)** y del
**servidor (Python)** en Chrome Trace Format, para abrirlos en
[Perfetto](https://ui.perfetto.dev).

Está **apagado por defecto** en los dos lados: hay que pedirlo explícitamente.

---

## Arquitectura

```text
Placa (firmware C)                     Colab (servidor Python)
━━━━━━━━━━━━━━━━━━                     ━━━━━━━━━━━━━━━━━━━━━━━
src/utils/trace/trace.c                server/runtime/unified_trace.py
  build con  ./scripts/build.sh -p       env  SUBTITLE_TRACE=1
        ↓                                        ↓
/tmp/fw_trace.jsonl                    logs/profiling/server_trace-<run>.jsonl
        └──────────────┬─────────────────────────┘
                       ↓
              scripts/merge_traces.py
                       ↓
              logs/unified_trace.json
                       ↓
              scripts/simplify_trace.py
                       ↓
           logs/presentation_trace.json → https://ui.perfetto.dev
```

Ambos lados escriben **una línea JSON por evento**, con las mismas garantías:
línea atómica, UTF-8 válido, archivo acotado y ningún fallo de profiling que
pueda tumbar el pipeline que está midiendo.

---

## Cómo capturar

La operación normal no requiere configurar variables ni compartir identificadores
entre la placa y Colab.

### 1. Iniciar firmware y servidor

```bash
./scripts/run.sh -p
```

Después, abrir `server/notebooks/nemotron_profiling.ipynb` en Colab, elegir GPU,
ejecutar `Runtime -> Run all` y dejar activa la última celda mientras se usa el
sistema. La placa puede arrancar primero: reintenta la conexión hasta que ngrok y
el servidor queden listos.

`run.sh -p` compila, despliega y reinicia el servicio con la instrumentación del
firmware. El notebook habilita internamente el tracer de servidor antes de crear
la misma app FastAPI usada en producción. No existe un backend especial para
profiling.

### 2. Terminar y recolectar

Interrumpir **una sola vez** la última celda del notebook. Su bloque de cierre:

- detiene Uvicorn y ejecuta el shutdown de FastAPI;
- cierra el tracer y conserva una copia en
  `MyDrive/Tesis-subtitles/profiling/`;
- inicia la descarga de `server_trace-<fecha>.jsonl` al navegador.

Una vez terminada la descarga, ejecutar sin argumentos:

```bash
./scripts/profile-report.sh
```

El wrapper reutiliza `scripts/board/find_board_ip.py` y
`scripts/merge_traces.py`: descubre la placa, detiene brevemente el servicio para
vaciar el trace, copia `/tmp/fw_trace.jsonl`, reinicia el servicio y selecciona
el `server_trace-*.jsonl` más nuevo de Downloads. Como alternativa de rescate,
también busca archivos copiados a `logs/profiling/inbox/`.

Cada recolección queda aislada bajo `logs/profiling/<fecha>/`:

```text
fw_trace.jsonl
server_trace.jsonl
summary.txt
unified_trace.json
presentation_trace.json
```

Abrir `presentation_trace.json` en <https://ui.perfetto.dev> para inspección
visual o figuras de la tesis. Abrir `unified_trace.json` únicamente cuando se
necesiten contadores, estados de conexión, argumentos de diagnóstico o flujos
por chunk.

El merge deja **una sola línea de tiempo**: los eventos del servidor quedan
ubicados sobre el reloj de la placa, con una flecha por chunk uniendo
`audio_ws_send` (placa) con `session_push_pcm` (servidor).

### Vista limpia para presentación

`scripts/simplify_trace.py` no altera ni reemplaza el trace completo. Genera una
segunda vista que detecta el intervalo desde `session_ready` hasta la pérdida de
conexión y desplaza su inicio a cero. El archivo contiene solamente metadata de
nombres y barras de duración completas (`ph: X`): no lleva banderas, eventos
instantáneos, contadores ni flechas.

Las filas conservadas son:

1. sesión STT estable;
2. captura ALSA;
3. envío WebSocket/TLS;
4. render del subtítulo;
5. ingreso y buffering de PCM;
6. inferencia streaming Nemotron;
7. entrega del transcript desde el servidor a la placa;
8. latencia desde el audio cubierto hasta el primer commit del overlay.

Las dos últimas son barras derivadas por correlación. Para la latencia completa
se usa `end_ms`, el chunk que alcanza esa posición de audio y exclusivamente el
primer `overlay_commit` de cada secuencia. Todos los timestamps y duraciones
siguen siendo los valores reales de la captura; sólo cambia la presentación.

También puede ejecutarse manualmente:

```bash
python3 scripts/simplify_trace.py \
  logs/profiling/<corrida>/unified_trace.json \
  logs/profiling/<corrida>/presentation_trace.json
```

### Cómo se alinean los relojes

Sin depender de NTP. Cada chunk de audio lleva en su header el timestamp
monotónico **de la placa**, y el servidor lo registra en
`audio_frame_rx.board_capture_ts_ns`. Cada uno de esos eventos es entonces una
muestra de los dos relojes a la vez:

```text
delta = hora_recepcion_servidor − hora_captura_placa = offset_relojes + transito
```

El tránsito es desconocido pero nunca negativo, así que **el delta más chico de
toda la corrida es la cota más ajustada del offset**. Usarlo implica que:

- el chunk más rápido de la corrida aparece llegando en el instante en que se
  capturó, y los demás se ubican después;
- ningún evento del servidor puede caer antes del evento de la placa que lo
  causó (la causalidad se preserva);
- el tiempo de red que se lee es **relativo al mejor chunk observado**, no un
  absoluto.

Si no hay ninguna muestra compartida, cae al par de anchors de tiempo real de
ambos tracers, que es precisión NTP. El script informa cuál de los dos métodos
usó, y lo deja en la metadata del archivo (`clock_alignment`).

El `run_id` de cada archivo queda como metadata descriptiva, pero no tiene que
coincidir: la alineación real usa los timestamps de audio compartidos.

> La latencia **end-to-end no depende de nada de esto**: empieza en
> `audio_chunk_ready` y termina en `overlay_commit`, y los dos son eventos de la
> placa, sobre el mismo reloj. La alineación sirve para *partir* ese total y ver
> en qué se fue el tiempo.

---

## Eventos que se emiten

### Firmware

| Track | Evento | Tipo | Significado |
| --- | --- | --- | --- |
| `usb-capture` | `alsa_read` | Slice | Tiempo bloqueado esperando el chunk de ALSA |
| `usb-capture` | `audio_chunk_ready` | Instant | PCM medido y estampado (`capture_seq`, `capture_ts_ns`) |
| `usb-capture` | `audio_enqueue` | Instant | Resultado real del handoff a la cola (`status`) |
| `stt-network` | `audio_ws_send` | Slice | Escritura WebSocket real (`audio_seq`, `capture_ts_ns`) |
| `stt-network` | `ws_state` | Instant | Transición de conexión, con motivo |
| `stt-network` | `transcript_decoded` | Instant | Transcript recibido (`transcript_seq`, bytes reales) |
| `qpc-main` | `transcript_dispatch` | Instant | Post a `SubtitleAO`, con `outcome` y `end_ms` |
| `qpc-main` | `subtitle_render` | Slice | Render completo, geometría y escritura a BRAM |
| `qpc-main` | `overlay_commit` | Instant | Enable escrito en la PL — **solo si tuvo éxito** |
| `qpc-main` | `overlay_error` | Instant | El write o el enable falló, con `status` |
| — | `pipeline_health` | Counter 1 Hz | Colas, drops, reconnects, errores |
| — | `process_health` | Counter 1 Hz | CPU user/sys, RSS máximo, context switches |

### Servidor

| Track | Evento | Tipo | Significado |
| --- | --- | --- | --- |
| `fastapi-loop` | `session_open` / `session_close` | Instant | Límites de sesión, con motivo de cierre |
| `fastapi-loop` | `audio_frame_rx` | Instant | Chunk recibido (`audio_seq`, `board_capture_ts_ns`) |
| `inference-worker` | `session_push_pcm` | Slice | Conversión y buffering (`board_capture_ts_ns`, `audio_end_sec`) |
| `inference-worker` | `nemotron_step` | Slice | Llamada completa al pipeline NeMo |
| `fastapi-loop` | `transcript_emit` | Instant | Resultado enviado a la placa |
| — | `server_health` | Counter 1 Hz | Audio bufferizado, pasos, errores |

### Metadata (ambos)

`process_name`, `thread_name` por track, `trace_start` (versión de formato,
`run_id`, `build_id`, anchors de reloj) y `trace_stats` al cerrar, con los
contadores de descarte del propio tracer.

> `nemotron_step` mide el **wall time** de `pipeline.transcribe_step()`. No es
> tiempo puro de kernels CUDA; para eso hace falta PyTorch Profiler, que es
> deliberadamente algo aparte.
>
> `overlay_commit` es el **commit lógico**: el bit de enable quedó escrito en la
> PL. No es evidencia de un píxel visible en HDMI. Un intento fallido sale como
> `overlay_error`, con nombre distinto justamente para que un análisis que se
> olvide de filtrar no pueda confundirlo con un subtítulo mostrado.

---

## Correlación entre placa y servidor

No hay un protocolo de profiling aparte. Se reusa lo que el pipeline ya
transporta.

**La clave primaria es `capture_ts_ns`**, el timestamp que la placa pone en el
header del chunk. Aparece en `audio_chunk_ready`, `audio_enqueue` y
`audio_ws_send` (placa) y como `board_capture_ts_ns` en `audio_frame_rx` y
`session_push_pcm` (servidor).

**Por qué no se usan las secuencias de audio.** Hay dos contadores distintos y no
son iguales:

| Campo | Dónde | Qué cuenta |
| --- | --- | --- |
| `capture_seq` | `audio_chunk_ready`, `audio_enqueue` | Chunks leídos desde que arrancó el proceso |
| `audio_seq` | `audio_ws_send`, y lo que ve el servidor | Chunks del protocolo, **se reinicia en cada sesión STT** |

Arrancan desfasados —se captura audio durante el handshake— y vuelven a
desfasarse en cada reconexión. Se conservan como dato secundario, pero nada
correlaciona por ellos.

**Transcripts**: `transcript_seq`, presente desde `transcript_emit` (servidor)
hasta `transcript_decoded`, `transcript_dispatch` y `overlay_commit` (placa).

El trace **no guarda texto de transcripts ni audio**: solo `text_bytes`,
`visible_chars` y las secuencias. Alcanza para reconciliar el pipeline y evita
poner contenido de habla en un artefacto de profiling.

## Cómo se calcula el end-to-end

La pregunta es "¿cuánto pasó entre que se habló y que apareció el subtítulo?".
Los dos extremos son eventos de la placa, así que **no depende de la alineación
de relojes**. Lo que sí hace falta es saber *qué audio describe cada subtítulo*, y
eso lo dice el `end_ms` del transcript.

```text
transcript_dispatch.end_ms   (¿hasta dónde llega el texto?)
   → session_push_pcm.audio_end_sec  (¿qué chunk cerró ese audio?)
   → session_push_pcm.board_capture_ts_ns  (¿cuándo lo capturó la placa?)
   → overlay_commit.ts − eso  =  latencia end-to-end
```

Requiere ambos traces, porque el mapeo posición-de-audio → timestamp lo aporta el
servidor. Si falta, el script dice por qué no puede calcularla en vez de inventar
un número.

> Emparejar el commit con el **último chunk capturado** en su lugar mide la edad
> del audio más nuevo del buffer —del orden de un período de chunk, ~20 ms— y no
> tiene nada que ver con la latencia. Una versión anterior de este script hacía
> eso y reportaba ~1 ms.

---

## Invariantes que una captura debe cumplir

Sirven para decidir si una corrida es utilizable:

- Ninguna línea JSON inválida ni UTF-8 reparado.
- Tracks separados para `qpc-main`, `usb-capture`, `stt-network`,
  `fastapi-loop` e `inference-worker`.
- `audio_chunk_ready` = `audio_enqueue`; los `audio_enqueue` con `status != 0`
  explican la diferencia con `audio_ws_send`.
- Toda slice es un evento completo (`ph: "X"`) con `dur`. No existen fases
  `B`/`E`, así que no puede haber slices sin cerrar.
- `trace_stats` reporta `dropped_full`, `dropped_truncated` y `write_errors`.
- Un build sin `-p` no crea ningún archivo.

---

## Escribir instrumentación nueva

### Firmware

```c
#include "app.h"   /* trae trace.h y g_trace */

/* Punto en el tiempo */
TRACE_INSTANT(g_trace, "audio_enqueue",
              TRACE_U64("audio_seq", seq),
              TRACE_I64("status", status));

/* Duración: una sola llamada, imposible dejarla abierta */
uint64_t const started = TRACE_NOW();
do_the_work();
TRACE_SLICE(g_trace, "subtitle_render", started, TRACE_I64("status", status));

/* Contador (1 Hz, no por operación) */
TRACE_COUNTER(g_trace, "pipeline_health", TRACE_U64("audio_txq_depth", depth));

/* Nombrar el track del thread, una vez al arrancarlo */
TRACE_THREAD(g_trace, "usb-capture");
```

Los argumentos son **tipados**: `TRACE_U64`, `TRACE_I64`, `TRACE_F64`,
`TRACE_BOOL`, `TRACE_STR`. El call site nunca arma JSON; el módulo escapa las
cadenas y las corta en un borde de code point UTF-8.

Usá `TRACE_INSTANT0` / `TRACE_SLICE0` cuando no haya argumentos.

### Servidor

```python
tracer.instant("audio_frame_rx", audio_seq=frame.seq, bytes=len(frame.payload))

with tracer.scope("nemotron_step", samples=1600):
    step_outputs = engine.step(...)          # la slice se emite aunque esto falle

tracer.counter("server_health", buffered_samples=320)
tracer.register_thread("inference-worker")
```

Cuando el tracing está apagado, `create_tracer()` devuelve un `NullTracer` con la
misma interfaz, así que los call sites se escriben sin condicionales.

---

## Sobrecarga

- **Build sin `-p`**: cero. Las macros no generan código ni referencian el
  contexto.
- **Build con `-p`**: los eventos se arman en un buffer de stack, se copian a un
  buffer de 32 KB bajo mutex y se vuelcan por tiempo o por ocupación. No hay un
  `write()` por evento.
- **Quién paga el flush**: el thread que emite el evento que cruza el umbral, lo
  que en teoría puede ser `qpc-main` o el thread de audio. `trace_stats` reporta
  `flushes` y `max_flush_us` justamente para no tener que suponerlo: en las
  mediciones el peor flush fue de **62 µs**, sobre un presupuesto de 1,5 s. Si en
  la placa ese número creciera, ahí sí se justificaría mover los writes a un
  thread dedicado.
- **Tamaño**: ~12 KB/s medidos. El tope por defecto (16 MB) corta cerca de los 20
  minutos; al llegar emite `trace_full` y cuenta los descartes.

---

## Tests

```bash
make test                                        # incluye test/utils/trace/test_trace.c
python3 -m unittest server.tests.test_unified_trace
python3 -m unittest server.tests.test_merge_traces
python3 -m unittest server.tests.test_simplify_trace
```

Cubren: formato y metadata, escape y UTF-8, slices completas, tope de tamaño,
concurrencia, tracing deshabilitado, que ningún fallo del tracer se propague, y
—en el stitcher— que la estimación de offset use el chunk más rápido y que
ningún evento del servidor quede antes de su causa en la placa.

---

## Referencias

- [Perfetto: formatos externos y Chrome JSON](https://perfetto.dev/docs/getting-started/other-formats)
- [Perfetto: track events, slices, counters, flows](https://perfetto.dev/docs/instrumentation/track-events)
- Revisión que originó este diseño: `docs/profiling_system_review_2026-09-07.md`
