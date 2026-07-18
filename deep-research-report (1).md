# Investigación técnica profunda sobre subtitulación incremental en vivo con Whisper y faster-whisper

## Resumen ejecutivo

A julio de 2026, el estado del arte **práctico** para “Whisper en vivo” no consiste, en general, en un modelo Whisper nativamente causal, sino en **envolver un modelo offline** con una política de emisión incremental. En la práctica, las dos familias más importantes hoy son: **LocalAgreement / commit de prefijo estable** sobre hipótesis rehechas periódicamente, y **AlignAtt**, que usa pesos de atención para frenar la decodificación antes de entrar en una “zona peligrosa” cerca del borde derecho del audio disponible. Whisper-Streaming popularizó la primera para habla larga y no segmentada; Simul-Whisper llevó Whisper a una política tipo AlignAtt; y SimulStreaming consolidó ambas líneas y afirma ser el mejor sistema del shared task IWSLT 2025 dentro de su escenario. citeturn41view3turn15view4turn12search2turn14view1turn42view0

Para tu objetivo específico —**subtítulos incrementales en español, estables, legibles y con latencia perceptual cercana a 1.5 s**— la conclusión principal es que **no conviene seguir afinando sólo `window/silence/beam` a ciegas**. Lo que más mueve la aguja en estabilidad visual no es el beam por sí solo, sino la **política de commit** y la **separación explícita entre texto provisional y texto comprometido**. Whisper-Streaming y los trabajos previos sobre LocalAgreement muestran que confirmar únicamente el prefijo común entre actualizaciones reduce revisiones; además, el paper de live subtitling recuerda que la latencia útil para el usuario no es sólo “inferencia”, sino también **cómo y cuándo se muestra** el texto en pantalla. citeturn38view3turn38view0turn31search1turn31search4

Tu latencia objetivo de ~1.5 s es **agresiva** frente a la literatura pública de Whisper incremental. Whisper-Streaming reportó **3.3 s de latencia media** en inglés sobre ESIC con una A40, y el propio paper explica que la latencia computacionalmente inconsciente se aproxima a unas dos veces el chunk size con LocalAgreement-2, aunque depende del idioma y del contexto. Eso no significa que 1.5 s sea imposible en tu sistema, pero sí que, con Whisper/faster-whisper, es más realista plantearlo como objetivo de **“primera aparición útil”** o **“prefijo estable corto”** que como **“frase final estable”** bajo ruido y audio continuo. citeturn15view0turn15view1turn41view3

La recomendación más accionable para tu tesis es esta. Si quieres **máximo valor con riesgo controlado**, mantén `faster-whisper` para inferencia y adopta una **política de commit explícita estilo LocalAgreement/longest-common-prefix**, junto con métricas de **latencia a primera aparición**, **latencia a estabilidad**, y **revision rate/flicker**. Si quieres acercarte más al estado del arte publicado, la referencia fuerte a estudiar y posiblemente adaptar es **SimulStreaming**, pero debes asumir que hoy está más acoplado a la implementación PyTorch/Whisper de AlignAtt que a CTranslate2/faster-whisper. citeturn38view3turn38view0turn17view5turn39view1turn14view1

Fecha de consulta y snapshot de repositorios: **2026-07-15**.

## Repositorios open source relevantes

### Tabla comparativa de repositorios

