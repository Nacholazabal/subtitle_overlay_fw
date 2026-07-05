# Streaming STT Run Log

Bitacora de debugging para el pipeline streaming:

```text
Board -> PC bridge -> Colab/VPS WebSocket STT server -> PC bridge -> Board
```

Objetivo: ajustar audio, segmentacion, partials/finals y UX hasta acercarnos a subtitulos live legibles con ~1.5s-2.0s de delay perceptual.

## Run S001 - 2026-07-05 17:42 - Streaming Defaults

Fuente: `python3 scripts/analyze_run.py` sobre `logs/stt_events.jsonl` y `logs/board_audio.wav`.

### Config

| Parametro | Valor |
| --- | ---: |
| engine | `stream_server` |
| transport | `websocket` |
| model | `small` |
| max_window_sec | `3.0` |
| min_silence_sec | `0.3` |
| partial_sec | `0.5` |
| partial_agreement | `1` |
| beam_size | `5` |
| VAD/filter | `True` |
| lossless | `True` |
| partial backpressure | `True` |

### Resultado

Veredicto subjetivo: mala run; transcript dificil de seguir y peor calidad percibida que el flujo default bueno anterior.

Resumen numerico:

| Area | Resultado |
| --- | --- |
| Audio | `CLIPPING`, 0.044% samples at ceiling |
| Duracion | 115.2s |
| Peak | 100% full scale |
| RMS | -17.5 dBFS |
| Noise floor | p10=-69.0 dBFS, median=-17.9 dBFS, p90=-14.7 dBFS |
| Dynamic range | 54.3 dB |
| Events | 145 total, 31 finals, 114 partials |
| Segment reasons | 31 `max_window`, 114 `partial_tick` |
| Dropped audio jobs | 28 |
| First partial | 0.50s window |
| First final | 3.00s window |
| GPU infer | p50=0.20s, p90=0.34s, max=1.00s |
| Server queue | p50=0.00s, p90=0.00s, max=0.26s |
| Bridge recv lag | p50=0.43s, p90=1.42s, max=4.23s |
| Display spacing | p50=0.52s, p90=0.98s |
| Updates under 1.5s | 138/144 all, 111/114 partials, 27/30 finals |

Transcript sample:

```text
HEAD: Hasta aca ha llegado la participacion para 16 perdio 1 a 0 contra a los octavos de fin del vape de penal...
TAIL: ...en un partido que tuvo de todo, en un partido... que tambien mucho nos quedamos con la imagen de Kylen en el papel.
```

### Lectura

- El pipeline no parece ser el cuello de botella: server queue e inferencia estan bien.
- El cambio de AGC floor parece ayudar al silencio: el p10 de ventanas bajo a -69.0 dBFS, mucho mejor que la run previa con piso ruidoso.
- Todavia hay clipping: 0.044% no es catastrofico, pero queremos llegar a `AUDIO [OK]`.
- El problema dominante es segmentacion: `31/31` finals pegaron en `max_window=3.0s`. El servidor no esta finalizando por silencio.
- Si todos los finals salen por cap, el modelo recibe ventanas cortadas artificialmente cada 3s. Eso puede explicar gran parte de la baja de calidad.
- La UX sigue demasiado cambiante: casi todos los eventos duran menos de 1.5s en pantalla.

### Hipotesis

1. `max_window_sec=3.0` es demasiado agresivo para esta senal y corta frases en lugares malos.
2. `min_silence_sec=0.3` no esta logrando finals por silencio; puede estar demasiado bajo o el detector de silencio/VAD server-side no esta viendo pausas como esperamos.
3. `partial_sec=0.5` + `partial_agreement=1` da lectura viva, pero genera churn visual fuerte y parciales menos estables.
4. El audio mejoro en piso de ruido, pero el clipping restante todavia puede ensuciar palabras puntuales.

### Proximo Experimento Propuesto

Mantener streaming y el AGC floor nuevo, pero volver los knobs temporales mas cerca del default bueno:

