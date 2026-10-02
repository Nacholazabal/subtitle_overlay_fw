# Perfilado unificado del sistema — corrida del 11 de septiembre de 2026

## Objetivo

Esta corrida caracteriza el costo y la latencia interna del camino completo de
subtitulado en vivo:

```text
captura ALSA en la placa
    → transmisión WebSocket/TLS
    → recepción y procesamiento en Colab
    → inferencia streaming Nemotron
    → recepción del transcript en la placa
    → render y commit del overlay
```

El firmware y el servidor registraron eventos independientes en formato Chrome
Trace JSONL. `scripts/merge_traces.py` colocó ambos sobre el reloj monotónico de
la placa usando el timestamp de captura que acompaña a cada chunk de audio. El
resultado puede abrirse en Perfetto.

Esta instrumentación mide software y transporte. Un `overlay_commit` confirma
que el firmware actualizó correctamente el buffer del overlay, pero no constituye
una captura óptica de los píxeles mostrados por el HDMI.

## Artefactos

Directorio de la corrida:

```text
logs/profiling/20260911-030524/
├── fw_trace.jsonl
├── server_trace.jsonl
├── summary.txt
├── unified_trace.json
└── presentation_trace.json
```

`unified_trace.json` conserva toda la evidencia de diagnóstico. La vista
`presentation_trace.json` deriva del mismo archivo, recorta la ventana conectada
y deja solamente barras de duración con nombres descriptivos para inspección y
figuras de la tesis.

Datos de procedencia relevantes:

| Elemento | Valor |
| --- | --- |
| Firmware | `4fa5f5b-dirty` |
| Servidor NeMo | `2639d4bef8d1` |
| Motor | Nemotron 3.5 ASR Streaming 0.6B |
| Idioma | `es-ES` |
| Lookahead configurado | 560 ms |
| Firmware: eventos registrados | 38.011 |
| Servidor: eventos registrados | 19.453 |
| Drops/errores del tracer | 0 en ambos extremos |

El sufijo `dirty` indica que el binario de firmware se construyó con cambios
locales sin commit. No invalida la medición, pero debe conservarse al documentar
su reproducibilidad.

## Ventana válida analizada

El servidor registró dos conexiones. La primera pertenecía al ejecutable que
estaba activo mientras terminaba el despliegue del firmware instrumentado. La
segunda corresponde al proceso de firmware incluido en `fw_trace.jsonl`.

La ventana válida de funcionamiento conectado en el reloj del firmware fue:

```text
inicio ready:       1,194 s
fin de conexión:  105,797 s
duración válida:  104,603 s
```

Para aislarla se conservaron sólo los eventos del servidor cuyo
`board_capture_ts_ns` también aparece en el trace de la placa. Esto produjo
5.210 chunks correlacionados y excluyó los 4.164 chunks de la conexión anterior.

## Resultados

### Latencia hasta la primera aparición

La latencia se calculó desde el final del audio descrito por cada transcript
(`end_ms`) hasta el **primer** `overlay_commit` correspondiente. Contar commits
posteriores del mismo transcript mediría redibujos, no su primera aparición.

| Percentil | Latencia |
| --- | ---: |
| Mínima | 452,3 ms |
| p50 | 475,2 ms |
| p90 | 496,2 ms |
| p95 | 514,6 ms |
| p99 | 1.032,8 ms |
| Máxima | 1.074,7 ms |
| Eventos bajo 1,5 s | 100% (176/176) |

Esta definición mide el retardo desde que termina la porción de audio que el
transcript declara cubrir hasta que el firmware confirma la primera actualización
del overlay. No mide desde el comienzo acústico de una palabra larga.

### Duración de las etapas

