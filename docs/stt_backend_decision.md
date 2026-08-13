# Decisión del backend STT

## Estado final

El servidor de producción de la tesis utiliza
`nvidia/nemotron-3.5-asr-streaming-0.6b` mediante la pipeline cache-aware oficial
de NVIDIA NeMo. La configuración seleccionada es:

- idioma `es-ES`;
- decoder RNN-T;
- lookahead 560 ms, contexto `[56, 6]`;
- `stop_history_eou_ms=600`;
- `residue_tokens_at_end=2`;
- NeMo fijado en `2639d4bef8d1450782263a8f616242acfb6fecb9`.

El código activo vive en `server/`. Faster-Whisper y SimulStreaming/AlignAtt se
retiraron del runtime, launchers, notebooks y tests al cerrar la selección del
motor. Se conserva únicamente su precedente documental y los resultados ya
generados.

## Precedente

La primera implementación utilizó Faster-Whisper/CTranslate2 con segmentación,
VAD y parciales administrados por el servidor. Permitió construir el protocolo,
la captura, el bridge, las métricas WER/CER y el banco físico, pero exigía
mantener una política incremental propia.

Luego se evaluó SimulStreaming con AlignAtt para comparar una política de
streaming publicada sobre Whisper. Su investigación, montaje y parámetros
quedaron registrados en:

- `docs/investigacion_stt_streaming_2026-07-24.md`;
- `docs/resumen_stt_streaming_y_simulstreaming.md`;
- `docs/simulstreaming_colab_setup.md` (archivo histórico, no ejecutable).

Finalmente se adoptó Nemotron porque ofrece inferencia RNN-T cache-aware y
endpointing como parte de una pipeline mantenida por NeMo. Los resultados y la
metodología final se encuentran en:

- `docs/nemotron_colab_setup.md`;
- `docs/nemotron_final_dataset_evaluation.md`;
- `docs/nemotron_physical_evaluation.md`.

La medición consolidada conservada es 12,19% WER para archivo completo, 19,04%
para streaming digital y 25,95% para replay físico por la placa.
