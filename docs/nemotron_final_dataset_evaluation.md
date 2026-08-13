# Evaluación final Nemotron con MediaSpeech ES

Esta ruta mide el modelo seleccionado sobre un corpus humano externo sin tocar
los scripts de prueba corta ni los sweeps que se usaron para elegir parámetros.
Es una evaluación fija: no debe usarse para volver a ajustar Nemotron y luego
presentar el mismo corpus como test no visto.

## Qué ejecuta ahora

La notebook `server/notebooks/nemotron_dataset_eval.ipynb` hace, en orden:

1. **Offline completo:** transcribe los 2.507 FLAC con el checkpoint completo y
   compara cada salida contra su TXT humano.
2. **Streaming acelerado:** abre una sesión cache-aware de producción por FLAC,
   alimenta sus frames sin pausas artificiales y compara el resultado contra el
   humano y contra la salida offline del paso anterior.

No levanta FastAPI, WebSocket ni ngrok. Tampoco requiere placa, bridge o
firmware. La segunda fase llama directamente a
`SharedNemotronModel.build_session()` y, por lo tanto, conserva el chunking,
caches, prompt español, endpointer y adaptación de transcripts que utiliza el
servidor real.

El replay físico constituye la tercera fase y conserva su propio reporte, sin
recalcular ni mezclar las métricas de estas dos fases. Sus resultados finales se
documentan en `docs/nemotron_physical_evaluation.md`.

## Configuración congelada

```text
engine                       = nemotron_3_5_nemo
model                        = nvidia/nemotron-3.5-asr-streaming-0.6b
target_lang                  = es-ES
decoder                      = greedy_batch RNN-T
sample_rate_hz               = 16000
latency_ms / lookahead       = 560
att_context_size             = [56, 6]
stop_history_eou_ms          = 600
residue_tokens_at_end        = 2
compute_dtype                = float32
AMP                          = true
NeMo commit                  = 2639d4bef8d1450782263a8f616242acfb6fecb9
```

El runner rechaza otra configuración. También guarda un fingerprint del corpus,
modelo, commit de NeMo, commit del proyecto y configuración. Si alguno cambia,
no permite anexar datos a la carpeta existente.

## Preparación de Drive

Subir únicamente el tar original a:

```text
MyDrive/
  TESIS/
    stt_benchmarks/
      mediaspeech_es/
        v1.1/
          ES.tgz
```

El archivo correcto tiene:

```text
SHA-256 = 07917baf12467f1467dd525d1f4747a807ba938e36156382ae5229a89d76bf52
tamaño  = 582251553 bytes
```

No conviene subir los 5.014 FLAC/TXT sueltos. La notebook verifica el hash,
copia el tar a `/content`, lo extrae allí y accede a Drive sólo para los caches y
resultados persistentes.

## Ejecución

1. Pushear esta implementación a `dev/nemotron`.
2. Abrir `server/notebooks/nemotron_dataset_eval.ipynb` en Colab.
3. Seleccionar un runtime con GPU.
4. Ejecutar `Runtime -> Run all`.
5. Dejar que termine offline y luego streaming.

Si Colab corta, ejecutar `Run all` otra vez. Cada JSONL es append-only y se
confirma un checkpoint cada 25 clips. Los clips cuyo último registro es exitoso
se omiten; los que fallaron se vuelven a intentar. La fase streaming no comienza
hasta que los 2.507 resultados offline estén completos.

Los tiempos orientativos observados antes de esta implementación son:

- offline: aproximadamente 30–60 minutos;
- streaming acelerado: aproximadamente 2–3 horas;
- primera carga/instalación de NeMo: varios minutos adicionales.

No son límites; la GPU asignada y Drive pueden cambiar esos tiempos.

## Resultados persistentes

La carpeta fija es:

```text
MyDrive/TESIS/stt_evaluations/mediaspeech_es/
  mediaspeech-es-v1.1__nemotron-560-600-2__v1/
```

Contiene:

| Archivo | Contenido |
| --- | --- |
| `evaluation.json` | identidad inmutable y fingerprint |
| `model_provenance.json` | GPU, torch, NeMo, modelo y revisión resuelta |
| `manifest.jsonl` | hashes y referencias humanas por clip |
| `offline_results.jsonl` | texto, WER/CER y RTF offline por clip |
| `streaming_results.jsonl` | texto, eventos, finales y RTF streaming por clip |
| `offline_progress.json` | checkpoint de fase 1 |
| `streaming_progress.json` | checkpoint de fase 2 |
| `summary.json` | métricas agregadas machine-readable |
| `report.md` | resumen legible |
| `errors.jsonl` | historial de fallos, incluso si un resume posterior los recuperó |

WER/CER se calculan contra las transcripciones humanas de MediaSpeech, no contra
una pseudorreferencia. El reporte conserva además streaming contra offline para
cuantificar la degradación incremental.

## Qué significa —y qué no— la fase acelerada

Sí permite medir:

- WER/CER streaming reales contra humano;
- degradación contra el techo offline del mismo modelo;
- RTF y throughput;
- parciales, revisiones, rollups y EOU del modelo;
- instante de audio en que aparece el primer evento.

No permite medir latencia wall-clock placa→HDMI, ACK del firmware ni píxeles
físicos. Esas propiedades pertenecen al replay físico posterior.

MediaSpeech suele cortar los clips en mitad de una oración. Por eso el runner
distingue siempre `model_eou` de `session_flush`: el segundo es sólo el final
artificial del archivo y no se presenta como detección acústica de fin de frase.

## Fase 3: replay físico

La etapa física fijó una selección reproducible, creó un cue-sheet con orden y
silencios, reprodujo el conjunto en tiempo real hacia la placa y guardó audio,
eventos y ACK. Alcanzó 209 clips completos y 49,71 minutos de habla, con WER
25,95% y CER 19,31%. No reemplaza las métricas offline/streaming anteriores: las
complementa con el recorrido físico end-to-end.