| Etapa | p50 | p90 | p95 | p99 | Máxima |
| --- | ---: | ---: | ---: | ---: | ---: |
| Lectura ALSA | 19,771 ms | 19,786 ms | 19,789 ms | 19,863 ms | 20,816 ms |
| Envío WebSocket en placa | 0,255 ms | 0,283 ms | 0,307 ms | 0,334 ms | 0,675 ms |
| Ingreso de PCM al servidor | 0,091 ms | 0,137 ms | 0,171 ms | 65,252 ms | 105,613 ms |
| Paso Nemotron | 58,980 ms | 81,042 ms | 86,507 ms | 100,111 ms | 104,780 ms |
| Render del subtítulo | 4,604 ms | 5,554 ms | 5,666 ms | 5,792 ms | 5,830 ms |

El buffer de entrada del servidor forma un diente de sierra normal: acumula
muestras hasta disponer de un bloque de inferencia y luego vuelve a bajar. Su
máximo observado fue 8.640 muestras, sin errores de sesión.

### Transporte placa → servidor

El tiempo adicional respecto del chunk más rápido fue:

| Percentil | Demora relativa |
| --- | ---: |
| p50 | 19,3 ms |
| p90 | 20,7 ms |
| p95 | 42,3 ms |
| p99 | 69,6 ms |
| Máxima | 205,0 ms |

Es una medición relativa: la sincronización toma el chunk más rápido como cota
del offset entre relojes. Sirve para caracterizar jitter y colas, no como tiempo
absoluto de red.

### Recursos y confiabilidad del firmware conectado

| Métrica | Resultado |
| --- | ---: |
| CPU media | 5,2% de un núcleo |
| CPU p90 | 5,8% de un núcleo |
| CPU máxima | 6,3% de un núcleo |
| RSS máxima | 4.732 KiB |
| Profundidad p90 de la cola de audio | 0 |
| Profundidad máxima de la cola de audio | 1 |
| Profundidad máxima del ring de eventos | 0 |
| Drops durante la ventana ready | 0 |
| Errores de protocolo | 0 |
| Drops de pool/cola/ring de subtítulos | 0/0/0 |

El servidor emitió 176 transcripts: 143 parciales y 33 finales. Los 176 fueron
aceptados por el pipeline de la placa. El overlay realizó 187 commits porque 11
secuencias provocaron un segundo redibujo; la latencia de aparición utiliza sólo
el primero.

Al cerrar deliberadamente el servidor, 20 chunks ya aceptados por el socket de
la placa no llegaron a registrarse en Colab. Todos se concentran en los últimos
400 ms de teardown y no representan pérdidas durante el estado estable.

## Corrección del valor end-to-end del reporte automático

El `summary.txt` original muestra aproximadamente 86 s de latencia end-to-end.
Ese valor es inválido y no debe citarse.

La causa es doble:

1. `audio_end_sec` comienza nuevamente en cero en cada conexión, pero el merger
   ordenó conjuntamente las posiciones de las dos conexiones del servidor.
2. El cálculo contó todos los `overlay_commit`, aunque una misma secuencia puede
   redibujarse más de una vez.

Al limitar los chunks a los timestamps presentes en el trace actual de firmware
y usar el primer commit de cada secuencia se obtienen los percentiles corregidos
de la sección de resultados.

Pendiente para el tooling: hacer que `scripts/merge_traces.py` aplique estos dos
criterios automáticamente.

## Hallazgo pendiente: consumo durante la desconexión

Después de interrumpir Colab, la CPU de la placa pasó de 5,2% a aproximadamente
100,5% de un núcleo durante el período de reconexión. En paralelo:

- `audio_txq_depth` subió a 16 y permaneció llena;
- `chunks_sent` dejó de crecer;
- `chunks_dropped_tx` comenzó a crecer;
- el worker realizó reintentos con backoff exponencial.

La causa observada en el código es que el worker llama a
`stt_audio_txq_wait()` durante el backoff, pero esa función sólo bloquea cuando
la cola está vacía. Como la captura continúa llenándola mientras no hay servidor,
la espera retorna inmediatamente y el worker gira comprobando el plazo de
reintento.

Este comportamiento no afecta los percentiles de la ventana conectada, pero es
un defecto de producción pendiente. Debe corregirse y verificarse en una corrida
posterior; no se modifica como parte de este informe.

## Cómo leer esta captura en Perfetto

