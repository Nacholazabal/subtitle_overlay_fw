# Nemotron 3.5 + NeMo en Colab

Esta es la primera etapa del experimento Nemotron. Valida instalación, modelo,
audios y streaming cache-aware directamente en Colab antes de agregar otro
servidor live al sistema.

En esta etapa no intervienen la placa, el bridge, el firmware ni ngrok.

## Qué está implementado

- Notebook: `scripts/colab_nemotron_probe.ipynb`.
- Modelo: `nvidia/nemotron-3.5-asr-streaming-0.6b`.
- Runtime: NVIDIA NeMo Speech desde el repositorio oficial.
- Idioma fijo: `es-ES`.
- Decoder: RNNT.
- Contexto inicial: `[56,3]`, equivalente al punto de 320 ms publicado para el
  modelo.
- Entrada: los tres `*-short.webm` ya utilizados por el banco de pruebas.
- Salida: transcripción de archivo completo, simulación streaming oficial,
  provenance y logs bajo Google Drive.

El modelo y la relación entre contexto y latencia están documentados en:

- <https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b>
- <https://docs.nvidia.com/nemo/speech/nightly/asr/inference.html>

## Preparación de Google Drive

No es necesario subir manualmente el checkpoint. La notebook lo descarga y lo
cachea en Drive.

El checkpoint es público. De todos modos, si existe un secret de Colab llamado
`HF_TOKEN`, la notebook lo usa para evitar límites de descarga sin imprimirlo ni
guardarlo en los resultados.

Los audios pueden continuar en la ubicación de SimulStreaming:

```text
MyDrive/TESIS/simulstreaming/audio/
├── desay-short.webm
├── noticiero-short.webm
└── rel-short.webm
```

La notebook también busca automáticamente:

```text
MyDrive/TESIS/stt-bench/audio/
MyDrive/TESIS/nemotron/audio/
MyDrive/Tesis-subtitles/simulstreaming/audio/
```

Si los archivos están en otra ubicación, editar solamente `AUDIO_DIR` en la
segunda celda de configuración.

Un `.txt` junto a un audio se agrega al manifest como `provided_txt`. Su ausencia
no bloquea la prueba. En ninguno de los casos se presenta esa referencia como
transcripción humana certificada.

Los caches y resultados se crean en:

```text
MyDrive/TESIS/nemotron/
├── cache/
│   ├── huggingface/
│   ├── nemo/
│   └── torch/
└── results/<timestamp>/
    ├── provenance.json
    ├── offline.json
    ├── streaming.log
    └── streaming-summary.json
```

## Cómo correrla

1. Desde WSL, pushear la rama para que Colab pueda clonarla:

   ```bash
   git push -u origin dev/nemotron
   ```

2. Abrir `scripts/colab_nemotron_probe.ipynb` en Colab.
3. Seleccionar una GPU T4 o mejor.
4. Ejecutar `Runtime -> Run all`.
5. Autorizar el montaje de Google Drive.
6. Esperar la descarga inicial del checkpoint y la instalación de NeMo.

Antes de preparar los audios, la notebook comprueba físicamente que el checkout
contenga `scripts/stt_nemotron_probe.py`, coloca el repo primero en `sys.path` y
elimina cualquier módulo previo llamado `scripts` que Colab haya conservado en
memoria. Esto evita importar por accidente un paquete homónimo o una copia vieja.

La primera ejecución es lenta porque instala NeMo y descarga aproximadamente
2.4 GB. Las siguientes reutilizan los pesos desde Drive, aunque NeMo se vuelve a
instalar en cada runtime efímero.

La celda de instalación agrega el checkout editable de NeMo al `sys.path` del
kernel actual y comprueba inmediatamente `import nemo.collections.asr`. Si esa
comprobación falla, la notebook se detiene ahí con el error real de instalación,
en lugar de avanzar hasta la carga del modelo.

## Qué debe demostrar esta etapa

Se considera exitosa cuando:

- la GPU es detectada;
- el checkpoint carga con soporte de `set_inference_prompt`;
- los tres audios se transcriben con `es-ES`;
- el script oficial cache-aware termina con código cero;
- el log muestra hipótesis incrementales y una transcripción final;
- no hay crecimiento anormal de memoria ni un tiempo total claramente mayor a
  la duración acumulada de los audios;
- se guardan las versiones y commits exactos utilizados.

`320 ms` es el contexto futuro algorítmico del modelo, no la latencia total hasta
el HDMI. La latencia end-to-end se medirá recién cuando el backend live use el
bridge y los ACK del firmware.

## Siguiente etapa, si pasa

Con un resultado exitoso se fijará el SHA de NeMo que funcionó y se implementará:

1. `stt_nemotron_backend.py`, con una sesión cache-aware persistente;
2. `stt_nemotron_server.py`, conservando `/health`, `/stt/offline` y
   `/stt/stream`;
3. el launcher y el perfil `audiotestnemotron.sh`;
4. la medición end-to-end contra Faster-Whisper y SimulStreaming.

No se modificará el firmware para esa integración.
