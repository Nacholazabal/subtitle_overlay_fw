# Resumen del trabajo de STT streaming

> Registro histórico de las etapas Faster-Whisper y SimulStreaming. El runtime
> activo fue retirado al adoptarse Nemotron; la decisión final está en
> `docs/stt_backend_decision.md`.

## Objetivo y arquitectura común

El proyecto captura el audio de una fuente HDMI, lo transcribe fuera de la FPGA y muestra subtítulos sobre el video de salida. El objetivo de diseño es aproximarse a una latencia perceptual de **1,5 s**.

```text
Placa (captura de audio)
  -> PCM por TCP
  -> bridge en PC/WSL
  -> servidor STT en Google Colab
  -> eventos partial/final por WebSocket
  -> bridge
  -> transcripts por TCP + ACK
  -> firmware
  -> overlay HDMI
```

Se implementaron dos servidores Colab alternativos. Ambos usan el mismo bridge, protocolo, firmware y banco de pruebas; sólo cambia cómo se obtiene y estabiliza el texto.

## Backend original: Faster-Whisper/CTranslate2

El primer backend usa **Faster-Whisper**, una implementación de Whisper sobre **CTranslate2**. CTranslate2 es un runtime optimizado para inferencia: reduce el costo computacional y la memoria respecto de ejecutar el modelo PyTorch original. El modelo utilizado fue Whisper multilingual `small`, en GPU, con `float16` y beam search de tamaño 5.

Whisper no es streaming nativo. Para volverlo incremental, nuestro servidor acumula audio y vuelve a transcribir una ventana creciente:

- `max_window_sec`: corte forzado de una frase demasiado larga.
- `min_silence_sec`: silencio requerido para cerrar una frase.
- `partial_sec`: frecuencia con la que se intenta emitir texto parcial.
- `partial_agreement`: cantidad de hipótesis consecutivas que deben coincidir para considerar estable un prefijo.
- `vad_threshold` / `vad_neg_threshold`: sensibilidad de Silero VAD para distinguir voz y silencio.
- `beam_size`: cantidad de alternativas exploradas al decodificar.
- `gain`: ganancia fija o normalización automática del audio.

### Experimentos realizados

Las primeras corridas usaron `window=3,0 s`, `silence=0,3 s`, `partial=0,5 s` y `agreement=1`. Daban sensación de respuesta rápida, pero demasiado cambio visual, cortes artificiales y texto poco estable. También se encontró que el AGC de la placa podía elevar el ruido y dificultar al VAD; al desactivarlo mejoró el contraste entre voz y silencio.

Después se construyó un banco automático que reproduce `desay-short`, `noticiero-short` y `rel-short`, deja 6 s de silencio entre ellos y genera WER/CER, latencias, legibilidad, confiabilidad, audio capturado, eventos y reconstrucción lógica del overlay.

En el sweep de seis configuraciones, `wider_context` (`window=4,0`, `silence=0,5`, `partial=1,0`, `agreement=2`, VAD `0,50/0,35`) obtuvo el mejor WER proxy: **23,52 %**, con p90 de recepción de **0,93 s**, confiabilidad de **100 %** y cero candidatos a alucinación. El barrido posterior mostró que `window=3,0 / silence=0,5` daba mejor legibilidad (**66,67 %**) con WER proxy **25,56 %** y p90 **1,09 s**. Por eso el baseline práctico quedó cerca de `partial=1,0`, `agreement=2`, VAD `0,50/0,35`, variando principalmente ventana y silencio.

Estos WER son contra una transcripción offline automática, no contra texto humano.

## Backend nuevo: SimulStreaming/AlignAtt

Se integró **SimulStreaming** como backend experimental. Usa el checkpoint PyTorch original de Whisper `small`, no el modelo CTranslate2 de Faster-Whisper.

La diferencia central es la política de emisión:

- El backend anterior confirma texto comparando hipótesis sucesivas y depende de ventanas/VAD propios.
- **AlignAtt** consulta la alineación de atención de Whisper y evita emitir tokens demasiado cercanos al borde todavía incompleto del audio.
- **VAC** controla los tramos de voz y los finales de segmento.
- Los commits de AlignAtt son texto ya considerado estable; el adaptador los convierte al mismo formato `partial/final` que entiende el sistema existente.

Se agregaron un notebook de Colab, servidor FastAPI, adaptador y scripts de test específicos, conservando `/health`, `/stt/offline` y `/stt/stream`. El upstream quedó fijado al commit `077ea37d5ab4ff98bc567e4507f140dc4e5d5ad6` y el modelo se valida por SHA-256.

