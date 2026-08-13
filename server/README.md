# Servidor STT Nemotron

Esta carpeta contiene todo el software que corre fuera del firmware para el
motor STT final de la tesis. El backend activo es Nemotron 3.5 ASR Streaming
ejecutado con NVIDIA NeMo en Colab.

## Entradas principales

| Acción | Entrada |
| --- | --- |
| Levantar el servidor en Colab | `notebooks/nemotron_server.ipynb` |
| Conectar placa, Colab y subtítulos | `./server/run.sh` |
| Probar los tres audios cortos | `./server/audio-test.sh` |
| Ejecutar el replay físico del corpus | `./server/physical-eval.sh` |
| Analizar una captura | `./server/analyze.sh` |
| Evaluar el corpus en Colab | `notebooks/nemotron_dataset_eval.ipynb` |

El uso normal es ejecutar toda la notebook del servidor, esperar
`HEALTH: ready` y luego lanzar `./server/run.sh` desde WSL. Para las pruebas
cortas se usa `./server/audio-test.sh`; ese wrapper levanta el mismo bridge y
consume el mismo protocolo de producción.

## Estructura

- `runtime/`: servidor FastAPI/WebSocket, inferencia Nemotron, bridge, protocolo
  y transporte TCP de subtítulos con ACK del firmware.
- `evaluation/`: manifest y runners del corpus MediaSpeech y del replay físico.
- `audio_tests/`: banco de tres audios, sweeps, análisis y reconstrucción lógica
  del overlay.
- `notebooks/`: puntos de entrada de Colab.
- `tests/`: pruebas Python del servidor y sus evaluaciones.

Los scripts de build, ejecución, coverage y lint del firmware permanecen en
`scripts/`. La historia de los motores descartados está en
`docs/stt_backend_decision.md`; no forman parte del runtime.