| Parametro | Probar |
| --- | ---: |
| max_window_sec | `4.0` |
| min_silence_sec | `0.5` |
| partial_sec | `0.8` |
| partial_agreement | `2` |

Criterios de exito:

- Bajar o eliminar clipping.
- Ver al menos algunos finals por silencio.
- Reducir `max_window` hits.
- Mejorar transcript sin perder completamente la sensacion live.
- Bajar el churn visual o al menos entender cuanto empeora/mejora con partials mas estables.

## Run S002 - 2026-07-05 17:52 - Streaming Defaults, Nueva Muestra

Fuente: `python3 scripts/analyze_run.py` sobre `logs/stt_events.jsonl` y `logs/board_audio.wav`.

### Config

Nota: aunque el experimento propuesto era probar `4.0 / 0.5 / 0.8 / 2`, el log instrumentado reporto la configuracion default streaming. Por lo tanto esta run no valida todavia ese experimento.

| Parametro | Valor |
| --- | ---: |
| engine | `stream_server` |
| transport | `websocket` |
| model | `small` |
| max_window_sec | `3.0` |
| min_silence_sec | `0.3` |
| partial_sec | `0.5` |
| partial_agreement | `1` |
| beam_size | `5` |
| VAD/filter | `True` |
| lossless | `True` |
| partial backpressure | `True` |

### Resultado

Veredicto subjetivo: algo mejor en lectura por presencia de parciales, pero todavia irregular. Hubo cambios de texto demasiado rapidos; algunas lineas parecen durar menos de un segundo.

Resumen numerico:

| Area | Resultado |
| --- | --- |
| Audio | `CLIPPING`, 0.054% samples at ceiling |
| Duracion | 67.0s |
| Peak | 100% full scale |
| RMS | -16.9 dBFS |
| Noise floor | p10=-22.1 dBFS, median=-17.7 dBFS, p90=-14.3 dBFS |
| Dynamic range | 7.8 dB |
| Events | 90 total, 21 finals, 69 partials |
| Segment reasons | 20 `max_window`, 1 `silence`, 69 `partial_tick` |
| Dropped audio jobs | 17 |
| First partial | 0.50s window |
| First final | 3.00s window |
| GPU infer | p50=0.19s, p90=0.40s, max=0.79s |
| Server queue | p50=0.00s, p90=0.00s, max=0.69s |
| Bridge recv lag | p50=0.43s, p90=2.04s, max=3.14s |
| Display spacing | p50=0.52s, p90=1.06s |
| Updates under 1.5s | 83/89 all, 67/69 partials, 16/20 finals |

Transcript sample:

```text
HEAD: juega defendiendose para que se siempre encuentre la manera de puede ganarle igual...
TAIL: ...Obviamente debe haber... el dolor obviamente haber tristeza por un tema de que siempre uno se ilusiona.
```

### Lectura

- La presencia de parciales sigue siendo buena para sensacion live, pero con `partial_sec=0.5` el texto cambia demasiado rapido.
- El problema visual reportado por el usuario se ve en metricas: `p50=0.52s` entre eventos y `83/89` updates duran menos de 1.5s.
- El VAD mejoro apenas respecto a S001 porque aparecio 1 final por `silence`, pero sigue practicamente cap-limited: `20/21` finals por `max_window`.
- El audio empeoro como senal de silencio frente a S001: p10 paso de `-69.0 dBFS` a `-22.1 dBFS`; el analyzer advierte que el VAD puede no ver silencio real.
- El pipeline sigue mayormente bien, pero hubo un backlog puntual de `0.69s` y `bridge recv lag` p90 subio a `2.04s`.
- La transcripcion sigue afectada por cortes artificiales y/o audio ruidoso; no conviene culpar al modelo todavia.

### Hipotesis