| Repositorio | URL exacta | Snapshot y mantenimiento | Arquitectura y backend | Política de partial/final y estabilidad | VAD y endpointing | Benchmarks publicados o claims útiles | Qué estudiar o reutilizar |
|---|---|---|---|---|---|---|---|
| **ufal/whisper_streaming** | `https://github.com/ufal/whisper_streaming` | ~3.6k stars, MIT, último commit visible **2025-11-12**; el propio README lo declara “outdated” y reemplazado por SimulStreaming. citeturn9view0turn41view3 | Reprocesa buffer de audio completo desde el inicio de la ventana activa; soporta `faster-whisper`, `whisper_timestamped`, `openai-api`, y el README habla de opciones de backend más diversas que SimulStreaming. citeturn37view4turn41view2 | Usa **LocalAgreement** con commit del **longest common prefix**; además genera prompt contextual desde texto ya confirmado para el audio que quedó fuera del buffer. Esto es exactamente la base conceptual de tus `partial_agreement=2`. citeturn41view3turn38view0turn38view2 | Tiene `--vac` recomendado, `--vad`, y trimming por `segment` o `sentence`; el README dice que `segment` rindió mejor en sus pruebas. citeturn37view0turn37view1turn37view3 | Paper IJCNLP demo: **3.3 s** de latencia media en inglés sobre ESIC con A40; el VAD mejoró calidad con poco impacto en algunos casos. citeturn15view0turn15view1 | **Muy recomendable** estudiar `whisper_online.py`, especialmente `HypothesisBuffer`, `prompt()`, `process_iter()`, y el servidor demo si quieres adaptar la lógica al pipeline Colab/WebSocket. citeturn38view3turn38view2 |
| **ufal/SimulStreaming** | `https://github.com/ufal/SimulStreaming` | ~600 stars, MIT, commits visibles hasta **2026-04-16**; README lo posiciona como follow-up de Whisper-Streaming y “state of the art 2025” en su shared task. citeturn5view1turn42view0 | Fusiona código de Whisper-Streaming, Simul-Whisper y OpenAI Whisper; backend Torch para Whisper/AlignAtt; soporta ASR y traducción en cascada con EuroLLM. Recomienda GPU con al menos **10 GB VRAM** para `large-v3`. citeturn17view2turn17view5 | Implementa **AlignAtt** y **LocalAgreement**; añade `frame_threshold`, controles de buffer, beams, prompts y contexto entre ventanas de 30 s. Para truncación de palabra final usa modelo **CIF** heredado de Simul-Whisper, aunque no hay CIF para `large-v3`. citeturn39view0turn39view2turn39view4 | Usa `--vac`; interfaz de simulación computacionalmente aware/unaware heredada de Whisper-Streaming. citeturn17view4turn39view2 | El README afirma ~**5×** más velocidad que WhisperStreaming y mejor calidad; el paper IWSLT 2025 describe el sistema como fusión de Simul-Whisper y Whisper-Streaming y lo usa con Whisper `large-v3`. La claim de 5× está en README y no en un benchmark reproducible de README detallado, así que hay que tratarla con cautela. citeturn42view0turn14view1 | **Estudio prioritario** si quieres una ruta ambiciosa: `simulstreaming_whisper.py`, `simulstreaming_whisper_server.py`, integración de `frame_threshold`, manejo de contexto, y la interfaz aware/unaware. citeturn39view0turn39view2 |
| **collabora/WhisperLive** | `https://github.com/collabora/WhisperLive` | ~4k stars, MIT, commits visibles hasta **2026-05-15**; mantenimiento claramente activo en 2026. citeturn9view1turn42view1 | Servidor WebSocket/REST “nearly-live” con backends `faster_whisper`, `tensorrt`, `openvino`, además de soporte ROCm y modo batch. Puede aceptar PCM crudo útil para sistemas embebidos. citeturn18view2turn18view5turn19view0 | Es muy bueno como **producto/servidor**, pero la documentación accesible no describe una política sofisticada de commit tipo LocalAgreement/AlignAtt; expone segmentos, word timestamps, hotwords y diarización, más orientado a serving que a investigación de commit policies. citeturn19view0 | Tiene opción `use_vad` del lado servidor; no documenta en README una política fina de endpointing/commit comparable a Whisper-Streaming. citeturn18view0 | No publica en README un benchmark metodológicamente comparable de latencia palabra→pantalla; sí aporta ingeniería práctica (single model mode, multi-client, batch, TensorRT/OpenVINO). citeturn18view4turn19view0 | **Muy reutilizable** para serving y protocolos: `run_server.py`, modo single-model, soporte PCM, batch e instrumentación Prometheus. Menos útil como referencia central de estabilización de hipótesis. citeturn19view0turn9view1 |
| **backspacetg/simul_whisper** | `https://github.com/backspacetg/simul_whisper` | 111 stars, Apache-2.0, repo pequeño con 15 commits; útil como artefacto de paper más que como plataforma larga. citeturn42view2 | Implementación PyTorch del paper **Simul-Whisper** con `AlignAttConfig`, `PaddedAlignAttWhisper`, buffers, `frame_threshold`, `buffer_len`, `min_seg_len`, y soporte de modelos base/small/medium/large-v2. citeturn42view2 | La estabilidad viene de **AlignAtt** más un detector de truncación en borde de chunk; el README explica el uso de un **240-sample buffer** para alinear mejor las CNN features entre streaming y offline. citeturn42view2 | No está orientado a habla larga no segmentada; el paper de IWSLT 2025 dice explícitamente que Simul-Whisper soporta ASR en habla segmentada por oración y simulación computacionalmente unaware. citeturn14view1 | El paper reporta degradación absoluta media de WER de sólo **1.46%** a chunk de 1 s frente al baseline offline, superando al baseline anterior. citeturn12search2 | **Muy importante** estudiar sus clases de AlignAtt y el mecanismo de truncation detection, pero **no** lo copiaría tal cual para tu pipeline largo/continuo sin adaptar un controlador de buffer y endpointing externo. citeturn42view2turn14view1 |
| **ggml-org/whisper.cpp example stream** | `https://github.com/ggml-org/whisper.cpp/blob/master/examples/stream/README.md` | `whisper.cpp` tiene ~51.8k stars; el ejemplo de streaming es explícitamente “naive”. citeturn26view1 | Captura micrófono y transcribe cada 500 ms o usa ventana deslizante con VAD básico. Es útil para edge/C++ por simplicidad y portabilidad. citeturn26view0 | No implementa commit sofisticado ni estabilización avanzada; el README mismo lo presenta como ejemplo simple. citeturn26view0 | Sliding-window VAD con `-vth`; el README sugiere **0.6** como valor general “OK”, pero aclara que conviene ajustarlo al caso de uso. citeturn26view0 | No publica WER/latencia de este modo como paper; sirve más como baseline ingenieril pequeño que como SOTA de subtitulado legible. citeturn26view0 | **Interesante** sólo si una versión futura de tu tesis quisiera mover parte de la lógica al lado FPGA/CPU/edge en C++; no es la mejor base para estabilidad visual avanzada. citeturn26view0 |
| **ScienceIO/whisper_streaming_web** | `https://github.com/ScienceIO/whisper_streaming_web` | 11 stars, MIT, 171 commits; extensión de Whisper-Streaming con FastAPI y WebSocket. citeturn36search1 | Browser/client demo con FFmpeg async streaming, múltiples usuarios, backend JS simple, opcional MLX y diarización beta. citeturn36search1 | Introduce una idea de interfaz muy valiosa: **“buffering preview”** que muestra el contenido no validado en gris y el validado en texto normal. Eso encaja casi exactamente con una política de overlay incremental legible. citeturn36search1 | Recomienda VAC para prevenir alucinaciones y alinea `min-chunk-size` frontend/backend. citeturn36search1 | No aporta benchmark fuerte; aporta sobre todo **UX incremental** y arquitectura web. citeturn36search1 | **Muy recomendable** copiar la semántica UI/protocolo de provisional vs confirmado, aunque no necesariamente su stack web completo. citeturn36search1 |
| **QuentinFuxa/WhisperLiveKit** | `https://github.com/QuentinFuxa/WhisperLiveKit` | ~10.5k stars, Apache-2.0, release **v0.2.24** el **2026-07-11**, desarrollo muy activo. citeturn25view0turn25view1 | Toolkit 2026 con servidor, WebSocket nativo, compatibilidad OpenAI/Deepgram, modo diff, backends `faster-whisper`, `whisper`, MLX, Qwen3-ASR, Voxtral y políticas `simulstreaming` o `localagreement`. citeturn21view0turn24view0turn24view4 | Es el integrador moderno más completo encontrado; permite elegir política, warmup, VAC/VAD, buffer sizing y beam. También ofrece **protocolos full vs diff**, valiosos para reducir tráfico y simplificar overlays. citeturn24view0turn24view2turn24view4 | Tiene VAC y VAD activados por defecto salvo `--no-vac/--no-vad`; en SimulStreaming backend expone `frame_threshold`, `audio-max-len`, `audio-min-len`, `beams`, `never-fire`, etc. citeturn24view0turn24view4 | Incluye tooling de benchmark (`wlk bench`, scatter plots), pero no usaría sus cifras como evidencia académica sin auditar corpus, hardware y modo aware/unaware. citeturn24view4 | **Muy útil** para aprender cómo empaquetar políticas de investigación en una API usable; aun así, es joven y rápido, así que hay riesgo de churn de API. Issues 2025–2026 muestran bugs en warmup y LocalAgreement/faster-whisper. citeturn23search9turn23search10 |

### Lectura crítica de lo que realmente importa