### Intervalos importantes

| Intervalo aproximado | Qué ocurre |
| --- | --- |
| 0–1,2 s | Inicio, TLS, WebSocket y negociación de la sesión |
| 1,2–105,8 s | Ventana válida: audio, inferencia y subtítulos conectados |
| 105,8–210,7 s | Colab apagado: backoff y hallazgo de CPU alta |

Para estudiar rendimiento normal hay que hacer zoom sobre 1,2–105,8 s. El
tramo posterior sirve exclusivamente para estudiar tolerancia a desconexiones.

### Tracks del firmware (`board 1`)

- `qpc-main 1201`: event loop de QP/C y trabajo coordinado por los Active
  Objects.
- `stt-network 1206`: conexión TLS/WebSocket, envío de audio y recepción de
  transcripts.
- `usb-capture 1207`: lecturas ALSA; debe permanecer periódico durante toda la
  corrida.
- `pipeline_health audio_txq_depth`: ocupación instantánea de la cola de audio.
- `pipeline_health chunks_sent`: contador acumulado de chunks transmitidos.
- `pipeline_health chunks_dropped_tx`: contador acumulado; importa su pendiente,
  no su valor aislado.
- `pipeline_health deliveries_accepted`: transcripts aceptados por el pipeline.
- `dropped_pool`, `dropped_queue` y `dropped_ring`: deben permanecer planos en
  cero.
- `process_health user_cpu_sec` y `system_cpu_sec`: tiempo de CPU acumulado. Una
  línea más inclinada significa mayor consumo; la altura absoluta no es un
  porcentaje instantáneo.

### Tracks del servidor (`Process 2`)

- `Thread 3399`: loop FastAPI/WebSocket.
- `Thread 3514`: worker de inferencia Nemotron.
- `server_health buffered_samples`: muestras pendientes; el diente de sierra es
  el comportamiento esperado del framing streaming.
- `chunks_received`: contador acumulado de chunks recibidos.
- `infer_wall_sec`: tiempo acumulado dentro de inferencia.
- `max_backlog_samples`: máximo histórico; por definición sube o queda plano,
  nunca baja.
- `session_errors`: debe permanecer en cero.
- `streaming_steps`: número acumulado de pasos ejecutados por Nemotron.

### Lectura visual recomendada

1. Colapsar inicialmente todos los contadores y dejar visibles los tres threads
   de la placa y los dos del servidor.
2. Hacer zoom sobre 1,2–105,8 s para eliminar el ruido de arranque y apagado.
3. Abrir `audio_ws_send`, `session_push_pcm`, `nemotron_step`,
   `transcript_dispatch`, `subtitle_render` y `overlay_commit`.
4. Seleccionar un rectángulo individual: el panel inferior muestra duración y
   argumentos como secuencia o timestamp.
5. Recién después desplegar los contadores para verificar que colas y drops se
   mantengan planos durante ese mismo intervalo.
6. Para estudiar el defecto de desconexión, comparar alrededor de 105,8 s el
   fin de `chunks_sent`, el crecimiento de `audio_txq_depth`/`chunks_dropped_tx`
   y el cambio de pendiente del tiempo de CPU.

## Texto breve reutilizable en la tesis

> El perfilado unificado correlacionó 5.210 bloques de audio entre el firmware y
> el servidor mediante timestamps monotónicos originados en la placa. Durante
> 104,6 s de operación estable no se observaron pérdidas en las colas del
> pipeline ni errores de protocolo. La latencia desde el final del audio cubierto
> por cada hipótesis hasta el primer commit del overlay presentó una mediana de
> 475,2 ms, un percentil 90 de 496,2 ms y un máximo de 1.074,7 ms; el 100% de las
> 176 actualizaciones quedó por debajo del objetivo de 1,5 s. El firmware consumió
> en promedio 5,2% de un núcleo y alcanzó 4.732 KiB de RSS, mientras que el paso
> de inferencia Nemotron presentó 59,0 ms de mediana y 81,0 ms en el percentil 90.