1. Esta run no aplico los knobs propuestos; todavia estamos midiendo el perfil agresivo default streaming.
2. El ruido/silencio no fue estable entre runs; S002 tuvo piso muy alto y poco rango dinamico, lo que vuelve a dificultar el VAD.
3. `partial_sec=0.5` garantiza muchos parciales, pero tambien explica lineas que desaparecen en menos de un segundo.
4. La UX necesita una regla de legibilidad ademas de la cadencia STT: no todo evento nuevo deberia forzar reemplazo inmediato si el texto anterior casi no se leyo.

### Proximo Experimento Propuesto

Repetir con Colab reiniciado o celda de config reejecutada, verificando que el analyze reporte exactamente:

| Parametro | Probar |
| --- | ---: |
| max_window_sec | `4.0` |
| min_silence_sec | `0.5` |
| partial_sec | `0.8` |
| partial_agreement | `2` |

Criterios de exito:

- Confirmar en `CONFIG` que los knobs fueron aplicados.
- Reducir finals por `max_window` y aumentar finals por `silence`.
- Bajar churn visual: menos updates bajo 1.5s.
- Mantener suficientes parciales para lectura continua.
- Lograr mejor transcript que S001/S002.

Idea pendiente si el problema visual persiste: agregar una politica de display hold/coalescing para que los parciales no reemplacen texto visible en menos de ~1.0s-1.5s, sin perder finals.

## Run S003 - 2026-07-05 17:59 - Streaming Defaults, Calidad Muy Mala

Fuente: `python3 scripts/analyze_run.py` sobre `logs/stt_events.jsonl` y `logs/board_audio.wav`.

### Config

| Parametro | Valor |
| --- | ---: |
| engine | `stream_server` |
| transport | `websocket` |
| model | `small` |
| max_window_sec | `3.0` |
| min_silence_sec | `0.3` |
| partial_sec | `0.5` |
| partial_agreement | `1` |
| beam_size | `5` |
| VAD/filter | `True` |
| lossless | `True` |
| partial backpressure | `True` |

Comparacion contra commits tempranos del streaming:

| Archivo/commit | Parametros relevantes |
| --- | --- |
| `scripts/stt_stream_server.py` en `ebfbd2e`, `5ebaf2c`, `d0c6ccc`, `39c8f1d` | `small`, `max_window=3.0`, `min_silence=0.35`, `partial=0.5`, `agreement=1`, `beam=5`, `vad_filter=True` |
| `scripts/colab_streaming_server.ipynb` en `d0c6ccc` y `39c8f1d` | `small`, `max_window=3.0`, `min_silence=0.30`, `partial=0.5`, `agreement=1`, `beam=5`, `vad_filter=True` |

Conclusion: no hay evidencia de una regresion de parametros default respecto a los primeros commits del streaming. Los knobs efectivos de esta run coinciden con el perfil inicial/agresivo del notebook.

### Resultado

Veredicto subjetivo: muy mala run; muchas palabras incorrectas y baja inteligibilidad.

Resumen numerico:

| Area | Resultado |
| --- | --- |
| Audio | `CLIPPING`, 0.062% samples at ceiling |
| Duracion | 72.0s |
| Peak | 100% full scale |
| RMS | -16.9 dBFS |
| Noise floor | p10=-21.7 dBFS, median=-17.7 dBFS, p90=-14.9 dBFS |
| Dynamic range | 6.8 dB |
| Events | 106 total, 22 finals, 84 partials |
| Segment reasons | 22 `max_window`, 84 `partial_tick` |
| Dropped audio jobs | 15 |
| First partial | 0.50s window |
| First final | 3.00s window |
| GPU infer | p50=0.20s, p90=0.37s, max=0.81s |
| Server queue | p50=0.00s, p90=0.00s, max=0.37s |
| Bridge recv lag | p50=0.43s, p90=1.44s, max=3.44s |
| Display spacing | p50=0.53s, p90=0.99s |
| Updates under 1.5s | 101/105 all, 80/83 partials, 21/22 finals |

Transcript sample:

```text
HEAD: Senor ha llegado la participacion de pa... Aguay en el Mundial 2026...
TAIL: ...Francia. es una seleccion que tiene muy buenos jugadores para mi la mejor del Mundial hasta ahora...
```