Si tu objetivo fuera sólo “tener STT rápido”, **WhisperLive** sería un candidato muy fuerte por ingeniería de serving. Pero tu objetivo es más estrecho y más difícil: **subtítulos incrementales estables y legibles**. Para eso, las piezas con más valor intelectual y reutilizable son **Whisper-Streaming** y **SimulStreaming**, porque exponen explícitamente la lógica de commit, buffer trimming, prompting contextual y simulación aware/unaware. citeturn41view3turn38view3turn39view2

La observación más importante es que **Whisper-Streaming no está “muerto”, pero sí quedó como baseline histórico**. El propio autor del repositorio dice que en 2025 está siendo reemplazado por **SimulStreaming**, que es “much faster and higher quality”, aunque también aclara que Whisper-Streaming todavía conserva la ventaja de más backends, entre ellos `faster-whisper`. Para una tesis centrada en un pipeline real con Colab y CTranslate2, esto es crucial: **el mejor algoritmo publicado y el backend que hoy te resulta más práctico no están perfectamente alineados**. citeturn41view2

Esa tensión define la recomendación estratégica. Si quieres una mejora robusta de corto plazo, toma la **política** de Whisper-Streaming y reimpléméntala sobre tu servidor `faster-whisper`. Si quieres máxima cercanía al estado del arte de papers recientes, estudia **SimulStreaming/AlignAtt**, pero asume un posible cambio de backend o un puente híbrido, porque AlignAtt depende de información interna de atención que `faster-whisper` no expone igual que la implementación Torch. Esto último es una **inferencia técnica** apoyada en cómo SimulStreaming y Simul-Whisper describen su dependencia de atención/decoder internos. citeturn39view1turn39view2turn42view2

## Papers y algoritmos primarios

### Tabla comparativa de papers y documentación primaria

| Trabajo | Qué aporta | Resultado o idea más útil para ti | Límites para tu caso |
|---|---|---|---|
| **Whisper** de Radford et al. | Base del modelo offline multilingüe; released model series y trade-off por tamaño. OpenAI añade `large-v3` y luego `turbo`; `turbo` prioriza velocidad y tiene degradación menor en accuracy general, pero no está pensado para traducción. citeturn11view1turn11view3turn11view5 | Justifica comparar `small`, `medium`, `large-v3`, `turbo` y fijar `language="es"`. También recuerda que el rendimiento varía por idioma. citeturn11view1turn11view3 | No es un paper de streaming; por sí solo no resuelve commits incrementales. citeturn11view1 |
| **CUNI-KIT IWSLT 2022** | Formaliza la onlinización de un modelo offline con **stable hypothesis detection** y generaliza **LA-n**. Define LocalAgreement como el **prefijo común más largo** de hipótesis consecutivas. citeturn14view2turn15view3 | Es la base teórica más limpia para tu actual `partial_agreement=2`. También propone separar calidad-latencia con tamaño de chunk y política de commit. citeturn14view2turn15view3 | Nace en SimulST más que en ASR monolingüe; algunas decisiones deben adaptarse a subtítulos de un solo idioma. citeturn14view2 |
| **Turning Whisper into Real-Time Transcription System** | Lleva Whisper a tiempo real con **LocalAgreement** y latencia auto-adaptativa; evalúa habla larga no segmentada. citeturn15view0turn41view3 | Reporta **3.3 s** de latencia media en inglés con A40 y explica el rol del VAD, del chunk mínimo y de la evaluación aware/unaware. citeturn15view0turn15view1turn37view4 | Sigue siendo un wrapper incremental de Whisper; no evita reprocesar el buffer. citeturn14view1 |
| **ALIGNATT** de Papi et al. | Política de inferencia que usa **atención** para decidir hasta dónde decodificar sin comprometer tokens alineados con los frames finales del audio disponible. Reporta mejoras de BLEU y reducciones de latencia en 8 pares de idiomas de MuST-C. citeturn15view4 | Conceptualmente, es la mejor referencia cuando quieres reducir revisiones **sin depender sólo de agreement entre hipótesis completas**. citeturn15view4 | Requiere acceso a atención y a internals del decoder; integración directa en `faster-whisper/CTranslate2` no es obvia. Esto es una **inferencia** apoyada en la naturaleza del método y en la implementación Torch de Simul-Whisper/SimulStreaming. citeturn15view4turn42view2 |
| **Simul-Whisper** | Aplica AlignAtt a Whisper y añade **truncation detection** en fronteras de chunk; reporta degradación absoluta media de WER de sólo **1.46%** con chunk de 1 s. citeturn12search2turn42view2 | Es la evidencia más fuerte de que Whisper puede acercarse a streaming razonable sin fine-tuning del backbone. citeturn12search2 | El propio paper IWSLT 2025 dice que soporta sólo ASR en habla segmentada por oración y simulación no computacionalmente consciente. citeturn14view1 |
| **CUNI IWSLT 2025 / SimulStreaming** | Sitúa **AlignAtt** por encima de LocalAgreement en 2025, describe la fusión Simul-Whisper + Whisper-Streaming, y explica por qué Whisper-Streaming es menos eficiente computacionalmente al reprocesar el buffer desde el inicio. citeturn14view1 | Es la referencia más actual para decidir entre perseverar con LocalAgreement o migrar hacia AlignAtt. citeturn14view1 | Su implementación Whisper usa Torch; no es una guía directa para CTranslate2. citeturn17view5 |
| **LAAL** | Corrige sesgos de Average Lagging cuando hay sobre-generación. citeturn30search0turn30search15 | Muy útil si presentas una métrica “académica” de simultaneidad junto a métricas de UX. citeturn30search15 | No mide flicker ni revisiones de subtítulos directamente. citeturn30search15 |
| **ATD** | Propone Average Token Delay como métrica más sensible a la duración de parciales y más cercana a latencia percibida en algunos escenarios. citeturn30search1turn30search10 | Útil si quieres una métrica secundaria más fina que AL/LAAL para parciales. citeturn30search10 | Sigue siendo métrica de simultaneidad, no de legibilidad visual por sí sola. citeturn30search10 |
| **Simultaneous Speech Translation for Live Subtitling** | Conecta directamente delay, reading speed y modo de visualización en subtitulado en vivo; encuentra que líneas en scroll pueden mantener delay cerca del umbral de 4 s con mejor legibilidad que el modo palabra-a-palabra. citeturn31search1turn31search4 | Importante para defender en tesis que **no basta con bajar WER**; hay que medir **display strategy**. citeturn31search4 | Es ST, no ASR monolingüe; el umbral de 4 s no debe tomarse como objetivo universal. citeturn31search1 |
| **Text stability / flicker papers** | Proponen flicker basado en vídeo/contraste o reducciones de flicker por reranking de hipótesis parciales. citeturn32search1turn32search13turn31search6turn32search5 | Te dan base bibliográfica para medir **revision rate/flicker**, algo muy valioso en subtitulación incremental. citeturn32search13turn31search6 | Su setting no es idéntico a Whisper-faster-whisper; conviene adaptar la métrica, no copiarla ciegamente. citeturn32search13turn31search6 |

