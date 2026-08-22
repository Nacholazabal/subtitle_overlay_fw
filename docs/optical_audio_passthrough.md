# Passthrough de audio por salida óptica

## Objetivo

La interfaz USB TI/Burr-Brown PCM2902 recibe el audio analógico utilizado por
STT y entrega una copia por S/PDIF/Toslink. El PCM2902 no ofrece un bypass
interno ADC → S/PDIF: Linux debe capturar las muestras y escribirlas de nuevo
al endpoint USB de reproducción.

## Camino de datos

La captura ALSA usa PCM S16_LE estéreo, 48 kHz y bloques de 20 ms. Cada bloque
se divide en dos caminos:

```text
PCM2902 capture, stereo
    ├── copia estéreo → cola fija → thread ALSA playback → S/PDIF/Toslink
    └── promedio L/R → mono → AGC opcional → WebSocket → Nemotron
```

El audio enviado a la salida óptica no pasa por el AGC digital de STT. La
reproducción usa tres períodos ALSA y comienza con un período disponible.

## Aislamiento y fallos

La reproducción no ocurre dentro de un handler QP/C ni bloquea la captura. Una
cola fija de cuatro bloques desacopla ambos threads y limita la acumulación a
80 ms. Si se llena, descarta el bloque más antiguo para conservar audio actual.

El passthrough es best-effort: si no puede abrir el dispositivo, crear su
thread o recuperarse de un error ALSA, se deshabilita la salida óptica pero la
captura mono y los subtítulos continúan. Los contadores de bloques escritos,
descartados y recuperaciones aparecen en los logs `usb-audio`.

## Configuración

Los defaults de producción son passthrough habilitado, el mismo dispositivo
ALSA para captura y reproducción, y volumen PCM al 100 %:

| Variable | Default | Uso |
| --- | --- | --- |
| `USB_AUDIO_PCM_DEVICE` | `hw:0,0` | Dispositivo ALSA de captura |
| `USB_AUDIO_PLAYBACK_PCM_DEVICE` | valor de captura | Dispositivo ALSA de reproducción |
| `SUBTITLE_USB_AUDIO_PASSTHROUGH_ENABLE` | `1` | `0` deshabilita la salida óptica |
| `SUBTITLE_USB_AUDIO_PLAYBACK_VOL_PCT` | `100` | Nivel PCM entre 0 y 100 % |

`./scripts/run.sh -s` escribe estos valores en
`/etc/default/subtitle-overlay`. El control de mixer se aplica como una
operación best-effort; la ausencia del control `PCM` no detiene el firmware.

## Verificación en placa

Después del despliegue manual, conectar un receptor a Toslink y confirmar en
el log:

```text
usb-audio: playback configured rate=48000 channels=2
usb-audio: optical playback thread started
usb-audio: ALSA configured rate=48000 channels=2
```

Durante audio continuo, `dropped=0` es el estado esperado. La salida óptica y
los subtítulos deben mantenerse simultáneamente; un fallo óptico deliberado no
debe interrumpir la conexión STT.
