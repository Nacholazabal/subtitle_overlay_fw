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