### Qué enseñan en conjunto

La lección conjunta de estos trabajos es que hay dos tipos de avance. Un primer grupo mejora la **política de emisión** sin tocar el backbone offline —LocalAgreement, LA-n, AlignAtt—. Un segundo grupo mejora la **experiencia visible** —display de subtítulos, flicker, estabilidad—. Tu sistema necesita ambos. Si optimizas sólo inferencia, seguirás teniendo parciales que “respiran” demasiado; si optimizas sólo la UI, seguirás cargando errores del ASR. citeturn15view3turn15view4turn31search4turn32search13

## Estrategias, selección de modelo y parámetros prácticos

### Comparación de estrategias de arquitectura y commit

| Estrategia | Latencia esperable | Impacto en WER | Estabilidad visual | Uso de GPU | Complejidad | Adecuación a español y a tu pipeline |
|---|---|---|---|---|---|---|
| **Reprocesar ventana deslizante completa** | Buena para primeras hipótesis si la ventana es corta; empeora cuando el buffer crece. citeturn37view4turn14view1 | Suele ser razonable porque conserva contexto reciente. citeturn41view3 | Mala si se publican hipótesis sin política de commit. citeturn36search1turn31search6 | Alto: redecodifica audio ya visto. citeturn14view1 | Baja | **Sí**; es lo que ya haces. |
| **Ventanas solapadas + deduplicación** | Similar o algo peor que la anterior; depende del solape. | Puede reducir cortes de palabra en fronteras. | Media; la deduplicación ayuda, pero no resuelve revisiones profundas. | Alto | Media | **Sí**, sobre todo como baseline controlado. |
| **LocalAgreement entre dos o más hipótesis** | Muy buena relación costo/beneficio; LA-2 suele ser el punto práctico. citeturn15view3turn38view0 | Ligera pérdida frente a offline, compensada por mejor timing. citeturn14view2turn15view1 | **Alta** para prefijos; excelente para overlay incremental. citeturn38view0turn31search6 | Medio-alto | Media | **Muy adecuada** a tu arquitectura actual con `faster-whisper`. |
| **Confirmación basada en timestamps/tokens** | Muy baja cuando los timestamps son fiables. | Riesgo si timestamps fluctúan en ruido o con cortes internos. | Media-alta si se usa conservadoramente. | Medio | Media | **Adecuada** como criterio auxiliar, no como único criterio. |
| **Commit de prefijos estables** | Similar a LocalAgreement; de hecho es su forma práctica. | Bueno si el prefijo se define bien. | **Alta** | Medio | Media | **Sí**, y es lo que deberías formalizar explícitamente en tesis. |
| **VAD + finalización por silencio** | Excelente para “finales”; peor para audio continuo sin pausas. citeturn15view1turn37view0 | Mejora calidad si evita basura no hablada; puede fragmentar mal si el threshold no acompaña al ruido. citeturn15view1turn27search10 | Alta para finales, media para parciales | Bajo-medio | Baja | **Sí**, pero no debe ser tu única política de finalización. |
| **Context prompting desde segmentos confirmados** | No baja latencia por sí mismo; mejora continuidad textual. citeturn38view2turn39view4 | Puede mejorar consistencia léxica y nombres propios. | Incrementa estabilidad semántica. | Medio | Baja-media | **Muy recomendable** para español y dominios repetitivos. |
| **Partial rápido + final preciso** | Muy buena percepción de inmediatez. | Si se separan beams/modelos, puedes contener WER final. | Alta si el partial es “gris” y el final “duro”. citeturn36search1 | Medio | Media | **Sí; probablemente tu mejor ruta práctica**. |
| **Modelo distinto para partials y finales** | Muy buena percepción si el partial usa modelo pequeño. | Riesgo de inconsistencias entre partial y final. | Media-alta si el UI marca bien el estado. | Alto por duplicación | Alta | **Posible**, pero más costoso de integrar en Colab. |
| **Beam 1 para partials y beam mayor para finales** | Reduce cola y backpressure en parciales; mantiene calidad final. Esto es una **inferencia** coherente con los parámetros expuestos por SimulStreaming/WhisperLiveKit y con el mayor costo de beam en decodificación. citeturn39view0turn24view4turn33view4 | Final mejor que partial; partial algo menos preciso. | Alta si el partial no se “sobrepromete”. | Medio | Baja-media | **Muy recomendable** para tu servidor actual. |
| **Reuse de encoder/cache** | Es la dirección correcta cuando es posible, porque evita reaplicar cómputo al pasado; AlignAtt va en ese espíritu aunque no como cache CTranslate2 explícita. citeturn15view4turn14view1 | Suele proteger calidad si el método está bien diseñado. | Alta | Bajo-medio | Alta | **Difícil** con `faster-whisper` puro hoy. |
| **Sistema realmente streaming vs wrapper incremental** | El realmente streaming gana en costo por segundo estable; Whisper clásico no lo es. WhisperLiveKit ya referencia modelos causales 2026 como Qwen3-ASR-causal fuera del ecosistema Whisper. citeturn21view0 | Puede mejorar la estabilidad estructural. | Muy alta | Bajo-medio | Muy alta o cambio de modelo | **Fuera del núcleo Whisper**, útil sólo como opción ambiciosa de comparación. |

La síntesis práctica es simple. Para tu tesis, las estrategias con mejor retorno son **LocalAgreement/prefijo estable**, **partial rápido + final preciso**, **beam asimétrico**, **prompt/context desde texto comprometido**, y **VAD como ayuda de endpointing, no como árbitro absoluto**. AlignAtt es la apuesta de mayor nivel técnico, pero la más difícil de casar con `faster-whisper`. citeturn38view3turn39view1turn24view4

### Selección del modelo para español en Colab

