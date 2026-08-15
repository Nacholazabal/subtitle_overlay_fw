# Servidor STT Nemotron

Esta carpeta contiene todo el software que corre fuera del firmware para el
motor STT final de la tesis. El backend activo es Nemotron 3.5 ASR Streaming
ejecutado con NVIDIA NeMo en Colab.

## Entradas principales

| Acción | Entrada |
| --- | --- |
| Levantar el servidor en Colab | `notebooks/nemotron_server.ipynb` |
| Instalar/actualizar el servicio autónomo de la placa | `./scripts/run.sh -s` |
| Probar los tres audios cortos | `./server/audio-test.sh` |
| Ejecutar el replay físico del corpus | `./server/physical-eval.sh` |
| Analizar una captura | `./server/analyze.sh` |
| Evaluar el corpus en Colab | `notebooks/nemotron_dataset_eval.ipynb` |

El uso normal es ejecutar toda la notebook del servidor y esperar
`HEALTH: ready`. Si el servicio ya está instalado, la placa se conecta sola;
no se lanza ningún bridge desde WSL. `./scripts/run.sh -s` se usa sólo al
instalar o actualizar el ejecutable de la placa. El banco histórico
`./server/audio-test.sh` conserva el bridge de evaluación y requiere un firmware
compatible con sus puertos TCP; no forma parte del camino autónomo.

## Estructura

- `runtime/`: servidor FastAPI/WebSocket, inferencia Nemotron, protocolo y el
  bridge conservado exclusivamente para evaluación.
- `evaluation/`: manifest y runners del corpus MediaSpeech y del replay físico.
- `audio_tests/`: banco de tres audios, sweeps, análisis y reconstrucción lógica
  del overlay.
- `notebooks/`: puntos de entrada de Colab.
- `tests/`: pruebas Python del servidor y sus evaluaciones.

Los scripts de build, ejecución, coverage y lint del firmware permanecen en
`scripts/`. La historia de los motores descartados está en
`docs/stt_backend_decision.md`; no forman parte del runtime.