### Parámetros principales de SimulStreaming

- `min_chunk_sec=1,0`: audio mínimo entre procesamientos.
- `beams=1`: decodificación greedy; valores mayores usan beam search.
- `use_vac=true`: activa el controlador de actividad de voz.
- `frame_threshold=25`: AlignAtt deja un margen de 25 frames, equivalentes a **0,50 s**.
- `frame_threshold=12`: alternativa más agresiva, equivalente a **0,24 s**.
- `audio_max_len=30`: límite del buffer de audio.
- `audio_min_len=0`: no exige un buffer adicional antes de decodificar.
- `never_fire=false`: sin CIF, permite la política de truncamiento de la última palabra del upstream.
- `never_fire=true`: impide truncar siempre la última palabra.

Se dejó preparado un sweep de cuatro casos combinando `frame_threshold` 25/12 y `never_fire` false/true. Todavía no constituye un resultado comparativo definitivo.

### Adaptaciones realizadas para el overlay

SimulStreaming entrega commits incrementales y puede acumular segmentos muy largos. Para adaptarlo a la pantalla se implementó una presentación tipo **roll-up**:

- las palabras confirmadas forman la línea actual;
- al llenarse la línea o terminar una oración, se emite un final y sube como contexto;
- la línea nueva continúa debajo;
- el texto enviado se limita al tamaño aceptado por el firmware;
- se conserva el texto completo y el delta original para análisis;
- al cerrar una sesión se fuerza el último final para no perder palabras.

Durante la puesta en marcha también se corrigieron la ruta/nombre del checkpoint, recarga de módulos en Colab, argumentos del factory upstream, anotaciones FastAPI, timestamps numéricos, unión literal de tokens —para no separar palabras como `polic` + `ía`— y campos de confiabilidad del reporte.

## Estado actual de SimulStreaming

La corrida completa `20260722-002450` demostró que la integración técnica funciona:

- 241 eventos generados, enviados y aceptados por la placa;
- ningún rechazo ni ACK perdido;
- latencia de ACK p50 de **18 ms** y máxima de **29 ms**;
- ningún chunk perdido durante esa sesión;
- secuencia y protocolo válidos.

Sin embargo, el rendimiento STT todavía no es bueno: WER proxy global **82,97 %**, p90 de latencia **13,21 s**, tiempo hasta el primer subtítulo **5,77 s**, legibilidad **5,46 %** y real-time factor **1,60**. Hubo picos de inferencia cercanos a 16,5 s. Además, la referencia offline de `desay-short` salió claramente incompleta, por lo que distorsiona el WER global.

Conclusión actual: **el recorrido placa–Colab–firmware está validado, pero SimulStreaming todavía necesita corrección de la referencia offline y tuning/rendimiento antes de compararlo justamente con Faster-Whisper**.

## Qué hace el firmware y qué no hace

El firmware se encarga de:

1. Capturar audio PCM mediante ALSA/USB y enviarlo al bridge en chunks con timestamps, secuencia y contador de pérdidas.
2. Recibir por TCP eventos NDJSON de subtítulos.
3. Crear una sesión nueva por conexión, validar secuencias y responder `session_ready`/`transcript_ack`.
4. Informar si cada evento fue aceptado o descartado por secuencia, pool o cola.
5. Mantener una línea actual y un final anterior como contexto.
6. Mostrar parciales atenuados, finales confirmados, envolver texto en hasta tres líneas y limpiar el overlay tras 5 s sin actualizaciones.
7. Escribir la máscara de caracteres en BRAM y habilitar su composición con el video HDMI.

El firmware **no** ejecuta Whisper, CTranslate2, PyTorch, VAD, VAC ni AlignAtt; tampoco decide qué palabras son correctas. Esas decisiones pertenecen al servidor STT de Colab. Un ACK `accepted` certifica que el evento llegó y fue encolado por el firmware, pero no demuestra por sí solo los píxeles físicos vistos en HDMI.

## Pendientes principales

- Crear las tres transcripciones humanas `.txt` para obtener WER/CER reales.
- Corregir/verificar la referencia offline de SimulStreaming.
- Investigar los picos de inferencia y conseguir `RTF < 1` de forma sostenida.
- Repetir una corrida normal y recién entonces ejecutar el sweep AlignAtt.
- Comparar ambos backends con el mismo audio, modelo, referencias humanas y métricas end-to-end.
