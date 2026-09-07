# Unified Tracing Guide

## Overview

Sistema de tracing unificado que captura eventos del **firmware (C)** y **server (Python)** en el mismo formato, permitiendo visualizar todo el pipeline end-to-end en Perfetto.

## Arquitectura

```
Firmware (C)              Server (Python)           Legacy
━━━━━━━━━━━━━             ━━━━━━━━━━━━━━━━━         ━━━━━━━
trace.c/h                 unified_trace.py          stt_events.jsonl
   ↓                           ↓                         ↓
/tmp/fw_trace.jsonl      logs/server_trace.jsonl   (convertido)
   └────────────────────────────┴──────────────────────┘
                              ↓
                    merge_traces.py
                              ↓
                   logs/unified_trace.json
                              ↓
                 https://ui.perfetto.dev
```

---

## Uso en Firmware (C)

### 1. Inicializar tracing

```c
#include "trace.h"

// En main() o init
trace_ctx_t* trace = trace_init(NULL);  // Output: /tmp/fw_trace.jsonl
if (!trace) {
    LOG_ERROR("Failed to init tracing");
}
```

### 2. Instrumentar código

#### **Eventos instantáneos** (punto en el tiempo)

```c
// Cuando llega un audio chunk
TRACE_INSTANT(trace, "audio_chunk_rx", 
              "\"seq\":%lu,\"bytes\":%zu", seq, chunk_size);

// Cuando se recibe un transcript
TRACE_INSTANT(trace, "transcript_rx",
              "\"seq\":%lu,\"text\":\"%.40s\"", seq, text);
```

#### **Eventos de duración** (medidos)

```c
// Opción A: Medir duración manualmente
uint64_t start_ns = trace_now_ns();
subtitle_text_render(...);
uint64_t dur_ns = trace_now_ns() - start_ns;

TRACE_DURATION(trace, "subtitle_render", dur_ns,
               "\"seq\":%lu,\"chars\":%zu", seq, strlen(text));
```

```c
// Opción B: Begin/End
uint64_t start = TRACE_BEGIN(trace, "bram_write", "\"seq\":%lu", seq);
subtitle_bram_write(...);
TRACE_END(trace, "bram_write", start, NULL);
```

```c
// Opción C: Scope helpers (para bloques)
{
    TRACE_SCOPE_START(trace, subtitle_pipeline);
    // ... todo el pipeline ...
    TRACE_SCOPE_END(trace, subtitle_pipeline);
}
```

### 3. Cerrar tracing

```c
// Al finalizar
trace_close(trace);
```

---

## Uso en Server (Python)

### Importar

```python
from server.runtime.unified_trace import UnifiedTracer, TraceScope
```

### Inicializar

```python
# Al inicio del bridge/server
tracer = UnifiedTracer(output_path="logs/server_trace.jsonl", source="bridge")

# O con context manager
with UnifiedTracer(source="stt") as tracer:
    # ...
```

### Instrumentar

```python
# Evento instantáneo
tracer.instant("audio_received", seq=seq, bytes=len(payload))

# Evento con duración
start = time.monotonic_ns()
result = process_audio(payload)
end = time.monotonic_ns()
tracer.duration("audio_processing", end - start, samples=len(result))

# Scope (context manager)
with TraceScope(tracer, "gpu_inference", model="nemotron"):
    result = model.infer(audio)
```

---

## Puntos de instrumentación recomendados

### **Pipeline crítico (audio → subtitle display)**

Estos son **los más importantes** — capturan la latencia end-to-end del pipeline de subtítulos.

#### **Firmware (C)**

| Ubicación | Evento | Tipo | Qué mide |
|-----------|--------|------|----------|
| `usb_audio_stream.c` | `audio_chunk_capture` | Instant | **T₀**: Audio sale del USB |
| `usb_audio_stream.c` | `audio_ws_send` | Instant | Audio → WebSocket |
| `stt_ws_client.c` | `transcript_ws_rx` | Instant | Respuesta STT llega |
| `SttAO.c` | `transcript_parsed` | Instant | JSON → texto limpio |
| `SubtitleAO.c` | `subtitle_render` | Duration | Render completo (font + layout) |
| `SubtitleAO.c` | `subtitle_display` | Instant | **T_final**: Visible en HDMI |

#### **Server (Python)**

| Ubicación | Evento | Tipo | Qué mide |
|-----------|--------|------|----------|
| `bridge.py` | `board_audio_rx` | Instant | Audio llega del board |
| `bridge.py` | `gpu_queue` | Instant | Audio → cola GPU |
| `nemotron.py` | `gpu_inference` | Duration | **Bottleneck GPU** (~800-1200ms) |
| `bridge.py` | `transcript_emit` | Instant | Resultado → board |

---

### **Contexto adicional (sistema general)**

Estos puntos dan visibilidad del resto del sistema sin ser abrumadores.

