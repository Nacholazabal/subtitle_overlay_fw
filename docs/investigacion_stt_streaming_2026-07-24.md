# Investigación técnica de STT streaming en español

**Fecha de corte:** 24 de julio de 2026  
**Objetivo:** elegir una solución ya implementada para sustituir el motor STT del servidor Colab/PC, sin cambiar la captura, transporte, ACK ni overlay del firmware.  
**Criterio rector:** reproducibilidad académica y subtítulos útiles por debajo de ~1,5 s, con la menor cantidad posible de política STT propia.

> **Conclusión ejecutiva:** probar primero **NVIDIA Nemotron 3.5 ASR Streaming 0.6B oficial mediante Transformers 5.13.1**, con idioma `es-ES` explícito y 320 ms de look-ahead. Es el candidato que mejor coincide con el problema: es streaming nativo, conserva estado real entre chunks, evita recalcular ventanas solapadas, cabe holgadamente en una T4 y sus pesos pueden usarse sin NIM. La segunda POC debe ser **WhisperLiveKit 0.2.24 + Faster-Whisper + `large-v3-turbo` + LocalAgreement**, porque es la opción Whisper menos invasiva que ya resuelve servidor, VAD/VAC, parciales, finales y PCM. El fallback inmediato, si WhisperLiveKit resulta frágil en Colab, es cambiar solamente `small` por `large-v3-turbo` en el servidor Faster-Whisper actual.

## 1. Resumen ejecutivo y decisión

### Primera opción: Nemotron 3.5 oficial

La recomendación principal es [`nvidia/nemotron-3.5-asr-streaming-0.6b`](https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b), no el NIM y no el port C. NVIDIA lo publicó el 4 de junio de 2026 como modelo de 600 M de parámetros, FastConformer cache-aware + RNN-T, con español `es-ES` y `es-US` en el nivel *transcription-ready*. Procesa cada frame nuevo una sola vez y conserva tanto los caches del encoder como el estado del decoder. Esto es streaming algorítmico real, no Whisper recortado en ventanas.