Los tamaños oficiales de Whisper siguen siendo, entre los que más te interesan, **small = 244M**, **medium = 769M**, **large = 1550M**, y **turbo = 798M**; `large-v3` es la variante grande moderna, y `turbo` es una versión acelerada derivada de `large-v3` con sólo 4 capas decodificadoras, más rápida pero con una pequeña degradación de accuracy general. OpenAI aclara además que `turbo` no está pensado para traducción, aunque para **transcripción** sí es una opción válida. citeturn11view3turn11view4turn11view5turn11view1

Para tu caso monolingüe en español, la jerarquía esperable de calidad offline es, en términos generales, **large-v3 ≥ turbo ≳ medium > small**, pero **las fuentes primarias no ofrecen un benchmark español incremental, comparable, en el mismo hardware y bajo las mismas políticas de commit** para esas cuatro opciones. Por eso, cualquier afirmación cuantitativa más específica sería débil; lo correcto en una tesis es presentar esa jerarquía como **expectativa razonable**, no como cifra cerrada. citeturn11view1turn11view3turn34search1

En velocidad/memoria sí hay evidencias útiles. `faster-whisper` documenta, en RTX 3070 Ti 8 GB, que `large-v2` en FP16 con beam 5 tarda **1m03s** para 13 minutos de audio y usa ~**4525 MB** de VRAM; en `int8` baja a **2926 MB** con algo más de tiempo. También documenta un benchmark de `distil-whisper-large-v3` con mejor tiempo que Transformers en el mismo hardware. CTranslate2 soporta `float16`, `int8`, `int8_float16`, entre otros tipos, y su documentación dice que la cuantización suele reducir tamaño y acelerar con poca o ninguna degradación apreciable. citeturn33view4turn11view2

Para Google Colab, la inferencia más segura es esta. **`small`** seguirá siendo la opción más robusta si tu prioridad absoluta es no acumular cola; **`medium`** es probablemente el primer candidato a probar si buscas bajar WER en español sin irte a una explosión de latencia; **`large-v3`** vale la pena sólo si el ruido real y el valor académico de la comparación justifican la presión extra sobre VRAM, warmup y cola; **`turbo`** merece ser probado porque ataca exactamente el cuello de botella de decodificación, pero su comportamiento en subtítulos incrementales en español debe validarse en tu corpus, no asumirse. Las partes “probablemente / merece ser probado” son **inferencias propias** apoyadas en tamaños oficiales, notas de OpenAI y benchmarks de `faster-whisper`. citeturn11view3turn11view4turn11view5turn33view4

La idea de usar **beam=1 en parciales y beam=5 en finales** es técnicamente sensata. SimulStreaming y WhisperLiveKit exponen beams/decoder como parámetros; `faster-whisper` recuerda además que el costo y la comparabilidad dependen mucho del beam. Para un pipeline en tiempo real, bajar beam en parciales reduce el tiempo de decodificación y, por lo tanto, la probabilidad de que el job parcial llegue tarde y sea descartado por backpressure. Eso último es una **inferencia de scheduling**, coherente con el modo aware/unaware de Whisper-Streaming/SimulStreaming. citeturn39view0turn24view4turn11view0turn37view4

### Tabla de parámetros encontrados en fuentes

| Parámetro o regla | Fuente | Valor o recomendación publicada | Cómo usarlo en tu sistema |
|---|---|---|---|
| `faster-whisper` VAD por defecto | README / issue oficial | Comportamiento **conservador**; elimina silencios sólo si duran más de **2 s** por defecto. citeturn27search3turn27search1 | Tu `min_silence_sec=0.3–0.5` es mucho más agresivo. Eso puede ser correcto para subtitulación, pero ya no estás en zona “default”; debes reportarlo como decisión experimental propia. |
| `Silero` threshold | issue del repo Silero | `threshold=0.5` por defecto; el equipo dice que conviene tunear por dataset, aunque “lazy 0.5 is pretty good for most datasets”. citeturn27search10 | Tu `vad_threshold=0.5` está perfectamente alineado con el punto de partida oficial. |
| Histéresis / negative threshold | fuentes accesibles del repo | No encontré en la documentación indexada del repo una regla universal tan fuerte como para consagrar un `neg_threshold` único; sí encontré, en tu campo de herramientas, implementaciones que usan histéresis y estados. citeturn27search5turn27search8 | Mantén `0.35` como elección experimental plausible, pero preséntala como **heurística validada por experimento**, no como estándar publicado. |
| `speech_pad_ms` / padding | wrappers derivados de Silero | Ejemplos derivados usan `speech_pad_ms=30` y `min_silence_duration_ms=100` en `VADIterator` tipo stream. citeturn27search12 | Útil como referencia para no cortar fonemas en borde, pero debes dejar claro que no es la única parametrización válida. |
| `whisper.cpp -vth` | example oficial | valor “alrededor de **0.6**” se considera razonable en general, pero debe ajustarse al uso. citeturn26view0 | Buen ancla comparativa si alguna vez armas un baseline C++/edge. |
| `min-chunk-size` en Whisper-Streaming | README oficial | ejemplo con **1 s**; en modo aware el chunk puede crecer si el cómputo tarda más que el periodo. citeturn37view4 | Esto describe muy bien tu problema de backpressure. |
| `buffer_trimming` | Whisper-Streaming | `segment` es el **default** y rindió mejor en sus pruebas que `sentence`. citeturn37view0 | Para subtitulado incremental monolingüe, `segment` es la apuesta inicial correcta. |
| `frame_threshold` AlignAtt | SimulStreaming | se mide en frames; para `large-v3`, **1 frame = 0.02 s**. Menor umbral = más rápido, mayor = más preciso, según la parametrización de tools como WhisperLiveKit. citeturn39view0turn24view4 | Si migras a AlignAtt, este será uno de los parámetros clave del trade-off. |
| `audio_max_len` / `audio_min_len` | SimulStreaming / WhisperLiveKit | ambos sistemas exponen longitudes máximas y mínimas de buffer precisamente para controlar costo y activación. citeturn39view3turn24view4 | Excelente candidato para reemplazar decisiones implícitas de `max_window_sec` por algo más formal. |
| `language` explícito | faster-whisper-server README | especificar idioma reduce tiempo de transcripción. citeturn20view4 | En tu caso, fijar siempre `es` es recomendable. |

La inferencia más importante sobre VAD es esta. En subtitulación en vivo con ruido de TV/fútbol de fondo, **no existe un threshold universal**. Lo que sí existe en las fuentes es una tendencia clara: empezar cerca de **0.5**, usar histéresis/estado, y ajustar en función del dominio; además, el VAD de `faster-whisper` está pensado como filtro conservador de batch/offline, no como controlador fino de subtítulo incremental de 1–1.5 s. citeturn27search10turn27search3turn37view0