#### **Firmware (C)**

| Ubicación | Evento | Tipo | Para qué |
|-----------|--------|------|----------|
| `VideoAO.c` | `video_mode_detect` | Instant | Cambios de resolución HDMI |
| `VideoAO.c` | `video_frame_complete` | Instant | Frame procesado (cada 16ms @ 60Hz) |
| `SystemAO.c` | `qpc_event_dispatch` | Instant | Eventos QP/C (carga del event loop) |
| `stt_ws_client.c` | `ws_connect` / `ws_disconnect` | Instant | Conexiones WebSocket |
| `subtitle_bram.c` | `bram_write` | Duration | Escritura a BRAM (debería ser <1ms) |

#### **Server (Python)**

| Ubicación | Evento | Tipo | Para qué |
|-----------|--------|------|----------|
| `bridge.py` | `ws_connection` | Instant | Board conecta/desconecta |
| `bridge.py` | `audio_queue_overflow` | Instant | Si se pierde audio por overflow |

---

## Workflow completo

### 1. **Build y deploy con tracing habilitado**

```bash
# Build especial, deploy e instalación del servicio (desde WSL)
./scripts/run.sh -p

# Para verificar solamente la compilación, sin tocar la placa:
./scripts/build.sh -p

```

### 2. **Capturar traces**

#### En el firmware:
```bash
# run.sh ya dejó el servicio instalado y corriendo. La IP se descubre por MAC.
BOARD_IP="$(python3 scripts/board/find_board_ip.py)"

# Después de capturar el intervalo deseado, copiar el trace.
scp -O "root@${BOARD_IP}:/tmp/fw_trace.jsonl" logs/
```

#### En el server:
```bash
# El servidor Python ya tiene tracing instrumentado
# Simplemente corre normalmente:
python3 server/runtime/bridge.py --stream-url ws://... --send-subtitles

# El trace se guarda automáticamente en logs/server_trace.jsonl
```

### 3. **Mergear traces**

```bash
# Combinar firmware + server + legacy STT events
python3 scripts/merge_traces.py \
  --fw logs/fw_trace.jsonl \
  --server logs/server_trace.jsonl \
  --stt logs/stt_events.jsonl \
  --output logs/unified_trace.json
```

### 4. **Visualizar en Perfetto**

```bash
# Abrir en browser
open https://ui.perfetto.dev

# Cargar logs/unified_trace.json
# Drag & drop el archivo en Perfetto
```

---

## Ejemplo de timeline esperada

```
Process: subtitle-bridge (PID 100)
├─ audio_received          ●━━━━━━━┓
├─ ws_send_audio                   ●━━━━┓
└─ transcript_emit                        ●

Process: subtitle_overlay_fw (PID 1234)
├─ audio_chunk_rx                             ●
├─ ws_frame_rx                                  ●━━┓
├─ transcript_rx                                    ●
├─ subtitle_render                                   ●━━━━━━━┓
├─ bram_write                                                ●━━┓
└─ overlay_enable                                                 ●

Process: stt (PID 100)
└─ final [GPU inference]        ●━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
   args: {queue_wait_sec: 0.5, gpu_infer_sec: 1.2}
```

---

## Análisis de latencia

Una vez en Perfetto, podés:

1. **Medir latencias end-to-end**
   - Desde `audio_received` (server) hasta `overlay_enable` (firmware)

2. **Identificar bottlenecks**
   - Ver qué etapa toma más tiempo (GPU, render, BRAM write)

3. **Analizar jitter**
   - Variación en tiempos de procesamiento

4. **Verificar orden de eventos**
   - Asegurar que el pipeline fluye correctamente

---

## Tracing por defecto: DESHABILITADO

El tracing está **apagado por defecto** (cero overhead).

Para habilitarlo, usar la flag `-p`:
```bash
# Build sin desplegar
./scripts/build.sh -p

# Build, deploy y servicio persistente
./scripts/run.sh -p

```

Sin `-p`, todas las macros `TRACE_*` se convierten en no-ops y el firmware no
crea `/tmp/fw_trace.jsonl`.

---

## Troubleshooting

### "No events in trace file"

- Verificar que `trace_init()` no devolvió NULL
- Verificar permisos de `/tmp/` en la board
- Ver logs de stderr: `trace: initialized → /tmp/fw_trace.jsonl`

### "Timestamps desfasados entre firmware y server"

Normal - cada proceso tiene su propio `t=0`. Perfetto maneja esto automáticamente mostrándolos en tracks separados.

### "Trace file muy grande"

- Instrumentar solo eventos clave (no loops muy frecuentes)
- Capturar traces cortos (10-30 segundos)
- Disable en producción con `CONFIG_TRACE_ENABLED (0)`

---

## Referencias

- Chrome Trace Format: https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/
- Perfetto UI: https://ui.perfetto.dev
- Trace Viewer: chrome://tracing
