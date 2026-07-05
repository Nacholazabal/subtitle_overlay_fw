# Streaming STT Setup

Esta es la ruta nueva para bajar el overhead de Colab/HTTP y acercarnos al objetivo final:

```text
Board -> PC bridge -> Colab/VPS WebSocket STT server -> PC bridge -> Board
```

La PC sigue existiendo sólo como puente de debug. El servidor ya usa el protocolo final: una sesión WebSocket full-duplex que recibe audio PCM y devuelve eventos de subtítulos por el mismo canal.

## 1. Colab

1. Abrir `scripts/colab_streaming_server.ipynb` en Google Colab.
2. Runtime -> Change runtime type -> GPU.
3. Correr las celdas.
4. Copiar el `WebSocket:` que imprime al final, por ejemplo:

```text
wss://abc123.ngrok-free.dev/stt/stream
```

El notebook levanta `scripts/stt_stream_server.py`, no una copia paralela de la lógica.

## 2. WSL / PC Bridge

El Python de Windows que abre `run_stt_colab_stream.sh` necesita `websockets`:

```powershell
pip install websockets
```

```bash
./scripts/run_stt_colab_stream.sh
```

Defaults del servidor streaming:

| Parámetro | Default |
| --- | --- |
| modelo | `small` |
| max window | `3.0s` |
| min silence | `0.35s` |
| partial | `0.5s` |
| partial agreement | `1` |
| beam | `5` |

El bridge:

- recibe el SAUDPCM TCP actual de la board en `:5000`;
- forwardea frames binarios al WebSocket;
- recibe transcript events del servidor;
- escribe `logs/stt_events.jsonl`;
- manda NDJSON compatible al firmware en `192.168.1.10:5001`.

## 3. Board

Por ahora se corre igual que antes. La board sigue creyendo que habla con el receiver local:

- `USB_AUDIO_TCP_HOST` apunta a la PC;
- `USB_AUDIO_TCP_PORT=5000`;
- `SUBTITLE_STT_RX_PORT=5001`.

## 4. Analyze

Después de una run:

```bash
./scripts/analyze
```

La sección `PIPELINE` ahora muestra un breakdown streaming si los eventos lo traen:

- `audio buffer`;
- `server queue`;
- `GPU infer`;
- `server emit lag`;
- `bridge recv lag`.

## Próximo Paso

Cuando el servidor streaming esté validado con Colab, el siguiente salto es firmware board-direct:

```text
Board -> wss://server/stt/stream -> Board
```

Eso reemplaza el bridge por un cliente WebSocket TLS en firmware y elimina la PC del loop.