## Métricas correctas para una tesis y para un overlay real

No conviene defender el objetivo de ~1.5 s con una sola métrica. La literatura de simultaneidad muestra que **AL**, **LAAL** y **ATD** capturan distintos aspectos de lag; la literatura de live subtitling muestra que **delay sin legibilidad visible no basta**; y la literatura de text stability muestra que **flicker/revisión** influye directamente en la experiencia de uso. citeturn30search15turn30search10turn31search4turn32search13

Para tu tesis, yo separaría las métricas en tres capas.

Primera capa, **calidad ASR final**. Aquí sí usaría **WER** y **CER** contra referencia humana, más una comparación contra pseudorreferencia offline sólo como métrica auxiliar interna de desarrollo, nunca como métrica principal de tesis. Whisper y OpenAI siguen usando WER/CER como base por idioma, así que son defendibles académicamente. citeturn11view1turn34search15

Segunda capa, **latencia incremental útil**. Aquí propondría medir: **tiempo hasta el primer subtítulo**, **latencia palabra→primera aparición**, **latencia palabra→texto estable**, y **latencia de finalización de frase**. En terminología de simultaneidad, puedes añadir **LAAL** como métrica académica secundaria y **ATD** como complemento cuando quieras reflejar mejor el coste temporal de parciales y cambios. Eso te permitiría argumentar “objetivo ~1.5 s” de un modo preciso: no como “latencia promedio del servidor”, sino como **latencia perceptual hasta una primera aparición útil y/o hasta un prefijo estable**. citeturn30search15turn30search10turn37view4

Tercera capa, **estabilidad visual y entrega real**. Aquí están tus métricas más originales y más valiosas: **revision rate** por token/palabra, **longitud media del prefijo estable**, **flicker rate** por evento visible, **frecuencia de actualización**, **tiempo mínimo de permanencia visible**, **partials skipped**, **finales perdidos**, **ack lógico firmware**, y **aparición física en HDMI**. Este último punto es importante: tu ACK actual certifica recepción y encolado, no la visibilidad del pixel. En tesis, yo separaría explícitamente **latencia lógica** y **latencia física**. La literatura de live captions y flicker te ayuda a justificar por qué la estabilidad visible debe medirse, no asumirse. citeturn31search6turn32search13turn31search18

Una formulación defendible sería esta. Tu sistema cumple el objetivo cuando logra, por ejemplo, **p90 ≤ 1.5 s** en **latencia palabra→primera aparición útil** y además mantiene **revision rate** por debajo de un umbral que definas experimentalmente, con **WER final** dentro de un rango aceptable. Eso es mucho más sólido que reclamar “1.5 s” mezclando inferencia, red, VAD y render HDMI en una sola cifra. citeturn37view4turn31search4turn32search13

## Recomendación concreta para tu sistema

### Opción conservadora

| Elemento | Recomendación |
|---|---|
| Modelo | `small` como baseline productivo; `medium` como candidato inmediato de mejora |
| Backend | `faster-whisper` + CTranslate2 |
| Window | `max_window_sec = 3.0` |
| Silence | `min_silence_sec = 0.5` para finales; no usarlo como único commit |
| Partial interval | `1.0 s` |
| Agreement/commit | Convertir tu `partial_agreement=2` en **commit formal de prefijo estable**; mostrar el resto sólo como provisional |
| VAD | Silero `0.5 / 0.35`, con experimentos limitados alrededor |
| Beam | `beam=1` parciales, `beam=5` finales |
| Contexto | Prompt desde texto ya confirmado fuera del buffer, estilo Whisper-Streaming |
| Latencia estimada | **Inferencia propia**: primer parcial útil p90 ≈ **1.1–1.6 s**; texto estable corto p90 ≈ **1.4–2.0 s** en tu arquitectura actual si el backpressure parcial se controla |
| Calidad estimada | Similar o algo mejor que tu baseline actual; la mejora principal vendría de estabilidad visible más que de WER bruto |
| Riesgo | Bajo |
| Trabajo de integración | Bajo-medio |

Esta es la opción que más recomendaría si tu prioridad es avanzar la tesis sin reescribir media plataforma. En esencia, no cambia tu backend ni tu infraestructura; cambia la **semántica del overlay** y la **lógica de commit**. Lo más importante es dejar de pensar en “partials reemplazables” como un stream plano y pasar a un modelo de **prefijo comprometido + cola provisional**. Esto está alineado con LocalAgreement y con la interfaz de preview no validado de `whisper_streaming_web`. citeturn38view3turn38view0turn36search1

### Opción intermedia

| Elemento | Recomendación |
|---|---|
| Modelo | `medium` para parciales y finales, o `small` para parciales con `medium` sólo para finales si el budget de Colab lo permite |
| Backend | `faster-whisper` para producción, inspirándote en **Whisper-Streaming** |
| Window | buffer máximo 3–4 s, trimming por `segment` |
| Silence | `0.4–0.6 s` |
| Partial interval | `0.75–1.0 s` |
| Agreement/commit | **LocalAgreement-2** explícito sobre palabras/timestamps, con política de commit por longest common prefix |
| VAD | VAC/VAD híbrido: VAD para recorte/flush y endpointing por silencio, pero commit gobernado por agreement |
| Beam | `1` parciales, `3–5` finales |
| Contexto | Prompt deslizante de ~200 caracteres confirmados, como en `prompt()` de Whisper-Streaming |
| Latencia estimada | **Inferencia propia**: prefijo estable p90 ≈ **1.3–1.8 s**; final de frase p90 ≈ **1.8–2.6 s** |
| Calidad estimada | Mejor equilibrio entre WER y estabilidad que la opción conservadora |
| Riesgo | Medio |
| Trabajo de integración | Medio |

Esta sería, en mi opinión, la **mejor opción global** para tu proyecto ahora mismo. Aprovecha lo que ya tienes, usa la literatura correcta, y es lo bastante seria para defenderse en una tesis porque se apoya en una política publicada y explicable matemáticamente. citeturn15view3turn41view3turn38view2

### Opción ambiciosa

