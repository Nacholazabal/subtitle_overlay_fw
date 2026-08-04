# Nemotron 3.5 + NeMo en Colab

Este documento cubre las dos etapas del experimento Nemotron:

1. **Probe** (`scripts/colab_nemotron_probe.ipynb`): valida instalación, modelo,
   audios y streaming cache-aware en Colab, sin placa ni bridge. **Ya ejecutado
   con éxito.**
2. **Servidor live** (`scripts/colab_nemotron_server.ipynb`): tercer backend STT
   conectado al bridge y al firmware existentes. Ver
   [Etapa 2: servidor live](#etapa-2-servidor-live).

---

## Etapa 1: probe

Valida instalación, modelo, audios y streaming cache-aware directamente en Colab
antes de agregar otro servidor live al sistema.

En esta etapa no intervienen la placa, el bridge, el firmware ni ngrok.

### Qué está implementado

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

Para la transcripción de archivo completo se desactiva el dataloader Lhotse de
esta revisión de NeMo. Con una lista simple de paths, ese dataloader genera
supervisiones sin idioma (`prompt=None`) para el modelo prompt-aware. La ruta
normal de NeMo usa el `target_lang=es-ES` del config para generar el prompt
dinámicamente y evita esa pérdida de información.

El modelo y la relación entre contexto y latencia están documentados en:

- <https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b>
- <https://docs.nvidia.com/nemo/speech/nightly/asr/inference.html>

### Preparación de Google Drive

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

### Cómo correrla

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
memoria. El `scripts/__init__.py` del proyecto además lo convierte en un paquete
explícito, evitando que un paquete homónimo instalado por NeMo tenga prioridad
sobre el namespace del repositorio.

La primera ejecución es lenta porque instala NeMo y descarga aproximadamente
2.4 GB. Las siguientes reutilizan los pesos desde Drive, aunque NeMo se vuelve a
instalar en cada runtime efímero.

La celda de instalación agrega el checkout editable de NeMo al `sys.path` del
kernel actual y comprueba inmediatamente `import nemo.collections.asr`. Si esa
comprobación falla, la notebook se detiene ahí con el error real de instalación,
en lugar de avanzar hasta la carga del modelo.

### Qué debe demostrar esta etapa

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

### Resultado del probe

El probe pasó. El SHA de NeMo quedó fijado en
`2639d4bef8d1450782263a8f616242acfb6fecb9` y con eso se construyó la etapa 2.

---

## Etapa 2: servidor live

Tercer backend STT, en paralelo a faster-whisper y SimulStreaming. Cambia
**solamente** el motor de inferencia en Colab. La placa, el bridge
(`stt_stream_bridge.py`), el protocolo de sesión (`stt_stream_protocol.py`), los
ACK del firmware y el overlay HDMI se reutilizan sin ninguna modificación.

```text
placa → bridge actual → WebSocket Colab → Nemotron/NeMo
      ← transcripts ← bridge ← ACK firmware ← overlay HDMI
```

### Archivos

| Archivo | Rol |
| --- | --- |
| `scripts/stt_nemotron_backend.py` | Config, provenance, sesión cache-aware, adapter de transcripts, offline |
| `scripts/stt_nemotron_server.py` | `GET /health`, `POST /stt/offline`, `WS /stt/stream` |
| `scripts/colab_nemotron_server.ipynb` | Notebook live (GPU → Drive → repo → NeMo pin → carga → uvicorn → ngrok) |
| `scripts/run_stt_colab_nemotron.sh` | Launcher del bridge (copia de `run_stt_colab_simulstream.sh`) |
| `scripts/audiotestnemotron.sh` | Wrapper fino del banco de pruebas, perfil `nemotron_3_5_nemo` |
| `test/test_stt_nemotron_backend.py`, `test/test_stt_nemotron_server.py` | Tests sin GPU |

### Motor de inferencia

Se usa la API oficial de inferencia streaming de NeMo del commit fijado, no un
subprocess por sesión y no una reimplementación:

| Pieza | API oficial usada |
| --- | --- |
| Construcción | `nemo.collections.asr.inference.factory.pipeline_builder.PipelineBuilder` → `CacheAwareRNNTPipeline` |
| Config | Misma forma que `examples/asr/conf/asr_streaming_inference/cache_aware_rnnt.yaml` |
| Sesión | `pipeline.open_session()` / `transcribe_step([Frame])` / `close_session()` |
| Idioma | `ASRRequestOptions.language_code='es-ES'` (prompt oficial del checkpoint) |
| Fin de frase | `RNNTGreedyEndpointing`, vía `endpointing.stop_history_eou` |

Los caches del encoder y las hipótesis previas viven en el estado por stream del
pipeline: no se recalcula audio pasado ni se lanza un proceso por sesión.

**El EOU oficial existe y es compatible con este checkpoint.** El pipeline
cache-aware es prompt-aware (`prompt_enabled`, `_build_prompt_vectors`), lo que
cubre `EncDecRNNTBPEModelWithPrompt`. Un EOU se manifiesta como
`TranscribeStepOutput.final_transcript` no vacío; el texto en curso llega como
`partial_transcript`. No se agregó Silero, WebRTC VAD, LocalAgreement, AlignAtt ni
ninguna heurística para decidir el EOU.

**Ojo con el default de idioma:** si no se pasa `language_code`, el pipeline usa
`en-US`. El backend siempre lo fija explícitamente en `es-ES`.

### Compatibilidad del prompt en NeMo fijado

El `CacheAwareRNNTPipeline` del commit fijado construye correctamente el vector
one-hot de idioma, pero su `CacheAwareRNNTInferenceWrapper.execute_step()` recibe
`prompt_vectors` y no lo aplica antes del decoder. En un checkpoint condicionado
por idioma esto no genera una excepción: el decoder puede devolver únicamente
blanks durante toda la sesión.

`stt_nemotron_backend.py` instala una corrección local y acotada sobre la instancia
del wrapper: concatena el prompt entregado por el propio pipeline al encoder output
y lo proyecta con `model.prompt_kernel`, exactamente como
`PromptStreamingMixin._apply_prompt_to_encoded`. El estado de `/health` y la
provenance exponen `prompt_projection_compat=true` mientras este workaround sea
necesario.

El arranque de Colab ya no considera suficiente un warmup con silencio. Antes de
abrir ngrok procesa los primeros 12 s de `desay-short.webm` desde Drive mediante la
misma sesión incremental del servidor y exige al menos un evento. Si el pipeline
vuelve a producir solamente blanks, queda en `failed` y no anuncia un falso
`HEALTH: ready`.

### Configuración inicial fija

- `target_lang=es-ES`
- decoder RNNT (`greedy_batch`)
- `att_context_size=[56,3]` (punto publicado de 320 ms)
- `stop_history_eou=800 ms`, `residue_tokens_at_end=2` (defaults oficiales)
- 16 kHz, tags `<es-ES>` eliminados
- `compute_dtype=float32` + AMP, igual que el probe en T4

Sin beam search, sin auto-language, sin context biasing, sin ITN y sin traducción.

`320 ms` es el **lookahead algorítmico del modelo**, no la latencia end-to-end
hasta el HDMI. En `/health`, en los eventos y en el summary aparece como
`lookahead_ms` justamente por eso.

### Display y partials

El adapter reutiliza la política de display del backend SimulStreaming (mismo
`bounded_tail` / `split_at_width` / límites de línea), adaptada a la salida
append-only de Nemotron: NeMo entrega la utterance completa en cada paso, no un
delta, así que el adapter lleva la cuenta de cuántas palabras ya se promovieron a
líneas finalizadas y sólo muestra el resto. Así una utterance de 60 s nunca llega
al firmware como una sola línea de 60 s.

Además: suprime partials idénticos, elimina `<es-ES>`, normaliza whitespace,
conserva puntuación y capitalización del modelo, y nunca reescribe texto ya
mostrado (si NeMo revisara un partial, se resincroniza por prefijo común y lo
cuenta en `partial_revisions`).

El protocolo distingue explícitamente tres motivos que el firmware representa
con `is_final=true`:

- `final_reason=model_eou`: `RNNTGreedyEndpointing` cerró la utterance;
- `final_reason=display_rollup`: el adapter promovió una línea por ancho o
  puntuación para mantener legible el overlay;
- `final_reason=session_flush`: cierre final al terminar la conexión.

Sólo `model_eou` cuenta como EOU del modelo. El roll-up es una política de
presentación copiada de los backends anteriores, no una decisión acústica.

Los overrides de `latency_ms`, `stop_history_eou_ms` y
`residue_tokens_at_end` reconfiguran el pipeline/endpointer entre sesiones. El
servidor permite una única sesión GPU activa, evitando que esa reconfiguración
comparta estado con otro stream.

### Timestamps

Siempre numéricos. Cada evento indica su origen en `timestamp_source`:

- `nemo_segments`: vienen de los `TextSegment` del EOU;
- `sample_clock`: derivados del conteo de muestras del stream.

El endpoint offline nunca fabrica timestamps: si NeMo no los da, devuelve
`segments: []` y `segments_source: "unavailable"`.

### Cómo correrlo

1. Desde WSL, pushear la rama:

   ```bash
   git push -u origin dev/nemotron
   ```

2. Abrir `scripts/colab_nemotron_server.ipynb` en Colab, GPU T4 o mejor,
   `Runtime -> Run all`.
3. Esperar a que imprima `HEALTH: ready` y las URLs. ngrok se levanta **después**
   de la readiness real; usa el dominio reservado
   `passage-capacity-wistful.ngrok-free.dev` (secret `NGROK_AUTHTOKEN`, opcional
   `NGROK_DOMAIN`).
4. Desde WSL:

   ```bash
   ./scripts/audiotestnemotron.sh
   ```

El banco aborta antes de reproducir audio si `/health` no reporta
`run_engine=nemotron_3_5_nemo`, así que nunca se mide el backend equivocado.

### Sweep

El sweep inicial está en `scripts/sweeps/nemotron_initial.json` y se ejecuta con:

```bash
./scripts/audiotestnemotron.sh --sweep
```

Compara los puntos publicados de lookahead 80, 160, 320 y 560 ms, manteniendo
fijos `stop_history_eou_ms=800`, `residue_tokens_at_end=2` y `target_lang=es-ES`.
El punto de 320 ms se intercala tres veces para medir variabilidad y detectar
deriva por orden de ejecución. Se excluye inicialmente 1120 ms porque su
lookahead consume por sí solo casi todo el objetivo end-to-end de 1,5 s.

El reporte agregado tiene tablas específicas de Nemotron: WER/CER proxy,
porcentaje bajo 1,5 s, p90/p95, tiempo hasta el primer subtítulo, p90 por audio,
deriva temporal, EOU del modelo, rollups, frecuencia de parciales y entrega ACK
a firmware. La confiabilidad sigue siendo estricta: cualquier ACK desconocido o
rechazo invalida esa corrida, aunque también se muestra el porcentaje efectivo
aceptado para explicar el resultado. El sweep continúa con los casos restantes y
termina como `complete_with_invalid_runs`; una corrida inválida nunca participa
en la selección de mejores casos.

Las referencias siguen siendo `offline_proxy`; el sweep sirve para seleccionar
una región prometedora, no para afirmar error real contra transcripción humana.
Cada lookahead genera/reutiliza su proxy offline con ese mismo punto operativo,
por lo que WER/CER expresan degradación live contra el proxy correspondiente y
no una comparación absoluta contra una única referencia humana común.

### Qué NO se tocó

Firmware, protocolo de la placa, `stt_stream_bridge.py`, los servidores
faster-whisper y SimulStreaming, y los defaults existentes del banco de pruebas.
No se agregaron endpoints, autenticación, Docker ni base de datos.
