# Evaluación física final de Nemotron

Esta es la fase física, separada de `server/audio-test.sh`, de los sweeps y de la
evaluación offline/streaming acelerada. Los resultados ya obtenidos no se
recalculan ni se modifican.

## Resultados finales consolidados

La evaluación conserva como resultados finales los siguientes valores:

| Camino | WER humano micro | CER humano micro |
| --- | ---: | ---: |
| Archivo completo directo al modelo | 12,19% | 6,76% |
| Streaming digital, sin placa | 19,04% | 12,05% |
| Replay físico pasando por la placa | 25,95% | 19,31% |

La diferencia entre archivo completo y streaming digital es de **6,85 puntos
porcentuales de WER** y **5,29 puntos porcentuales de CER**. La diferencia entre
streaming digital y replay físico es de **6,91 puntos porcentuales de WER** y
**7,26 puntos porcentuales de CER**.

La corrida física produjo 3.251,74 s de audio capturado. El análisis utiliza los
209 clips enteros comprendidos en ese intervalo, equivalentes a 49,71 minutos
de habla. Sobre el mismo alcance, la salida confirmada mediante ACK del firmware
obtuvo WER 26,43% y CER 19,94%. Fueron aceptados 5.064 de 5.099 eventos; no hubo
drops de audio.

El WER/CER físico se calcula como transcript continuo: se concatenan en orden
las referencias humanas de los 209 clips y los finales cronológicos del modelo.
Esto evita introducir errores artificiales cuando un rollup de Nemotron cruza
el límite entre dos clips. El reporte completo está en
`logs/physical-evals/20260809-185650/partial-report.md`.

## Interpretación del costo de streaming

La pérdida respecto a archivo completo es el costo medido de ejecutar el modelo
bajo el contrato de producción incremental. En modo offline, `transcribe()`
recibe el archivo entero. En streaming, la pipeline cache-aware recibe bloques
no solapados, conserva estado y sólo dispone de 560 ms de contexto derecho;
además debe decidir cierres mediante el endpointer y publicar únicamente texto
confirmado.

Por lo tanto, que el modelo sea streaming nativo significa que evita recalcular
ventanas solapadas y ofrece una curva configurable entre latencia y exactitud;
no significa que la salida incremental deba coincidir con la transcripción del
archivo completo. Para este corpus, la cifra representativa del servidor de
producción es 19,04% WER, mientras que 12,19% expresa la capacidad del mismo
checkpoint cuando dispone del archivo completo.

## Corte metodológico respecto a la corrida congelada

La fase offline congelada (WER humano micro 12,19% y CER 6,76%) sigue siendo la
referencia válida de calidad sobre archivos completos. La fase streaming
acelerada congelada se conserva como evidencia histórica, pero fue producida
antes de corregir dos problemas del adaptador: una revisión al cerrar una frase
podía reenviar texto ya promovido y el último frame de archivo se contaba como
EOU acústico. Por decisión experimental no se vuelve a ejecutar ni se altera
ese artefacto. La corrida física documentada aquí será la medición streaming
final del adaptador corregido contra referencias humanas.

El subconjunto físico se elige únicamente por duración, velocidad de habla y un
orden SHA-256 fijo. No usa WER, CER ni hipótesis antiguas para seleccionar casos.

## Configuración congelada

- Modelo: `nvidia/nemotron-3.5-asr-streaming-0.6b`
- NeMo: `2639d4bef8d1450782263a8f616242acfb6fecb9`
- Idioma: `es-ES`
- Lookahead: 560 ms (`[56, 6]`)
- `stop_history_eou`: 600 ms
- `residue_tokens_at_end`: 2

## Preparación local única

No se necesita Colab, GPU ni NeMo para preparar el replay. Con `ES.tgz`
verificado y extraído, ejecutar localmente:

```bash
python3 server/evaluation/prepare_physical.py \
  --dataset-root /ruta/al/ES-extraido \
  --output-dir /ruta/al/physical-v1
```

La CLI usa solamente Python estándar y `ffmpeg`: lee duración desde STREAMINFO
de FLAC, calcula velocidad con las referencias humanas, selecciona
aproximadamente 60 minutos mediante estratos fijos y genera:

- `physical-replay.flac`;
- `cue-sheet.json`, con cada referencia humana y su posición exacta;
- `bundle.json`, con hashes de integridad.

Si se dispone del directorio completo de la evaluación congelada, se puede
agregar `--evaluation-dir`; no es necesario para la selección ni para las
métricas físicas.

La selección es determinista y no utiliza el WER del modelo, para evitar elegir
clips a favor o en contra del resultado.

## Replay físico

Con la carpeta local del bundle pronta, Colab Nemotron activo, placa/HDMI/audio
conectados y el firmware ejecutándose:

```bash
./server/physical-eval.sh --bundle /mnt/c/ruta/al/bundle
```

También se puede fijar una vez:

```bash
export NEMOTRON_PHYSICAL_BUNDLE=/mnt/c/ruta/al/bundle
./server/physical-eval.sh
```

La corrida espera simultáneamente audio de la placa, sesión Colab y handshake
del canal de subtítulos. Reproduce una sola pista, captura el PCM real, conserva
eventos y ACKs y escribe bajo `logs/physical-evals/<timestamp>/`:

- `report.md` y `report.json`;
- `live/events.jsonl` y `live/board_acks.jsonl`;
- `live/board_audio.wav` y `live/bridge.log`;
- `visual-observation.md` para registrar la observación HDMI.

El análisis conserva la correlación de forma de onda como diagnóstico. Para la
corrida final parcial, el alcance se corta mediante los relojes de reproducción
y captura y el WER/CER se calcula sobre el transcript continuo contra las
referencias humanas.
La latencia automática principal va desde que el audio final declarado por un
evento estuvo disponible en el bridge hasta que firmware respondió
`transcript_ack: accepted`. La primera aparición desde el inicio de clip se
informa por separado; no equivale a latencia exacta desde el inicio de cada
palabra porque no hay forced alignment por palabra.

Un ACK aceptado certifica recepción y encolado en firmware, no los píxeles HDMI;
esa última evidencia queda en la observación visual.
