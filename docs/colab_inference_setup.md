# Google Colab Inference Setup

Esta configuración mueve la inferencia de STT de tu PC a Google Colab para aprovechar la GPU (T4/V100) y reducir drásticamente los tiempos de inferencia.

## Arquitectura

```
Board (audio) → PC (stt_receiver.py) → Colab GPU (inference) → PC → Board (subtitles)
      ↑         Local TCP:5000           ngrok/HTTP            Local TCP:5001
```

## Setup

### 1. Notebook de Colab

1. Abrir [colab_inference_server.ipynb](../scripts/colab_inference_server.ipynb) en Google Colab
2. Runtime → Change runtime type → **T4 GPU** (gratis)
3. **Configurar túnel público** (elegir una opción):
   - **Opción A (recomendada):** Crear cuenta gratis en [ngrok](https://dashboard.ngrok.com/signup), obtener authtoken, y configurarlo en la celda correspondiente
   - **Opción B:** Usar localtunnel (sin registro, puede ser menos estable) - correr la celda alternativa al final
4. Correr todas las celdas (tarda ~1-2 min en cargar el modelo)
5. Copiar la URL pública que aparece al final (ej: `https://abc123.ngrok.io`)

### 2. PC Local

En WSL, correr:

```bash
STT_COLAB_URL=https://abc123.ngrok.io ./scripts/run_stt_colab.sh
```

Esto abre una ventana de PowerShell con el receptor de audio que:
- Recibe audio de la board (puerto 5000)
- Lo envía a Colab para inferencia
- Recibe transcripciones de Colab
- Las envía de vuelta a la board (puerto 5001)

### 3. Board

Correr el firmware normalmente con `/deploy-and-run`. La board se conecta a la PC y todo funciona transparente — no sabe que la inferencia está en Colab.

## Parámetros Importantes

Estos parámetros controlan la segmentación de audio (VAD) y se aplican **en la PC**, no en Colab:

| Parámetro | Default | Descripción |
|-----------|---------|-------------|
| `STT_MAX_WINDOW_SEC` | 4.0 | Máximo de segundos de audio antes de forzar un corte (incluso sin pausa). Antes era 8.0, ahora 4.0 para menor latencia. |
| `STT_MIN_SILENCE_SEC` | 0.5 | Mínimo de segundos de silencio para finalizar una frase |
| `STT_PARTIAL_SEC` | 0.8 | Intervalo para pedir hipótesis parciales a Colab (0 = deshabilitado, finals-only) |
| `STT_PARTIAL_AGREEMENT` | 2 | Cantidad de parciales consecutivos que deben compartir prefijo antes de mostrarlo (`1` = sin estabilización) |
| `STT_GAIN` | 0 | Ganancia de audio (0 = auto-normalización) |
| `STT_BEAM_SIZE` | 5 | Beam size de Whisper (mayor = más preciso pero más lento) |

### Cambio de MAX_WINDOW_SEC: 8.0 → 4.0

El default cambió de 8s a 4s para:
- Reducir latencia end-to-end
- Evitar que frases largas acumulen demasiado delay
- Mejor UX: subtítulos aparecen más rápido

Si querés el comportamiento anterior (frases más largas):
```bash
STT_MAX_WINDOW_SEC=8.0 STT_COLAB_URL=... ./scripts/run_stt_colab.sh
```

## Performance Esperada

### Inferencia local (CPU):
- Model `small` + CPU: ~0.5-1.5s por chunk (puede dropear audio en tiempo real)
- Partials deshabilitados por defecto para no sobrecargar la CPU

### Inferencia Colab (GPU T4):
- Model `small` + GPU: ~0.05-0.15s por chunk (**10-20x más rápido**)
- Latencia de red PC↔Colab: ~50-200ms roundtrip
- Partials estabilizados por defecto: `STT_PARTIAL_SEC=0.8` y `STT_PARTIAL_AGREEMENT=2`
- Total: debería reducir latencia end-to-end considerablemente

## Troubleshooting

### Colab notebook se desconecta
- Colab Free tiene límite de inactividad (~30-60 min)
- Necesitas mantener el tab abierto
- Alternativa: Colab Pro (~$10/mes) tiene sesiones más largas

### Error "colab server health check failed"

- Verificar que la celda de túnel (ngrok/localtunnel) esté corriendo
- Copiar la URL correcta (incluir `https://`)
- Verificar firewall/antivirus

### Error "ngrok authentication failed"

- Necesitas crear cuenta gratis: <https://dashboard.ngrok.com/signup>
- Obtener token: <https://dashboard.ngrok.com/get-started/your-authtoken>
- Configurarlo en la celda correspondiente del notebook
- **O** usar la opción alternativa con localtunnel (sin auth)

### Audio se dropea
- Aumentar `STT_MAX_WINDOW_SEC` para chunks más grandes (menos requests)
- Verificar latencia de red (`ping` al server de ngrok)
- Considerar `STT_LOSSLESS_LIVE=` (deshabilitar) si buffers crecen

### Transcripciones vacías
- Verificar que el modelo en Colab sea compatible (default: `small`)
- Revisar logs en Colab notebook
- Probar aumentar `STT_GAIN` si audio está muy bajo

## Costos

- **Colab Free**: Gratis, GPU T4, límites de tiempo/inactividad
- **Colab Pro**: $10/USD/mes, mejor GPU (V100), sesiones más largas
- **ngrok**: Gratis para uso personal, límites de ancho de banda

## Próximos Pasos

Si Colab funciona bien pero querés algo más estable:
- [ ] Migrar a GCP Cloud Run / AWS Lambda con GPU
- [ ] Evaluar Gemini Live API (managed, sin mantener servidor)
- [ ] Self-host en máquina con GPU local