| Elemento | Recomendación |
|---|---|
| Modelo | `large-v3` o `turbo` para comparar; si migras a AlignAtt estricto, priorizar la variante realmente soportada por la implementación elegida |
| Backend | **SimulStreaming / Simul-Whisper style** con Torch Whisper y AlignAtt |
| Window | chunk mínimo ~1.0 s, más `frame_threshold` como principal perilla de latencia |
| Silence | VAC recomendado; endpointing por silencio sólo para flush largo |
| Partial interval | gobernado por chunk mínimo y política de atención |
| Agreement/commit | **AlignAtt**; opcionalmente combinado con truncación de última palabra |
| VAD | VAC + controlador de buffer |
| Beam | empezar con 1 y subir sólo si el costo lo tolera |
| Contexto | prompts y contexto entre ventanas, como en SimulStreaming |
| Latencia estimada | Potencialmente la mejor entre las opciones Whisper publicadas, pero **sin garantía directa en tu pipeline Colab/WebSocket** |
| Calidad estimada | La más prometedora de las opciones Whisper incrementales publicadas |
| Riesgo | Alto |
| Trabajo de integración | Alto |

La razón para tratarla como ambiciosa es clara. SimulStreaming es hoy la referencia más cercana al estado del arte **dentro del ecosistema Whisper** que encontré, pero su ventaja viene justamente de una integración más íntima con el decoder y la atención. Eso choca con la simplicidad y velocidad de `faster-whisper/CTranslate2`. En una tesis, esta opción es excelente como **línea futura o experimento comparativo alto riesgo/alto premio**, no necesariamente como camino único de producción. citeturn14view1turn39view1turn39view2

### Recomendación final

Si tuviera que elegir una sola ruta accionable para tu sistema actual, elegiría la **opción intermedia**: `faster-whisper` + **LocalAgreement-2 explícito** + **beam asimétrico** + **prompt contextual de texto confirmado** + **overlay con estado provisional/confirmado**. Esa combinación es la que mejor equilibra integrabilidad, rigor bibliográfico y probabilidad real de acercarte a **1.5 s perceptuales** sin destruir legibilidad. citeturn38view3turn36search1turn37view4

## Plan experimental eficiente

No te conviene una matriz combinatoria grande. Conviene un plan secuencial en el orden que pediste, controlando variación de Colab y separando error de modelo, error de streaming y error de entrega.

### Diseño general

Usaría un corpus pequeño pero estratificado en español con al menos tres condiciones: **limpio**, **ruidoso moderado**, y **ruido continuo/TV/fútbol**. Para cada condición, prepara una **referencia humana** con timestamps aproximados de palabra o, si no es viable, al menos de segmentos cortos. Mantén fijo el pipeline de red y firmware durante la fase inicial para que el error principal venga del ASR/streaming. La evaluación de HDMI físico debería ir al final como experimento separado. Esta separación es una recomendación metodológica propia apoyada en la diferencia entre métricas de simultaneidad y métricas de display. citeturn31search4turn32search13

### Secuencia exacta de experimentos

| Fase | Qué comparas | Qué mantienes fijo | Métrica primaria | Criterio de descarte |
|---|---|---|---|---|
| **Modelo** | `small` vs `medium` vs `turbo`, y sólo después `large-v3` | `window=3.0`, `silence=0.5`, partial cada 1.0 s, beam simétrico 1/1 para speed screen | WER final, RTF, p90 palabra→primera aparición | Descartar modelos cuyo p90 de primera aparición exceda claramente tu objetivo aun con beam 1 |
| **Window / silence** | 2–3 combinaciones: `(3.0,0.5)`, `(3.0,0.4)`, `(4.0,0.5)` | modelo ganador de fase 1, sin agreement sofisticado todavía | WER final, p90 finalización, cortes por VAD | Descartar settings con sobrefragmentación o finales demasiado tardíos |
| **Política de partials** | full replacement vs prefijo comprometido + cola provisional | mismo modelo y VAD | revision rate, flicker, prefijo estable | Si la política no reduce revisiones visibles de forma clara, descártala |
| **Agreement / commit** | none vs LA-2 vs timestamp-assisted prefix commit | lo demás fijo | latencia a estabilidad, revision rate | Mantener sólo políticas que mejoren estabilidad sin disparar WER final |
| **Beam partial/final** | `1/1`, `1/3`, `1/5`, `3/5` | modelo/policy ganadores | p90 primera aparición, p90 estabilidad, WER final | Descartar configuraciones que introduzcan cola notable |
| **VAD** | `0.5/0.35` vs dos variantes pequeñas alrededor | todo lo demás fijo | finales correctos, falsos flush, WER en ruido | Descartar thresholds que empiecen a cortar palabras o a no cerrar nunca |
| **Context prompting** | off vs prompt de confirmados | configuración ganadora | nombres propios, coherencia textual, WER/CER | Mantener sólo si mejora consistente o al menos no empeora latencia |

### Réplicas y control de Colab

Para cada corrida clave, yo haría **tres réplicas** no consecutivas y con orden intercalado A-B-C / B-C-A / C-A-B, para amortiguar la variabilidad de GPU compartida y warm/cold cache. Whisper-Streaming y SimulStreaming distinguen explícitamente entre simulación **computationally aware** y **unaware**; úsalo como inspiración metodológica: mide siempre ambas cosas en tu sistema, aunque sea de forma casera. La versión aware es la que vale para producto; la unaware te da el **lower bound algorítmico**. citeturn37view4turn39view2

Además, registra por experimento: tiempo de warmup, cola de GPU, tiempo de inferencia p50/p90, jobs parciales omitidos, finales perdidos, RTT WebSocket y tiempo lógico hasta ACK de firmware. Si luego haces una fase HDMI física, añade una medida con marca visual sincronizada para cuantificar **entrega lógica vs aparición real**. Esa separación no está resuelta por los repositorios, pero es exactamente el tipo de contribución experimental fuerte que diferencia una tesis de una demo. citeturn31search4turn32search13

## Riesgos, preguntas abiertas, qué copiar o adaptar y bibliografía

### Riesgos y preguntas abiertas

El riesgo técnico más grande es este: **AlignAtt parece superior a LocalAgreement en 2025, pero hoy no cae naturalmente sobre `faster-whisper/CTranslate2`**. Si tu tesis necesita quedarse cerca de Colab + CTranslate2, puede que el mejor algoritmo publicado no sea el mejor algoritmo integrable. citeturn14view1turn39view1

El segundo riesgo es metodológico. Muchos repositorios publican demos, claims de “real-time” o benchmarks parciales, pero **muy pocos separan con rigor** inferencia, cola, política de commit, red, UI y render físico. Por eso debes evitar cualquier cifra que mezcle esas capas como si fuera una sola latencia universal. citeturn37view4turn31search4