El primer ensayo debe usar la integración oficial de [Transformers 5.13.1](https://huggingface.co/docs/transformers/v5.13.1/en/model_doc/nemotron3_5_asr), porque tiene muchas menos dependencias que NeMo y permite comprobar rápidamente en una T4:

- que el checkpoint carga y cabe;
- que `es-ES` funciona mejor que `auto` en los tres audios del proyecto;
- que el generador mantiene los caches entre chunks;
- que 320 ms ofrece un punto razonable entre exactitud y latencia;
- que la salida RNN-T es incremental y suficientemente estable para enviarla al overlay.

La integración streaming de Transformers está marcada como **experimental y su API puede cambiar**. Por eso hay que fijar `transformers==5.13.1`, registrar el hash/revisión del modelo y no confundir “oficial” con “maduro”. Si pasa la prueba de modelo pero la falta de endpointing/timestamps obliga a escribir demasiada lógica, el siguiente paso dentro de la misma alternativa es [NeMo 26.06 y su Pipeline API](https://docs.nvidia.com/nemo/speech/nightly/asr/inference.html), que ya contempla EOU, timestamps y opciones por stream.

### Segunda opción: WhisperLiveKit + Whisper Turbo

La segunda opción es [WhisperLiveKit 0.2.24](https://pypi.org/project/whisperlivekit/) con backend Faster-Whisper, modelo `large-v3-turbo`, idioma `es` explícito, PCM crudo y política LocalAgreement. No es streaming nativo: debajo sigue habiendo Whisper offline y recomputación. Sin embargo, es la mejor alternativa operacional para dejar de mantener una política artesanal, porque entrega:

- WebSocket nativo y compatible con Deepgram;
- PCM `s16le`, 16 kHz mono sin FFmpeg con `--pcm-input`;
- VAD y VAC;
- texto efímero separado de líneas confirmadas;
- señal de cierre y flush;
- timestamps de segmento;
- un protocolo `diff` con secuencia monotónica, particularmente fácil de adaptar al protocolo actual.

La POC debe usar **LocalAgreement**, no el backend SimulStreaming/AlignAtt por defecto, para separar dos preguntas: “¿Turbo mejora el sistema?” y “¿AlignAtt funciona con Turbo?”. El soporte oficial de SimulStreaming se documenta para `large-v3`, no para Turbo; el Turbo reduce el decoder de 32 a 4 capas y existen reportes de incompatibilidades de tensores/alignment heads al mezclarlo con AlignAtt. Ejecutarlo puede funcionar en determinadas revisiones, pero no es una combinación que deba asumirse validada.

### Baselines que se conservan

1. **Faster-Whisper `small` actual, congelado**: `partial=1,0 s`, `agreement=2`, VAD `0,50/0,35`, `window=3–4 s`, `silence=0,5 s`. Resultado conocido: WER `offline_proxy` 23,52 %, p90 recepción 0,93 s, confiabilidad 100 %. Es el baseline de producción experimental.
2. **SimulStreaming/AlignAtt `small`, commit ya fijado `077ea37d...`**: se conserva como baseline de investigación y como evidencia de integración, no como candidato a optimizar indefinidamente. El recorrido funcionó al 100 %, pero RTF 1,60, p90 13,21 s y picos de 16,5 s lo dejan fuera del objetivo hasta demostrar otra cosa.
3. **Faster-Whisper `large-v3-turbo` en el servidor actual**: es el cambio mínimo y debe medirse antes de tocar la política. Puede ganar exactitud y velocidad de decoder, aunque no garantiza parciales más estables.

### Descartes y postergaciones

- **OpenWhispr**: aplicación Electron completa, no un backend Colab reutilizable. Su `main` actual sí prueba que sherpa-onnx + Nemotron puede entregar parciales/finales, pero extraer esa ruta implica reconstruir su arquitectura. Se usa como evidencia, no como dependencia.
- **`kdrkdrkdr/nemotron-asr-streaming.c` en la Arty Z7**: inviable. El archivo de pesos ocupa ~0,62 GiB y la placa tiene 512 MB de DDR3 antes de Linux, buffers de video y memoria de trabajo. En PC puede ser un experimento CPU, no la arquitectura final.
- **Parakeet TDT 0.6B v3**: excelente ASR español offline, pero su modo publicado como “streaming” usa 10 s de contexto izquierdo, chunks de 2 s y 2 s de contexto derecho. Es streaming simulado/buffered y no resuelve la latencia mejor que Nemotron.
- **NVIDIA NIM**: servidor técnicamente completo, pero exige GPU con compute capability >= 8.0, al menos 16 GB, NVIDIA Container Runtime y credenciales NGC. La T4 de Colab es compute capability 7.5 y queda fuera de la matriz soportada. No es la POC Colab adecuada.
- **Voxtral Mini 4B Realtime 2602**: rival nativo muy serio y único candidato adicional que justifica un ensayo posterior. Sus pesos BF16 y vLLM exigen >=16 GB; una T4 de 16 GB queda en el límite y no soporta BF16 nativamente. Debe probarse sólo si se dispone de una L4 de 24 GB reproducible.
- **Moonshine en español**: el modelo español publicado es `Base` no streaming; los checkpoints streaming actuales son sólo ingleses. Además, los pesos no ingleses usan licencia no comercial.
- **Collabora WhisperLive y Speaches**: servidores útiles, pero siguen envolviendo Whisper/Faster-Whisper y no ofrecen una política de estabilización más convincente que WhisperLiveKit para este caso.

### Respuesta corta para detener la investigación algorítmica

**Primero:** Nemotron 3.5 oficial, Transformers 5.13.1, `es-ES`, 320 ms, T4.  
**Segunda opción:** WhisperLiveKit 0.2.24 + Faster-Whisper `large-v3-turbo` + LocalAgreement.  
**Fallback de menor riesgo:** el servidor actual, sustituyendo sólo `small` por `large-v3-turbo` y conservando todos los demás parámetros.

---

## 2. Correcciones conceptuales necesarias

### 2.1 “Streaming” describe varias cosas distintas

Se usarán estas categorías:

- **A — modelo con streaming nativo:** arquitectura causal o cache-aware entrenada para recibir audio incremental y conservar estado. Ejemplos: Nemotron 3.5 y Voxtral Realtime.
- **B — modelo offline adaptado:** el modelo ve ventanas finitas y una política externa recompone/estabiliza hipótesis. Ejemplos: Faster-Whisper actual, SimulStreaming y Parakeet TDT con inferencia chunked.
- **C — aplicación o servidor:** integra uno o varios motores y añade transporte, VAD, sesiones o UI; no define por sí solo la arquitectura del modelo. Ejemplos: WhisperLiveKit, Collabora WhisperLive, Speaches y OpenWhispr.
- **D — servicio cloud propietario:** expone streaming pero no pesos/runtime reproducible. Ejemplo: Voxtral Realtime alojado por Mistral; también los proveedores que usa OpenWhispr para reuniones.

Un servidor WebSocket no convierte automáticamente un modelo offline en streaming nativo. “Recibo audio por WebSocket” y “el encoder conserva estado causal” son propiedades diferentes.

### 2.2 La latencia no es un único número

Para no mezclar fenómenos, el informe y las futuras corridas deben separar:

| Magnitud | Definición operativa | Quién la causa |
|---|---|---|
| Latencia algorítmica | Audio futuro que el modelo/política necesita antes de poder decidir; por ejemplo, look-ahead de 320 ms | Arquitectura y configuración STT |
| Tiempo de inferencia | Duración real de cada llamada/step de cómputo | Modelo, runtime, GPU/CPU y carga |
| RTF | tiempo de cómputo / duración de audio procesado | Capacidad sostenida; debe ser <1 |
| Transporte | placa → bridge → Colab y retorno hasta bridge/firmware | Red, túnel, colas, framing |
| TTFT/TTFP | desde inicio de voz hasta primer token/parcial visible | Todas las capas anteriores |
| Tiempo a texto estable | instante en que un prefijo aparece por última vez y ya no cambia | Política de commit/decoder |
| Latencia de final | desde fin de voz hasta evento `final` | Endpointing/VAD + inferencia + transporte |
| ACK | evento recibido y encolado por firmware | Protocolo, no calidad STT ni píxeles HDMI |

El “streaming latency = 320 ms” de Nemotron es sólo el look-ahead configurado. No afirma que el texto llegue a la pantalla en 320 ms. Del mismo modo, una inferencia de 100 ms no prueba TTFT de 100 ms si se esperan 1 s de audio antes de ejecutarla.

### 2.3 Parcial, estable y final no son sinónimos

- **Parcial:** hipótesis visible que puede cambiar.
- **Prefijo estable:** texto que, por contrato del motor o por observación retrospectiva, ya no se revoca.
- **Final:** cierre de una unidad de habla; no debería revisarse.

Nemotron RNN-T produce tokens de forma acumulativa, pero la API básica de Transformers sólo entrega texto; no aporta por sí sola el contrato de `is_final`. NIM sí entrega `is_final`, y NeMo Pipeline incluye EOU. WhisperLiveKit separa `buffer_transcription` efímero de `lines` confirmadas. El adaptador del proyecto debe mapear esas semánticas, no inventar otra votación de hipótesis.

### 2.4 Los WER publicados no se pueden comparar directamente con el 23,52 % actual

El resultado del proyecto es `offline_proxy`, generado automáticamente y sin referencia humana. Los resultados de Nemotron, Parakeet y Voxtral son WER sobre FLEURS/MLS/CoVoST, con normalizaciones y datos diferentes. Sirven para confirmar que el español es una capacidad real, no para predecir el WER del noticiero HDMI.

Antes de elegir un ganador se necesitan transcripciones humanas de los tres audios. Hasta entonces:

- todo WER/CER interno debe seguir rotulado `offline_proxy`;
- no debe existir un “score global” que mezcle ese proxy con latencia;
- una referencia offline producida por cada candidato no es común ni imparcial;
- el 82,97 % de SimulStreaming no debe interpretarse como precisión real porque una referencia quedó truncada.

### 2.5 “Tiempo real” requiere estabilidad sostenida

RTF promedio <1 no basta si hay picos largos. Para subtítulos, una cola que crece durante 20 s puede tener promedio final aceptable y experiencia inutilizable. La condición mínima debe ser:

- RTF mediano y p90/p95;
- backlog máximo de audio pendiente;
- porcentaje del tiempo con backlog creciente;
- p90/p95 del tiempo de inferencia por actualización;
- prueba continua de al menos 10–15 minutos, además de los tres clips cortos.

---

## 3. Tabla comparativa completa

### 3.1 Capacidades y encaje arquitectónico

| Candidato | Clase | Streaming real | Español | Estado entre chunks | Parcial/final y timestamps | Endpointing | Encaje con el proyecto |
|---|---:|---|---|---|---|---|---|
| Nemotron 3.5 + Transformers 5.13.1 | A | Sí, cache-aware | `es-ES`, `es-US` transcription-ready | caches de atención/convolución + estado RNN-T conservados por `generate` | texto incremental; final/EOU y timestamps no resueltos por el ejemplo básico | hay que cerrar utterances externamente | **Primera POC**; adaptador fino |
| Nemotron 3.5 + NeMo 26.06 | A | Sí | Sí | sí | Pipeline documenta EOU, word timestamps y opciones por stream | integrado en Pipeline | Ruta oficial si Transformers queda corto |
| Nemotron 3.5 NIM 1.2.0 | A + servidor | Sí | Sí, `type=multi` | sí | gRPC/Realtime, parciales, `is_final`, timestamps | integrado; `stop-history`, `force_eou` | Excelente producto, mala POC Colab/T4 |
| sherpa-onnx 1.12.x + Nemotron INT8 | A + runtime | Sí | Sí | `OnlineRecognizer` stateful | resultados online con texto, tokens, timestamps e `is_final`; servidor WS | reglas de endpointing integradas | **Ruta PC posterior muy prometedora** |
| `nemotron-asr-streaming.c` W8A8 | A + runtime | Sí | Sí | caches FastConformer + estado RNN-T | CLI emite texto; sin protocolo estructurado documentado | no VAD/EOU; Ctrl-C hace flush | Experimento CPU; no placa |
| Voxtral Mini 4B Realtime 2602 | A | Sí, encoder causal | Sí, 13 idiomas | atención causal/sliding window | API realtime vLLM | sesión realtime; verificar semántica EOU | Finalista condicional con L4, no T4 |
| Faster-Whisper `small` actual | B | No | Sí | redecodifica ventana | protocolo propio partial/final | Silero + reglas propias | Baseline congelado |
| Faster-Whisper `large-v3-turbo` actual | B | No | Sí | igual al actual | igual al actual | igual al actual | **Cambio mínimo/fallback** |
| SimulStreaming/AlignAtt `small` | B | No; adaptación de Whisper | Sí | buffer/contexto de política, no encoder causal | confirmado/no confirmado + `is_final` | VAC | Baseline de investigación; hoy demasiado lento |
| WhisperLiveKit + Turbo | C sobre B | No en backend Whisper | Sí | LocalAgreement o AlignAtt administra hipótesis | `buffer_transcription`, líneas confirmadas, timestamps de segmento; WS diff/Deepgram | VAD + VAC + flush | **Segunda POC** |
| Collabora WhisperLive + Turbo | C sobre B | No | Sí | ventanas Faster-Whisper/TensorRT/OpenVINO | segmentos, `completed`, palabras opcionales | VAD opcional | Alternativa de servidor simple |
| Speaches + Faster-Whisper | C sobre B | No | Sí | depende de Faster-Whisper/Realtime API | SSE y Realtime API | servidor | Útil por compatibilidad OpenAI; no ventaja STT clara |
| OpenWhispr | C | Depende del motor | Sí | app integra whisper.cpp/sherpa/cloud | su rama principal integra parciales/finales de sherpa | lógica de app | No reutilizar como backend; usar como referencia |
| Parakeet TDT 0.6B v3 | B en modo chunked | No para el checkpoint publicado | Sí, muy bueno offline | recomputa buffers/contexto | timestamps excelentes offline | VAD/segmentación externa | No elegir para latencia nativa |
| Moonshine Spanish Base | B/no aplicable | **No para español** | Sí offline | no checkpoint español streaming | librería de eventos | VAD propio | Descartado por capacidad/licencia |
| Voxtral Realtime alojado por Mistral | D | Sí | Sí | gestionado por proveedor | API propietaria | gestionado | Referencia cloud, no resultado reproducible local |

### 3.2 Operación, Colab, licencias y madurez

Las cifras de VRAM/arranque marcadas **estimación** deben medirse: no existen benchmarks primarios homogéneos sobre una T4 para todos los candidatos.

| Candidato | T4 16 GB | VRAM/RAM esperada | Complejidad de arranque | Licencia relevante | Madurez al 24-07-2026 |
|---|---|---|---|---|---|
| Nemotron + Transformers | Sí, el modelo cabe | **estimación:** 2–6 GB GPU, según dtype/overhead | baja-media; ~2–3 GB de pesos, 5–15 min cold **estimado** | pesos OpenMDW 1.1; Transformers Apache-2.0 | modelo v1 de junio; API streaming experimental |
| Nemotron + NeMo | Sí | **estimación:** 3–8 GB | alta; instalación desde `main`, Cython/PyTorch/NeMo | pesos OpenMDW 1.1; NeMo Apache-2.0 | framework maduro; integración del modelo nueva |
| Nemotron NIM | **No soportada**: T4 CC 7.5 | perfil oficial multi batch 32: 6 GB GPU + 8 GB CPU | muy alta en Colab; Docker NVIDIA + NGC | licencia/terms NIM además de pesos | producto oficial 1.2.0, junio 2026 |
| sherpa-onnx Nemotron INT8 | Sí en CPU; GPU a validar | modelo ~650 MB + runtime/activaciones | media; bundle por chunk fijo | runtime Apache-2.0; pesos OpenMDW 1.1 | proyecto muy maduro/activo; conversión del 11-06-2026 |
| port C W8A8 | CPU sí; GPU no usa | archivo 0,62 GiB + working set | baja para CLI, alta para servidor completo | el repo declara OpenMDW 1.1, sin licencia de código separada clara | 48 commits, 7 estrellas, 0 releases/issues/PR; riesgo alto |
| Voxtral Realtime | T4 marginal/no recomendable | >=16 GB oficial; pesos BF16 ~9 GB + KV/cache | alta; vLLM y stack CUDA | Apache-2.0 | oficial de febrero 2026; runtime realtime aún joven |
| Faster-Whisper Turbo | Sí | ~3–6 GB **estimado**; OpenAI informa ~6 GB para runtime original | baja en servidor existente | MIT | modelo/runtime ampliamente usados; Faster-Whisper 1.2.1 |
| SimulStreaming | Sí con `small`; `large-v3` recomienda >=10 GB | medido debe registrarse | media-alta; fork PyTorch y checkpoint `.pt` | MIT | proyecto académico activo, sin estabilidad de servidor garantizada |
| WhisperLiveKit Turbo | Sí, sujeto a compatibilidad CUDA | ~3–7 GB **estimado** | media; `pip`, modelo y warm-up; evitar extras incompatibles | repo Apache-2.0; dependencias/pesos conservan licencias | 0.2.24 beta, 11-07-2026; 813 commits, actividad alta |
| Collabora WhisperLive | Sí con Faster-Whisper | similar a Faster-Whisper | media; TensorRT no conviene en Colab | MIT | 0.9.0 beta, 02-06-2026; activo |
| Speaches | Sí | depende del backend | media-alta, orientado a Docker | MIT | 0.9.0-rc.3 era la última release visible; API en movimiento |
| OpenWhispr | No es un target Colab limpio | app Electron completa, assets grandes | muy alta para extraer sólo STT | MIT; modelos aparte | 1.7.2, commit `e35a364`, 20-05-2026; app activa |
| Parakeet TDT v3 | Sí | >=2 GB RAM según card; GPU cabe | media con NeMo, baja con sherpa offline | CC-BY-4.0 | estable como offline; no nativo streaming |
| Moonshine Spanish | Sí CPU | 58 M | baja | español: Moonshine Community License no comercial | proyecto activo; capacidad requerida aún ausente |

---

## 4. Análisis individual de los candidatos

### 4.1 NVIDIA Nemotron 3.5 ASR Streaming 0.6B

#### Qué es y por qué importa

Es el candidato más alineado con la tesis. Su encoder FastConformer de 24 capas mantiene caches por capa para self-attention y convolución; el decoder RNN-T conserva su estado. Los chunks son no solapados. A diferencia de Whisper, no hay una ventana creciente que se vuelve a codificar completa en cada actualización.

NVIDIA ofrece estos puntos de operación:

| `att_context_size` / look-ahead | Latencia algorítmica declarada | WER FLEURS español, LangID | WER FLEURS español, auto |
|---|---:|---:|---:|
| `[56,0]` | 80 ms | 4,87 % | 5,04 % |
| `[56,1]` | 160 ms | 4,64 % | 4,82 % |
| `[56,3]` | 320 ms | 4,39 % | 4,48 % |
| `[56,6]` | 560 ms | 4,26 % | 4,34 % |
| `[56,13]` | 1120 ms | 4,11 % | 4,13 % |

Son resultados *self-reported* sobre FLEURS, normalizados y no comparables al `offline_proxy` del proyecto. Aun así, muestran dos cosas útiles: español es uno de sus idiomas fuertes y fijar el idioma es ligeramente mejor que autodetectarlo. Para este sistema, `es-ES` debe ser el valor inicial; `auto` sólo se justifica si el contenido realmente cambia de idioma.

#### Transformers frente a NeMo frente a NIM

**Transformers 5.13.1** es la ruta correcta para el primer gate. El ejemplo oficial entrega al `generate()` un generador de features y usa `TextIteratorStreamer`. Internamente se pasan `encoder_past_key_values`, cache de padding y cache del decoder entre chunks; no es una sucesión de llamadas offline independientes.

Hay una inconsistencia que la POC debe registrar: la model card NVIDIA enumera 80/160/320/560/1120 ms, pero la documentación 5.13.1 muestra `supported_streaming_latencies_ms = {3: 320, 0: 80, 6: 560, 13: 1120}`, omitiendo 160 ms. El notebook no debe asumir: debe imprimir el diccionario real del processor instalado y abortar si el punto solicitado no existe.

**NeMo 26.06** conviene si el modelo gana pero la API mínima obliga a escribir endpointing, timestamps o manejo de múltiples utterances. Su Pipeline API streaming unificada distingue pipelines buffered y cache-aware, y documenta EOU, word timestamps, biasing e ITN por stream. El costo es una instalación más pesada y mayor riesgo de incompatibilidad en una sesión efímera de Colab.

**NIM 1.2.0** es el servidor más completo: gRPC/realtime, `is_final`, parciales, puntuación opcional, `stop-history` y `force_eou`. NVIDIA recomienda `stop-history >=560 ms`, aunque para Nemotron permite bajar hasta 80 ms. No requiere un VAD propio para saber cuándo un resultado es final. No obstante, [la matriz de soporte](https://docs.nvidia.com/nim/speech/latest/reference/support-matrix/asr.html) exige compute capability >=8.0 y 16 GB; T4 queda excluida. También requiere contenedor, NGC y términos NIM. Es una opción de despliegue institucional, no la primera POC académica.

#### Riesgos conocidos

- API Transformers experimental.
- El ejemplo oficial streaming sólo devuelve texto; hay que validar timestamps y límites de utterance.
- NIM documenta puntuación no siempre consistente, ausencia de confianza por palabra para RNNT, timestamps idénticos en algunos tokens y posible pérdida de primeras palabras si el audio comienza sin ~80 ms de silencio.
- Es un modelo publicado hace menos de dos meses; “oficial” no elimina el riesgo de regresión.
- OpenMDW 1.1 permite el uso del modelo bajo sus términos, pero no debe describirse como MIT/Apache. Para redistribuir pesos o un producto hay que conservar atribución y revisar la licencia completa.

### 4.2 `kdrkdrkdr/nemotron-asr-streaming.c`

El [runtime C](https://github.com/kdrkdrkdr/nemotron-asr-streaming.c) es técnicamente interesante: C sin PyTorch/ONNX, kernels NEON/AVX2/scalar, pesos W8A8 memory-mapped, caches completos del FastConformer y decoder RNN-T stateful. Su CLI acepta `stdin` PCM 16 kHz y el archivo convertido ocupa ~0,62 GiB. Publica unos 7× realtime en un portátil Apple Silicon de 8 cores; esa cifra no predice rendimiento en x86 Colab ni Cortex-A9.

Frente al modelo oficial:

- soporta 80/160/320/560 ms, no documenta 1120 ms;
- agrega una heurística propia de recuperación ante code switching;
- no trae WebSocket, `partial/final` estructurado, timestamps ni endpointing;
- su validación contra NeMo y sus benchmarks provienen del mismo autor;
- al corte tiene 48 commits, 7 estrellas, ningún fork/release/issue/PR visible;
- no presenta una licencia de código independiente estándar; afirma seguir OpenMDW 1.1 por ser conversión del modelo.

**PC:** razonable como POC secundaria de CPU si se quiere eliminar CUDA o medir privacidad local.  
**Colab:** posible, pero desperdicia la GPU y no tiene ventaja probada frente al oficial.  
**Arty Z7-20:** descartado. La placa tiene [Cortex-A9 dual a 650 MHz, 512 MB DDR3, 220 DSP y 4,9 Mbit de BRAM](https://digilent.com/shop/arty-z7-zynq-7000-soc-development-board/). El archivo de pesos ya supera la RAM física; faltan activaciones, caches, sistema operativo y video. Hacerlo viable requeriría almacenamiento/streaming de pesos y un acelerador FPGA completo, un proyecto distinto a esta tesis.

### 4.3 sherpa-onnx con Nemotron 3.5

[`sherpa-onnx`](https://github.com/k2-fsa/sherpa-onnx) cambia la evaluación del ecosistema C/ONNX. Es un proyecto Apache-2.0, activo, multiplataforma, con API online y servidores WebSocket; al corte su changelog llega a 1.12.38. Su documentación ya ofrece [Nemotron 3.5 multilingual INT8](https://k2-fsa.github.io/sherpa/onnx/nemo/nemotron-streaming.html), con bundles de aproximadamente 650 MB y selección de idioma por stream.

La API online expone `text`, tokens, timestamps e `is_final`; además tiene detección de endpoint y reset de stream. Es, por tanto, una base de servidor mucho más madura que envolver el port C. OpenWhispr `main` ya integra precisamente esta ruta: usa el servidor online sherpa, fusiona JSON partial/final y hace flush con fallback.

Reservas:

- la conversión INT8 y los bundles son comunitarios, no publicación NVIDIA;
- la documentación de sherpa lista 80/160/560/1120 ms y omite 320 ms, otra discrepancia que obliga a fijar el bundle exacto;
- no hay benchmark primario de español ni RTF para el PC del proyecto;
- los pesos siguen OpenMDW aunque el runtime sea Apache-2.0.

Recomendación: si Nemotron oficial gana la POC, medir después sherpa-onnx en PC/WSL como ruta de despliegue local y comparar transcripción token a token, WER humano y latencia contra el original. No debe sustituir la validación del checkpoint original.

### 4.4 Whisper `large-v3-turbo` y Faster-Whisper

OpenAI describe [`turbo`](https://github.com/openai/whisper/blob/main/README.md) como una versión optimizada de `large-v3`: 809 M de parámetros, ~6 GB de VRAM en el runtime original y ~8× la velocidad relativa de `large` en A100, con degradación mínima de exactitud. La [model card](https://huggingface.co/openai/whisper-large-v3-turbo) explica que mantiene la arquitectura de `large-v3` pero reduce el decoder de 32 a 4 capas. Código y pesos son MIT.

No es streaming nativo. `transcribe()` sigue leyendo el archivo y procesando ventanas deslizantes de 30 s. Faster-Whisper es un runtime CTranslate2 eficiente, no una política streaming. Su versión 1.1.0 añadió soporte para Turbo y la última release estable visible es 1.2.1.

#### ¿Qué tan invasivo es cambiar `small` por Turbo?

En el backend actual es el ensayo menos invasivo posible:

- cambiar el identificador a `large-v3-turbo`;
- mantener `float16`, `language=es`, VAD, ventanas, silencio y acuerdo;
- repetir con `beam_size=1` y `beam_size=5`, predeclarando cuál es primario;
- no reutilizar el `offline_proxy` generado por otro motor como ground truth.

Turbo puede mejorar exactitud y bajar inferencia pese a tener más parámetros que `small`, porque el decoder es mucho más corto. No necesariamente estabiliza parciales: el problema de revisar texto viene de redecodificar audio incompleto y de la política de ventanas. Es posible que una hipótesis más capaz sea estable antes, pero eso debe medirse.

#### ¿SimulStreaming soporta Turbo oficialmente?

No hay soporte oficial documentado. [SimulStreaming](https://github.com/ufal/SimulStreaming) declara soporte añadido para `large-v3`, carga checkpoints PyTorch `.pt` y usa capas/alignment heads de Whisper para AlignAtt. Turbo cambia drásticamente el número de capas de decoder. WhisperLiveKit tiene usuarios que lo han intentado, pero [el issue 152](https://github.com/QuentinFuxa/WhisperLiveKit/issues/152) muestra al menos una incompatibilidad de dimensiones en una configuración Turbo/SimulStreaming.

La conclusión correcta es “experimental, puede requerir alignment heads/revisión compatible”, no “imposible”. Para una comparación limpia, Turbo debe probarse primero con Faster-Whisper + política actual y con WhisperLiveKit + LocalAgreement.

### 4.5 WhisperLiveKit

[WhisperLiveKit](https://github.com/QuentinFuxa/WhisperLiveKit) es un servidor/kit, no un modelo. Al corte, PyPI publica 0.2.24 (11-07-2026), Python 3.11–3.13, estado beta; el repositorio muestra 813 commits y alta actividad. La licencia del repositorio es Apache-2.0, aunque los metadatos de PyPI mezclan clasificadores MIT/Apache; para el código fuente debe prevalecer el archivo LICENSE del repo y para cada modelo/dependencia su licencia propia.

La [API](https://github.com/QuentinFuxa/WhisperLiveKit/blob/main/docs/API.md) encaja especialmente bien:

- `/asr` recibe frames binarios;
- `--pcm-input` espera exactamente PCM `s16le`, 16 kHz mono;
- `mode=diff` entrega `seq`, `new_lines`, `n_lines`, poda y buffers reemplazables;
- `buffer_transcription` es texto efímero;
- `lines` son segmentos confirmados;
- un frame vacío fuerza el drain y `ready_to_stop` confirma que terminó;
- la API compatible con Deepgram expone `is_final`, `speech_final`, `UtteranceEnd` y `SpeechStarted`.

Para este proyecto debe arrancarse sin diarización, traducción ni auto-language. Esos extras agregan memoria, modelos gated y puntos de falla sin mejorar el objetivo. La configuración primaria propuesta es:

```text
backend=faster-whisper
backend-policy=localagreement
model=large-v3-turbo
language=es
pcm-input=true
VAD=true
VAC=true
mode=diff
```

Riesgos Colab:

- release rápida/beta;
- extras CUDA 12.9 pueden chocar con la imagen preinstalada; empezar con el paquete base, no con todos los extras;
- descarga y warm-up del modelo deben completarse antes de publicar `/health`;
- sus benchmarks públicos principales son H100 y francés/inglés, no T4/español;
- timestamps de palabras de la capa compatible con Deepgram son interpolados; para el overlay bastan timestamps de segmento, pero no deben presentarse como alineación exacta.

### 4.6 Collabora WhisperLive

[Collabora WhisperLive](https://github.com/collabora/WhisperLive) 0.9.0 es una implementación “nearly-live” MIT con backends Faster-Whisper, TensorRT y OpenVINO. Tiene PCM crudo `int16`, VAD opcional, palabras con timestamps/probabilidad, segmentos `completed`, modelo único reutilizado y batching multiusuario.

Es una opción razonable si se busca un servidor simple y conservador. Para Colab conviene Faster-Whisper; construir el engine TensorRT agrega Docker, TensorRT-LLM y compilación sin responder la pregunta de investigación. Frente a WhisperLiveKit, ofrece más runtimes y menos política académica de estabilización. Para subtítulos parciales estables, WhisperLiveKit tiene mejor semántica de buffer/commit y protocolo diff.

### 4.7 OpenWhispr

[OpenWhispr](https://github.com/OpenWhispr/openwhispr) 1.7.2 es una aplicación Electron/React completa de dictado, reuniones, notas y agentes. Su flujo Whisper local publicado sigue MediaRecorder → Blob → archivo temporal → whisper.cpp. Las reuniones en vivo usan proveedores cloud como OpenAI Realtime, AssemblyAI o Deepgram. Su API pública administra notas e historial de transcripciones; no es un endpoint de PCM streaming reutilizable.

La rama principal actual sí incorpora Parakeet/Nemotron mediante sherpa-onnx online, parciales/finales y fallback a decode completo. Esa implementación es evidencia valiosa de que la ruta sherpa funciona, pero está acoplada a procesos Electron, IPC, sidecars, UI y gestión de modelos. Reutilizar OpenWhispr implicaría más trabajo que llamar directamente a sherpa-onnx.

Clasificación: C. Inspiración y prueba de integración, no backend del proyecto.

### 4.8 NVIDIA Parakeet

[`parakeet-tdt-0.6b-v3`](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3) es un FastConformer-TDT de 600 M, 25 idiomas europeos, puntuación/capitalización y timestamps de carácter/palabra/segmento. Es CC-BY-4.0. NVIDIA publica WER español 3,45 % FLEURS, 4,39 % MLS y 3,41 % CoVoST, sin puntuación/capitalización en el scoring.

El problema no es la calidad offline, sino la forma de inferencia: su ejemplo “streaming” usa 10 s de contexto izquierdo, chunk de 2 s y 2 s de contexto derecho. Es un pipeline buffered con modelo offline. El propio NIM lo enumera como perfil offline para TDT v3. Parakeet RNNT multilingual 1.1B sí se sirve streaming mediante NIM, pero es mayor, buffered y menos eficiente que Nemotron cache-aware según la comparación de NVIDIA.

Conclusión: conservar como candidato de transcripción offline/PC y referencia de exactitud, no como primera solución a latencia.

### 4.9 Voxtral Mini 4B Realtime

[`mistralai/Voxtral-Mini-4B-Realtime-2602`](https://huggingface.co/mistralai/Voxtral-Mini-4B-Realtime-2602) es el segundo modelo A realmente relevante. Tiene encoder causal, sliding-window attention, 4 B de parámetros, 13 idiomas y licencia Apache-2.0. Mistral recomienda 480 ms; permite múltiplos de 80 ms entre 80 y 1200, además de 2400. Publica WER español FLEURS 3,31 % a 480 ms, 5,34 % a 160 ms y 2,98 % a 960 ms, otra vez no comparables al proyecto.

Su ruta realtime recomendada es vLLM `/v1/realtime`. Aunque Transformers >=5.2 puede cargarlo, el ejemplo de Transformers es generación sobre audio completo; la infraestructura streaming oficialmente recomendada sigue siendo vLLM. El requisito declarado es una GPU con >=16 GB. En una T4 de exactamente 16 GB quedan pesos BF16, KV/cache, buffers y runtime sin margen, y T4 no acelera BF16 como Ampere/Lovelace. Una L4 de 24 GB es la plataforma sensata.

Debe quedar como **challenge POC opcional**, no como fallback: si Nemotron falla por calidad y se consigue L4 estable, Voxtral es preferible a seguir inventando políticas Whisper.

### 4.10 Moonshine, Speaches y otras opciones

**Moonshine:** la librería es madura y su arquitectura streaming inglesa es interesante, pero la tabla oficial publica `Spanish Base 58M`, no un Spanish Streaming. Los únicos `Tiny/Small/Medium Streaming` listados son ingleses. El código es MIT; los modelos no ingleses usan licencia comunitaria no comercial. No satisface el requisito actual.

**Speaches:** servidor MIT orientado a compatibilidad OpenAI, Faster-Whisper y Docker. Puede devolver transcripción por SSE y posee Realtime API, pero sigue siendo clase C sobre un modelo B. La última release visible es 0.9.0-rc.3 y la API ha cambiado activamente. Es más útil como “Ollama para STT/TTS” que como solución de estabilidad de subtítulos.

**sherpa-onnx:** sí se conserva por su Nemotron online, no por ejecutar Whisper/Parakeet offline con VAD y llamarlo streaming.

**Servicios cloud:** Mistral aloja Voxtral Realtime a USD 0,006/min según su model card. Son útiles como control superior o demo, pero introducen costo, privacidad, disponibilidad externa y una caja negra que debilita la reproducibilidad de la tesis. No deben ser la implementación principal si el modelo abierto local cumple.

---

## 5. Viabilidad específica en Google Colab

### 5.1 Matriz práctica

| Ruta | T4 gratuita | L4 24 GB | Persistencia | Tiempo de setup **estimado** | Riesgo principal |
|---|---|---|---|---:|---|
| Nemotron Transformers | **Sí** | Sí | cache HF en Drive opcional | 5–15 min cold | API experimental/EOU |
| Nemotron NeMo | Sí | Sí | conviene cachear checkpoint, no entorno completo | 10–30 min | dependencias y build/install |
| Nemotron NIM | No soportada | teóricamente sí, pero Colab/Docker no apropiado | imagen/NGC | alto | CC, Docker, términos y privilegios |
| sherpa Nemotron INT8 CPU | Sí | Sí | bundle ~650 MB en Drive | 3–10 min | RTF CPU desconocido |
| Faster-Whisper Turbo | **Sí** | Sí | cache CTranslate2/HF | 5–15 min | sigue siendo offline adaptado |
| WhisperLiveKit Turbo | **Sí** | Sí | fijar wheel y modelo | 8–20 min | stack CUDA y release beta |
| SimulStreaming small | ya demostrado | Sí | checkpoint `.pt` y commit | conocido | RTF/picos actuales |
| Voxtral Realtime vLLM | no recomendable | **Sí** | pesos ~9 GB | 15–40 min | OOM/compilación/vLLM |

### 5.2 Reglas para notebooks reproducibles

Cada notebook debe:

1. imprimir GPU, compute capability, VRAM, Python, PyTorch, CUDA, cuDNN y versiones de paquetes;
2. fijar versiones y revisión del modelo, no instalar siempre `main` salvo NeMo cuando NVIDIA lo exija;
3. validar SHA/revisión del checkpoint y registrar licencia;
4. separar estado `installing`, `loading`, `warming_up`, `ready`, `failed`;
5. hacer warm-up real antes de abrir el túnel;
6. registrar tiempo de instalación, descarga, carga, warm-up y pico de VRAM;
7. probar primero un archivo local, después simulación realtime y recién después placa/bridge;
8. no montar simultáneamente NeMo, WLK, Voxtral y Faster-Whisper en el mismo runtime; usar notebooks/runtimes limpios;
9. guardar sólo artefactos reproducibles en Drive: resultados, hashes, referencias humanas y, si la licencia lo permite, cache de modelo;
10. fijar una sola sesión GPU para la evaluación de latencia.

### 5.3 Costos/licencias

- Los pesos Nemotron pueden descargarse de Hugging Face sin necesidad documentada de NGC/NIM. No hay un costo de API por inferencia propia; rigen OpenMDW 1.1 y el costo de Colab.
- Transformers, NeMo y sherpa son runtimes abiertos, pero sus licencias no reemplazan la del modelo.
- NIM tiene un camino de prueba/enterprise y credencial NGC; no debe presentarse como equivalente gratuito a descargar los pesos.
- Whisper/Turbo y Faster-Whisper son MIT.
- Voxtral abierto es Apache-2.0; la API alojada se cobra por minuto.
- Parakeet v3 es CC-BY-4.0, con obligación de atribución.
- Moonshine español no es comercial; aunque una tesis académica probablemente encaje, no aporta streaming español y no merece la ambigüedad.

---

## 6. Arquitectura recomendada

```text
Arty Z7 / firmware (sin cambios STT)
  captura PCM + sequence + timestamps + lost_chunks
        |
        v
bridge PC/WSL existente
  transporte, reconexión, session_start/end, ACK correlation
        |
        v
adaptador STT fino y reemplazable
  /health + /stt/offline + /stt/stream
  - NO vota hipótesis ni redecodifica ventanas si el motor ya es streaming
  - traduce eventos upstream a {partial, final, seq, timestamps}
        |
        +--> Engine A: Nemotron oficial (Transformers; NeMo si requiere EOU/timestamps)
        |
        +--> Engine B: proxy WhisperLiveKit /asr?language=es&mode=diff
        |
        +--> Baselines: Faster-Whisper actual / SimulStreaming fijado
        |
        v
bridge -> TCP transcript -> firmware -> ACK -> overlay HDMI
```

### Responsabilidades exactas

**Firmware:** no cambia. Captura, secuencia, pérdidas, recepción NDJSON, ACK, colas y presentación roll-up.

**Bridge:** conserva el contrato actual y añade sólo identificación/versionado del engine en manifest/health. No decide estabilidad textual.

**Adaptador:**

- valida PCM y sesión;
- conserva una sesión de motor por conexión;
- mapea texto incremental del motor a `partial`;
- mapea EOU/segmentos confirmados a `final`;
- adjunta tiempos del motor sin inventar precisión;
- fuerza drain al `session_end`;
- nunca aplica LocalAgreement encima de Nemotron ni una segunda política encima de WhisperLiveKit.

**Motor:** dueño de caches, decodificación, commit y, cuando lo provee, endpointing.

### Decisión sobre EOU de Nemotron

Transformers básico no entrega `is_final`. Para la POC de modelo pueden usarse límites conocidos de archivo/sesión. Para la prueba end-to-end continua hay dos caminos, en orden:

1. usar la Pipeline API de NeMo para EOU y timestamps;
2. si se conserva Transformers, reutilizar Silero exclusivamente como detector de fin de utterance (inicialmente 560 ms), terminar/drain la sesión RNN-T y arrancar la siguiente con ~80 ms de silencio inicial.

Ese segundo camino no es otra política de estabilización: no redecodifica ni decide qué palabras confirmar; sólo determina el límite de segmento. Aun así, debe medirse contra NeMo para no mantener lógica innecesaria.

---

## 7. POC A — Nemotron 3.5 oficial

### Hipótesis

Con `es-ES`, 320 ms y una T4, Nemotron puede sostener RTF <1, emitir texto incremental sin churn significativo y entregar el primer subtítulo dentro de 1,5 s en el pipeline físico.

### Entorno fijado

```text
GPU primaria: NVIDIA T4 16 GB
Python: 3.11 (o versión exacta disponible, registrada)
transformers: 5.13.1
modelo: nvidia/nemotron-3.5-asr-streaming-0.6b, revisión fijada
dtype: float16 en CUDA, si el modelo/prueba de equivalencia lo admite
idioma primario: es-ES
look-ahead primario: 3 = 320 ms
variantes secundarias: 0 = 80 ms y 6 = 560 ms
decoder: greedy RNN-T del modelo
```

### Secuencia de gates

**Gate A0 — disponibilidad**

- instalar `transformers==5.13.1`, PyTorch/Accelerate y audio mínimo;
- imprimir `supported_streaming_latencies_ms`;
- cargar modelo y registrar pico VRAM/tiempos;
- verificar que no hubo offload accidental a CPU.

**Gate A1 — equivalencia funcional**

- transcribir cada audio completo con el camino offline oficial;
- simular streaming a velocidad infinita y a tiempo real con 320 ms;
- comparar transcript completo offline vs streaming, sin usar uno como referencia humana;
- probar `es-ES`, `es-US` y `auto`, dejando `es-ES` como primario.

**Gate A2 — capacidad sostenida**

- concatenar 10–15 min de audio/pausas;
- medir RTF por step, backlog, VRAM y crecimiento de caches;
- exigir ausencia de OOM y backlog no creciente.

**Gate A3 — semántica de salida**

- demostrar si los chunks del `TextIteratorStreamer` son append-only;
- medir TTFT, frecuencia y longitud de emisiones;
- validar drain al final;
- determinar si Transformers expone timestamps útiles. Si no, registrar la carencia, no fabricarlos.

**Gate A4 — integración end-to-end**

- conservar `/health`, `/stt/offline` y `/stt/stream` actuales;
- mapear emisiones a `partial` y EOU a `final`;
- ejecutar los tres clips con 6 s de pausa y luego una corrida continua;
- confirmar ACK, secuencia, pérdidas y overlay lógico.

### Parámetros que se barren

Sólo dos dimensiones al comienzo:

| Variante | Idioma | Look-ahead | Motivo |
|---|---|---:|---|
| A-primary | `es-ES` | 320 ms | equilibrio recomendado |
| A-fast | `es-ES` | 80 ms | límite inferior de latencia |
| A-accurate | `es-ES` | 560 ms | más contexto, aún compatible con 1,5 s parcial |
| A-auto | `auto` | 320 ms | cuantificar costo de autodetección |

No barrer VAD, beam, ventanas y acuerdos porque el modelo no los necesita para decodificar. El umbral EOU se evalúa después y por separado.

### Criterios de éxito

- carga/warm-up reproducible en T4;
- RTF p95 <0,8 y ningún backlog creciente en 15 min;
- p90 de primer parcial visible <=1,5 s;
- p90 de texto estable <=1,5 s;
- WER humano no peor que baseline por más de 2 puntos absolutos y preferentemente mejor;
- 100 % sesiones completas, finales aceptados y cero pérdida de chunks;
- churn de parciales claramente menor que el backend por ventanas;
- sin más de una capa fina de endpointing/adaptación.

### Criterios de abandono

- RTF >=1 sostenido en T4;
- primeras palabras perdidas aun con pre-roll validado;
- falta de EOU/timestamps obliga a recrear un servidor complejo y NeMo tampoco lo resuelve de forma reproducible;
- WER humano peor que baseline >5 puntos absolutos en dos repeticiones;
- API/instalación no puede fijarse de manera reproducible.

---

## 8. POC B — WhisperLiveKit + Faster-Whisper Turbo

### Hipótesis

WhisperLiveKit puede reemplazar la política custom actual y conservar la robustez del transporte, mientras Turbo reduce WER y/o cómputo respecto de `small` sin exceder 1,5 s.

### Entorno fijado

```text
whisperlivekit: 0.2.24
backend: faster-whisper
backend-policy: localagreement
model: large-v3-turbo
language: es
PCM: s16le, 16 kHz, mono
mode: diff
VAD/VAC: habilitados
diarización/traducción: deshabilitadas
una sesión GPU
```

### Por qué LocalAgreement primero

El proyecto ya probó AlignAtt/SimulStreaming y encontró RTF/picos inaceptables con `small`. Usar Turbo + AlignAtt simultáneamente cambiaría modelo, runtime, integración y política, además de entrar en una combinación no oficialmente validada. LocalAgreement con Faster-Whisper permite evaluar WLK/Turbo con menos variables y con el backend rápido que ya se conoce.

### Adaptación mínima

El adaptador consume `/asr?language=es&mode=diff`:

- `buffer_transcription` → `partial` reemplazable;
- cada `new_lines[]` → `final` o commit acumulado, respetando secuencia;
- `remaining_time_transcription` → métrica de backlog, no texto;
- frame vacío al `session_end` → drain;
- `ready_to_stop` → sesión completa;
- timestamps `start/end` → metadatos de segmento.

No debe aplicar encima `partial_agreement=2`, ventanas de 3–4 s ni Silero del servidor anterior. WLK ya es dueño de esas decisiones.

### Gates

1. instalar wheel fijado en runtime limpio y verificar `wlk version/check`;
2. cargar Turbo y warm-up antes de readiness;
3. enviar los tres archivos por PCM directo a `/asr` y guardar JSON diff íntegro;
4. medir RTF/backlog/churn sin placa;
5. insertar el adaptador de protocolo y repetir el banco end-to-end;
6. sólo si pasa, comparar LocalAgreement con SimulStreaming dentro de WLK, usando la misma versión y model, como experimento secundario.

### Criterios de éxito/abandono

Los mismos de POC A, más:

- `n_lines` siempre consistente después de aplicar diffs;
- drain sin perder la última línea;
- menos replacements por minuto que el servidor actual;
- si WLK falla por instalación/API, no dedicar una fase larga a repararlo: ejecutar el fallback Faster-Whisper Turbo en el servidor actual.

---

## 9. Diseño del experimento justo

### 9.1 Sistemas mínimos a comparar

| ID | Sistema | Configuración primaria predeclarada |
|---|---|---|
| S0 | Faster-Whisper `small` actual | configuración baseline congelada |
| S1 | Faster-Whisper `large-v3-turbo` actual | mismos parámetros que S0; beam primario fijado antes de ver resultados |
| S2 | SimulStreaming/AlignAtt `small` | commit/config actual; corregir referencia, no tunear contra test |
| S3 | Nemotron 3.5 oficial | Transformers/NeMo, `es-ES`, 320 ms |
| S4 | WhisperLiveKit + Turbo | Faster-Whisper, LocalAgreement, VAD/VAC |
| S5 opcional | Voxtral Realtime | L4, 480 ms, vLLM fijado |

### 9.2 Dataset

**Obligatorio antes de decidir:** transcribir manualmente `desay-short`, `noticiero-short` y `rel-short`. Una segunda persona debe revisar ortografía, números, nombres propios y límites dudosos. Guardar:

- texto verbatim con muletillas y repeticiones;
- versión normalizada para WER;
- marcas de inicio/fin de voz por clip;
- decisiones de normalización documentadas.

El corpus es pequeño. Sirve para seleccionar arquitectura dentro del proyecto, no para afirmar superioridad general en español. Para la tesis conviene añadir al menos 20–30 minutos de voz española con distintos hablantes, ruido/música y velocidad, siempre con referencia humana o un corpus público licenciado.

### 9.3 Dos niveles de audio

1. **Entrada digital común:** exactamente los mismos samples PCM a cada motor. Aísla algoritmo/runtime.
2. **Pipeline físico:** reproducción HDMI/USB → captura de placa → transporte → STT → firmware. Mide el sistema real.

No se deben comparar motores usando capturas físicas distintas. Primero registrar una captura canónica y reproducirla a todos; después hacer al menos tres corridas físicas por sistema para medir variabilidad.

### 9.4 Repeticiones y orden

- un cold run documentado, excluido de latencia steady-state pero incluido en costo de arranque;
- mínimo 5 repeticiones warm por sistema para clips cortos;
- orden de motores/clips aleatorizado o rotado;
- misma clase de GPU; si Voxtral usa L4, reportarlo en una tabla separada y no atribuir toda diferencia al modelo;
- una prueba continua de 15 min por finalista;
- fijar versiones, seeds cuando existan, beam y dtype.

### 9.5 Instrumentación temporal

Registrar tiempos monotónicos en cada frontera:

```text
t_audio_origin          muestra/seq sale de la fuente de prueba
t_board_capture         firmware captura el chunk
t_bridge_send           bridge envía al servidor
t_server_receive        servidor recibe el chunk
t_infer_start/end       step del motor
t_server_event          parcial/final producido
t_bridge_receive        evento vuelve al PC
t_fw_receive            firmware recibe
t_fw_ack                ACK aceptado/rechazado
```

Los clocks deben correlacionarse. Para medición digital, usar el reloj del harness y números de muestra. Para placa/PC, calibrar offset/RTT o calcular tramos que comparten reloj. No restar timestamps de máquinas distintas sin sincronización.

El ACK prueba recepción/encolado. Para afirmar latencia física de píxel se necesita instrumentación visual —cámara de alta velocidad, fotodiodo o timestamp del pipeline de video—; mientras no exista, reportar “latencia hasta ACK/overlay lógico”, no “hasta pantalla”.

### 9.6 Métricas

**Exactitud**

- WER humano normalizado, primario;
- CER humano;
- WER verbatim secundario;
- WER por clip y agregado por número total de palabras, no promedio simple de porcentajes;
- `offline_proxy` sólo como columna diagnóstica, nunca como verdad.

**Latencia**

- TTFT/primer parcial no vacío;
- tiempo al primer prefijo estable, calculado retrospectivamente contra la secuencia final;
- latencia por palabra estable: emisión estable − fin acústico de la palabra, si hay alineación humana;
- latencia de final: recepción final − fin de voz;
- p50/p90/p95/p99 y máximo;
- porcentaje <=1,0 s, <=1,5 s y <=2,0 s.

**Cómputo**

- RTF p50/p90/p95 y global;
- tiempo de inferencia por step;
- backlog de audio p95/máximo;
- VRAM pico y RAM;
- tiempo cold install/download/load/warm-up.

**Estabilidad visual**

- actualizaciones por minuto;
- replacements por minuto;
- caracteres retirados / caracteres finales;
- profundidad máxima de revisión en palabras;
- *normalized erasure* o edit overhead;
- tiempo que cada parcial permanece visible;
- finales vacíos/duplicados y número de roll-ups.

**Confiabilidad**

- chunks generados/enviados/recibidos/perdidos;
- eventos generados/enviados/aceptados/rechazados;
- ACK faltantes y p50/p90/máximo;
- sesiones incompletas, reconnects, lost finals y errores de protocolo;
- exactitud de `seq` y del estado `n_lines` en WLK.

### 9.7 Gates de decisión

Un candidato sólo pasa si:

1. confiabilidad end-to-end = 100 % en las corridas de aceptación;
2. RTF p95 <1 y backlog no crece en 15 min;
3. p90 de primer texto estable <=1,5 s;
4. WER/CER humano no muestran regresión material frente a S0;
5. no hay más churn visual que S0;
6. arranque puede reproducirse desde runtime limpio con versiones fijadas;
7. licencia y distribución son compatibles con la tesis.

Si S3 y S4 pasan, elegir por este orden: WER humano, p90 de texto estable, confiabilidad, complejidad mantenida, cold-start. No elegir por un promedio ponderado opaco.

---

## 10. Cómo presentar los resultados en la tesis

La contribución no debe formularse como “se inventó un nuevo algoritmo STT”. La arquitectura y el aporte experimental son distintos:

1. **Plataforma embebida heterogénea:** FPGA para video/timing/overlay y ARM/Linux para captura/orquestación.
2. **Protocolo robusto end-to-end:** sesiones, secuencias, pérdidas, ACK, parciales/finales y degradación controlada.
3. **Adaptador independiente del motor:** permite comparar modelos A y B sin modificar firmware ni overlay.
4. **Metodología de evaluación:** separa latencia algorítmica, cómputo, transporte, estabilidad textual y confiabilidad.
5. **Evaluación empírica en español:** misma captura física, referencias humanas, métricas de exactitud y experiencia visual.
6. **Resultado de ingeniería:** selección justificada del motor que cumple el presupuesto bajo restricciones reales de Colab/PC y Arty Z7.

Una redacción defendible sería:

> Se diseñó y validó una arquitectura de subtitulado en vivo desacoplada del motor ASR, con transporte confiable entre una plataforma Zynq y un backend de inferencia. Sobre esa arquitectura se compararon un modelo offline adaptado y un modelo cache-aware de streaming nativo mediante métricas de exactitud, latencia estable, carga computacional, estabilidad visual y confiabilidad end-to-end en español.

Limitaciones que deben declararse:

- corpus propio pequeño;
- Colab no es infraestructura de producción y varía por sesión;
- túnel/red agregan jitter;
- WER publicados externamente no son comparables;
- ACK no equivale a observación física del HDMI;
- APIs/modelos de 2026 son recientes;
- sin referencias humanas, las corridas anteriores sólo tienen `offline_proxy`.

---

## 11. Registro de fuentes y versiones

Fuentes primarias consultadas el **24-07-2026**; las ramas `main` son mutables y deben fijarse por SHA al comenzar cada POC.

| Proyecto/modelo | Versión/revisión de referencia | Actividad/madurez observada | Fuente primaria |
|---|---|---|---|
| Nemotron 3.5 ASR Streaming | modelo v1, publicado 04-06-2026 | nuevo; 600 M, OpenMDW 1.1 | [model card NVIDIA](https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b) |
| Transformers Nemotron | 5.13.1; disponible desde 5.13.0 | streaming marcado experimental | [documentación](https://huggingface.co/docs/transformers/v5.13.1/en/model_doc/nemotron3_5_asr) |
| NeMo Speech | 26.06 / nightly consultada | Pipeline streaming activo | [inference docs](https://docs.nvidia.com/nemo/speech/nightly/asr/inference.html) |
| Speech NIM Nemotron | contenedor 1.2.0, release 26.05.0 | docs actualizadas 25-06-2026 | [release notes](https://docs.nvidia.com/nim/speech/latest/about/release-notes.html) |
| sherpa-onnx | changelog 1.12.38; bundle Nemotron 11-06-2026 | proyecto muy activo, Apache-2.0 | [Nemotron streaming docs](https://k2-fsa.github.io/sherpa/onnx/nemo/nemotron-streaming.html) |
| port C Nemotron | rama `w8a8`, 48 commits, sin releases | 7 estrellas, 0 forks/issues/PR visibles | [repositorio](https://github.com/kdrkdrkdr/nemotron-asr-streaming.c) |
| Whisper Turbo | `large-v3-turbo`, 809 M | modelo establecido, MIT | [OpenAI Whisper](https://github.com/openai/whisper/blob/main/README.md) |
| Faster-Whisper | 1.2.1; Turbo desde 1.1.0 | runtime maduro/activo | [releases](https://github.com/SYSTRAN/faster-whisper/releases) |
| SimulStreaming | repo MIT; proyecto fijado en `077ea37d...` | académico, soporte documentado para large-v3 | [repositorio](https://github.com/ufal/SimulStreaming) |
| WhisperLiveKit | 0.2.24, 11-07-2026 | beta; 813 commits, actividad alta | [PyPI](https://pypi.org/project/whisperlivekit/), [API](https://github.com/QuentinFuxa/WhisperLiveKit/blob/main/docs/API.md) |
| Collabora WhisperLive | 0.9.0, 02-06-2026 | beta activa, MIT | [PyPI](https://pypi.org/project/whisper-live/), [repo](https://github.com/collabora/WhisperLive) |
| OpenWhispr | 1.7.2, commit release `e35a364`, 20-05-2026 | app activa, 1612 commits visibles | [releases](https://github.com/OpenWhispr/openwhispr/releases), [arquitectura](https://github.com/OpenWhispr/openwhispr/blob/main/CLAUDE.md) |
| Parakeet TDT v3 | publicado 14-08-2025 | maduro para offline, CC-BY-4.0 | [model card](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3) |
| Voxtral Realtime | `2602`, 04-02-2026 | nativo, Apache-2.0, vLLM reciente | [model card open weights](https://huggingface.co/mistralai/Voxtral-Mini-4B-Realtime-2602) |
| Moonshine | `main` consultado | activo; streaming español aún ausente | [repositorio/model table](https://github.com/moonshine-ai/moonshine) |
| Speaches | 0.9.0-rc.3 visible | API activa/en transición, MIT | [repositorio](https://github.com/speaches-ai/speaches), [releases](https://github.com/speaches-ai/speaches/releases) |
| Arty Z7-20 | XC7Z020, 512 MB DDR3 | restricción física estable | [Digilent](https://digilent.com/shop/arty-z7-zynq-7000-soc-development-board/) |

## Decisión final

Si se detiene aquí la búsqueda de algoritmos, el orden de trabajo recomendado es:

1. crear las referencias humanas;
2. ejecutar **POC A: Nemotron 3.5 oficial/Transformers, `es-ES`, 320 ms, T4**;
3. como experimento posterior, ejecutar el cambio mínimo **Faster-Whisper Turbo**;
4. ejecutar **POC B: WhisperLiveKit + Turbo + LocalAgreement**;
5. comparar S0–S4 con el protocolo y gates anteriores;
6. si Nemotron gana pero Transformers complica EOU, migrar el mismo modelo a NeMo Pipeline; si se necesita PC CPU, evaluar sherpa-onnx;
7. sólo abrir Voxtral si hay L4 reproducible y Nemotron no alcanza calidad/estabilidad;
8. no invertir más tiempo en portar STT a la Arty ni en reconstruir OpenWhispr.

La apuesta técnica principal es **Nemotron 3.5**. La apuesta operacional conservadora es **WhisperLiveKit + Turbo**. El salvavidas de menor costo es **Turbo en el servidor actual**.