### Lectura

- El pipeline sigue rapido y sin cola sostenida. No parece un problema de GPU ni de WebSocket.
- La captura esta muy mala para STT/VAD: p10=-21.7 dBFS y rango de solo 6.8 dB significa que incluso las ventanas mas "silenciosas" estan muy altas.
- La segmentacion esta completamente cap-limited: `22/22` finals pegaron en `max_window=3.0s`; no hubo ningun final por silencio.
- La mala calidad de palabras probablemente viene de dos fuentes juntas: audio ruidoso/clipeado y frases cortadas artificialmente cada 3s.
- El churn visual sigue muy alto: casi todos los eventos duran menos de 1.5s, y los finals tambien se pisan rapido.
- Comparado contra los primeros commits del streaming, los parametros efectivos son esencialmente los mismos; por ahora el candidato principal no es "cambio de knobs", sino "calidad/estado del audio de esta captura + segmentacion sin silencio".

### Hipotesis

1. El ruido/senal de entrada esta variando muchisimo entre runs. S001 tuvo p10=-69 dBFS; S002/S003 estan cerca de -22 dBFS, que es demasiado alto para silencio.
2. Cuando el piso de ruido sube, el VAD no encuentra pausas y todo cae por `max_window`.
3. Cortar por `max_window=3.0s` cada vez es aceptable como emergencia de latencia, pero no como segmentacion principal para calidad.
4. Aunque el modelo `small` sea el mismo, Whisper recibe ventanas peores que en el flujo HTTP/default: mas ruido y cortes menos naturales.

### Proximo Experimento Propuesto

Antes de tocar modelo, aislar audio/segmentacion:

1. Confirmar en la consola de la board los niveles `in_peak`, `gain`, `out_peak` durante silencio y durante voz.
2. Si el silencio vuelve a tener p10 alrededor de -22 dBFS, no seguir tuneando STT todavia: hay que corregir captura/AGC/fuente.
3. Probar una run con knobs menos agresivos, verificando en `CONFIG`:

| Parametro | Probar |
| --- | ---: |
| max_window_sec | `4.0` |
| min_silence_sec | `0.5` |
| partial_sec | `0.8` |
| partial_agreement | `2` |

4. Si la calidad sigue mala con audio limpio y ventanas mas largas, recien ahi comparar `small` contra un modelo mayor o cambiar estrategia de segmentacion.

## Run S004 - 2026-07-05 18:18 - Knobs 4.0/0.5/0.8/2

Fuente: `python3 scripts/analyze_run.py` sobre `logs/stt_events.jsonl` y `logs/board_audio.wav`.

### Config

Esta run si tomo los knobs del experimento.

| Parametro | Valor |
| --- | ---: |
| engine | `stream_server` |
| transport | `websocket` |
| model | `small` |
| max_window_sec | `4.0` |
| min_silence_sec | `0.5` |
| partial_sec | `0.8` |
| partial_agreement | `2` |
| beam_size | `5` |
| VAD/filter | `True` |
| lossless | `True` |
| partial backpressure | `True` |

### Resultado

Veredicto subjetivo: mejor cadencia visual que S002/S003, pero la transcripcion sigue con errores importantes. El VAD sigue siendo el problema central.

Resumen numerico:

| Area | Resultado |
| --- | --- |
| Audio | `CLIPPING`, 0.042% samples at ceiling |
| Duracion | 93.5s |
| Peak | 100% full scale |
| RMS | -17.0 dBFS |
| Noise floor | p10=-21.7 dBFS, median=-17.7 dBFS, p90=-14.9 dBFS |
| Dynamic range | 6.8 dB |
| Events | 65 total, 24 finals, 41 partials |
| Segment reasons | 21 `max_window`, 3 `silence`, 41 `partial_tick` |
| Dropped audio jobs | 1 |
| First visible partial | 1.60s+ late-ish, first emitted event window 2.40s |
| First final | 4.00s window |
| GPU infer | p50=0.23s, p90=0.35s, max=0.64s |
| Server queue | p50=0.00s, p90=0.00s, max=0.00s |
| Bridge recv lag | p50=0.46s, p90=0.81s, max=1.14s |
| Display spacing | p50=0.90s, p90=2.43s |
| Updates under 1.5s | 42/64 all, 37/41 partials, 5/23 finals |