El tercer riesgo es de mantenimiento. Whisper-Streaming sigue siendo brillante como referencia, pero su propio autor lo da por superado por SimulStreaming. Simul-Whisper, por su parte, es valioso como artefacto de paper, pero no parece la base más madura para un sistema largo y mantenible. WhisperLiveKit es potentísimo, aunque muy cambiante; sus issues muestran precisamente problemas en warmup y combinaciones policy/backend. citeturn41view2turn42view2turn23search9turn23search10

### Qué copiar o adaptar

Si sólo pudiera señalar **archivos concretos** para estudiar o portar, elegiría estos:

**De `ufal/whisper_streaming`**
- `whisper_online.py`: la clase **`HypothesisBuffer`**, especialmente `insert()` y `flush()`, porque ahí está la lógica de **longest common prefix** de dos actualizaciones consecutivas. citeturn38view3turn38view0
- `whisper_online.py`: `prompt()` y `process_iter()`, porque resuelven muy elegantemente el **prompt contextual desde texto comprometido** y el **reprocesado controlado** del buffer. citeturn38view2
- `silero_vad_iterator.py`: como referencia del controlador de voz/silencio en stream. citeturn37view5

**De `ufal/SimulStreaming`**
- `simulstreaming_whisper.py`: para entender cómo empaquetan **AlignAtt**, `frame_threshold`, `audio_max_len`, `audio_min_len` y prompts/contexto. citeturn39view0turn39view3
- `simulstreaming_whisper_server.py`: como referencia de servidor y simulación aware/unaware. citeturn39view2
- La lógica de truncación de última palabra con `cif_ckpt_path`, aunque con la salvedad de que no hay CIF para `large-v3`. citeturn39view2

**De `backspacetg/simul_whisper`**
- `AlignAttConfig`
- `PaddedAlignAttWhisper`
- `SegmentWrapper`
- La mecánica del **240-sample boundary buffer** y el flujo `model.infer()` / `model.refresh_segment()` para entender el coste y las suposiciones de borde. citeturn42view2

**De `ScienceIO/whisper_streaming_web`**
- La idea de **preview no validado en gris** y confirmado en texto normal. Aunque sea un detalle de frontend, para subtitulado embebido es probablemente una de las ideas de UX más rentables de toda la búsqueda. citeturn36search1

**De `WhisperLive`**
- `run_server.py` y el modo **single model mode**, además del soporte **raw PCM**. Esto encaja muy bien con tu bridge FPGA→PC→GPU. citeturn19view0

Mi recomendación concreta de adaptación sería: **copiar la política y las estructuras de commit de Whisper-Streaming, no su backend completo**; copiar la semántica visual provisional/confirmado de `whisper_streaming_web`; y usar WhisperLive como inspiración de serving/PCM si necesitas endurecer el servidor. SimulStreaming quedaría como referencia avanzada para una segunda etapa, o como baseline de comparación académica si decides prototipar AlignAtt en paralelo. citeturn38view3turn36search1turn19view0turn14view1

### Bibliografía con enlaces directos

- `https://github.com/ufal/whisper_streaming` — repo principal Whisper-Streaming. citeturn41view3  
- `https://www.afnlp.org/conferences/ijcnlp2023/proceedings/main-demo/cdrom/pdf/2023.ijcnlp-demo.3.pdf` — *Turning Whisper into Real-Time Transcription System*. citeturn14view0  
- `https://github.com/ufal/SimulStreaming` — repo principal SimulStreaming. citeturn17view2  
- `https://aclanthology.org/2025.iwslt-1.41.pdf` — *Simultaneous Translation with Offline Speech and LLM Models in CUNI Submission to IWSLT 2025*. citeturn14view1  
- `https://github.com/backspacetg/simul_whisper` — repo Simul-Whisper. citeturn42view2  
- `https://arxiv.org/abs/2406.10052` — *Simul-Whisper: Attention-Guided Streaming Whisper with Truncation Detection*. citeturn12search2  
- `https://www.isca-archive.org/interspeech_2023/papi23_interspeech.pdf` — *ALIGNATT*. citeturn14view3  
- `https://aclanthology.org/2022.iwslt-1.24.pdf` — *CUNI-KIT System for Simultaneous Speech Translation Task at IWSLT 2022*. citeturn14view2  
- `https://github.com/collabora/WhisperLive` — repo WhisperLive. citeturn42view1  
- `https://github.com/ScienceIO/whisper_streaming_web` — FastAPI/WebSocket extension con preview incremental. citeturn36search1  
- `https://github.com/QUENTINFUXA/WHISPERLIVEKIT` — toolkit 2026 de integración y benchmarking. citeturn25view0  
- `https://github.com/ggml-org/whisper.cpp/blob/master/examples/stream/README.md` — ejemplo de streaming de whisper.cpp. citeturn26view0  
- `https://github.com/SYSTRAN/faster-whisper` — backend oficial `faster-whisper`. citeturn11view0  
- `https://opennmt.net/CTranslate2/quantization.html` — cuantización en CTranslate2. citeturn11view2  
- `https://github.com/openai/whisper` — repo oficial Whisper. citeturn11view1  
- `https://github.com/openai/whisper/blob/main/model-card.md` — model card oficial con tamaños y releases. citeturn11view3  
- `https://github.com/openai/whisper/discussions/2363` — discusión oficial de `turbo`. citeturn11view5  
- `https://github.com/snakers4/silero-vad` — repo oficial Silero VAD. citeturn28view0  
- `https://aclanthology.org/2022.autosimtrans-1.2/` — *Length-Adaptive Average Lagging*. citeturn30search15  
- `https://www.isca-archive.org/interspeech_2023/kano23_interspeech.pdf` — *Average Token Delay*. citeturn30search10  
- `https://arxiv.org/abs/2107.08807` — *Simultaneous Speech Translation for Live Subtitling: from Delay to Display*. citeturn31search1  
- `https://research.google/pubs/modeling-and-improving-text-stability-in-live-captions/` — texto sobre estabilidad/flicker en live captions. citeturn32search13  
- `https://ieeexplore.ieee.org/iel7/10022052/10022330/10023016.pdf` — *Flickering Reduction with Partial Hypothesis Reranking for Streaming ASR*. citeturn32search5

En una sola frase: **para tu tesis, la mejor decisión no es buscar el “parámetro mágico”, sino formalizar un subtitulador de dos capas —prefijo comprometido y cola provisional— con LocalAgreement explícito sobre faster-whisper, medir estabilidad de verdad, y usar SimulStreaming como referencia aspiracional más que como dependencia inmediata**. citeturn38view3turn14view1turn36search1