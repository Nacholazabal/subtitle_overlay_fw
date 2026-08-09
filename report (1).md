# Evaluación final Nemotron sobre MediaSpeech ES

Estado: **complete**

## Identidad congelada

- Dataset: `mediaspeech_es 1.1` (2507 clips; referencias humanas)
- Archive SHA-256: `07917baf12467f1467dd525d1f4747a807ba938e36156382ae5229a89d76bf52`
- Modelo: `nvidia/nemotron-3.5-asr-streaming-0.6b`
- Motor: `nemotron_3_5_nemo` / NeMo `2639d4bef8d1450782263a8f616242acfb6fecb9`
- Idioma: `es-ES`
- Streaming: lookahead `560 ms`, contexto `[56, 6]`, EOU `600 ms`, residuo `2`
- Commit del proyecto: `b2eb5942c689d665054e2d86843966653195fb54`

## Resultados

| Fase | Estado | Clips | WER micro humano | CER micro humano | RTF p50 | RTF p90 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Offline | complete | 2507/2507 | 12.19% | 6.76% | 0.021011 | 0.02521 |
| Streaming acelerado | complete | 2507/2507 | 19.04% | 12.05% | 0.112922 | 0.145978 |

## Degradación streaming

- WER micro streaming contra offline: 13.47%
- Delta macro WER humano streaming-offline (media): 6.87%
- Eventos: 62284; model EOU: 4491; display rollups: 9054; session flushes: 0

## Alcance de lo medido

La fase offline mide la capacidad del modelo sobre el archivo completo. La fase streaming usa la misma `NemotronSession` cache-aware de producción, pero alimenta los frames sin dormir en tiempo real.

> Esta corrida **no** mide todavía latencia física placa→HDMI. El tiempo de primera aparición se expresa en el reloj del audio del modelo y el RTF mide cómputo. El replay físico de una hora queda reservado para la fase 3.

> El final de cada FLAC es una frontera artificial. `session_flush` no se interpreta como EOU acústico; sólo `model_eou` representa una decisión del endpointer.

## Artefactos

- `evaluation.json`: identidad y fingerprint inmutables
- `manifest.jsonl`: pares audio/referencia y hashes
- `offline_results.jsonl`: resultados por clip de fase 1
- `streaming_results.jsonl`: resultados/eventos por clip de fase 2
- `*_progress.json`: checkpoints reanudables
- `summary.json`, `report.md`, `errors.jsonl`: agregados finales