Transcript sample:

```text
HEAD: igual con dos delanteros con cuatro defensores, lo mas probable es que te comas una bolida...
TAIL: ...la seleccion paraguaya en un partido que tuvo de todo...
```

### Lectura

- Los knobs nuevos si se aplicaron.
- La cadencia visual mejoro: finals bajo 1.5s bajaron de casi todos a `5/23`.
- La cola practicamente desaparecio: `dropped=1`, `server queue max=0.00s`.
- El costo es que el primer parcial visible llega mas tarde: con `partial_sec=0.8` y `agreement=2`, el primer texto estable puede aparecer recien cerca de 1.6s-2.4s.
- El VAD sigue cap-limited: `21/24` finals por `max_window=4.0s`; solo 3 por `silence`.
- El audio sigue sin silencios claros: p10=-21.7 dBFS y rango 6.8 dB, igual de malo que S003 para detectar pausas.

### Hipotesis

1. `partial_sec=0.8` y `agreement=2` son mejores para UX estable, pero pueden sentirse lentos al inicio.
2. El VAD no falla por `min_silence_sec=0.5` solamente; falla porque el input que ve tiene muy poco contraste entre "silencio" y voz.
3. El server aplica una segunda etapa de gain/normalizacion cuando `gain=0.0`; con AGC en la board, esto puede estar ayudando poco o contaminando la decision de VAD.
4. Necesitamos instrumentar el VAD por dentro: cuantos segmentos Silero ve, donde termina el ultimo speech, trailing silence real, y speech ratio por ventana.

### Plan VAD

Paso 1: aislar gain antes de tocar modelo.

- Probar `GAIN = 1.0` en Colab/server para que el VAD use el audio tal cual llega de la board, sin auto-normalizacion server-side.
- Mantener `max_window=4.0`, `min_silence=0.5`, `partial=0.8`, `agreement=2`.
- Criterio: si suben finals por `silence`, la segunda normalizacion estaba perjudicando el VAD.

Paso 2: instrumentar VAD.

- Agregar a eventos: `vad_segment_count`, `vad_speech_ratio`, `vad_last_speech_end_sec`, `trailing_silence_sec` ya existe pero falta reportarlo mejor.
- Agregar al analyze una seccion VAD con stats de trailing silence y ratio de speech.
- Criterio: saber si Silero ve todo como speech continuo o si ve pausas pero no llegan a `min_silence_sec`.

Paso 3: tuning de VAD.

- Exponer `VAD_THRESHOLD` y/o opciones Silero si `faster_whisper.vad.VadOptions` las soporta.
- Probar threshold mas alto para ruido (`0.6`/`0.7`) si Silero marca ruido como speech.
- Probar `min_silence_sec=0.35` con `max_window=4.0` si vemos pausas reales de 0.35-0.5s.

Paso 4: fallback de segmentacion.

- Si Silero sigue viendo todo como speech, agregar detector RMS/noise-floor adaptive como criterio auxiliar de pausa.
- No reemplazar Silero de una; usarlo como "silence candidate" para cortar cuando hay baja energia sostenida.

Paso 5: calidad STT.

- Solo despues de mejorar audio/VAD, comparar `small` contra modelo mayor o `VAD_FILTER=False` en `transcribe`.
- Si los cortes naturales mejoran pero palabras siguen malas, el siguiente sospechoso pasa a ser modelo/config de inferencia.

## Template

```md
## Run SXXX - YYYY-MM-DD HH:MM - Nombre

Fuente:

### Config

### Resultado

### Lectura

### Hipotesis

### Proximo Experimento
```
